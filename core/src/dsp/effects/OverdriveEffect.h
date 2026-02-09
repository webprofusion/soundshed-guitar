#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Overdrive pedal style soft clipper with tone control.
   */
  class OverdriveEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      UpdateToneCoefficient();
      Reset();
    }

    void Reset() override
    {
      mToneStateL = 0.0f;
      mToneStateR = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float driveGain = 1.0f + 19.0f * mDrive;
      const float levelGain = static_cast<float>(std::pow(10.0, mLevelDb * 0.05));

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float wetL = std::tanh(inL * driveGain);
        float wetR = std::tanh(inR * driveGain);

        wetL = ApplyTone(wetL, mToneStateL);
        wetR = ApplyTone(wetR, mToneStateR);

        wetL *= levelGain;
        wetR *= levelGain;

        const float outL = inL * (1.0f - mMix) + wetL * mMix;
        const float outR = inR * (1.0f - mMix) + wetR * mMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "drive")
      {
        mDrive = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "tone")
      {
        mTone = static_cast<float>(std::clamp(value, 0.0, 1.0));
        UpdateToneCoefficient();
      }
      else if (key == "level")
      {
        mLevelDb = static_cast<float>(std::clamp(value, -12.0, 12.0));
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "drive")
        return mDrive;
      if (key == "tone")
        return mTone;
      if (key == "level")
        return mLevelDb;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "overdrive"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    void UpdateToneCoefficient()
    {
      const float minHz = 600.0f;
      const float maxHz = 6000.0f;
      const float cutoff = minHz + (maxHz - minHz) * mTone;
      const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
      mToneCoef = 1.0f - std::exp(-x);
    }

    float ApplyTone(float input, float &state)
    {
      state += mToneCoef * (input - state);
      return state;
    }

    float mDrive = 0.5f;
    float mTone = 0.5f;
    float mLevelDb = 0.0f;
    float mMix = 1.0f;

    float mToneCoef = 0.0f;
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
  };

  inline void RegisterOverdriveEffect()
  {
    EffectTypeInfo info;
    info.type = "overdrive";
    info.displayName = "Overdrive";
    info.category = "dynamics";
    info.description = "Soft clipping overdrive";
    info.requiresResource = false;
    info.parameters = {
      {"drive", "Drive", 0.5, 0.0, 1.0, "amount"},
      {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
      {"level", "Level", 0.0, -12.0, 12.0, "dB"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("overdrive", info, []()
      { return std::make_unique<OverdriveEffect>(); });
  }

} // namespace guitarfx
