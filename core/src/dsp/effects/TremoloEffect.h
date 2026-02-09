#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Tremolo effect (amplitude modulation).
   */
  class TremoloEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      Reset();
    }

    void Reset() override
    {
      mPhase = 0.0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const double phaseInc = 2.0 * kPi * mRateHz / std::max(1.0, mSampleRate);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float lfo = static_cast<float>(std::sin(mPhase));
        const float shaped = ShapeLfo(lfo, mShape);
        const float mod = 0.5f * (1.0f + shaped);
        const float gain = (1.0f - mDepth) + mDepth * mod;

        const float wetL = inL * gain;
        const float wetR = inR * gain;

        const float outL = inL * (1.0f - mMix) + wetL * mMix;
        const float outR = inR * (1.0f - mMix) + wetR * mMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;

        mPhase += phaseInc;
        if (mPhase >= 2.0 * kPi)
          mPhase -= 2.0 * kPi;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "rate")
      {
        mRateHz = static_cast<float>(std::clamp(value, 0.1, 12.0));
      }
      else if (key == "depth")
      {
        mDepth = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "shape")
      {
        mShape = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "rate")
        return mRateHz;
      if (key == "depth")
        return mDepth;
      if (key == "shape")
        return mShape;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "tremolo"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

  private:
    static constexpr double kPi = 3.14159265358979323846;

    static float ShapeLfo(float lfo, float shape)
    {
      const float amount = 1.0f + shape * 8.0f;
      return std::tanh(lfo * amount) / std::tanh(amount);
    }

    float mRateHz = 4.0f;
    float mDepth = 0.7f;
    float mShape = 0.0f;
    float mMix = 1.0f;

    double mPhase = 0.0;
  };

  inline void RegisterTremoloEffect()
  {
    EffectTypeInfo info;
    info.type = "tremolo";
    info.displayName = "Tremolo";
    info.category = "modulation";
    info.description = "Amplitude modulation tremolo";
    info.requiresResource = false;
    info.parameters = {
      {"rate", "Rate", 4.0, 0.1, 12.0, "Hz"},
      {"depth", "Depth", 0.7, 0.0, 1.0, "amount"},
      {"shape", "Shape", 0.0, 0.0, 1.0, "amount"},
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}
    };

    EffectRegistry::Instance().Register("tremolo", info, []()
      { return std::make_unique<TremoloEffect>(); });
  }

} // namespace guitarfx
