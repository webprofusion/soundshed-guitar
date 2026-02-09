#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <cmath>
#include <algorithm>

namespace guitarfx
{
  /**
   * VCA-style compressor with standard controls.
   */
  class CompressorEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
      mSampleRate = sampleRate;
      UpdateCoefficients();
    }

    void Reset() override
    {
      mEnvelope = 0.0f;
      mGainReduction = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        // Peak detection
        float peak = std::max(std::abs(inL), std::abs(inR));

        // Convert to dB
        float peakDb = (peak > 1e-10f) ? 20.0f * std::log10(peak) : -200.0f;

        // Compute gain reduction
        float targetGainDb = 0.0f;
        if (peakDb > mThresholdDb)
        {
          float overDb = peakDb - mThresholdDb;
          float compressedDb = overDb / mRatio;
          targetGainDb = compressedDb - overDb; // This is negative
        }

        // Apply knee softening
        if (mKnee > 0.0f)
        {
          float halfKnee = mKnee * 0.5f;
          if (peakDb > mThresholdDb - halfKnee && peakDb < mThresholdDb + halfKnee)
          {
            float x = peakDb - mThresholdDb + halfKnee;
            float t = x / mKnee;
            float overDb = t * t * mKnee * 0.5f;
            float compressedDb = overDb / mRatio;
            targetGainDb = compressedDb - overDb;
          }
        }

        // Envelope follower with attack/release
        float targetEnv = -targetGainDb; // Make positive for easier math
        if (targetEnv > mEnvelope)
          mEnvelope += mAttackCoef * (targetEnv - mEnvelope);
        else
          mEnvelope += mReleaseCoef * (targetEnv - mEnvelope);

        // Apply gain
        float gainDb = -mEnvelope + mMakeupDb;
        float gain = std::pow(10.0f, gainDb * 0.05f);

        // Mix
        float outL = inL * gain;
        float outR = inR * gain;

        float mixedL = inL * (1.0f - mMix) + outL * mMix;
        float mixedR = inR * (1.0f - mMix) + outR * mMix;

        if (outputs[0])
          outputs[0][i] = mixedL;
        if (outputs[1])
          outputs[1][i] = mixedR;

        mGainReduction = mEnvelope;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "threshold")
      {
        mThresholdDb = static_cast<float>(std::clamp(value, -60.0, 0.0));
      }
      else if (key == "ratio")
      {
        mRatio = static_cast<float>(std::clamp(value, 1.0, 20.0));
      }
      else if (key == "attack")
      {
        mAttackMs = static_cast<float>(std::clamp(value, 0.1, 500.0));
        UpdateCoefficients();
      }
      else if (key == "release")
      {
        mReleaseMs = static_cast<float>(std::clamp(value, 10.0, 2000.0));
        UpdateCoefficients();
      }
      else if (key == "knee")
      {
        mKnee = static_cast<float>(std::clamp(value, 0.0, 24.0));
      }
      else if (key == "makeup")
      {
        mMakeupDb = static_cast<float>(std::clamp(value, 0.0, 24.0));
      }
      else if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "threshold")
        return mThresholdDb;
      if (key == "ratio")
        return mRatio;
      if (key == "attack")
        return mAttackMs;
      if (key == "release")
        return mReleaseMs;
      if (key == "knee")
        return mKnee;
      if (key == "makeup")
        return mMakeupDb;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] double GetGainReduction() const { return mGainReduction; }
    [[nodiscard]] std::string GetType() const override { return "compressor_vca"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    void UpdateCoefficients()
    {
      if (mSampleRate > 0)
      {
        mAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs * 0.001 * mSampleRate)));
        mReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs * 0.001 * mSampleRate)));
      }
    }

    // Parameters
    float mThresholdDb = -20.0f;
    float mRatio = 4.0f;
    float mAttackMs = 10.0f;
    float mReleaseMs = 100.0f;
    float mKnee = 6.0f;
    float mMakeupDb = 0.0f;
    float mMix = 1.0f;

    // Coefficients
    float mAttackCoef = 0.0f;
    float mReleaseCoef = 0.0f;

    // State
    float mEnvelope = 0.0f;
    float mGainReduction = 0.0f;
  };

  /**
   * Opto-style compressor with slower, smoother response.
   */
  class OptoCompressorEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
      mSampleRate = sampleRate;
      UpdateCoefficients();
    }

    void Reset() override
    {
      mEnvelope = 0.0f;
      mOptoCellState = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        // RMS detection for smoother opto response
        float inputPower = (inL * inL + inR * inR) * 0.5f;
        mEnvelope += mDetectCoef * (inputPower - mEnvelope);
        float rms = std::sqrt(mEnvelope);

        // Convert to dB
        float rmsDb = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -200.0f;

        // Opto cell simulation - slow, program-dependent response
        float targetReduction = 0.0f;
        if (rmsDb > mThresholdDb)
        {
          float overDb = rmsDb - mThresholdDb;
          targetReduction = overDb * (1.0f - 1.0f / mRatio);
        }

        // Opto cell timing is program-dependent
        float optoSpeed = (targetReduction > mOptoCellState) ? mAttackCoef : mReleaseCoef;
        // Slower release when more compression
        if (targetReduction < mOptoCellState)
          optoSpeed *= std::max(0.2f, 1.0f - mOptoCellState * 0.02f);

        mOptoCellState += optoSpeed * (targetReduction - mOptoCellState);

        // Apply gain
        float gainDb = -mOptoCellState + mMakeupDb;
        float gain = std::pow(10.0f, gainDb * 0.05f);

        float outL = inL * gain * mMix + inL * (1.0f - mMix);
        float outR = inR * gain * mMix + inR * (1.0f - mMix);

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "threshold")
        mThresholdDb = static_cast<float>(std::clamp(value, -60.0, 0.0));
      else if (key == "ratio")
        mRatio = static_cast<float>(std::clamp(value, 1.0, 20.0));
      else if (key == "attack")
      {
        mAttackMs = static_cast<float>(std::clamp(value, 5.0, 200.0));
        UpdateCoefficients();
      }
      else if (key == "release")
      {
        mReleaseMs = static_cast<float>(std::clamp(value, 50.0, 3000.0));
        UpdateCoefficients();
      }
      else if (key == "makeup")
        mMakeupDb = static_cast<float>(std::clamp(value, 0.0, 24.0));
      else if (key == "mix")
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "threshold")
        return mThresholdDb;
      if (key == "ratio")
        return mRatio;
      if (key == "attack")
        return mAttackMs;
      if (key == "release")
        return mReleaseMs;
      if (key == "makeup")
        return mMakeupDb;
      if (key == "mix")
        return mMix;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "compressor_opto"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    void UpdateCoefficients()
    {
      if (mSampleRate > 0)
      {
        mAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs * 0.001 * mSampleRate)));
        mReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs * 0.001 * mSampleRate)));
        mDetectCoef = static_cast<float>(1.0 - std::exp(-1.0 / (5.0 * 0.001 * mSampleRate)));
      }
    }

    float mThresholdDb = -20.0f;
    float mRatio = 3.0f;
    float mAttackMs = 20.0f;
    float mReleaseMs = 300.0f;
    float mMakeupDb = 0.0f;
    float mMix = 1.0f;

    float mAttackCoef = 0.0f;
    float mReleaseCoef = 0.0f;
    float mDetectCoef = 0.0f;

    float mEnvelope = 0.0f;
    float mOptoCellState = 0.0f;
  };

  inline void RegisterCompressorEffects()
  {
    // VCA compressor
    {
      EffectTypeInfo info;
      info.type = "compressor_vca";
      info.displayName = "VCA Compressor";
      info.category = "dynamics";
      info.description = "Clean, precise VCA-style compressor";
      info.requiresResource = false;
      info.parameters = {
          {"threshold", "Threshold", -20.0, -60.0, 0.0, "dB"},
          {"ratio", "Ratio", 4.0, 1.0, 20.0, ":1"},
          {"attack", "Attack", 10.0, 0.1, 500.0, "ms"},
          {"release", "Release", 100.0, 10.0, 2000.0, "ms"},
          {"knee", "Knee", 6.0, 0.0, 24.0, "dB"},
          {"makeup", "Makeup", 0.0, 0.0, 24.0, "dB"},
          {"mix", "Mix", 1.0, 0.0, 1.0, ""}};

      EffectRegistry::Instance().Register("compressor_vca", info, []()
                                          { return std::make_unique<CompressorEffect>(); });
    }

    // Opto compressor
    {
      EffectTypeInfo info;
      info.type = "compressor_opto";
      info.displayName = "Opto Compressor";
      info.category = "dynamics";
      info.description = "Smooth optical-style compressor";
      info.requiresResource = false;
      info.parameters = {
          {"threshold", "Threshold", -20.0, -60.0, 0.0, "dB"},
          {"ratio", "Ratio", 3.0, 1.0, 20.0, ":1"},
          {"attack", "Attack", 20.0, 5.0, 200.0, "ms"},
          {"release", "Release", 300.0, 50.0, 3000.0, "ms"},
          {"makeup", "Makeup", 0.0, 0.0, 24.0, "dB"},
          {"mix", "Mix", 1.0, 0.0, 1.0, ""}};

      EffectRegistry::Instance().Register("compressor_opto", info, []()
                                          { return std::make_unique<OptoCompressorEffect>(); });
    }
  }

} // namespace guitarfx
