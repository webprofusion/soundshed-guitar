#pragma once

#include "dsp/effects/DriveOutputLimiter.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <atomic>
#include <algorithm>
#include <cmath>

namespace guitarfx
{
/**
 * Fuzz effectwith aggressive nonlinearity and tone control.
 */
class FuzzEffect : public EffectProcessor
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
        UpdateBodyFilterCoefficient();
        UpdateToneCoefficient();
        Reset();
    }

    void Reset() override
    {
        mToneStateL = 0.0f;
        mToneStateR = 0.0f;
        mBodyStateL = 0.0f;
        mBodyStateR = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const float drive = mDrive.load(std::memory_order_relaxed);
        const float tone = mTone.load(std::memory_order_relaxed);
        const float levelDb = mLevelDb.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const float toneCoef = mToneCoef.load(std::memory_order_relaxed);

        const float stageOneGain = 1.0f + 10.0f * drive;
        const float stageTwoGain = 1.0f + 18.0f * drive;
        const float levelGain = static_cast<float>(std::pow(10.0, levelDb * 0.05));
        const float bias = 0.05f + 0.18f * drive;
        const float bodyBlend = 0.18f + 0.22f * drive;
        const float gate = 0.01f + 0.04f * drive * drive;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;

            const float bodyL = ApplyOnePole(inL, mBodyStateL, mBodyCoef);
            const float bodyR = ApplyOnePole(inR, mBodyStateR, mBodyCoef);

            float wetL = ShapeFuzz(inL, bodyL, stageOneGain, stageTwoGain, bias, gate) + bodyL * bodyBlend;
            float wetR = ShapeFuzz(inR, bodyR, stageOneGain, stageTwoGain, bias, gate) + bodyR * bodyBlend;

            wetL = ApplyTone(wetL, mToneStateL, toneCoef, tone);
            wetR = ApplyTone(wetR, mToneStateR, toneCoef, tone);

            wetL *= levelGain;
            wetR *= levelGain;

            const float outL = drive_output_limiter::SoftClipNearCeiling(inL * (1.0f - mix) + wetL * mix);
            const float outR = drive_output_limiter::SoftClipNearCeiling(inR * (1.0f - mix) + wetR * mix);

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
        if (key == "drive")
        {
            mDrive.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "tone")
        {
            mTone.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
            UpdateToneCoefficient();
        }
        else if (key == "level")
        {
            mLevelDb.store(static_cast<float>(std::clamp(value, -12.0, 12.0)), std::memory_order_relaxed);
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
        if (key == "drive")
        {
            return mDrive.load(std::memory_order_relaxed);
        }
        if (key == "tone")
        {
            return mTone.load(std::memory_order_relaxed);
        }
        if (key == "level")
        {
            return mLevelDb.load(std::memory_order_relaxed);
        }
        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "fuzz";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    void UpdateToneCoefficient()
    {
        const float t = mTone.load(std::memory_order_relaxed);
        const float minHz = 500.0f;
        const float maxHz = 4200.0f;
        const float cutoff = minHz + (maxHz - minHz) * t;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mToneCoef.store(1.0f - std::exp(-x), std::memory_order_relaxed);
    }

    void UpdateBodyFilterCoefficient()
    {
        const float cutoff = 220.0f;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mBodyCoef = 1.0f - std::exp(-x);
    }

    static float ApplyOnePole(float input, float& state, float coefficient)
    {
        state += coefficient * (input - state);
        return state;
    }

    static float ApplyTone(float input, float& state, float toneCoef, float tone)
    {
        const float dark = ApplyOnePole(input, state, toneCoef);
        if (tone <= 0.5f)
        {
            const float blend = tone * 2.0f;
            return dark + (input - dark) * blend;
        }

        const float presence = input + (input - dark) * 0.20f;
        const float blend = (tone - 0.5f) * 2.0f;
        return input + (presence - input) * blend;
    }

    static float ApplyGate(float input, float threshold)
    {
        const float magnitude = std::max(0.0f, std::abs(input) - threshold);
        return std::copysign(magnitude / std::max(1.0f - threshold, 1.0e-4f), input);
    }

    static float ShapeFuzz(float input, float body, float stageOneGain, float stageTwoGain, float bias, float gate)
    {
        const float woollyInput = input + body * 0.45f;
        const float biased = std::tanh((woollyInput + bias) * stageOneGain);
        const float center = std::tanh(bias * stageOneGain);
        const float stageOne = biased - center;

        const float stageTwoInput = stageOne * stageTwoGain - bias * 0.35f;
        const float stageTwo = std::copysign(1.0f - std::exp(-std::abs(stageTwoInput)), stageTwoInput);
        const float cleanup = std::clamp(std::abs(input) * (2.5f + 0.35f * stageOneGain), 0.0f, 1.0f);
        const float blended = stageOne * (1.0f - cleanup) * 0.85f + stageTwo * cleanup;
        return ApplyGate(blended, gate);
    }

    std::atomic<float> mDrive{0.7f};
    std::atomic<float> mTone{0.5f};
    std::atomic<float> mLevelDb{0.0f};
    std::atomic<float> mMix{1.0f};

    std::atomic<float> mToneCoef{0.0f};
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
    float mBodyStateL = 0.0f;
    float mBodyStateR = 0.0f;
    float mBodyCoef = 0.0f;
};

inline void RegisterFuzzEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kFuzz;
    info.aliases = {"fuzz"};
    info.displayName = "Fuzz";
    info.category = "drive";
    info.description = "Aggressive fuzz distortion";
    info.requiresResource = false;
    info.parameters = {{"drive", "Drive", 0.7, 0.0, 1.0, "amount"},
                       {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
                       {"level", "Level", 0.0, -12.0, 12.0, "dB"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<FuzzEffect>(); });
}

} // namespace guitarfx
