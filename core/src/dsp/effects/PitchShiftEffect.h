#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/TimeDomainPitchShifter.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Pitch-shift effect with a direct semitone control and two engines.
 *
 * The shift applied is `semitones` held inside the node's own range (`minSemitones` to
 * `maxSemitones`), rounded to a whole semitone while `stepMode` is on. Automation reads the
 * same range through GetAutomationRange(), so an expression pedal sweeps exactly that
 * interval: gliding when free, stepping when snapped.
 *
 * Engines (`engine`):
 *   - High Quality (0, the default, so presets from before the switch sound as they did):
 *     Signalsmith Stretch. 80 ms of latency, and a new shift is only heard about 40 ms after
 *     it is set, since Stretch takes it at its next analysis frame and fades it in over its
 *     synthesis window. See SignalsmithSupport.h.
 *   - Low Latency (1): TimeDomainPitchShifter. A new shift is heard on the next sample, and the
 *     audio is 5-15 ms late depending on the shift; the price is a faint flutter on chords.
 *     This is the one for an expression pedal.
 *
 * Latency contract:
 *   - When shifting: report the engine's latency and delay the dry signal by it before the
 *     wet/dry mix, so a partial mix does not comb.
 *   - When transparent (0 st): pass the input through and report 0 latency.
 *
 * Every change of path crossfades over kPathFadeSeconds: into and out of the transparent
 * bypass, and from one engine to the other, which both run until the fade ends. The engines'
 * latencies differ from the bypass's, so a hard switch jumped the audio in time (80 ms, for
 * High Quality) with a click; faded, the jump is a short blend instead.
 *
 * Both engines' input histories are recorded on every sample, idle or not, so either can
 * start on real audio rather than replaying whatever it was left holding.
 */
class PitchShiftEffect : public EffectProcessor
{
  public:
    static constexpr double kPathFadeSeconds = 0.010;

    enum class Engine
    {
        HighQuality = 0,
        LowLatency = 1
    };

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mPathFadeStep = 1.0f / std::max(1.0f, static_cast<float>(sampleRate * kPathFadeSeconds));

        mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        ConfigureSignalsmithLive(mStretch, 2, sampleRate);
        mLive.Prepare(sampleRate);
        // Mark configured before ApplyTranspose so preloaded semitones (SetParam
        // before Prepare) are applied to the stretch engine. Matches TransposeEffect.
        mConfigured = true;
        ApplyTranspose();
        mDry.Prepare(SignalsmithTotalLatencySamples(mStretch), mStretch.seekLength(), maxBlockSize);
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mStretch.reset();
            mLive.Reset();
        }

        mDry.Reset();
        // Nothing has been fed yet, so an engine that starts next re-seeks onto whatever history
        // has accumulated by then.
        mNeedsEngage = true;
        mLiveNeedsEngage = true;

        // No fade from whatever path was playing before: there is nothing to fade from.
        mDryGain = TargetGain(Path::Dry);
        mHighQualityGain = TargetGain(Path::HighQuality);
        mLowLatencyGain = TargetGain(Path::LowLatency);
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs)
        {
            return;
        }

        // Clamp to allocated buffer size to prevent out-of-bounds writes
        numSamples = std::min(numSamples, mMaxBlockSize);

        if (numSamples <= 0)
        {
            return;
        }

        if (!mConfigured)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const float dryTarget = TargetGain(Path::Dry);
        const float highQualityTarget = TargetGain(Path::HighQuality);
        const float lowLatencyTarget = TargetGain(Path::LowLatency);
        const bool runHighQuality = mHighQualityGain > 0.0f || highQualityTarget > 0.0f;
        const bool runLowLatency = mLowLatencyGain > 0.0f || lowLatencyTarget > 0.0f;

        // Transparent and settled: no engine latency, report 0 via GetLatencySamples().
        if (!runHighQuality && !runLowLatency)
        {
            // Both engines are idle but their histories are not: they are what an engine
            // starting later begins on, so keep recording the input.
            for (int i = 0; i < numSamples; ++i)
            {
                const float inL = inputs[0] ? inputs[0][i] : 0.0f;
                const float inR = inputs[1] ? inputs[1][i] : 0.0f;
                mDry.Push(inL, inR);
                mLive.Write(inL, inR);
            }

            CopyStereoInputToOutput(inputs, outputs, numSamples);
            mNeedsEngage = true;
            mLiveNeedsEngage = true;
            return;
        }

        if (runHighQuality)
        {
            if (mNeedsEngage)
            {
                EngageSignalsmith(mStretch, mDry);
                mNeedsEngage = false;
            }

            if (static_cast<size_t>(numSamples) > mWetL.size())
            {
                mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
                mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
                mZero.resize(static_cast<size_t>(numSamples), 0.0f);
            }

            float* inputPtrs[2] = {inputs[0] ? inputs[0] : mZero.data(), inputs[1] ? inputs[1] : mZero.data()};
            float* wetPtrs[2] = {mWetL.data(), mWetR.data()};

            // Equal in/out lengths = pitch-only (no time stretch).
            mStretch.process(inputPtrs, numSamples, wetPtrs, numSamples);
        }
        else
        {
            mNeedsEngage = true;
        }

        if (runLowLatency && mLiveNeedsEngage)
        {
            mLive.Engage();
            mLiveNeedsEngage = false;
        }
        else if (!runLowLatency)
        {
            mLiveNeedsEngage = true;
        }

        const float dryMix = static_cast<float>(1.0 - mMix);
        const float wetMix = static_cast<float>(mMix);
        const int highQualityLatency = SignalsmithTotalLatencySamples(mStretch);
        const int lowLatencyLatency = mLive.NominalLatencySamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;

            // Unconditional: the histories have to stay current for a later Mix turn-down or
            // engine start, not just for this block's blend.
            float dryL = 0.0f;
            float dryR = 0.0f;
            mDry.PushAndRead(inL, inR, highQualityLatency, dryL, dryR);
            mLive.Write(inL, inR);

            float sumL = mDryGain * inL;
            float sumR = mDryGain * inR;
            float gainSum = mDryGain;

            if (runHighQuality)
            {
                sumL += mHighQualityGain * (dryL * dryMix + mWetL[static_cast<size_t>(i)] * wetMix);
                sumR += mHighQualityGain * (dryR * dryMix + mWetR[static_cast<size_t>(i)] * wetMix);
                gainSum += mHighQualityGain;
            }

            if (runLowLatency)
            {
                float wetL = 0.0f;
                float wetR = 0.0f;
                mLive.Process(wetL, wetR);
                float alignedL = 0.0f;
                float alignedR = 0.0f;
                mLive.ReadDelayed(lowLatencyLatency, alignedL, alignedR);
                sumL += mLowLatencyGain * (alignedL * dryMix + wetL * wetMix);
                sumR += mLowLatencyGain * (alignedR * dryMix + wetR * wetMix);
                gainSum += mLowLatencyGain;
            }

            // Divided by the gains' sum, so a fade that changes direction halfway (a pedal
            // passing back through 0 st) never dips.
            const float scale = gainSum > 1.0e-6f ? 1.0f / gainSum : 0.0f;

            if (outputs[0])
            {
                outputs[0][i] = sumL * scale;
            }

            if (outputs[1])
            {
                outputs[1][i] = sumR * scale;
            }

            mDryGain = StepToward(mDryGain, dryTarget);
            mHighQualityGain = StepToward(mHighQualityGain, highQualityTarget);
            mLowLatencyGain = StepToward(mLowLatencyGain, lowLatencyTarget);
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "semitones")
        {
            mSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "minSemitones")
        {
            mMinSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "maxSemitones")
        {
            mMaxSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "stepMode")
        {
            mStepMode = value >= 0.5;
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
            return;
        }
        else if (key == "engine")
        {
            // Only a target: the paths crossfade in Process, so this is safe on the audio thread.
            mEngine = value >= 0.5 ? Engine::LowLatency : Engine::HighQuality;
            return;
        }
        else
        {
            return;
        }

        ApplyTranspose();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "semitones")
        {
            return mSemitones;
        }

        if (key == "minSemitones")
        {
            return mMinSemitones;
        }

        if (key == "maxSemitones")
        {
            return mMaxSemitones;
        }

        if (key == "stepMode")
        {
            return mStepMode ? 1.0 : 0.0;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "engine")
        {
            return mEngine == Engine::LowLatency ? 1.0 : 0.0;
        }

        return 0.0;
    }

    [[nodiscard]] bool GetAutomationRange(const std::string& key, ParamRange& range) const override
    {
        if (key != "semitones")
        {
            return false;
        }

        range.minValue = LowerBound();
        range.maxValue = UpperBound();
        range.step = mStepMode ? 1.0 : 0.0;
        return true;
    }

    /// The shift actually applied, after the range and the snap.
    [[nodiscard]] double GetAppliedSemitones() const
    {
        return mAppliedSemitones;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "pitch_shift";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        if (!mConfigured || IsTransparent())
        {
            return 0;
        }

        return mEngine == Engine::LowLatency ? mLive.NominalLatencySamples() : SignalsmithTotalLatencySamples(mStretch);
    }

  private:
    enum class Path
    {
        Dry,
        HighQuality,
        LowLatency
    };

    [[nodiscard]] float TargetGain(Path path) const
    {
        if (IsTransparent())
        {
            return path == Path::Dry ? 1.0f : 0.0f;
        }

        const Path engine = mEngine == Engine::LowLatency ? Path::LowLatency : Path::HighQuality;
        return path == engine ? 1.0f : 0.0f;
    }

    [[nodiscard]] float StepToward(float gain, float target) const
    {
        return gain < target ? std::min(target, gain + mPathFadeStep) : std::max(target, gain - mPathFadeStep);
    }

    [[nodiscard]] bool IsTransparent() const
    {
        return std::abs(mAppliedSemitones) < 1.0e-9;
    }

    // The bounds are read in either order, so a preset loading them one at a time never
    // clamps one against the other's previous value.
    [[nodiscard]] double LowerBound() const
    {
        return std::min(mMinSemitones, mMaxSemitones);
    }

    [[nodiscard]] double UpperBound() const
    {
        return std::max(mMinSemitones, mMaxSemitones);
    }

    void ApplyTranspose()
    {
        // Snap first and clamp after, so the range wins when a bound is not a whole semitone.
        const double requested = mStepMode ? std::round(mSemitones) : mSemitones;
        mAppliedSemitones = std::clamp(requested, LowerBound(), UpperBound());

        // A free range glides between a controller's steps; a snapped one lands on each at once.
        mLive.SetSemitones(mAppliedSemitones, !mStepMode);

        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        // Tonality limit is normalised to sample rate (Signalsmith API contract).
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mStretch.setTransposeSemitones(static_cast<float>(mAppliedSemitones), tonalityLimit);
    }

    static constexpr double kTonalityLimitHz = 8000.0;

    static constexpr double kHardMinSemitones = -12.0;
    static constexpr double kHardMaxSemitones = 12.0;

    double mSemitones = 0.0;
    double mMinSemitones = kHardMinSemitones;
    double mMaxSemitones = kHardMaxSemitones;
    bool mStepMode = true;
    double mAppliedSemitones = 0.0;
    double mMix = 1.0;
    Engine mEngine = Engine::HighQuality;
    bool mConfigured = false;
    bool mNeedsEngage = true;
    bool mLiveNeedsEngage = true;

    float mPathFadeStep = 1.0f / 480.0f;
    float mDryGain = 1.0f;
    float mHighQualityGain = 0.0f;
    float mLowLatencyGain = 0.0f;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    SignalsmithDryHistory mDry;
    TimeDomainPitchShifter mLive;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
};

inline void RegisterPitchShiftEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kPitchShift;
    info.aliases = {"pitch_shift"};
    info.displayName = "Pitch Shift";
    info.category = "pitch";
    info.description = "Pitch shift, free or in whole semitones, within a range an expression pedal sweeps; "
                       "the Low Latency engine follows a pedal instantly";
    info.requiresResource = false;
    // semitones keeps its declared step of 1 for renderers that do not know about stepMode,
    // which is on by default. The params panel and automation take the live range and step
    // from the node instead.
    info.parameters = {{"semitones", "Semitones", 0.0, -12.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"engine", "Engine", 0.0, 0.0, 1.0, "enum", "", false, 1.0, {"High Quality", "Low Latency"}},
                       {"stepMode", "Snap to Semitone", 1.0, 0.0, 1.0, "toggle"},
                       {"minSemitones", "Range Min", -12.0, -12.0, 12.0, "st", "", false, 1.0},
                       {"maxSemitones", "Range Max", 12.0, -12.0, 12.0, "st", "", false, 1.0}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<PitchShiftEffect>(); });
}
} // namespace guitarfx
