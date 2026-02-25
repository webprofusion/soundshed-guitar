#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <atomic>
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
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
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
      const float drive = mDrive.load(std::memory_order_relaxed);
      const float levelDb = mLevelDb.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);
      const float toneCoef = mToneCoef.load(std::memory_order_relaxed);

      const float driveGain = 1.0f + 19.0f * drive;
      const float levelGain = static_cast<float>(std::pow(10.0, levelDb * 0.05));

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float wetL = std::tanh(inL * driveGain);
        float wetR = std::tanh(inR * driveGain);

        wetL = ApplyTone(wetL, mToneStateL, toneCoef);
        wetR = ApplyTone(wetR, mToneStateR, toneCoef);

        wetL *= levelGain;
        wetR *= levelGain;

        const float outL = inL * (1.0f - mix) + wetL * mix;
        const float outR = inR * (1.0f - mix) + wetR * mix;

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

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "drive")
        return mDrive.load(std::memory_order_relaxed);
      if (key == "tone")
        return mTone.load(std::memory_order_relaxed);
      if (key == "level")
        return mLevelDb.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
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
      const float cutoff = minHz + (maxHz - minHz) * mTone.load(std::memory_order_relaxed);
      const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
      mToneCoef.store(1.0f - std::exp(-x), std::memory_order_relaxed);
    }

    static float ApplyTone(float input, float &state, float toneCoef)
    {
      state += toneCoef * (input - state);
      return state;
    }

    std::atomic<float> mDrive{0.5f};
    std::atomic<float> mTone{0.5f};
    std::atomic<float> mLevelDb{0.0f};
    std::atomic<float> mMix{1.0f};

    std::atomic<float> mToneCoef{0.0f};
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
