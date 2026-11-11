#pragma once

/**
 * Vinyl: the surface of a worn record laid over the signal.
 *
 * Three things a record adds, each on its own control:
 *
 *   - Crackle: pops and ticks from dust and scratches. Each is an impulse through a short
 *     resonance, at a random size and a random moment, a few to a few dozen a second.
 *   - Dust: the steady hush between them, noise shaped like a groove's (most of it in the
 *     upper mids).
 *   - Wear: the top end a worn groove no longer holds, as a low-pass that comes down from
 *     16 kHz to 3.5 kHz.
 *
 * Crackle and Dust are added to the signal; they do not depend on it, so they are there in the
 * gaps between notes, as on a record. Put it last in a chain, after the amp and cabinet: in front
 * of a distortion the noise would be amplified with the guitar.
 *
 * Both channels get the same surface, so the effect never widens a mono signal. The noise comes
 * from a fixed generator: the same settings always give the same record.
 */

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DelayLineSupport.h"
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
namespace vinyl
{
/// Pops a second with Crackle at 1. A badly kept record; the control is squared, so the middle
/// of its travel is a handful a second.
inline constexpr double kMaxPopsPerSecond = 40.0;
/// The largest pop's peak, about -26 dBFS: plainly there under a clean guitar, never a spike.
inline constexpr float kMaxPopPeak = 0.05f;
inline constexpr double kPopResonanceHz = 3200.0;
inline constexpr double kPopResonanceQ = 2.0;

/// Dust at 1, as the RMS of the noise added: about -58 dBFS.
inline constexpr double kMaxDustRms = 0.00125;
inline constexpr double kDustCentreHz = 2400.0;
inline constexpr double kDustQ = 0.6;

inline constexpr double kWearMinHz = 3500.0;
inline constexpr double kWearMaxHz = 16000.0;
/// Wear at or under this is out of the path.
inline constexpr double kWearBypassBelow = 0.005;

[[nodiscard]] inline double WearHz(double wear) noexcept
{
    return kWearMaxHz * std::pow(kWearMinHz / kWearMaxHz, std::clamp(wear, 0.0, 1.0));
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("well-loved", "Well Loved"),
        b.Make("new-pressing", "New Pressing", {{"crackle", 0.12}, {"dust", 0.2}, {"wear", 0.1}}),
        b.Make("charity-shop", "Charity Shop", {{"crackle", 0.7}, {"dust", 0.6}, {"wear", 0.6}}),
        b.Make("run-out-groove", "Run-Out Groove", {{"crackle", 1.0}, {"dust", 0.85}, {"wear", 0.8}}),
        b.Make("dust-only", "Dust Only", {{"crackle", 0.0}, {"dust", 0.6}, {"wear", 0.3}}),
    };
}
} // namespace vinyl

