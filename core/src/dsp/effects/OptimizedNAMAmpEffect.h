#pragma once

/**
 * Optimized Neural Amp Modeler effect - uses SIMD-accelerated processing.
 *
 * This is a high-performance replacement for NAMAmpEffect that uses the
 * custom optimized NAM implementation instead of the upstream library.
 *
 * Key improvements:
 * - SIMD-vectorized activation functions (AVX/SSE)
 * - Fused gated activation kernels for WaveNet
 * - Reduced virtual dispatch overhead
 * - Falls back to original NAM library for unsupported architectures
 */

#include "dsp/EffectProcessor.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/simd/OptimizedNAM.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <filesystem>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>
#include <variant>

// Forward declare factory registration helper to avoid linker dead-stripping
namespace nam
{
  namespace factory
  {
    void ForceFactoryRegistration();
  }
}

namespace guitarfx
{

// Simple biquad filter (Transposed Direct Form II) for amp tone stack.
// Coefficients use Audio EQ Cookbook formulas (RBJ).
struct AmpToneBiquad
{
  double b0 = 1.0, b1 = 0.0, b2 = 0.0;
  double a1 = 0.0, a2 = 0.0;
  double s1 = 0.0, s2 = 0.0;

  void Reset() { s1 = s2 = 0.0; }

  float Process(float x)
  {
    double y = b0 * x + s1;
    s1 = b1 * x - a1 * y + s2;
    s2 = b2 * x - a2 * y;
    return static_cast<float>(y);
  }

  // Low-shelving filter (slope S=1)
  void SetLowShelf(double freqHz, double gainDb, double sampleRate)
  {
    static constexpr double kPi = 3.14159265358979323846;
    const double A    = std::pow(10.0, gainDb / 40.0);
    const double w0   = 2.0 * kPi * freqHz / sampleRate;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double sqA  = std::sqrt(A);
    // alpha for S=1: sin(w0)/2 * sqrt(2) = sin(w0)/sqrt(2)
    const double alpha = sinw / std::sqrt(2.0);
    const double a0   = (A+1) + (A-1)*cosw + 2*sqA*alpha;
    b0 =  A * ((A+1) - (A-1)*cosw + 2*sqA*alpha) / a0;
    b1 = 2*A * ((A-1) - (A+1)*cosw)              / a0;
    b2 =  A * ((A+1) - (A-1)*cosw - 2*sqA*alpha) / a0;
    a1 = -2 * ((A-1) + (A+1)*cosw)               / a0;
    a2 =      ((A+1) + (A-1)*cosw - 2*sqA*alpha) / a0;
  }

  // High-shelving filter (slope S=1)
  void SetHighShelf(double freqHz, double gainDb, double sampleRate)
  {
    static constexpr double kPi = 3.14159265358979323846;
    const double A    = std::pow(10.0, gainDb / 40.0);
    const double w0   = 2.0 * kPi * freqHz / sampleRate;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double sqA  = std::sqrt(A);
    const double alpha = sinw / std::sqrt(2.0);
    const double a0   = (A+1) - (A-1)*cosw + 2*sqA*alpha;
    b0 =  A * ((A+1) + (A-1)*cosw + 2*sqA*alpha) / a0;
    b1 = -2*A * ((A-1) + (A+1)*cosw)             / a0;
    b2 =  A * ((A+1) + (A-1)*cosw - 2*sqA*alpha) / a0;
    a1 =  2 * ((A-1) - (A+1)*cosw)               / a0;
    a2 =      ((A+1) - (A-1)*cosw - 2*sqA*alpha) / a0;
  }

  // Peaking (bell) EQ
  void SetPeaking(double freqHz, double gainDb, double Q, double sampleRate)
  {
    static constexpr double kPi = 3.14159265358979323846;
    const double A    = std::pow(10.0, gainDb / 40.0);
    const double w0   = 2.0 * kPi * freqHz / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * Q);
    const double a0   = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0;
    b1 = (-2.0 * cosw)     / a0;
    b2 = (1.0 - alpha * A) / a0;
    a1 = (-2.0 * cosw)     / a0;
    a2 = (1.0 - alpha / A) / a0;
  }
};

/**
 * Optimized Neural Amp Modeler effect.
 *
 * Automatically uses SIMD-optimized processing for WaveNet and LSTM models,
 * falling back to the original NAM library for unsupported architectures.
 */
