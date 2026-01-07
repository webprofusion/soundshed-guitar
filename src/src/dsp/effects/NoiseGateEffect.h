#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>

namespace namguitar
{
  /**
   * Simple noise gate effect.
   */
  class NoiseGateEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      mEnvelope = 0.0f;
    }

    void Reset() override
    {
      mEnvelope = 0.0f;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      const float threshold = static_cast<float>(std::pow(10.0, mThresholdDb / 20.0));
      const float attackCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mAttackMs * 0.001)));
      const float releaseCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mReleaseMs * 0.001)));

      for (int i = 0; i < numSamples; ++i)
      {
        // Get peak of both channels
        float peak = 0.0f;
        if (inputs[0])
          peak = std::max(peak, std::abs(inputs[0][i]));
        if (inputs[1])
          peak = std::max(peak, std::abs(inputs[1][i]));

        // Envelope follower
        if (peak > mEnvelope)
          mEnvelope = attackCoef * mEnvelope + (1.0f - attackCoef) * peak;
        else
          mEnvelope = releaseCoef * mEnvelope + (1.0f - releaseCoef) * peak;

        // Gate
        float gain = (mEnvelope > threshold) ? 1.0f : 0.0f;

        // Apply gain
        for (int ch = 0; ch < 2; ++ch)
        {
          if (inputs[ch] && outputs[ch])
          {
            outputs[ch][i] = inputs[ch][i] * gain;
          }
        }
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "threshold" || key == "thresholdDb")
        mThresholdDb = value;
      else if (key == "attack" || key == "attackMs")
        mAttackMs = std::max(0.1, value);
      else if (key == "release" || key == "releaseMs")
        mReleaseMs = std::max(1.0, value);
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "threshold" || key == "thresholdDb")
        return mThresholdDb;
      if (key == "attack" || key == "attackMs")
        return mAttackMs;
      if (key == "release" || key == "releaseMs")
        return mReleaseMs;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "gate_noise"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    double mThresholdDb = -60.0;
    double mAttackMs = 1.0;
    double mReleaseMs = 50.0;
    float mEnvelope = 0.0f;
  };

  inline void RegisterNoiseGateEffect()
  {
    EffectTypeInfo info;
    info.type = "gate_noise";
    info.displayName = "Noise Gate";
    info.category = "dynamics";
    info.description = "Simple noise gate";
    info.requiresResource = false;
    info.parameters = {
      {"thresholdDb", "Threshold", -60.0, -80.0, 0.0, "dB"},
      {"attackMs", "Attack", 1.0, 0.1, 50.0, "ms"},
      {"releaseMs", "Release", 50.0, 1.0, 500.0, "ms"}
    };

    EffectRegistry::Instance().Register("gate_noise", info, []() {
      return std::make_unique<NoiseGateEffect>();
    });
  }

} // namespace namguitar
