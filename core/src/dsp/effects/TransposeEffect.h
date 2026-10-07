#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/SpliceTransposer.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Transpose: the whole input shifted by whole semitones, for playing in another tuning.
 *
 * Engines (`engine`):
 *   - High Quality (0, the default, so presets from before the second engine sound as they
 *     did): Signalsmith Stretch, 80 ms of latency. See SignalsmithSupport.h.
 *   - Low Latency (1): SpliceTransposer, a time-domain shifter that keeps attacks on time
 *     (they arrive 3-6 ms late shifting down) and reports its tap's mean delay, 16 ms. The
 *     global transpose always runs this one (GlobalChainEditor). docs/transpose-engine.md has
 *     the measurements.
 *
 * Latency contract:
 *   - When shifting: report the engine's latency and delay the dry signal by it before the
 *     wet/dry mix, so a partial mix does not comb.
 *   - When transparent (0 st): pass the input through and report 0 latency.
 *
 * Every change of path crossfades over kPathFadeSeconds: into and out of the transparent bypass,
 * and from one engine to the other, which both run until the fade ends. A hard switch jumped the
 * audio by the engine's latency with a click.
 *
 * Both engines' input histories are recorded on every sample, idle or not, so either can start on
 * real audio rather than replaying whatever it was left holding.
 */
class TransposeEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    static constexpr double kPathFadeSeconds = 0.010;

    enum class Engine
    {
        HighQuality = 0,
        LowLatency = 1
    };

    static constexpr double kLowLatencyEngine = 1.0;

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
        // Nothing has been fed yet, so an engine that starts next begins on whatever history has
        // accumulated by then.
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
            // Both engines are idle but their histories are not: they are what an engine starting
            // later begins on, so keep recording the input.
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

            // Divided by the gains' sum, so a fade that turns back halfway never dips.
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
            const int clamped = static_cast<int>(std::round(std::clamp(value, kMinSemitones, kMaxSemitones)));

            if (clamped != mSemitones)
            {
                mSemitones = clamped;
                ApplyTranspose();
            }
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "engine")
        {
            // Only a target: the paths crossfade in Process, so this is safe on the audio thread.
            mEngine = value >= 0.5 ? Engine::LowLatency : Engine::HighQuality;
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "semitones")
        {
            return static_cast<double>(mSemitones);
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

    [[nodiscard]] std::string GetType() const override
    {
        return "transpose";
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

    static constexpr double kMinSemitones = -36.0;
    static constexpr double kMaxSemitones = 12.0;

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
        return mSemitones == 0;
    }

    void ApplyTranspose()
    {
        mLive.SetSemitones(static_cast<double>(mSemitones), false);

        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        // Tonality limit is normalised to sample rate (Signalsmith API contract).
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mStretch.setTransposeSemitones(static_cast<float>(mSemitones), tonalityLimit);
    }

    static constexpr double kTonalityLimitHz = 16000.0;

    int mSemitones = 0;
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
    SpliceTransposer mLive;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
};

inline void RegisterTransposeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTranspose;
    info.aliases = {"transpose"};
    info.displayName = "Transpose";
    info.category = "pitch";
    info.description = "Transpose in whole semitones; the Low Latency engine keeps every attack on time";
    info.requiresResource = false;
    info.parameters = {{"semitones", "Semitones", 0.0, -36.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"engine", "Engine", 0.0, 0.0, 1.0, "enum", "", false, 1.0, {"High Quality", "Low Latency"}}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TransposeEffect>(); });
}
} // namespace guitarfx