class OptimizedNAMAmpEffect : public EffectProcessor
{
public:
  void Prepare(double sampleRate, int maxBlockSize) override
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
    mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferL.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferR.resize(static_cast<size_t>(maxBlockSize));
    mFallbackInputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mFallbackInputBufferR.resize(static_cast<size_t>(maxBlockSize));
    mFallbackOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mFallbackOutputBufferR.resize(static_cast<size_t>(maxBlockSize));

    if (mOptimizedModelLeft)
    {
      mOptimizedModelLeft->Reset(sampleRate, maxBlockSize);
    }
    if (mOptimizedModelRight)
    {
      mOptimizedModelRight->Reset(sampleRate, maxBlockSize);
    }
    if (mFallbackModelLeft)
    {
      mFallbackModelLeft->Reset(sampleRate, maxBlockSize);
    }
    if (mFallbackModelRight)
    {
      mFallbackModelRight->Reset(sampleRate, maxBlockSize);
    }
    CheckSampleRateMismatch();
    UpdateToneStack();
  }

  void Reset() override
  {
    if (mOptimizedModelLeft)
    {
      mOptimizedModelLeft->Reset(mSampleRate, mMaxBlockSize);
    }
    if (mOptimizedModelRight)
    {
      mOptimizedModelRight->Reset(mSampleRate, mMaxBlockSize);
    }
    if (mFallbackModelLeft)
    {
      mFallbackModelLeft->Reset(mSampleRate, mMaxBlockSize);
    }
    if (mFallbackModelRight)
    {
      mFallbackModelRight->Reset(mSampleRate, mMaxBlockSize);
    }

    std::fill(mInputBufferL.begin(), mInputBufferL.end(), 0.0f);
    std::fill(mInputBufferR.begin(), mInputBufferR.end(), 0.0f);
    std::fill(mOutputBufferL.begin(), mOutputBufferL.end(), 0.0f);
    std::fill(mOutputBufferR.begin(), mOutputBufferR.end(), 0.0f);
    std::fill(mDryBufferL.begin(), mDryBufferL.end(), 0.0f);
    std::fill(mDryBufferR.begin(), mDryBufferR.end(), 0.0f);
    std::fill(mFallbackInputBufferL.begin(), mFallbackInputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mFallbackInputBufferR.begin(), mFallbackInputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mFallbackOutputBufferL.begin(), mFallbackOutputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mFallbackOutputBufferR.begin(), mFallbackOutputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));

    for (int ch = 0; ch < 2; ++ch)
    {
      mBassFilter[ch].Reset();
      mMidFilter[ch].Reset();
      mTrebleFilter[ch].Reset();
      mPresenceFilter[ch].Reset();
    }
  }

  void Process(float** inputs, float** outputs, int numSamples) override
  {
    EnsureLevelTargetsCurrent();

    // Clamp to allocated buffer size to prevent out-of-bounds writes
    numSamples = std::min(numSamples, mMaxBlockSize);

    if (!inputs[0] && !inputs[1])
    {
      if (outputs[0])
        std::fill_n(outputs[0], numSamples, 0.0f);
      if (outputs[1])
        std::fill_n(outputs[1], numSamples, 0.0f);
      return;
    }

    const float inputGainF = static_cast<float>(mInputGain);
    for (int i = 0; i < numSamples; ++i)
    {
      float inL = inputs[0] ? inputs[0][i] : 0.0f;
      float inR = inputs[1] ? inputs[1][i] : inL;
      mDryBufferL[i] = inL;
      mDryBufferR[i] = inR;
      mInputBufferL[i] = inL * inputGainF;
      mInputBufferR[i] = inR * inputGainF;
    }

    const bool hasOptimized = mOptimizedModelLeft && mOptimizedModelRight;
    const bool hasFallback = mFallbackModelLeft && mFallbackModelRight;

    if ((hasOptimized || hasFallback) && mEnabled)
    {
      const float wetMix = static_cast<float>(mMix);
      const float dryMix = 1.0f - wetMix;

      if (hasOptimized)
      {
        mOptimizedModelLeft->process(mInputBufferL.data(), mOutputBufferL.data(), numSamples);
        mOptimizedModelRight->process(mInputBufferR.data(), mOutputBufferR.data(), numSamples);

        const float outputGainF = static_cast<float>(mOutputGain);
        for (int i = 0; i < numSamples; ++i)
        {
          float outL = mOutputBufferL[i];
          outL = mBassFilter[0].Process(outL);
          outL = mMidFilter[0].Process(outL);
          outL = mTrebleFilter[0].Process(outL);
          outL = mPresenceFilter[0].Process(outL);
          outL *= outputGainF;
          outL = mDryBufferL[i] * dryMix + outL * wetMix;

          float outR = mOutputBufferR[i];
          outR = mBassFilter[1].Process(outR);
          outR = mMidFilter[1].Process(outR);
          outR = mTrebleFilter[1].Process(outR);
          outR = mPresenceFilter[1].Process(outR);
          outR *= outputGainF;
          outR = mDryBufferR[i] * dryMix + outR * wetMix;

          if (outputs[0])
            outputs[0][i] = outL;
          if (outputs[1])
            outputs[1][i] = outR;
        }
      }
      else
      {
        for (int i = 0; i < numSamples; ++i)
        {
          mFallbackInputBufferL[i] = static_cast<NAM_SAMPLE>(mInputBufferL[i]);
          mFallbackInputBufferR[i] = static_cast<NAM_SAMPLE>(mInputBufferR[i]);
        }

        NAM_SAMPLE* inputPtrL = mFallbackInputBufferL.data();
        NAM_SAMPLE* outputPtrL = mFallbackOutputBufferL.data();
        NAM_SAMPLE* inputPtrsL[1] = { inputPtrL };
        NAM_SAMPLE* outputPtrsL[1] = { outputPtrL };
        mFallbackModelLeft->process(inputPtrsL, outputPtrsL, numSamples);

        NAM_SAMPLE* inputPtrR = mFallbackInputBufferR.data();
        NAM_SAMPLE* outputPtrR = mFallbackOutputBufferR.data();
        NAM_SAMPLE* inputPtrsR[1] = { inputPtrR };
        NAM_SAMPLE* outputPtrsR[1] = { outputPtrR };
        mFallbackModelRight->process(inputPtrsR, outputPtrsR, numSamples);

        const float outputGainF = static_cast<float>(mOutputGain);
        for (int i = 0; i < numSamples; ++i)
        {
          float outL = static_cast<float>(mFallbackOutputBufferL[i]);
          outL = mBassFilter[0].Process(outL);
          outL = mMidFilter[0].Process(outL);
          outL = mTrebleFilter[0].Process(outL);
          outL = mPresenceFilter[0].Process(outL);
          outL *= outputGainF;
          outL = mDryBufferL[i] * dryMix + outL * wetMix;

          float outR = static_cast<float>(mFallbackOutputBufferR[i]);
          outR = mBassFilter[1].Process(outR);
          outR = mMidFilter[1].Process(outR);
          outR = mTrebleFilter[1].Process(outR);
          outR = mPresenceFilter[1].Process(outR);
          outR *= outputGainF;
          outR = mDryBufferR[i] * dryMix + outR * wetMix;

          if (outputs[0])
            outputs[0][i] = outL;
          if (outputs[1])
            outputs[1][i] = outR;
        }
      }
    }
    else
    {
      for (int i = 0; i < numSamples; ++i)
      {
        const float outL = mInputBufferL[i];
        const float outR = mInputBufferR[i];
        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }
  }

  void SetParam(const std::string& key, double value) override
  {
    if (key == "inputGain")
    {
      mUserInputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      UpdateEffectiveGains();
    }
    else if (key == "outputGain")
    {
      mUserOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      UpdateEffectiveGains();
    }
    else if (key == "mix")
    {
      mMix = std::clamp(value, 0.0, 1.0);
    }
    else if (key == "autoLevelInput")
    {
      mAutoLevelInput = value > 0.5;
      RecalculateAutoGains();
    }
    else if (key == "autoLevelOutput")
    {
      mAutoLevelOutput = value > 0.5;
      RecalculateAutoGains();
    }
    else if (key == "calibrationInputLevel")
    {
      if (std::isfinite(value))
        mCalibrationInputLevel = value;
      else
        mCalibrationInputLevel.reset();
      RecalculateAutoGains();
    }
    else if (key == "calibrationOutputLevel")
    {
      if (std::isfinite(value))
        mCalibrationOutputLevel = value;
      else
        mCalibrationOutputLevel.reset();
      RecalculateAutoGains();
    }
    else if (key == "bass")
    {
      mBassDb = std::clamp(value, -10.0, 10.0);
      UpdateToneStack();
    }
    else if (key == "mid")
    {
      mMidDb = std::clamp(value, -10.0, 10.0);
      UpdateToneStack();
    }
    else if (key == "treble")
    {
      mTrebleDb = std::clamp(value, -10.0, 10.0);
      UpdateToneStack();
    }
    else if (key == "presence")
    {
      mPresenceDb = std::clamp(value, -10.0, 10.0);
      UpdateToneStack();
    }
    else if (key == "enabled")
    {
      mEnabled = value > 0.5;
    }
  }

  void SetConfig(const std::string& key, const std::string& value) override
  {
    if (key == "autoLevelInput")
    {
      mAutoLevelInput = ParseBool(value);
      RecalculateAutoGains();
    }
    else if (key == "autoLevelOutput")
    {
      mAutoLevelOutput = ParseBool(value);
      RecalculateAutoGains();
    }
    else if (key == "useOptimized")
    {
      mPreferOptimized = ParseBool(value);
      // Reload model with new preference if we have a path
      if (!mModelPath.empty())
      {
        LoadResource(mModelPath);
      }
    }
  }

  [[nodiscard]] double GetParam(const std::string& key) const override
  {
    if (key == "inputGain")
      return 20.0 * std::log10(mUserInputGain);
    if (key == "outputGain")
      return 20.0 * std::log10(mUserOutputGain);
    if (key == "mix")
      return mMix;
    if (key == "bass")
      return mBassDb;
    if (key == "mid")
      return mMidDb;
    if (key == "treble")
      return mTrebleDb;
    if (key == "presence")
      return mPresenceDb;
    if (key == "enabled")
      return mEnabled ? 1.0 : 0.0;
    return 0.0;
  }

  bool LoadResources(const std::vector<ResourceRef> &refs,
                     const std::vector<std::filesystem::path> &paths) override
  {
    if (paths.empty())
      return false;

    const ResourceRef *ref = refs.empty() ? nullptr : &refs.front();
    return LoadModelResource(paths.front(), ref);
  }

  bool LoadResource(const std::filesystem::path& resourcePath) override
  {
    return LoadModelResource(resourcePath, nullptr);
  }

  [[nodiscard]] bool HasResource() const override
  {
    return (mOptimizedModelLeft && mOptimizedModelRight)
      || (mFallbackModelLeft && mFallbackModelRight);
  }

  [[nodiscard]] std::filesystem::path GetResourcePath() const override
  {
    return mModelPath;
  }

  [[nodiscard]] std::string GetType() const override { return "amp_nam_optimized"; }
  [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  // Query whether using optimized implementation
  [[nodiscard]] bool IsUsingOptimized() const { return mUsingOptimized; }

private:
  static std::optional<double> ReadResourceMetadataDouble(const ResourceRef *ref, const std::string &key)
  {
    if (!ref)
      return std::nullopt;

    const auto it = ref->metadata.find(key);
    if (it == ref->metadata.end())
      return std::nullopt;

    try
    {
      return std::stod(it->second);
    }
    catch (...)
    {
      return std::nullopt;
    }
  }

  bool LoadModelResource(const std::filesystem::path& resourcePath, const ResourceRef *ref)
  {
    try
    {
      mOptimizedModelLeft.reset();
      mOptimizedModelRight.reset();
      mFallbackModelLeft.reset();
      mFallbackModelRight.reset();

      bool loaded = false;

      if (mPreferOptimized)
      {
        auto optimizedLeft = nam::LoadOptimizedModelWrapper(resourcePath);
        auto optimizedRight = nam::LoadOptimizedModelWrapper(resourcePath);
        if (optimizedLeft && optimizedRight && optimizedLeft->IsValid() && optimizedRight->IsValid())
        {
          mOptimizedModelLeft = std::move(optimizedLeft);
          mOptimizedModelRight = std::move(optimizedRight);
          mOptimizedModelLeft->Reset(mSampleRate, mMaxBlockSize);
          mOptimizedModelRight->Reset(mSampleRate, mMaxBlockSize);
          loaded = true;
          mUsingOptimized = true;
        }
      }

      if (!loaded)
      {
        auto modelLeft = ::nam::get_dsp(resourcePath);
        auto modelRight = ::nam::get_dsp(resourcePath);
        if (!modelLeft || !modelRight)
          return false;

        modelLeft->Reset(mSampleRate, mMaxBlockSize);
        modelRight->Reset(mSampleRate, mMaxBlockSize);
        mFallbackModelLeft = std::move(modelLeft);
        mFallbackModelRight = std::move(modelRight);
        loaded = true;
        mUsingOptimized = false;
      }

      if (!loaded)
        return false;

      mModelPath = resourcePath;
      mResourceNormalizationGainDb = ReadResourceMetadataDouble(ref, "normalizationGainDb");
      CheckSampleRateMismatch();

      // Extract metadata
      if (mOptimizedModelLeft)
      {
        mModelInputLevel = mOptimizedModelLeft->HasInputLevel()
          ? std::optional<double>(mOptimizedModelLeft->GetInputLevel())
          : std::nullopt;
        mModelOutputLevel = mOptimizedModelLeft->HasOutputLevel()
          ? std::optional<double>(mOptimizedModelLeft->GetOutputLevel())
          : std::nullopt;
        mModelLoudness = mOptimizedModelLeft->HasLoudness()
          ? std::optional<double>(mOptimizedModelLeft->GetLoudness())
          : std::nullopt;
      }
      else if (mFallbackModelLeft)
      {
        mModelInputLevel = mFallbackModelLeft->HasInputLevel()
          ? std::optional<double>(mFallbackModelLeft->GetInputLevel())
          : std::nullopt;
        mModelOutputLevel = mFallbackModelLeft->HasOutputLevel()
          ? std::optional<double>(mFallbackModelLeft->GetOutputLevel())
          : std::nullopt;
        mModelLoudness = mFallbackModelLeft->HasLoudness()
          ? std::optional<double>(mFallbackModelLeft->GetLoudness())
          : std::nullopt;
      }

      RecalculateAutoGains();
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  // Model storage - one or the other will be used
  std::unique_ptr<nam::OptimizedDSPWrapper> mOptimizedModelLeft;
  std::unique_ptr<nam::OptimizedDSPWrapper> mOptimizedModelRight;
  std::unique_ptr<::nam::DSP> mFallbackModelLeft;
  std::unique_ptr<::nam::DSP> mFallbackModelRight;

  std::filesystem::path mModelPath;
  bool mUsingOptimized = false;
  bool mPreferOptimized = true;
  bool mSampleRateMismatch = false;  // Default to preferring optimized
  bool mPrepared = false;

  std::vector<float> mInputBufferL;
  std::vector<float> mInputBufferR;
  std::vector<float> mOutputBufferL;
  std::vector<float> mOutputBufferR;
  std::vector<float> mDryBufferL;
  std::vector<float> mDryBufferR;
  std::vector<NAM_SAMPLE> mFallbackInputBufferL;
  std::vector<NAM_SAMPLE> mFallbackInputBufferR;
  std::vector<NAM_SAMPLE> mFallbackOutputBufferL;
  std::vector<NAM_SAMPLE> mFallbackOutputBufferR;

  double mUserInputGain = 1.0;
  double mUserOutputGain = 1.0;
  double mAutoInputGain = 1.0;
  double mAutoOutputGain = 1.0;
  double mInputGain = 1.0;
  double mOutputGain = 1.0;
  double mMix = 1.0;
  bool mAutoLevelInput = false;
  bool mAutoLevelOutput = true;
  std::optional<double> mModelInputLevel;
  std::optional<double> mModelOutputLevel;
  std::optional<double> mModelLoudness;
  std::optional<double> mResourceNormalizationGainDb;
  std::optional<double> mCalibrationInputLevel;
  std::optional<double> mCalibrationOutputLevel;
  bool mEnabled = true;
  std::uint64_t mLevelTargetsRevision = 0;

  // Tone stack EQ parameters (dB, -10..+10, default 0)
  double mBassDb     = 0.0; // Low shelf  ~100 Hz
  double mMidDb      = 0.0; // Peak EQ    ~500 Hz
  double mTrebleDb   = 0.0; // High shelf ~3200 Hz
  double mPresenceDb = 0.0; // Peak EQ    ~6300 Hz

  AmpToneBiquad mBassFilter[2];
  AmpToneBiquad mMidFilter[2];
  AmpToneBiquad mTrebleFilter[2];
  AmpToneBiquad mPresenceFilter[2];

  void UpdateToneStack()
  {
    if (mSampleRate <= 0.0)
      return;
    for (int ch = 0; ch < 2; ++ch)
    {
      mBassFilter[ch].SetLowShelf(100.0,  mBassDb,     mSampleRate);
      mMidFilter[ch].SetPeaking(500.0,  mMidDb, 1.0, mSampleRate);
      mTrebleFilter[ch].SetHighShelf(3200.0, mTrebleDb, mSampleRate);
      mPresenceFilter[ch].SetPeaking(6300.0, mPresenceDb, 1.5, mSampleRate);
    }
  }

  void UpdateEffectiveGains()
  {
    mInputGain = mUserInputGain * mAutoInputGain;
    mOutputGain = mUserOutputGain * mAutoOutputGain;
  }

  void RecalculateAutoGains()
  {
    mAutoInputGain = 1.0;
    mAutoOutputGain = 1.0;

    if (mAutoLevelOutput)
    {
      if (mResourceNormalizationGainDb.has_value())
      {
        const double deltaDb = std::clamp(*mResourceNormalizationGainDb, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
      else if (mModelLoudness.has_value())
      {
        const double deltaDb = std::clamp(GetNominalOperatingLevelDbfs() - *mModelLoudness, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
    }

    mLevelTargetsRevision = GetLevelTargetsRevision();
    UpdateEffectiveGains();
  }

  void EnsureLevelTargetsCurrent()
  {
    const auto revision = GetLevelTargetsRevision();
    if (revision != mLevelTargetsRevision)
      RecalculateAutoGains();
  }

  void CheckSampleRateMismatch()
  {
    double expectedSR = -1.0;
    if (mOptimizedModelLeft)
      expectedSR = mOptimizedModelLeft->GetExpectedSampleRate();
    else if (mFallbackModelLeft)
      expectedSR = mFallbackModelLeft->GetExpectedSampleRate();

    const bool mismatch = (expectedSR > 0.0 && std::abs(expectedSR - mSampleRate) > 1.0);
    if (mismatch && !mSampleRateMismatch && mPrepared)
    {
      std::cerr << "[OptimizedNAMAmpEffect] Sample rate mismatch: model expects "
                << static_cast<int>(expectedSR) << " Hz, plugin running at "
                << static_cast<int>(mSampleRate) << " Hz - output quality may be degraded\n";
    }
    mSampleRateMismatch = mismatch;
  }

  static bool ParseBool(const std::string& value)
  {
    return value == "1" || value == "true" || value == "True" || value == "TRUE";
  }

  static std::optional<double> ParseDouble(const std::string& value)
  {
    try
    {
      return std::stod(value);
    }
    catch (...)
    {
      return std::nullopt;
    }
  }
};

inline void RegisterOptimizedNAMAmpEffect()
{
  // Ensure NAM factory registrations are not optimized out by the linker
  ::nam::factory::ForceFactoryRegistration();

  EffectTypeInfo info;
  info.type = EffectGuids::kAmpNamOptimized;
  info.aliases = {"amp_nam_optimized"};
  info.displayName = "Neural Amp (NAM)";
  info.category = "amp";
  info.description = "Neural Amp Modeler (NAM) with SIMD-optimized processing";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.resourceFilterHint = {"amp", "full-rig"};
  info.parameters = {
    {"inputGain",             "Input",              0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"bass",                  "Bass",               0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"mid",                   "Mid",                0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"treble",                "Treble",             0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"presence",              "Presence",           0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"outputGain",            "Output",             0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"mix",                   "Mix",                1.0,    0.0,   1.0,  "amount", "Advanced", true},
    {"autoLevelOutput",       "Auto Level Output",  1.0,    0.0,   1.0,  "toggle", "Advanced", true}
  };

  EffectRegistry::Instance().Register(info.type, info, []()
  {
    return std::make_unique<OptimizedNAMAmpEffect>();
  });
}

} // namespace guitarfx
