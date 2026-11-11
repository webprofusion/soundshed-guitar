#pragma once

/**
 * Vowel Filter: a filter shaped like a mouth, so the guitar says "oo", "oh", "ah", "eh", "ee".
 *
 * A vowel is told apart from another by where its first three resonances (formants) sit. This
 * runs the signal through three band-passes at a vowel's formants and moves them from one vowel
 * to the next, which is the sound of a talk box or of a wah with a voice. The Vowel control goes
 * through U, O, A, E, I, the order in which the second formant climbs, so a sweep opens the mouth
 * from "oo" to "ee" without doubling back.
 *
 * Three ways to move it:
 *   - Manual: it stays at Vowel.
 *   - LFO: it sweeps around Vowel at Rate, as far as Depth.
 *   - Envelope: picking harder opens it further, by Depth for a note at Sensitivity's level.
 *
 * The formants are designed afresh every few samples, so the sweep is smooth, and both channels
 * are moved together, so it never widens a mono signal. Before the amp it talks through the
 * distortion, as a wah does; after it, it is cleaner and more obviously a filter.
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
namespace vowel_filter
{
inline constexpr int kFormants = 3;
inline constexpr int kVowels = 5;

/// The first three formants of each vowel, in Hz (Peterson and Barney's averages for a man's
/// voice, rounded), in the order the Vowel control visits them: U, O, A, E, I.
inline constexpr double kFormantHz[kVowels][kFormants] = {
    {300.0, 870.0, 2240.0},  // U, as in "boot"
    {570.0, 840.0, 2410.0},  // O, as in "bought"
    {730.0, 1090.0, 2440.0}, // A, as in "father"
    {530.0, 1840.0, 2480.0}, // E, as in "bet"
    {270.0, 2290.0, 3010.0}, // I, as in "beet"
};

/// Each formant's level against the first: the higher ones are weaker in a voice.
inline constexpr double kFormantGain[kFormants] = {1.0, 0.5, 0.25};
/// How sharp each formant is at Resonance 0 and at 1.
inline constexpr double kMinQ = 3.0;
inline constexpr double kMaxQ = 14.0;
/// Three narrow bands pass far less than the whole signal; this brings a guitar back to about
/// where it was at the default Resonance.
inline constexpr double kMakeupDb = 14.0;
/// The formants are redesigned this often.
inline constexpr int kControlInterval = 16;

inline const std::vector<std::string>& ModeLabels()
{
    static const std::vector<std::string> labels = {"Manual", "LFO", "Envelope"};
    return labels;
}

/// The frequency of `formant` at `position` in 0..1 along U, O, A, E, I, moved between
/// neighbouring vowels by ratio, as pitch is heard.
[[nodiscard]] inline double FormantHz(double position, int formant) noexcept
{
    const double scaled = std::clamp(position, 0.0, 1.0) * (kVowels - 1);
    const int lower = std::min(static_cast<int>(scaled), kVowels - 2);
    const double t = scaled - lower;
    return kFormantHz[lower][formant] * std::pow(kFormantHz[lower + 1][formant] / kFormantHz[lower][formant], t);
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        b.Defaults("talking-wah", "Talking Wah"),
        b.Make("slow-yawn", "Slow Yawn", {{"mode", 1.0}, {"rate", 0.35}, {"depth", 1.0}, {"vowel", 0.5}}),
        b.Make("robot-chatter", "Robot Chatter",
               {{"mode", 1.0}, {"rate", 5.0}, {"depth", 0.7}, {"resonance", 0.8}, {"vowel", 0.5}}),
        b.Make("fixed-ah", "Fixed Ah", {{"mode", 0.0}, {"vowel", 0.5}, {"resonance", 0.6}}),
        b.Make("fixed-ee", "Fixed Ee", {{"mode", 0.0}, {"vowel", 1.0}, {"resonance", 0.6}}),
    };
}
} // namespace vowel_filter

class VowelFilterEffect : public EffectProcessor
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
        mEnvelope.Prepare(sampleRate);
        mEnvelope.SetTimes(4.0, 180.0);
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mStates)
        {
            for (auto& state : channel)
            {
                state.Reset();
            }
        }

        mEnvelope.Reset();
        mPhase = 0.0;
        mCountdown = 0;
        mPosition = -1.0;
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

        if (key == "mode")
        {
            mMode.store(static_cast<int>(std::lround(std::clamp(value, 0.0, 2.0))), std::memory_order_relaxed);
        }
        else if (key == "rate")
        {
            mRate.store(static_cast<float>(std::clamp(value, 0.05, 10.0)), std::memory_order_relaxed);
        }
        else if (std::atomic<float>* target = FindAmount(key))
        {
            target->store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "mode")
        {
            return mMode.load(std::memory_order_relaxed);
        }

        if (key == "rate")
        {
            return mRate.load(std::memory_order_relaxed);
        }

        const std::atomic<float>* source = const_cast<VowelFilterEffect*>(this)->FindAmount(key);
        return source ? static_cast<double>(source->load(std::memory_order_relaxed)) : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "vowel_filter";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    static constexpr double kTwoPi = 6.28318530717958648;

    [[nodiscard]] std::atomic<float>* FindAmount(const std::string& key) noexcept
    {
        if (key == "vowel")
        {
            return &mVowel;
        }

        if (key == "depth")
        {
            return &mDepth;
        }

        if (key == "sensitivity")
        {
            return &mSensitivity;
        }

        if (key == "resonance")
        {
            return &mResonance;
        }

        if (key == "mix")
        {
            return &mMix;
        }

        return nullptr;
    }

    /// Constant-peak band-pass: unity at its centre, whatever the Q.
    [[nodiscard]] BiquadCoefficients BandPass(double hz, double q) const noexcept
    {
        const double w = kTwoPi * std::clamp(hz, 40.0, 0.45 * mSampleRate) / mSampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
    }

    void Design(double position, double q) noexcept
    {
        for (int f = 0; f < vowel_filter::kFormants; ++f)
        {
            mSections[static_cast<std::size_t>(f)] = BandPass(vowel_filter::FormantHz(position, f), q);
        }
    }

    void Run(const float* const* in, float* const* out, int channels, int numSamples) noexcept
    {
        const int mode = mMode.load(std::memory_order_relaxed);
        const double vowel = mVowel.load(std::memory_order_relaxed);
        const double depth = mDepth.load(std::memory_order_relaxed);
        const double sensitivity = mSensitivity.load(std::memory_order_relaxed);
        const double resonance = mResonance.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const double q = vowel_filter::kMinQ * std::pow(vowel_filter::kMaxQ / vowel_filter::kMinQ, resonance);
        const double phaseStep = kTwoPi * mRate.load(std::memory_order_relaxed) / mSampleRate;
        // The level a note has to reach to open the filter by the whole of Depth: -6 dBFS at
        // Sensitivity 0, down to -42 dBFS at 1.
        const double fullScale = std::pow(10.0, (-6.0 - 36.0 * sensitivity) / 20.0);
        const auto makeup = static_cast<float>(std::pow(10.0, vowel_filter::kMakeupDb / 20.0));
        bool finite = true;

        for (int i = 0; i < numSamples; ++i)
        {
            float dry[2] = {0.0f, 0.0f};
            float sum = 0.0f;

            for (int c = 0; c < channels; ++c)
            {
                const float sample = in[c] ? in[c][i] : 0.0f;
                dry[c] = IsFinite(sample) ? std::clamp(sample, -8.0f, 8.0f) : 0.0f;
                sum += dry[c];
            }

            const double level = mEnvelope.Process(sum / static_cast<float>(channels));

            if (--mCountdown <= 0)
            {
                mCountdown = vowel_filter::kControlInterval;
                double target = vowel;

                if (mode == 1)
                {
                    target = vowel + 0.5 * depth * std::sin(mPhase);
                }
                else if (mode == 2)
                {
                    target = vowel + depth * std::min(1.0, level / fullScale);
                }

                target = std::clamp(target, 0.0, 1.0);
                // The first design of a run lands on the target; after that the mouth takes a
                // few milliseconds to move, which is also what keeps a knob turn from zipping.
                mPosition = mPosition < 0.0 ? target : mPosition + 0.35 * (target - mPosition);
                Design(mPosition, q);
            }

            mPhase += phaseStep;

            if (mPhase >= kTwoPi)
            {
                mPhase -= kTwoPi;
            }

            for (int c = 0; c < channels; ++c)
            {
                auto& states = mStates[static_cast<std::size_t>(c)];
                double voiced = 0.0;

                for (int f = 0; f < vowel_filter::kFormants; ++f)
                {
                    voiced += vowel_filter::kFormantGain[f] *
                              states[static_cast<std::size_t>(f)].Process(mSections[static_cast<std::size_t>(f)],
                                                                          static_cast<double>(dry[c]));
                }

                const float wet = static_cast<float>(voiced) * makeup;
                const float result = dry[c] + mix * (wet - dry[c]);
                finite = finite && IsFinite(result);

                if (out[c])
                {
                    out[c][i] = IsFinite(result) ? result : 0.0f;
                }
            }
        }

        if (!finite)
        {
            for (auto& channel : mStates)
            {
                for (auto& state : channel)
                {
                    state.Reset();
                }
            }
        }
    }

    std::atomic<int> mMode{2};
    std::atomic<float> mVowel{0.15f};
    std::atomic<float> mRate{1.0f};
    std::atomic<float> mDepth{0.8f};
    std::atomic<float> mSensitivity{0.5f};
    std::atomic<float> mResonance{0.5f};
    std::atomic<float> mMix{1.0f};

    std::array<BiquadCoefficients, vowel_filter::kFormants> mSections{};
    std::array<std::array<biquad::State, vowel_filter::kFormants>, 2> mStates{};
    delay_line::EnvelopeFollower mEnvelope;
    double mPhase = 0.0;
    double mPosition = -1.0;
    int mCountdown = 0;
};

inline void RegisterVowelFilterEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kVowelFilter;
    info.aliases = {"vowel_filter"};
    info.displayName = "Vowel Filter";
    info.category = "modulation";
    info.description = "A filter shaped like a mouth: the guitar says oo, oh, ah, eh, ee. By hand, by LFO, "
                       "or by how hard you pick";
    info.requiresResource = false;
    info.parameters = {
        {"mode", "Mode", 2.0, 0.0, 2.0, "enum", "", false, 1.0, vowel_filter::ModeLabels()},
        {"vowel", "Vowel", 0.15, 0.0, 1.0, "amount"},
        {"depth", "Depth", 0.8, 0.0, 1.0, "amount"},
        WithLogTaper({"rate", "Rate", 1.0, 0.05, 10.0, "Hz", "LFO"}),
        {"sensitivity", "Sensitivity", 0.5, 0.0, 1.0, "amount", "Envelope"},
        {"resonance", "Resonance", 0.5, 0.0, 1.0, "amount"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = vowel_filter::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<VowelFilterEffect>(); });
}
} // namespace guitarfx