class VinylEffect : public EffectProcessor
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

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mPopFilter = BandPass(vinyl::kPopResonanceHz, vinyl::kPopResonanceQ, sampleRate);
        mDustFilter = BandPass(vinyl::kDustCentreHz, vinyl::kDustQ, sampleRate);
        mDesignedWear = -1.0f;
        Reset();
    }

    void Reset() override
    {
        mPopState.Reset();
        mDustState.Reset();
        mWearStates = {};
        mRng.Seed(0x33a71c5du);
        mWearShare = -1.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {inputs[0], inputs[1]};
        float* out[2] = {outputs[0], outputs[1]};
        Run(in, out, 2, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {input, nullptr};
        float* out[2] = {output, nullptr};
        Run(in, out, 1, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (std::atomic<float>* target = Find(key))
        {
            target->store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        const std::atomic<float>* source = const_cast<VinylEffect*>(this)->Find(key);
        return source ? static_cast<double>(source->load(std::memory_order_relaxed)) : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "vinyl";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

  private:
    /// Constant-peak band-pass: unity gain at its centre, so a filter's level is its input's.
    [[nodiscard]] static BiquadCoefficients BandPass(double hz, double q, double sampleRate) noexcept
    {
        const double w = 2.0 * biquad::kPi * std::min(hz, 0.45 * sampleRate) / sampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
    }

    [[nodiscard]] std::atomic<float>* Find(const std::string& key) noexcept
    {
        if (key == "crackle")
        {
            return &mCrackle;
        }

        if (key == "dust")
        {
            return &mDust;
        }

        if (key == "wear")
        {
            return &mWear;
        }

        if (key == "mix")
        {
            return &mMix;
        }

        return nullptr;
    }

    void Run(const float* const* in, float* const* out, int channels, int numSamples) noexcept
    {
        const float crackle = mCrackle.load(std::memory_order_relaxed);
        const float dust = mDust.load(std::memory_order_relaxed);
        const float wear = mWear.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const bool wearOn = wear > static_cast<float>(vinyl::kWearBypassBelow);

        if (wearOn && wear != mDesignedWear)
        {
            mWearFilter = biquad::LowPass(vinyl::WearHz(wear), biquad::kButterworthQ, mSampleRate);
            mDesignedWear = wear;
        }

        if (mWearShare < 0.0f)
        {
            mWearShare = wearOn ? 1.0f : 0.0f;
        }

        // Squared, so the lower half of each knob is the useful half.
        const auto popChance = static_cast<float>(crackle * crackle * vinyl::kMaxPopsPerSecond / mSampleRate);
        const float dustGain = dust * dust * static_cast<float>(vinyl::kMaxDustRms / kDustUnitRms);
        const auto fadeStep = static_cast<float>(1000.0 / (kFadeMs * mSampleRate));
        bool finite = true;

        for (int i = 0; i < numSamples; ++i)
        {
            float surface = 0.0f;

            if (popChance > 0.0f)
            {
                float impulse = 0.0f;

                // One draw decides whether a pop starts here; a second sizes and signs it.
                if (0.5f * (mRng.NextBipolar() + 1.0f) < popChance)
                {
                    const float size = mRng.NextBipolar();
                    // Cubed: most pops are ticks, a few are loud.
                    impulse = size * size * size;
                }

                surface += kPopGain * vinyl::kMaxPopPeak *
                           static_cast<float>(mPopState.Process(mPopFilter, static_cast<double>(impulse)));
            }

            if (dustGain > 0.0f)
            {
                surface += dustGain *
                           static_cast<float>(mDustState.Process(mDustFilter, static_cast<double>(mRng.NextBipolar())));
            }

            const float wearTarget = wearOn ? 1.0f : 0.0f;
            mWearShare = (mWearShare < wearTarget) ? std::min(wearTarget, mWearShare + fadeStep)
                                                   : std::max(wearTarget, mWearShare - fadeStep);

            for (int c = 0; c < channels; ++c)
            {
                const float sample = in[c] ? in[c][i] : 0.0f;
                const float dry = IsFinite(sample) ? sample : 0.0f;
                float wet = dry;

                if (wearOn || mWearShare > 0.0f)
                {
                    const auto worn = static_cast<float>(
                        mWearStates[static_cast<std::size_t>(c)].Process(mWearFilter, static_cast<double>(dry)));
                    wet += mWearShare * (worn - wet);
                }

                const float result = dry + mix * (wet + surface - dry);
                finite = finite && IsFinite(result);

                if (out[c])
                {
                    out[c][i] = IsFinite(result) ? result : 0.0f;
                }
            }
        }

        if (!finite || !IsFinite(mPopState.s1) || !IsFinite(mDustState.s1) || !IsFinite(mWearStates[0].s1) ||
            !IsFinite(mWearStates[1].s1))
        {
            mPopState.Reset();
            mDustState.Reset();
            mWearStates = {};
        }
    }

    /// RMS of uniform noise through the dust band-pass, and the peak an impulse leaves the pop
    /// resonance at, both measured at 48 kHz: what turns the two controls into the levels above.
    static constexpr double kDustUnitRms = 0.22;
    static constexpr float kPopGain = 1.0f / 0.19f;
    static constexpr double kFadeMs = 20.0;

    std::atomic<float> mCrackle{0.4f};
    std::atomic<float> mDust{0.35f};
    std::atomic<float> mWear{0.3f};
    std::atomic<float> mMix{1.0f};

    BiquadCoefficients mPopFilter;
    BiquadCoefficients mDustFilter;
    BiquadCoefficients mWearFilter;
    biquad::State mPopState;
    biquad::State mDustState;
    std::array<biquad::State, 2> mWearStates{};
    delay_line::Rng mRng;
    float mDesignedWear = -1.0f;
    float mWearShare = -1.0f;
};

inline void RegisterVinylEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kVinyl;
    info.aliases = {"vinyl"};
    info.displayName = "Vinyl";
    info.category = "drive";
    info.description = "A worn record's surface: crackle, dust and a duller top end. Put it last in a chain";
    info.requiresResource = false;
    info.parameters = {
        {"crackle", "Crackle", 0.4, 0.0, 1.0, "amount"},
        {"dust", "Dust", 0.35, 0.0, 1.0, "amount"},
        {"wear", "Wear", 0.3, 0.0, 1.0, "amount"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = vinyl::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<VinylEffect>(); });
}
} // namespace guitarfx
