#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/DriveOutputLimiter.h"
#include <atomic>
#include <cmath>
#include <algorithm>

namespace guitarfx
{
    /**
     * VCA-style compressor with peak detection, soft-knee gain computer,
     * attack/release smoothing, makeup gain, dry/wet mix, and optional soft clip.
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
      // Clear state when sample rate changes to ensure clean initialization
      Reset();
    }

    void Reset() override
    {
      mEnvelope[0] = 0.0f;
      mEnvelope[1] = 0.0f;
      mGainReduction = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
      const float ratio = mRatio.load(std::memory_order_relaxed);
      const float knee = mKnee.load(std::memory_order_relaxed);
      const float makeupDb = mMakeupDb.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);
      const float softClip = mSoftClip.load(std::memory_order_relaxed);
      const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
      const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
      constexpr float kSoftClipTransparentKnee = 0.995f;
      constexpr float kSoftClipMaxKneeReduction = 0.075f;
      // Move the soft-clip knee from nearly transparent at 0.995 to 0.92 at
      // full softClip so makeup gain is rounded off before the hard ceiling.
      const float clipKnee =
          kSoftClipTransparentKnee - kSoftClipMaxKneeReduction * std::clamp(softClip, 0.0f, 1.0f);
      const auto computeTargetGainDb = [thresholdDb, ratio, knee](float detectorDb)
      {
        // Gain computer: above-threshold level is reduced by the ratio; the
        // optional knee eases into compression around the threshold.
        float targetGainDb = 0.0f;
        if (detectorDb > thresholdDb)
        {
          const float overDb = detectorDb - thresholdDb;
          const float compressedDb = overDb / ratio;
          targetGainDb = compressedDb - overDb;
        }

        if (knee > 0.0f)
        {
          const float halfKnee = knee * 0.5f;
          if (detectorDb > thresholdDb - halfKnee && detectorDb < thresholdDb + halfKnee)
          {
            const float x = detectorDb - thresholdDb + halfKnee;
            const float t = x / knee;
            const float overDb = t * t * knee * 0.5f;
            const float compressedDb = overDb / ratio;
            targetGainDb = compressedDb - overDb;
          }
        }

        return targetGainDb;
      };

      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : inL;
        const float inputByChannel[2] = {inL, inR};
        float mixedByChannel[2] = {0.0f, 0.0f};

        for (int ch = 0; ch < 2; ++ch)
        {
          const float peak = std::abs(inputByChannel[ch]);
          const float peakDb = (peak > 1e-10f) ? 20.0f * std::log10(peak) : -200.0f;
          const float targetGainDb = computeTargetGainDb(peakDb);
          const float targetEnv = -targetGainDb;
          if (targetEnv > mEnvelope[ch])
            mEnvelope[ch] += attackCoef * (targetEnv - mEnvelope[ch]);
          else
            mEnvelope[ch] += releaseCoef * (targetEnv - mEnvelope[ch]);

          const float gainDb = -mEnvelope[ch] + makeupDb;
          const float gain = std::pow(10.0f, gainDb * 0.05f);
          const float wetOut = inputByChannel[ch] * gain;
          mixedByChannel[ch] = inputByChannel[ch] * (1.0f - mix) + wetOut * mix;
        }

        float mixedL = mixedByChannel[0];
        float mixedR = mixedByChannel[1];

        if (softClip > 0.0f)
        {
          mixedL = drive_output_limiter::SoftClipNearCeiling(mixedL, clipKnee, 1.0f);
          mixedR = drive_output_limiter::SoftClipNearCeiling(mixedR, clipKnee, 1.0f);
        }

        if (outputs[0])
          outputs[0][i] = mixedL;
        if (outputs[1])
          outputs[1][i] = mixedR;

        mGainReduction = std::max(mEnvelope[0], mEnvelope[1]);
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
      else if (key == "softClip")
      {
        mSoftClip.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
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
      if (key == "softClip")
        return mSoftClip.load(std::memory_order_relaxed);
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
    std::atomic<float> mSoftClip{0.0f};

    // Coefficients (derived from params + sample rate, updated atomically)
    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};

    // State (audio thread only, no synchronization needed)
    float mEnvelope[2] = {0.0f, 0.0f};
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
      // Clear state when sample rate changes to ensure clean initialization
      Reset();
    }

    void Reset() override
    {
      mEnvelope[0] = 0.0f;
      mEnvelope[1] = 0.0f;
      mOptoCellState[0] = 0.0f;
      mOptoCellState[1] = 0.0f;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
      const float ratio = mRatio.load(std::memory_order_relaxed);
      const float makeupDb = mMakeupDb.load(std::memory_order_relaxed);
      const float mix = mMix.load(std::memory_order_relaxed);
      const float softClip = mSoftClip.load(std::memory_order_relaxed);
      const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
      const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
      const float detectCoef = mDetectCoef.load(std::memory_order_relaxed);
      const float clipKnee = 0.995f - 0.075f * std::clamp(softClip, 0.0f, 1.0f);

      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : inL;
        const float inputByChannel[2] = {inL, inR};
        float outputByChannel[2] = {0.0f, 0.0f};

        for (int ch = 0; ch < 2; ++ch)
        {
          const float inputPower = inputByChannel[ch] * inputByChannel[ch];
          mEnvelope[ch] += detectCoef * (inputPower - mEnvelope[ch]);
          const float rms = std::sqrt(mEnvelope[ch]);
          const float rmsDb = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -200.0f;

          float targetReduction = 0.0f;
          if (rmsDb > thresholdDb)
          {
            const float overDb = rmsDb - thresholdDb;
            targetReduction = overDb * (1.0f - 1.0f / ratio);
          }

          float optoSpeed = (targetReduction > mOptoCellState[ch]) ? attackCoef : releaseCoef;
          if (targetReduction < mOptoCellState[ch])
            optoSpeed *= std::max(0.2f, 1.0f - mOptoCellState[ch] * 0.02f);

          mOptoCellState[ch] += optoSpeed * (targetReduction - mOptoCellState[ch]);

          const float gainDb = -mOptoCellState[ch] + makeupDb;
          const float gain = std::pow(10.0f, gainDb * 0.05f);
          outputByChannel[ch] = inputByChannel[ch] * gain * mix + inputByChannel[ch] * (1.0f - mix);
        }

        float outL = outputByChannel[0];
        float outR = outputByChannel[1];

        if (softClip > 0.0f)
        {
          outL = drive_output_limiter::SoftClipNearCeiling(outL, clipKnee, 1.0f);
          outR = drive_output_limiter::SoftClipNearCeiling(outR, clipKnee, 1.0f);
        }

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
      else if (key == "softClip")
        mSoftClip.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
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
      if (key == "softClip")
        return mSoftClip.load(std::memory_order_relaxed);
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
    std::atomic<float> mSoftClip{0.0f};

    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};
    std::atomic<float> mDetectCoef{0.0f};

    float mEnvelope[2] = {0.0f, 0.0f};
    float mOptoCellState[2] = {0.0f, 0.0f};
  };

  inline void RegisterCompressorEffects()
  {
    // VCA compressor
    {
      EffectTypeInfo info;
      info.type = EffectGuids::kCompressorVca;
      info.aliases = {"compressor_vca"};
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
          {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
          {"softClip", "Soft Clip", 0.0, 0.0, 1.0, "amount", "Output", true}};

      EffectRegistry::Instance().Register(info.type, info, []()
                                          { return std::make_unique<CompressorEffect>(); });
    }

    // Opto compressor
    {
      EffectTypeInfo info;
      info.type = EffectGuids::kCompressorOpto;
      info.aliases = {"compressor_opto"};
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
          {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
          {"softClip", "Soft Clip", 0.0, 0.0, 1.0, "amount", "Output", true}};

      EffectRegistry::Instance().Register(info.type, info, []()
                                          { return std::make_unique<OptoCompressorEffect>(); });
    }
  }

} // namespace guitarfx
