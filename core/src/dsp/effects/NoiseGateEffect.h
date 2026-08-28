#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
/**
 * Noise gate with peak envelope tracking, hold time, and attack/release smoothing.
 */
class NoiseGateEffect : public EffectProcessor
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
        mEnvelope[0] = 0.0f;
        mEnvelope[1] = 0.0f;
        mHoldSamplesRemaining[0] = 0;
        mHoldSamplesRemaining[1] = 0;
    }

    void Reset() override
    {
        mEnvelope[0] = 0.0f;
        mEnvelope[1] = 0.0f;
        mHoldSamplesRemaining[0] = 0;
        mHoldSamplesRemaining[1] = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        // If Prepare() failed or has not run, stay transparent rather than divide by zero.
        if (mSampleRate <= 0.0)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const float threshold = static_cast<float>(std::pow(10.0, mThresholdDb / 20.0));
        const float attackCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mAttackMs * 0.001)));
        const float releaseCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mReleaseMs * 0.001)));
        const int holdSamples = static_cast<int>(mSampleRate * mHoldMs * 0.001);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (!outputs[ch])
                {
                    continue;
                }

                const float inputSample = inputs[ch] ? inputs[ch][i] : ((ch == 1 && inputs[0]) ? inputs[0][i] : 0.0f);
                const float peak = std::abs(inputSample);

                // Track peaks quickly and decay with release; open while above threshold
                // or while the hold counter is still active.
                if (peak > mEnvelope[ch])
                {
                    mEnvelope[ch] = attackCoef * mEnvelope[ch] + (1.0f - attackCoef) * peak;
                }
                else
                {
                    mEnvelope[ch] = releaseCoef * mEnvelope[ch] + (1.0f - releaseCoef) * peak;
                }

                float gain = 0.0f;

                if (mEnvelope[ch] > threshold)
                {
                    mHoldSamplesRemaining[ch] = holdSamples;
                    gain = 1.0f;
                }
                else if (mHoldSamplesRemaining[ch] > 0)
                {
                    --mHoldSamplesRemaining[ch];
                    gain = 1.0f;
                }

                outputs[ch][i] = inputSample * gain;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "threshold" || key == "thresholdDb")
        {
            mThresholdDb = value;
        }
        else if (key == "attack" || key == "attackMs")
        {
            mAttackMs = std::max(0.1, value);
        }
        else if (key == "hold" || key == "holdMs")
        {
            mHoldMs = std::max(0.0, value);
        }
        else if (key == "release" || key == "releaseMs")
        {
            mReleaseMs = std::max(1.0, value);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "threshold" || key == "thresholdDb")
        {
            return mThresholdDb;
        }

        if (key == "attack" || key == "attackMs")
        {
            return mAttackMs;
        }

        if (key == "hold" || key == "holdMs")
        {
            return mHoldMs;
        }

        if (key == "release" || key == "releaseMs")
        {
            return mReleaseMs;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "dynamics_gate";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    double mThresholdDb = -60.0;
    double mAttackMs = 1.0;
    double mHoldMs = 50.0;
    double mReleaseMs = 50.0;
    float mEnvelope[2] = {0.0f, 0.0f};
    int mHoldSamplesRemaining[2] = {0, 0};
};

inline void RegisterNoiseGateEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDynamicsGate;
    info.aliases = {"dynamics_gate", "gate_noise"};
    info.displayName = "Noise Gate";
    info.category = "dynamics";
    info.description = "Simple noise gate";
    info.requiresResource = false;
    info.parameters = {{"thresholdDb", "Threshold", -60.0, -80.0, 0.0, "dB"},
                       {"attackMs", "Attack", 1.0, 0.1, 50.0, "ms"},
                       {"releaseMs", "Release", 50.0, 1.0, 500.0, "ms"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<NoiseGateEffect>(); });
}
} // namespace guitarfx
