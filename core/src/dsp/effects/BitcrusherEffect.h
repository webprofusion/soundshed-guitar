#pragma once

/**
 * Bitcrusher: fewer bits and a lower sample rate, as an early sampler or a games console had.
 *
 * Two independent kinds of damage:
 *
 *   - Bits quantises the level. The quantiser truncates toward zero, so the output is never
 *     louder than the input, silence stays silent, and at low settings quiet notes fall into
 *     the dead zone around zero and gate off, which is much of the sound.
 *   - Downsample holds each sample for that many, with no filter before or after. The images
 *     and aliases that leaves are the effect, not a fault, so nothing here oversamples.
 *
 * Both are continuous: 10.5 bits and a hold of 3.3 samples are meaningful and let the controls
 * be swept without stepping.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace bitcrusher
{
inline constexpr double kMinBits = 1.0;
inline constexpr double kMaxBits = 16.0;
inline constexpr double kMinDownsample = 1.0;
inline constexpr double kMaxDownsample = 50.0;

/// `x` on a grid of 2^bits levels across [-1, 1), truncated toward zero.
[[nodiscard]] inline float Quantise(float x, float scale) noexcept
{
    const float clamped = std::clamp(x, -1.0f, 1.0f);
    return std::min(std::trunc(clamped * scale), scale - 1.0f) / scale;
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("subtle-grit", "Subtle Grit"),
        b.Make("8-bit", "8-Bit", {{"bits", 8.0}, {"downsample", 4.0}}),
        b.Make("sampler", "Sampler", {{"bits", 12.0}, {"downsample", 3.0}, {"mix", 0.85}}),
        b.Make("broken", "Broken", {{"bits", 4.0}, {"downsample", 14.0}}),
    };
}
} // namespace bitcrusher

class BitcrusherEffect : public EffectProcessor
{
  public:
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        Reset();
    }

    void Reset() override
    {
        mHeld = {};
        // More than two holds behind, so the first sample of a run is taken and starts a hold.
        const auto behind = static_cast<float>(2.0 * bitcrusher::kMaxDownsample);
        mCounter = {behind, behind};
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        for (int channel = 0; channel < 2; ++channel)
        {
            RunChannel(channel, inputs[channel], outputs[channel], numSamples);
        }
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        RunChannel(0, input, output, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "bits")
        {
            mBits.store(static_cast<float>(std::clamp(value, bitcrusher::kMinBits, bitcrusher::kMaxBits)),
                        std::memory_order_relaxed);
        }
        else if (key == "downsample")
        {
            mDownsample.store(
                static_cast<float>(std::clamp(value, bitcrusher::kMinDownsample, bitcrusher::kMaxDownsample)),
                std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bits")
        {
            return mBits.load(std::memory_order_relaxed);
        }

        if (key == "downsample")
        {
            return mDownsample.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "bitcrusher";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

  private:
    void RunChannel(int channel, const float* input, float* output, int numSamples) noexcept
    {
        if (!output)
        {
            return;
        }

        const float scale = std::exp2(mBits.load(std::memory_order_relaxed) - 1.0f);
        const float hold = mDownsample.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        float held = mHeld[static_cast<std::size_t>(channel)];
        float counter = mCounter[static_cast<std::size_t>(channel)];

        for (int i = 0; i < numSamples; ++i)
        {
            const float raw = input ? input[i] : 0.0f;
            const float dry = IsFinite(raw) ? raw : 0.0f;

            counter += 1.0f;

            if (counter >= hold)
            {
                // Keep the remainder, so a fractional hold averages out to its length.
                // More than one hold behind means a fresh start or a shortened hold: start level.
                counter -= hold;

                if (counter >= hold)
                {
                    counter = 0.0f;
                }

                held = bitcrusher::Quantise(dry, scale);
            }

            output[i] = dry + mix * (held - dry);
        }

        mHeld[static_cast<std::size_t>(channel)] = held;
        mCounter[static_cast<std::size_t>(channel)] = counter;
    }

    std::atomic<float> mBits{12.0f};
    std::atomic<float> mDownsample{2.0f};
    std::atomic<float> mMix{1.0f};
    std::array<float, 2> mHeld = {};
    std::array<float, 2> mCounter = {};
};

inline void RegisterBitcrusherEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kBitcrusher;
    info.aliases = {"bitcrusher"};
    info.displayName = "Bitcrusher";
    info.category = "drive";
    info.description = "Lo-fi digital: fewer bits and a lower sample rate, aliasing left in";
    info.requiresResource = false;
    info.parameters = {
        {"bits", "Bits", 12.0, bitcrusher::kMinBits, bitcrusher::kMaxBits, "bits"},
        WithLogTaper({"downsample", "Downsample", 2.0, bitcrusher::kMinDownsample, bitcrusher::kMaxDownsample, "x"}),
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = bitcrusher::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<BitcrusherEffect>(); });
}
} // namespace guitarfx
