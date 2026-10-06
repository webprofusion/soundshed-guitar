#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/ModulationPresets.h"
#include "dsp/effects/TempoSync.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace guitarfx
{
namespace tremolo
{
/// Index order is stored in presets: append, never reorder.
enum class Mode
{
    Classic,  ///< the whole signal's level
    Harmonic, ///< lows and highs in opposite phase, as the 60s brown amps did it
    Pan,      ///< the signal moved between the speakers
    Slicer    ///< the level stepped through a rhythm pattern, a step each LFO cycle
};

inline const std::vector<std::string>& ModeLabels()
{
    static const std::vector<std::string> labels = {"Classic", "Harmonic", "Pan", "Slicer"};
    return labels;
}

/// Sixteen steps each, most significant bit first: 1 plays, 0 is cut by Depth.
/// Index order is stored in presets: append, never reorder.
inline constexpr std::array<std::uint16_t, 8> kPatterns = {
    0b1010101010101010, // Pulse
    0b1011101110111011, // Gallop
    0b1101110111011101, // Reverse Gallop
    0b0101010101010101, // Offbeat
    0b1001001010010010, // Tresillo (3+3+2)
    0b1011011011010110, // Syncopated
    0b1100110011001100, // Half Time
    0b1000100010101111, // Build
};

inline const std::vector<std::string>& PatternLabels()
{
    static const std::vector<std::string> labels = {"Pulse",    "Gallop",     "Reverse Gallop", "Offbeat",
                                                    "Tresillo", "Syncopated", "Half Time",      "Build"};
    return labels;
}

inline constexpr int kPatternSteps = 16;

/// The Harmonic split's default corner. The brown amps' crossover sat in the low mids, so the
/// lows and the highs each carry about half of a guitar.
inline constexpr double kDefaultCrossoverHz = 650.0;

/// A Slicer step's edge, at Shape 0 and at full Shape as a share of the step.
inline constexpr double kSliceEdgeMinMs = 1.5;
inline constexpr double kSliceEdgeMaxShare = 0.35;
} // namespace tremolo

/**
 * Tremolo: the level moved by an LFO, four ways.
 *
 * Classic moves the whole signal. Harmonic splits it at Crossover and moves the lows and the
 * highs in opposite phase, so the level barely changes and the tone sweeps instead. Pan moves
 * the signal between left and right at constant power, turning a mono input stereo. Slicer
 * steps through a sixteen-step rhythm, a step each LFO cycle (so with Sync on, Division is the
 * step length); Shape softens the steps' edges and Depth is how far the cut steps go down.
 */
class TremoloEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mDesignedCrossover = -1.0f;
        Reset();
    }

    void Reset() override
    {
        mPhase = 0.0;
        mStep = 0;
        mSliceGain = 1.0f;
        mSplitStates = {};
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const auto mode = static_cast<tremolo::Mode>(mMode.load(std::memory_order_relaxed));

        switch (mode)
        {
        case tremolo::Mode::Harmonic:
            ProcessHarmonic(inputs, outputs, numSamples);
            return;
        case tremolo::Mode::Pan:
            ProcessPan(inputs, outputs, numSamples);
            return;
        case tremolo::Mode::Slicer:
            ProcessSlicer(inputs, outputs, numSamples);
            return;
        case tremolo::Mode::Classic:
            break;
        }

        const float rateHz = GetEffectiveRateHz();
        const float depth = mDepth.load(std::memory_order_relaxed);
        const float shape = mShape.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);

        const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;

            float lfo = static_cast<float>(std::sin(mPhase));
            const float shaped = ShapeLfo(lfo, shape);
            const float mod = 0.5f * (1.0f + shaped);
            const float gain = (1.0f - depth) + depth * mod;

            const float wetL = inL * gain;
            const float wetR = inR * gain;

            const float outL = inL * (1.0f - mix) + wetL * mix;
            const float outR = inR * (1.0f - mix) + wetR * mix;

            if (outputs[0])
            {
                outputs[0][i] = outL;
            }

            if (outputs[1])
            {
                outputs[1][i] = outR;
            }

            mPhase += phaseInc;

            // Wrap phase to prevent floating-point precision drift over long runtimes
            if (mPhase >= 2.0 * kPi)
            {
                mPhase = std::fmod(mPhase, 2.0 * kPi);
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "bpm")
        {
            mBpm.store(tempo_sync::ClampBpm(value), std::memory_order_relaxed);
        }
        else if (key == "syncMode")
        {
            mSyncMode.store(tempo_sync::ClampSyncMode(value), std::memory_order_relaxed);
        }
        else if (key == "syncDivision")
        {
            mSyncDivision.store(tempo_sync::ClampDivision(value), std::memory_order_relaxed);
        }
        else if (key == "rate")
        {
            mRateHz.store(static_cast<float>(std::clamp(value, 0.1, 12.0)), std::memory_order_relaxed);
        }
        else if (key == "depth")
        {
            mDepth.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "shape")
        {
            mShape.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "mode")
        {
            mMode.store(static_cast<int>(std::lround(std::clamp(value, 0.0, 3.0))), std::memory_order_relaxed);
        }
        else if (key == "pattern")
        {
            mPattern.store(static_cast<int>(
                               std::lround(std::clamp(value, 0.0, static_cast<double>(tremolo::kPatterns.size() - 1)))),
                           std::memory_order_relaxed);
        }
        else if (key == "crossover")
        {
            mCrossoverHz.store(static_cast<float>(std::clamp(value, 200.0, 2000.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm.load(std::memory_order_relaxed);
        }

        if (key == "syncMode")
        {
            return mSyncMode.load(std::memory_order_relaxed);
        }

        if (key == "syncDivision")
        {
            return mSyncDivision.load(std::memory_order_relaxed);
        }

        if (key == "effectiveRate")
        {
            return GetEffectiveRateHz();
        }

        if (key == "rate")
        {
            return mRateHz.load(std::memory_order_relaxed);
        }

        if (key == "depth")
        {
            return mDepth.load(std::memory_order_relaxed);
        }

        if (key == "shape")
        {
            return mShape.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        if (key == "mode")
        {
            return mMode.load(std::memory_order_relaxed);
        }

        if (key == "pattern")
        {
            return mPattern.load(std::memory_order_relaxed);
        }

        if (key == "crossover")
        {
            return mCrossoverHz.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "tremolo";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    /// Pan moves a mono input between the speakers; every other mode moves both together.
    [[nodiscard]] bool ProducesStereoOutput() const override
    {
        return mMode.load(std::memory_order_relaxed) == static_cast<int>(tremolo::Mode::Pan) &&
               mDepth.load(std::memory_order_relaxed) > 0.0f && mMix.load(std::memory_order_relaxed) > 0.0f;
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    [[nodiscard]] float GetEffectiveRateHz() const
    {
        if (mSyncMode.load(std::memory_order_relaxed) != tempo_sync::kSyncModeTempo)
        {
            return mRateHz.load(std::memory_order_relaxed);
        }

        const double bpm = mBpm.load(std::memory_order_relaxed);
        const int division = mSyncDivision.load(std::memory_order_relaxed);

        // A Slicer step may be as short as the shortest division: a 1/32 at 300 bpm is 20 Hz.
        const double maxRate =
            mMode.load(std::memory_order_relaxed) == static_cast<int>(tremolo::Mode::Slicer) ? 40.0 : 12.0;
        return static_cast<float>(std::clamp(tempo_sync::DivisionRateHz(bpm, division), 0.1, maxRate));
    }

    static float ShapeLfo(float lfo, float shape)
    {
        const float amount = 1.0f + shape * 8.0f;
        return std::tanh(lfo * amount) / std::tanh(amount);
    }

    /// The Classic gain curve at `phase` radians, so the modes share one LFO and one Shape.
    [[nodiscard]] static float GainAt(double phase, float depth, float shape)
    {
        const float mod = 0.5f * (1.0f + ShapeLfo(static_cast<float>(std::sin(phase)), shape));
        return (1.0f - depth) + depth * mod;
    }

    void AdvancePhase(double phaseInc)
    {
        mPhase += phaseInc;

        if (mPhase >= 2.0 * kPi)
        {
            mPhase = std::fmod(mPhase, 2.0 * kPi);
            mStep = (mStep + 1) % tremolo::kPatternSteps;
        }
    }

    void ProcessHarmonic(float** inputs, float** outputs, int numSamples)
    {
        const float crossover = mCrossoverHz.load(std::memory_order_relaxed);

        if (crossover != mDesignedCrossover)
        {
            mSplit = biquad::LowPass(crossover, biquad::kButterworthQ, mSampleRate);
            mDesignedCrossover = crossover;
        }

        const float depth = mDepth.load(std::memory_order_relaxed);
        const float shape = mShape.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const double phaseInc = 2.0 * kPi * GetEffectiveRateHz() / std::max(1.0, mSampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            // Lows and highs in opposite phase. The highs are what the low-pass left, so the two
            // always add back to the input exactly.
            const float lowGain = GainAt(mPhase, depth, shape);
            const float highGain = GainAt(mPhase + kPi, depth, shape);

            for (int channel = 0; channel < 2; ++channel)
            {
                const float in = inputs[channel] ? inputs[channel][i] : 0.0f;
                const auto low = static_cast<float>(mSplitStates[channel].Process(mSplit, static_cast<double>(in)));
                const float wet = low * lowGain + (in - low) * highGain;

                if (outputs[channel])
                {
                    outputs[channel][i] = in * (1.0f - mix) + wet * mix;
                }
            }

            AdvancePhase(phaseInc);
        }

        // A value too large to be audio would otherwise stick in the split for good.
        for (auto& state : mSplitStates)
        {
            if (!IsFinite(state.s1) || !IsFinite(state.s2))
            {
                state.Reset();
            }
        }
    }

    void ProcessPan(float** inputs, float** outputs, int numSamples)
    {
        const float depth = mDepth.load(std::memory_order_relaxed);
        const float shape = mShape.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const double phaseInc = 2.0 * kPi * GetEffectiveRateHz() / std::max(1.0, mSampleRate);
        constexpr float kSqrt2 = 1.41421356f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : inL;

            // Constant power: centred, both sides at unity; hard over, one side at +3 dB and the
            // other silent, so a mono input is as loud wherever it is.
            const float position = depth * ShapeLfo(static_cast<float>(std::sin(mPhase)), shape);
            const float angle = static_cast<float>(0.25 * kPi) * (position + 1.0f);
            const float gainL = kSqrt2 * std::cos(angle);
            const float gainR = kSqrt2 * std::sin(angle);

            if (outputs[0])
            {
                outputs[0][i] = inL * (1.0f - mix) + inL * gainL * mix;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR * (1.0f - mix) + inR * gainR * mix;
            }

            AdvancePhase(phaseInc);
        }
    }

    void ProcessSlicer(float** inputs, float** outputs, int numSamples)
    {
        const float rateHz = GetEffectiveRateHz();
        const float depth = mDepth.load(std::memory_order_relaxed);
        const float shape = mShape.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const auto pattern = tremolo::kPatterns[static_cast<std::size_t>(
            std::clamp(mPattern.load(std::memory_order_relaxed), 0, static_cast<int>(tremolo::kPatterns.size()) - 1))];
        const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);

        // Each step's edges glide over a share of the step that Shape sets.
        const double stepMs = 1000.0 / std::max(0.1f, rateHz);
        const double edgeMs = std::max(tremolo::kSliceEdgeMinMs, shape * tremolo::kSliceEdgeMaxShare * stepMs);
        const auto glide = static_cast<float>(1.0 - std::exp(-1000.0 / (edgeMs * std::max(1.0, mSampleRate))));

        for (int i = 0; i < numSamples; ++i)
        {
            const bool on = (pattern >> (tremolo::kPatternSteps - 1 - mStep)) & 1u;
            mSliceGain += glide * ((on ? 1.0f : 1.0f - depth) - mSliceGain);

            for (int channel = 0; channel < 2; ++channel)
            {
                const float in = inputs[channel] ? inputs[channel][i] : 0.0f;

                if (outputs[channel])
                {
                    outputs[channel][i] = in * (1.0f - mix) + in * mSliceGain * mix;
                }
            }

            AdvancePhase(phaseInc);
        }
    }

    std::atomic<float> mRateHz{4.0f};
    std::atomic<float> mDepth{0.7f};
    std::atomic<float> mShape{0.0f};
    std::atomic<float> mMix{1.0f};
    std::atomic<float> mCrossoverHz{static_cast<float>(tremolo::kDefaultCrossoverHz)};
    std::atomic<double> mBpm{tempo_sync::kDefaultBpm};
    std::atomic<int> mSyncMode{tempo_sync::kSyncModeOff};
    std::atomic<int> mSyncDivision{4};
    std::atomic<int> mMode{0};
    std::atomic<int> mPattern{0};

    double mPhase = 0.0;
    int mStep = 0;
    float mSliceGain = 1.0f;
    float mDesignedCrossover = -1.0f;
    BiquadCoefficients mSplit;
    std::array<biquad::State, 2> mSplitStates{};
};

inline void RegisterTremoloEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTremolo;
    info.aliases = {"tremolo"};
    info.displayName = "Tremolo";
    info.category = "modulation";
    info.description = "Tremolo four ways: Classic, Harmonic (lows against highs), Pan (auto-pan) and Slicer "
                       "(rhythm patterns)";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.parameters = {
        {"mode", "Mode", 0.0, 0.0, 3.0, "enum", "", false, 1.0, tremolo::ModeLabels()},
        {"rate", "Rate", 4.0, 0.1, 12.0, "Hz"},
        {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "timing", false, 1.0, tempo_sync::SyncModeLabels()},
        {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "timing", false, 1.0, tempo_sync::DivisionLabels()},
        {"depth", "Depth", 0.7, 0.0, 1.0, "amount"},
        {"shape", "Shape", 0.0, 0.0, 1.0, "amount"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
        {"pattern", "Pattern", 0.0, 0.0, 7.0, "enum", "Slicer", false, 1.0, tremolo::PatternLabels()},
        WithLogTaper({"crossover", "Crossover", tremolo::kDefaultCrossoverHz, 200.0, 2000.0, "Hz", "Harmonic", true}),
    };
    info.presets = modulation_presets::Tremolo(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TremoloEffect>(); });
}
} // namespace guitarfx
