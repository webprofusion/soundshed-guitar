#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Auto-wah effect using envelope follower and bandpass filter.
   */
  class AutoWahEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateEnvelopeCoefficients();
      Reset();
    }

    void Reset() override
    {
      mEnvelope = 0.0f;
      mBandL = 0.0f;
      mLowL = 0.0f;
      mBandR = 0.0f;
      mLowR = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        const float mono = 0.5f * (std::abs(inL) + std::abs(inR));
        UpdateEnvelope(mono);

        const float envNorm = std::clamp(mEnvelope * (1.0f + mSensitivity * 9.0f), 0.0f, 1.0f);
        const float targetFreq = mMinFreq + (mMaxFreq - mMinFreq) * envNorm;

        const float g = std::tan(static_cast<float>(kPi) * targetFreq / static_cast<float>(mSampleRate));
        const float R = 1.0f / std::max(0.1f, mResonance);
        const float h = 1.0f / (1.0f + g * (g + R));

        const float v1L = (g * (inL - mLowL) + mBandL) * h;
        const float v2L = mLowL + g * v1L;
        mBandL = v1L;
        mLowL = v2L;

        const float v1R = (g * (inR - mLowR) + mBandR) * h;
        const float v2R = mLowR + g * v1R;
        mBandR = v1R;
        mLowR = v2R;

        const float bandL = mBandL;
        const float bandR = mBandR;

        const float outL = inL * (1.0f - mMix) + bandL * mMix;
        const float outR = inR * (1.0f - mMix) + bandR * mMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "sensitivity")
      {
        mSensitivity = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "minFreq")
      {
        mMinFreq = static_cast<float>(std::clamp(value, 200.0, 1000.0));
      }
      else if (key == "maxFreq")
      {
        mMaxFreq = static_cast<float>(std::clamp(value, 800.0, 5000.0));
      }
      else if (key == "resonance")
      {
        mResonance = static_cast<float>(std::clamp(value, 0.5, 10.0));
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "sensitivity")
        return mSensitivity;
      if (key == "minFreq")
        return mMinFreq;
      if (key == "maxFreq")
        return mMaxFreq;
      if (key == "resonance")
        return mResonance;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "auto_wah"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    void UpdateEnvelopeCoefficients()
    {
      const float attackMs = 5.0f;
      const float releaseMs = 80.0f;
      mAttackCoef = 1.0f - std::exp(-1.0f / (attackMs * 0.001f * static_cast<float>(mSampleRate)));
      mReleaseCoef = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * static_cast<float>(mSampleRate)));
    }

    void UpdateEnvelope(float input)
    {
      if (input > mEnvelope)
        mEnvelope += mAttackCoef * (input - mEnvelope);
      else
        mEnvelope += mReleaseCoef * (input - mEnvelope);
    }

    float mSensitivity = 0.6f;
    float mMinFreq = 300.0f;
    float mMaxFreq = 2800.0f;
    float mResonance = 2.5f;
    float mMix = 1.0f;

    float mEnvelope = 0.0f;
    float mAttackCoef = 0.0f;
    float mReleaseCoef = 0.0f;

    float mBandL = 0.0f;
    float mLowL = 0.0f;
    float mBandR = 0.0f;
    float mLowR = 0.0f;
  };

  inline void RegisterAutoWahEffect()
  {
    EffectTypeInfo info;
    info.type = "auto_wah";
    info.displayName = "Auto-Wah";
    info.category = "modulation";
    info.description = "Envelope-controlled bandpass filter";
    info.requiresResource = false;
    info.parameters = {
      {"sensitivity", "Sensitivity", 0.6, 0.0, 1.0, "amount"},
      {"minFreq", "Min Freq", 300.0, 200.0, 1000.0, "Hz"},
      {"maxFreq", "Max Freq", 2800.0, 800.0, 5000.0, "Hz"},
      {"resonance", "Resonance", 2.5, 0.5, 10.0, "Q"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("auto_wah", info, []()
      { return std::make_unique<AutoWahEffect>(); });
  }

} // namespace guitarfx
