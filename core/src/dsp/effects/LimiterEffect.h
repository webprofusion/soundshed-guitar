#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/DriveOutputLimiter.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
/**
 * Brickwall limiter with instantaneous gain reduction and smoothed release.
 * Prevents output from exceeding a ceiling (dBFS).
 */
class LimiterEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        mSampleRate = sampleRate;
        UpdateReleaseCoefficient();
    }

    void Reset() override
    {
        mGain[0] = 1.0f;
        mGain[1] = 1.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const float ceiling = static_cast<float>(std::pow(10.0, mCeilingDb * 0.05));
        const float softClip = mSoftClip;
        const float safeCeiling = std::max(ceiling, 1.0e-6f);
        const float clipKnee = 0.995f - 0.075f * std::clamp(softClip, 0.0f, 1.0f);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : inL;
            const float inputByChannel[2] = {inL, inR};
            float outputByChannel[2] = {0.0f, 0.0f};

            for (int ch = 0; ch < 2; ++ch)
            {
                const float peak = std::abs(inputByChannel[ch]);
                float targetGain = 1.0f;
                if (peak > ceiling && peak > 1e-12f)
                {
                    targetGain = ceiling / peak;
                }

                // Attack clamps immediately on overs, release eases back to unity.
                if (targetGain < mGain[ch])
                {
                    mGain[ch] = targetGain;
                }
                else
                {
                    mGain[ch] += mReleaseCoef * (1.0f - mGain[ch]);
                }

                outputByChannel[ch] = inputByChannel[ch] * mGain[ch];
            }

            float outL = outputByChannel[0];
            float outR = outputByChannel[1];

            if (softClip > 0.0f)
            {
                outL = drive_output_limiter::SoftClipNearCeiling(outL / safeCeiling, clipKnee, 1.0f) * safeCeiling;
                outR = drive_output_limiter::SoftClipNearCeiling(outR / safeCeiling, clipKnee, 1.0f) * safeCeiling;
            }
            else
            {
                outL = std::clamp(outL, -ceiling, ceiling);
                outR = std::clamp(outR, -ceiling, ceiling);
            }

            if (outputs[0])
            {
                outputs[0][i] = outL;
            }
            if (outputs[1])
            {
                outputs[1][i] = outR;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "ceiling")
        {
            mCeilingDb = static_cast<float>(std::clamp(value, -24.0, 0.0));
        }
        else if (key == "release")
        {
            mReleaseMs = static_cast<float>(std::clamp(value, 1.0, 500.0));
            UpdateReleaseCoefficient();
        }
        else if (key == "softClip")
        {
            mSoftClip = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "ceiling")
        {
            return mCeilingDb;
        }
        if (key == "release")
        {
            return mReleaseMs;
        }
        if (key == "softClip")
        {
            return mSoftClip;
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "limiter_brickwall";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    void UpdateReleaseCoefficient()
    {
        if (mSampleRate > 0)
        {
            mReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs * 0.001 * mSampleRate)));
        }
        else
        {
            mReleaseCoef = 0.0f;
        }
    }

    double mSampleRate = 44100.0;
    float mCeilingDb = -0.1f;
    float mReleaseMs = 50.0f;
    float mSoftClip = 0.0f;
    float mReleaseCoef = 0.0f;
    float mGain[2] = {1.0f, 1.0f};
};

inline void RegisterLimiterEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kLimiterBrickwall;
    info.aliases = {"limiter_brickwall"};
    info.displayName = "Brickwall Limiter";
    info.category = "dynamics";
    info.description = "Instant-attack limiter with adjustable ceiling";
    info.requiresResource = false;
    info.parameters = {{"ceiling", "Ceiling", -0.1, -24.0, 0.0, "dB"},
                       {"release", "Release", 50.0, 1.0, 500.0, "ms"},
                       {"softClip", "Soft Clip", 0.0, 0.0, 1.0, "amount", "Output", true}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<LimiterEffect>(); });
}

} // namespace guitarfx
