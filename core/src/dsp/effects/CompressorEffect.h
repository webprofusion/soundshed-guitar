#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <atomic>
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
      if (!ValidatePrepare(sampleRate, 1))
        return;
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
      const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
      const float ratio = mRatio.load(std::memory_order_relaxed);
      const float knee = mKnee.load(std::memory_order_relaxed);
      const float makeupDb = mMakeupDb.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);
      const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
      const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);

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
        if (peakDb > thresholdDb)
        {
          float overDb = peakDb - thresholdDb;
          float compressedDb = overDb / ratio;
          targetGainDb = compressedDb - overDb; // This is negative
        }

        // Apply knee softening
        if (knee > 0.0f)
        {
          float halfKnee = knee * 0.5f;
          if (peakDb > thresholdDb - halfKnee && peakDb < thresholdDb + halfKnee)
          {
            float x = peakDb - thresholdDb + halfKnee;
            float t = x / knee;
            float overDb = t * t * knee * 0.5f;
            float compressedDb = overDb / ratio;
            targetGainDb = compressedDb - overDb;
          }
        }

        // Envelope follower with attack/release
        float targetEnv = -targetGainDb; // Make positive for easier math
        if (targetEnv > mEnvelope)
          mEnvelope += attackCoef * (targetEnv - mEnvelope);
        else
          mEnvelope += releaseCoef * (targetEnv - mEnvelope);

        // Apply gain
        float gainDb = -mEnvelope + makeupDb;
        float gain = std::pow(10.0f, gainDb * 0.05f);

        // Mix
        float outL = inL * gain;
        float outR = inR * gain;

        float mixedL = inL * (1.0f - mix) + outL * mix;
        float mixedR = inR * (1.0f - mix) + outR * mix;

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
        mThresholdDb.store(static_cast<float>(std::clamp(value, -60.0, 0.0)), std::memory_order_relaxed);
      }
      else if (key == "ratio")
      {
        mRatio.store(static_cast<float>(std::clamp(value, 1.0, 20.0)), std::memory_order_relaxed);
      }
      else if (key == "attack")
      {
        mAttackMs.store(static_cast<float>(std::clamp(value, 0.1, 500.0)), std::memory_order_relaxed);
        UpdateCoefficients();
      }
      else if (key == "release")
      {
        mReleaseMs.store(static_cast<float>(std::clamp(value, 10.0, 2000.0)), std::memory_order_relaxed);
        UpdateCoefficients();
      }
      else if (key == "knee")
      {
        mKnee.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
      }
      else if (key == "makeup")
      {
        mMakeupDb.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
      }
      else if (key == "mix")
      {
        mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "threshold")
        return mThresholdDb.load(std::memory_order_relaxed);
      if (key == "ratio")
        return mRatio.load(std::memory_order_relaxed);
      if (key == "attack")
        return mAttackMs.load(std::memory_order_relaxed);
      if (key == "release")
        return mReleaseMs.load(std::memory_order_relaxed);
      if (key == "knee")
        return mKnee.load(std::memory_order_relaxed);
      if (key == "makeup")
        return mMakeupDb.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
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
        mAttackCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs.load(std::memory_order_relaxed) * 0.001 * mSampleRate))), std::memory_order_relaxed);
        mReleaseCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs.load(std::memory_order_relaxed) * 0.001 * mSampleRate))), std::memory_order_relaxed);
      }
    }

    // Parameters
    std::atomic<float> mThresholdDb{-20.0f};
    std::atomic<float> mRatio{4.0f};
    std::atomic<float> mAttackMs{10.0f};
    std::atomic<float> mReleaseMs{100.0f};
    std::atomic<float> mKnee{6.0f};
    std::atomic<float> mMakeupDb{0.0f};
    std::atomic<float> mMix{1.0f};

    // Coefficients (derived from params + sample rate, updated atomically)
    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};

    // State (audio thread only, no synchronization needed)
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
      if (!ValidatePrepare(sampleRate, 1))
        return;
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
      const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
      const float ratio = mRatio.load(std::memory_order_relaxed);
      const float makeupDb = mMakeupDb.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);
      const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
      const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
      const float detectCoef = mDetectCoef.load(std::memory_order_relaxed);

      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : 0.0f;

        // RMS detection for smoother opto response
        float inputPower = (inL * inL + inR * inR) * 0.5f;
        mEnvelope += detectCoef * (inputPower - mEnvelope);
        float rms = std::sqrt(mEnvelope);

        // Convert to dB
        float rmsDb = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -200.0f;

        // Opto cell simulation - slow, program-dependent response
        float targetReduction = 0.0f;
        if (rmsDb > thresholdDb)
        {
          float overDb = rmsDb - thresholdDb;
          targetReduction = overDb * (1.0f - 1.0f / ratio);
        }

        // Opto cell timing is program-dependent
        float optoSpeed = (targetReduction > mOptoCellState) ? attackCoef : releaseCoef;
        // Slower release when more compression
        if (targetReduction < mOptoCellState)
          optoSpeed *= std::max(0.2f, 1.0f - mOptoCellState * 0.02f);

        mOptoCellState += optoSpeed * (targetReduction - mOptoCellState);

        // Apply gain
        float gainDb = -mOptoCellState + makeupDb;
        float gain = std::pow(10.0f, gainDb * 0.05f);

        float outL = inL * gain * mix + inL * (1.0f - mix);
        float outR = inR * gain * mix + inR * (1.0f - mix);

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "threshold")
        mThresholdDb.store(static_cast<float>(std::clamp(value, -60.0, 0.0)), std::memory_order_relaxed);
      else if (key == "ratio")
        mRatio.store(static_cast<float>(std::clamp(value, 1.0, 20.0)), std::memory_order_relaxed);
      else if (key == "attack")
      {
        mAttackMs.store(static_cast<float>(std::clamp(value, 5.0, 200.0)), std::memory_order_relaxed);
        UpdateCoefficients();
      }
      else if (key == "release")
      {
        mReleaseMs.store(static_cast<float>(std::clamp(value, 50.0, 3000.0)), std::memory_order_relaxed);
        UpdateCoefficients();
      }
      else if (key == "makeup")
        mMakeupDb.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
      else if (key == "mix")
        mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "threshold")
        return mThresholdDb.load(std::memory_order_relaxed);
      if (key == "ratio")
        return mRatio.load(std::memory_order_relaxed);
      if (key == "attack")
        return mAttackMs.load(std::memory_order_relaxed);
      if (key == "release")
        return mReleaseMs.load(std::memory_order_relaxed);
      if (key == "makeup")
        return mMakeupDb.load(std::memory_order_relaxed);
      if (key == "mix")
        return mMix.load(std::memory_order_relaxed);
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "compressor_opto"; }
    [[nodiscard]] std::string GetCategory() const override { return "dynamics"; }

  private:
    void UpdateCoefficients()
    {
      if (mSampleRate > 0)
      {
        mAttackCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs.load(std::memory_order_relaxed) * 0.001 * mSampleRate))), std::memory_order_relaxed);
        mReleaseCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs.load(std::memory_order_relaxed) * 0.001 * mSampleRate))), std::memory_order_relaxed);
        mDetectCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (5.0 * 0.001 * mSampleRate))), std::memory_order_relaxed);
      }
    }

    std::atomic<float> mThresholdDb{-20.0f};
    std::atomic<float> mRatio{3.0f};
    std::atomic<float> mAttackMs{20.0f};
    std::atomic<float> mReleaseMs{300.0f};
    std::atomic<float> mMakeupDb{0.0f};
    std::atomic<float> mMix{1.0f};

    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};
    std::atomic<float> mDetectCoef{0.0f};

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
