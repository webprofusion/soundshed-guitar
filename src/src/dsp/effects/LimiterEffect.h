#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Brickwall limiter with instantaneous attack and smoothed release.
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
      mGain = 1.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float ceiling = static_cast<float>(std::pow(10.0, mCeilingDb * 0.05));

      for (int i = 0; i < numSamples; ++i)
      {
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;

        const float peak = std::max(std::abs(inL), std::abs(inR));
        float targetGain = 1.0f;
        if (peak > ceiling && peak > 1e-12f)
        {
          targetGain = ceiling / peak;
        }

        if (targetGain < mGain)
        {
          mGain = targetGain;
        }
        else
        {
          mGain += mReleaseCoef * (1.0f - mGain);
        }

        float outL = inL * mGain;
        float outR = inR * mGain;

        outL = std::clamp(outL, -ceiling, ceiling);
        outR = std::clamp(outR, -ceiling, ceiling);

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
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
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "ceiling")
        return mCeilingDb;
      if (key == "release")
        return mReleaseMs;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "limiter_brickwall"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

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
    float mReleaseCoef = 0.0f;
    float mGain = 1.0f;
  };

  inline void RegisterLimiterEffect()
  {
    EffectTypeInfo info;
    info.type = "limiter_brickwall";
    info.displayName = "Brickwall Limiter";
    info.category = "dynamics";
    info.description = "Instant-attack limiter with adjustable ceiling";
    info.requiresResource = false;
    info.parameters = {
      {"ceiling", "Ceiling", -0.1, -24.0, 0.0, "dB"},
      {"release", "Release", 50.0, 1.0, 500.0, "ms"}
    };

    EffectRegistry::Instance().Register("limiter_brickwall", info, []()
      { return std::make_unique<LimiterEffect>(); });
  }

} // namespace guitarfx
