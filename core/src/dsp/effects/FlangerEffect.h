#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/TempoSync.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Flanger effectusing short modulated delay with feedback.
 */
class FlangerEffect : public EffectProcessor
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
        const double maxDelayMs = 10.0;
        mBufferSize = static_cast<int>(std::ceil(maxDelayMs * sampleRate / 1000.0)) + 2;
        mDelayBufferL.assign(mBufferSize, 0.0f);
        mDelayBufferR.assign(mBufferSize, 0.0f);
        UpdateFeedbackDamping();
        Reset();
    }

    void Reset() override
    {
        std::fill(mDelayBufferL.begin(), mDelayBufferL.end(), 0.0f);
        std::fill(mDelayBufferR.begin(), mDelayBufferR.end(), 0.0f);
        mWriteIndex = 0;
        mPhase = 0.0;
        mFeedbackStateL = 0.0f;
        mFeedbackStateR = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (mBufferSize == 0)
        {
            return;
        }

        if (!mEnabled)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (outputs[ch])
                {
                    if (inputs[ch])
                    {
                        std::copy_n(inputs[ch], numSamples, outputs[ch]);
                    }
                    else if (ch == 1 && inputs[0])
                    {
                        std::copy_n(inputs[0], numSamples, outputs[ch]);
                    }
                    else
                    {
                        std::fill_n(outputs[ch], numSamples, 0.0f);
                    }
                }
            }
            return;
        }

        const float rateHz = GetEffectiveRateHz();
        const float depthMs = mDepthMs.load(std::memory_order_relaxed);
        const float delayMs = mDelayMs.load(std::memory_order_relaxed);
        const float feedback = mFeedback.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);

        const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;

            const float lfoL = static_cast<float>(std::sin(mPhase));
            const float lfoR = static_cast<float>(std::sin(mPhase + kHalfPi));

            const float modL = 0.5f * (1.0f + lfoL);
            const float modR = 0.5f * (1.0f + lfoR);

            const float delayMsL = delayMs + depthMs * modL;
            const float delayMsR = delayMs + depthMs * modR;

            const float delayedL = ReadDelaySafe(mDelayBufferL, delayMsL);
            const float delayedR = ReadDelaySafe(mDelayBufferR, delayMsR);

            // Condition the feedback path before writing back into the modulated
            // comb filter. Hard clipping here creates sharp discontinuities that
            // a downstream convolver turns into broadband noise.
            float feedbackL = ApplyFeedbackDamping(mFeedbackStateL, delayedL) * feedback;
            float feedbackR = ApplyFeedbackDamping(mFeedbackStateR, delayedR) * feedback;
            feedbackL = SoftLimit(feedbackL);
            feedbackR = SoftLimit(feedbackR);

            float writeL = inL + feedbackL;
            float writeR = inR + feedbackR;
            if (!std::isfinite(writeL))
            {
                writeL = 0.0f;
            }
            if (!std::isfinite(writeR))
            {
                writeR = 0.0f;
            }
            mDelayBufferL[mWriteIndex] = writeL;
            mDelayBufferR[mWriteIndex] = writeR;

            float outL = inL * (1.0f - mix) + delayedL * mix;
            float outR = inR * (1.0f - mix) + delayedR * mix;

            // Safety: guard downstream effects against NaN/Inf
            if (!std::isfinite(outL))
            {
                outL = 0.0f;
            }
            if (!std::isfinite(outR))
            {
                outR = 0.0f;
            }

            if (outputs[0])
            {
                outputs[0][i] = outL;
            }
            if (outputs[1])
            {
                outputs[1][i] = outR;
            }

            AdvanceWriteIndex();
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
            mRateHz.store(static_cast<float>(std::clamp(value, 0.05, 5.0)), std::memory_order_relaxed);
        }
        else if (key == "depth")
        {
            mDepthMs.store(static_cast<float>(std::clamp(value, 0.0, 5.0)), std::memory_order_relaxed);
        }
        else if (key == "delay")
        {
            mDelayMs.store(static_cast<float>(std::clamp(value, 0.1, 5.0)), std::memory_order_relaxed);
        }
        else if (key == "feedback")
        {
            mFeedback.store(static_cast<float>(std::clamp(value, 0.0, 0.85)), std::memory_order_relaxed);
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
            return mDepthMs.load(std::memory_order_relaxed);
        }
        if (key == "delay")
        {
            return mDelayMs.load(std::memory_order_relaxed);
        }
        if (key == "feedback")
        {
            return mFeedback.load(std::memory_order_relaxed);
        }
        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "flanger";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kHalfPi = 1.57079632679489661923;

    [[nodiscard]] float GetEffectiveRateHz() const
    {
        if (mSyncMode.load(std::memory_order_relaxed) != tempo_sync::kSyncModeTempo)
        {
            return mRateHz.load(std::memory_order_relaxed);
        }

        const double bpm = mBpm.load(std::memory_order_relaxed);
        const int division = mSyncDivision.load(std::memory_order_relaxed);
        return static_cast<float>(std::clamp(tempo_sync::DivisionRateHz(bpm, division), 0.05, 5.0));
    }

    void UpdateFeedbackDamping()
    {
        constexpr float kFeedbackCutoffHz = 6000.0f;
        const float x = static_cast<float>(2.0 * kPi * kFeedbackCutoffHz / std::max(1.0, mSampleRate));
        mFeedbackDampingCoef = 1.0f - std::exp(-x);
    }

    float ApplyFeedbackDamping(float& state, float sample)
    {
        state += mFeedbackDampingCoef * (sample - state);
        return state;
    }

    static float SoftLimit(float sample)
    {
        constexpr float kDrive = 2.0f;
        return std::tanh(sample * kDrive) / kDrive;
    }

    float ReadDelaySafe(const std::vector<float>& buffer, float delayMs)
    {
        const float delaySamples = delayMs * static_cast<float>(mSampleRate) / 1000.0f;
        float readIndex = static_cast<float>(mWriteIndex) - delaySamples;
        const float bufSize = static_cast<float>(mBufferSize);
        readIndex = std::fmod(readIndex + bufSize, bufSize);

        const int index0 = static_cast<int>(readIndex);
        const int index1 = (index0 + 1) % mBufferSize;
        const float frac = readIndex - static_cast<float>(index0);

        const float result = buffer[index0] * (1.0f - frac) + buffer[index1] * frac;
        // A NaN in the history (from a prior corrupted buffer) must not re-enter
        // the feedback loop, as it would corrupt every subsequent output sample.
        return std::isfinite(result) ? result : 0.0f;
    }

    void AdvanceWriteIndex()
    {
        mWriteIndex = (mWriteIndex + 1) % mBufferSize;
    }

    std::vector<float> mDelayBufferL;
    std::vector<float> mDelayBufferR;
    int mBufferSize = 0;
    int mWriteIndex = 0;

    std::atomic<float> mRateHz{0.25f};
    std::atomic<float> mDepthMs{2.0f};
    std::atomic<float> mDelayMs{1.0f};
    std::atomic<float> mFeedback{0.2f};
    std::atomic<float> mMix{0.5f};
    std::atomic<double> mBpm{tempo_sync::kDefaultBpm};
    std::atomic<int> mSyncMode{tempo_sync::kSyncModeOff};
    std::atomic<int> mSyncDivision{4};

    double mPhase = 0.0;
    float mFeedbackStateL = 0.0f;
    float mFeedbackStateR = 0.0f;
    float mFeedbackDampingCoef = 1.0f;
};

inline void RegisterFlangerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kFlanger;
    info.aliases = {"flanger"};
    info.displayName = "Flanger";
    info.category = "modulation";
    info.description = "Short modulated delay flanger";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.parameters = {
        {"rate", "Rate", 0.25, 0.05, 5.0, "Hz"},
        {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "timing", false, 1.0, tempo_sync::SyncModeLabels()},
        {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "timing", false, 1.0, tempo_sync::DivisionLabels()},
        {"depth", "Depth", 2.0, 0.0, 5.0, "ms"},
        {"delay", "Delay", 1.0, 0.1, 5.0, "ms"},
        {"feedback", "Feedback", 0.2, 0.0, 0.85, "amount"},
        {"mix", "Mix", 0.5, 0.0, 1.0, "amount"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<FlangerEffect>(); });
}

} // namespace guitarfx
