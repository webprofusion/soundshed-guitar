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
 * Overdrive pedalstyle soft clipper with tone control.
 */
class OverdriveEffect : public EffectProcessor
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
        UpdateInputFilterCoefficient();
        UpdateToneCoefficient();
        Reset();
    }

    void Reset() override
    {
        mToneStateL = 0.0f;
        mToneStateR = 0.0f;
        mInputLowStateL = 0.0f;
        mInputLowStateR = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const float drive = mDrive.load(std::memory_order_relaxed);
        const float tone = mTone.load(std::memory_order_relaxed);
        const float levelDb = mLevelDb.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);
        const float toneCoef = mToneCoef.load(std::memory_order_relaxed);

        const float driveGain = 1.0f + 9.0f * drive;
        const float levelGain = static_cast<float>(std::pow(10.0, levelDb * 0.05));
        const float bodyRestore = 0.18f - 0.05f * drive;
        const float lowTighten = 0.82f + 0.10f * drive;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;

            const float lowL = ApplyOnePole(inL, mInputLowStateL, mInputLowCoef);
            const float lowR = ApplyOnePole(inR, mInputLowStateR, mInputLowCoef);

            const float shapedInL = (inL - lowL * lowTighten + 0.12f * inL) * driveGain;
            const float shapedInR = (inR - lowR * lowTighten + 0.12f * inR) * driveGain;

            float wetL = ShapeOverdrive(shapedInL) + lowL * bodyRestore;
            float wetR = ShapeOverdrive(shapedInR) + lowR * bodyRestore;

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
        return "overdrive";
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
        const float minHz = 650.0f;
        const float maxHz = 3600.0f;
        const float cutoff = minHz + (maxHz - minHz) * t;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mToneCoef.store(1.0f - std::exp(-x), std::memory_order_relaxed);
    }

    void UpdateInputFilterCoefficient()
    {
        const float cutoff = 180.0f;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mInputLowCoef = 1.0f - std::exp(-x);
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

        const float presence = input + (input - dark) * 0.45f;
        const float blend = (tone - 0.5f) * 2.0f;
        return input + (presence - input) * blend;
    }

    static float ShapeOverdrive(float input)
    {
        const float asym = input >= 0.0f ? std::tanh(input * 1.25f) : std::tanh(input * 0.9f);
        const float rounded = input / (1.0f + std::abs(input));
        return 0.75f * asym + 0.25f * rounded;
    }

    std::atomic<float> mDrive{0.5f};
    std::atomic<float> mTone{0.5f};
    std::atomic<float> mLevelDb{0.0f};
    std::atomic<float> mMix{1.0f};

    std::atomic<float> mToneCoef{0.0f};
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
    float mInputLowStateL = 0.0f;
    float mInputLowStateR = 0.0f;
    float mInputLowCoef = 0.0f;
};

inline void RegisterOverdriveEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kOverdrive;
    info.aliases = {"overdrive"};
    info.displayName = "Overdrive";
    info.category = "drive";
    info.description = "Soft clipping overdrive";
    info.requiresResource = false;
    info.parameters = {{"drive", "Drive", 0.5, 0.0, 1.0, "amount"},
                       {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
                       {"level", "Level", 0.0, -12.0, 12.0, "dB"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<OverdriveEffect>(); });
}
} // namespace guitarfx
