#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <cmath>

namespace guitarfx
{
  /**
   * Simple noise gate effect.
   */
  class NoiseGateEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;
      mEnvelope = 0.0f;
    }

    void Reset() override
    {
      mEnvelope = 0.0f;
      mHoldSamplesRemaining = 0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      // Guard against div-by-zero if Prepare() not yet called
      if (mSampleRate <= 0.0)
      {
        for (int ch = 0; ch < 2; ++ch)
          if (inputs[ch] && outputs[ch])
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
        return;
      }

      const float threshold = static_cast<float>(std::pow(10.0, mThresholdDb / 20.0));
      const float attackCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mAttackMs * 0.001)));
      const float releaseCoef = static_cast<float>(std::exp(-1.0 / (mSampleRate * mReleaseMs * 0.001)));
      const int holdSamples = static_cast<int>(mSampleRate * mHoldMs * 0.001);

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

        // Hold: when signal is above threshold, reset hold counter and keep gate open
        float gain;
        if (mEnvelope > threshold)
        {
          mHoldSamplesRemaining = holdSamples;
          gain = 1.0f;
        }
        else if (mHoldSamplesRemaining > 0)
        {
          --mHoldSamplesRemaining;
          gain = 1.0f;
        }
        else
        {
          gain = 0.0f;
        }

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

    void SetParam(const std::string &key, double value) override
    {
      if (key == "threshold" || key == "thresholdDb")
        mThresholdDb = value;
      else if (key == "attack" || key == "attackMs")
        mAttackMs = std::max(0.1, value);
      else if (key == "hold" || key == "holdMs")
        mHoldMs = std::max(0.0, value);
      else if (key == "release" || key == "releaseMs")
        mReleaseMs = std::max(1.0, value);
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "threshold" || key == "thresholdDb")
        return mThresholdDb;
      if (key == "attack" || key == "attackMs")
        return mAttackMs;
      if (key == "hold" || key == "holdMs")
        return mHoldMs;
      if (key == "release" || key == "releaseMs")
        return mReleaseMs;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "dynamics_gate"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    double mThresholdDb = -60.0;
    double mAttackMs = 1.0;
    double mHoldMs = 50.0;
    double mReleaseMs = 50.0;
    float mEnvelope = 0.0f;
    int mHoldSamplesRemaining = 0;
  };

  inline void RegisterNoiseGateEffect()
  {
    EffectTypeInfo info;
    info.type = "dynamics_gate"; // Changed from "gate_noise" to match preset JSON
    info.displayName = "Noise Gate";
    info.category = "dynamics";
    info.description = "Simple noise gate";
    info.requiresResource = false;
    info.parameters = {
        {"thresholdDb", "Threshold", -60.0, -80.0, 0.0, "dB"},
        {"attackMs", "Attack", 1.0, 0.1, 50.0, "ms"},
        {"releaseMs", "Release", 50.0, 1.0, 500.0, "ms"}};

    EffectRegistry::Instance().Register("dynamics_gate", info, []()
                                        { return std::make_unique<NoiseGateEffect>(); });
  }

} // namespace guitarfx
