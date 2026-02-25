#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <atomic>
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
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
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
      const float rateHz = mRateHz.load(std::memory_order_relaxed);
      const float depth = mDepth.load(std::memory_order_relaxed);
      const float shape = mShape.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);

      const double phaseInc = 2.0 * kPi * rateHz / std::max(1.0, mSampleRate);

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        float lfo = static_cast<float>(std::sin(mPhase));
        const float shaped = ShapeLfo(lfo, shape);
        const float mod = 0.5f * (1.0f + shaped);
        const float gain = (1.0f - depth) + depth * mod;

        const float wetL = inL * gain;
        const float wetR = inR * gain;

        const float outL = inL * (1.0f - mix) + wetL * mix;
        const float outR = inR * (1.0f - mix) + wetR * mix;

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
        mRateHz.store(static_cast<float>(std::clamp(value, 0.1, 12.0)), std::memory_order_relaxed);
      }
      else if (key == "depth")
      {
        mDepth.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
      }
      else if (key == "shape")
      {
        mShape.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
      }
      else if (key == "mix")
      {
        mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "rate")
        return mRateHz.load(std::memory_order_relaxed);
      if (key == "depth")
        return mDepth.load(std::memory_order_relaxed);
      if (key == "shape")
        return mShape.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
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

    std::atomic<float> mRateHz{4.0f};
    std::atomic<float> mDepth{0.7f};
    std::atomic<float> mShape{0.0f};
    std::atomic<float> mMix{1.0f};

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
