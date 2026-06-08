#pragma once

/**
 * Neural Amp Modeler effect variant used for the optimized NAM type IDs.
 *
 * Processing relies on upstream NeuralAmpModelerCore DSP with standard
 * compiler optimizations enabled by the build configuration.
 */

#include "dsp/EffectProcessor.h"
#include "dsp/BlockSincResampler.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/NAMSampleRate.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "dsp/simd/OptimizedNAM.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <filesystem>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <cmath>
#include <variant>

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
 * NAM core-backed amp effect with tone controls.
 */
class OptimizedNAMAmpEffect : public EffectProcessor
{
public:
  void Prepare(double sampleRate, int maxBlockSize) override
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

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

    ConfigureModelProcessing();
    UpdateToneStack();
  }

  void Reset() override
  {
    ResetActiveModel(mModelLeft);
    ResetActiveModel(mModelRight);

    std::fill(mInputBufferL.begin(), mInputBufferL.end(), 0.0f);
    std::fill(mInputBufferR.begin(), mInputBufferR.end(), 0.0f);
    std::fill(mOutputBufferL.begin(), mOutputBufferL.end(), 0.0f);
    std::fill(mOutputBufferR.begin(), mOutputBufferR.end(), 0.0f);
    std::fill(mModelInputBufferL.begin(), mModelInputBufferL.end(), 0.0f);
    std::fill(mModelInputBufferR.begin(), mModelInputBufferR.end(), 0.0f);
    std::fill(mModelOutputBufferL.begin(), mModelOutputBufferL.end(), 0.0f);
    std::fill(mModelOutputBufferR.begin(), mModelOutputBufferR.end(), 0.0f);
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

    const bool hasFallback = HasActiveModel(mModelLeft) && HasActiveModel(mModelRight);

    if (hasFallback && mEnabled)
    {
      const float wetMix = static_cast<float>(mMix);
      const float dryMix = 1.0f - wetMix;
      const bool wetOnly = wetMix >= 0.9999f;
      const bool toneNeutral = IsToneStackNeutral();

      ProcessFallbackModels(numSamples);

      const float outputGainF = static_cast<float>(mOutputGain);

      if (toneNeutral && wetOnly && NearlyEqual(mOutputGain, 1.0))
      {
        for (int i = 0; i < numSamples; ++i)
        {
          if (outputs[0])
            outputs[0][i] = mOutputBufferL[i];
          if (outputs[1])
            outputs[1][i] = mOutputBufferR[i];
        }
        return;
      }

      if (toneNeutral)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          float outL = mOutputBufferL[i] * outputGainF;
          float outR = mOutputBufferR[i] * outputGainF;
          if (!wetOnly)
          {
            outL = mDryBufferL[i] * dryMix + outL * wetMix;
            outR = mDryBufferR[i] * dryMix + outR * wetMix;
          }
          if (outputs[0])
            outputs[0][i] = outL;
          if (outputs[1])
            outputs[1][i] = outR;
        }
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        float outL = mOutputBufferL[i];
        outL = mBassFilter[0].Process(outL);
        outL = mMidFilter[0].Process(outL);
        outL = mTrebleFilter[0].Process(outL);
        outL = mPresenceFilter[0].Process(outL);
        outL *= outputGainF;
        if (!wetOnly)
          outL = mDryBufferL[i] * dryMix + outL * wetMix;

        float outR = mOutputBufferR[i];
        outR = mBassFilter[1].Process(outR);
        outR = mMidFilter[1].Process(outR);
        outR = mTrebleFilter[1].Process(outR);
        outR = mPresenceFilter[1].Process(outR);
        outR *= outputGainF;
        if (!wetOnly)
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
        const float outL = mInputBufferL[i];
        const float outR = mInputBufferR[i];
        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }
  }

  [[nodiscard]] bool SupportsMonoProcessing() const override { return true; }

  void ProcessMono(float *input, float *output, int numSamples) override
  {
    EnsureLevelTargetsCurrent();

    numSamples = std::min(numSamples, mMaxBlockSize);
    if (!output || numSamples <= 0)
      return;

    if (!input)
    {
      std::fill_n(output, numSamples, 0.0f);
      return;
    }

    const float inputGainF = static_cast<float>(mInputGain);
    for (int i = 0; i < numSamples; ++i)
    {
      const float in = input[i];
      mDryBufferL[i] = in;
      mInputBufferL[i] = in * inputGainF;
    }

    const bool hasFallback = HasActiveModel(mModelLeft);
    if (hasFallback && mEnabled)
    {
      const float wetMix = static_cast<float>(mMix);
      const float dryMix = 1.0f - wetMix;
      const bool wetOnly = wetMix >= 0.9999f;
      const bool toneNeutral = IsToneStackNeutral();

      ProcessFallbackModelMono(numSamples);

      const float outputGainF = static_cast<float>(mOutputGain);

      if (toneNeutral && wetOnly && NearlyEqual(mOutputGain, 1.0))
      {
        std::copy_n(mOutputBufferL.data(), numSamples, output);
        return;
      }

      if (toneNeutral)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          float out = mOutputBufferL[i] * outputGainF;
          if (!wetOnly)
            out = mDryBufferL[i] * dryMix + out * wetMix;
          output[i] = out;
        }
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        float out = mOutputBufferL[i];
        out = mBassFilter[0].Process(out);
        out = mMidFilter[0].Process(out);
        out = mTrebleFilter[0].Process(out);
        out = mPresenceFilter[0].Process(out);
        out *= outputGainF;
        if (!wetOnly)
          out = mDryBufferL[i] * dryMix + out * wetMix;
        output[i] = out;
      }
    }
    else
    {
      for (int i = 0; i < numSamples; ++i)
      {
        output[i] = mInputBufferL[i];
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
    else if (key == "useNamInputMetadata")
    {
      mUseNamInputMetadata = value > 0.5;
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
    else if (key == "useNamInputMetadata")
    {
      mUseNamInputMetadata = ParseBool(value);
      RecalculateAutoGains();
    }
    else if (key == "autoLevelOutput")
    {
      mAutoLevelOutput = ParseBool(value);
      RecalculateAutoGains();
    }
    else if (key == "slimmableSize")
    {
      if (const auto parsed = ParseDouble(value); parsed.has_value())
        SetGlobalNamSlimmableSize(*parsed);
      ApplySlimmableToActiveModel(mModelLeft);
      ApplySlimmableToActiveModel(mModelRight);
    }
    else if (key == "useOptimized")
    {
      (void)value;
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
    return HasActiveModel(mModelLeft) && HasActiveModel(mModelRight);
  }

  [[nodiscard]] std::filesystem::path GetResourcePath() const override
  {
    return mModelPath;
  }

  [[nodiscard]] std::string GetType() const override { return "amp_nam_optimized"; }
  [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  [[nodiscard]] bool IsUsingOptimized() const
  {
    return std::holds_alternative<std::unique_ptr<nam::OptimizedDSPWrapper>>(mModelLeft)
      && std::holds_alternative<std::unique_ptr<nam::OptimizedDSPWrapper>>(mModelRight)
      && std::get<std::unique_ptr<nam::OptimizedDSPWrapper>>(mModelLeft)
      && std::get<std::unique_ptr<nam::OptimizedDSPWrapper>>(mModelRight);
  }

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
      ClearActiveModels();

      auto optimizedLeft = nam::LoadOptimizedModelWrapper(resourcePath);
      auto optimizedRight = nam::LoadOptimizedModelWrapper(resourcePath);
      if (optimizedLeft && optimizedRight)
      {
        mModelLeft = std::move(optimizedLeft);
        mModelRight = std::move(optimizedRight);
      }
      else
      {
        auto modelLeft = ::nam::get_dsp(resourcePath);
        auto modelRight = ::nam::get_dsp(resourcePath);
        if (!modelLeft || !modelRight)
          return false;

        ApplyGlobalNamSlimmableSize(modelLeft.get());
        ApplyGlobalNamSlimmableSize(modelRight.get());

        mModelLeft = std::move(modelLeft);
        mModelRight = std::move(modelRight);
      }

      mModelPath = resourcePath;
      mResourceNormalizationGainDb = ReadResourceMetadataDouble(ref, "normalizationGainDb");
      ConfigureModelProcessing();

      mModelInputLevel = ActiveModelHasInputLevel(mModelLeft)
        ? std::optional<double>(ActiveModelGetInputLevel(mModelLeft))
        : std::nullopt;
      mModelOutputLevel = ActiveModelHasOutputLevel(mModelLeft)
        ? std::optional<double>(ActiveModelGetOutputLevel(mModelLeft))
        : std::nullopt;
      mModelLoudness = ActiveModelHasLoudness(mModelLeft)
        ? std::optional<double>(ActiveModelGetLoudness(mModelLeft))
        : std::nullopt;

      RecalculateAutoGains();
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  using FallbackModelPtr = std::unique_ptr<::nam::DSP>;
  using OptimizedModelPtr = std::unique_ptr<nam::OptimizedDSPWrapper>;
  using ActiveModel = std::variant<FallbackModelPtr, OptimizedModelPtr>;

  ActiveModel mModelLeft;
  ActiveModel mModelRight;

  std::filesystem::path mModelPath;
  bool mResamplingActive = false;
  double mModelSampleRate = 44100.0;
  int mMaxModelBlockSize = 512;

  std::vector<float> mInputBufferL;
  std::vector<float> mInputBufferR;
  std::vector<float> mOutputBufferL;
  std::vector<float> mOutputBufferR;
  std::vector<float> mModelInputBufferL;
  std::vector<float> mModelInputBufferR;
  std::vector<float> mModelOutputBufferL;
  std::vector<float> mModelOutputBufferR;
  std::vector<float> mDryBufferL;
  std::vector<float> mDryBufferR;
  std::vector<NAM_SAMPLE> mFallbackInputBufferL;
  std::vector<NAM_SAMPLE> mFallbackInputBufferR;
  std::vector<NAM_SAMPLE> mFallbackOutputBufferL;
  std::vector<NAM_SAMPLE> mFallbackOutputBufferR;
  std::vector<float> mOptimizedInputScratch;
  std::vector<float> mOptimizedOutputScratch;

  BlockSincResampler mInputResampler;
  BlockSincResampler mOutputResampler;

  double mUserInputGain = 1.0;
  double mUserOutputGain = 1.0;
  double mAutoInputGain = 1.0;
  double mAutoOutputGain = 1.0;
  double mInputGain = 1.0;
  double mOutputGain = 1.0;
  double mMix = 1.0;
  bool mAutoLevelInput = false;
  bool mAutoLevelOutput = true;
  bool mUseNamInputMetadata = false;
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

  [[nodiscard]] static bool NearlyEqual(double a, double b, double epsilon = 1.0e-6)
  {
    return std::abs(a - b) <= epsilon;
  }

  [[nodiscard]] bool IsToneStackNeutral() const
  {
    return NearlyEqual(mBassDb, 0.0)
      && NearlyEqual(mMidDb, 0.0)
      && NearlyEqual(mTrebleDb, 0.0)
      && NearlyEqual(mPresenceDb, 0.0);
  }

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

    if (mAutoLevelInput && mUseNamInputMetadata && mModelInputLevel.has_value())
    {
      const double deltaDb = std::clamp(-*mModelInputLevel, -24.0, 24.0);
      mAutoInputGain = std::pow(10.0, deltaDb / 20.0);
    }

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

  [[nodiscard]] double ResolveModelSampleRate() const
  {
    if (!HasActiveModel(mModelLeft))
      return mSampleRate;

    const double expectedSR = ActiveModelGetExpectedSampleRate(mModelLeft);
    return ResolveNamModelProcessingSampleRate(expectedSR, mSampleRate);
  }

  void ConfigureModelProcessing()
  {
    mModelSampleRate = ResolveModelSampleRate();
    // Match NeuralAmpModelerPlugin behavior: resample on any SR mismatch.
    mResamplingActive = NeedsNamRuntimeResampling(mModelSampleRate, mSampleRate);
    mMaxModelBlockSize = mResamplingActive
      ? BlockSincResampler::ComputeMaxOutputFrameCount(mMaxBlockSize, mSampleRate, mModelSampleRate)
      : mMaxBlockSize;
    mMaxModelBlockSize = std::max(1, mMaxModelBlockSize);

    mModelInputBufferL.resize(static_cast<size_t>(mMaxModelBlockSize));
    mModelInputBufferR.resize(static_cast<size_t>(mMaxModelBlockSize));
    mModelOutputBufferL.resize(static_cast<size_t>(mMaxModelBlockSize));
    mModelOutputBufferR.resize(static_cast<size_t>(mMaxModelBlockSize));
    mFallbackInputBufferL.resize(static_cast<size_t>(mMaxModelBlockSize));
    mFallbackInputBufferR.resize(static_cast<size_t>(mMaxModelBlockSize));
    mFallbackOutputBufferL.resize(static_cast<size_t>(mMaxModelBlockSize));
    mFallbackOutputBufferR.resize(static_cast<size_t>(mMaxModelBlockSize));
    mOptimizedInputScratch.resize(static_cast<size_t>(mMaxModelBlockSize));
    mOptimizedOutputScratch.resize(static_cast<size_t>(mMaxModelBlockSize));

    mInputResampler.Prepare(mSampleRate, mModelSampleRate, mMaxBlockSize, SampleRateConversionQuality::HighPerformance);
    mOutputResampler.Prepare(mModelSampleRate, mSampleRate, mMaxModelBlockSize, SampleRateConversionQuality::HighPerformance);

    ResetActiveModel(mModelLeft);
    ResetActiveModel(mModelRight);
  }

  [[nodiscard]] int GetModelFrameCount(int numSamples) const
  {
    int modelFrames = BlockSincResampler::ComputeOutputFrameCount(numSamples, mSampleRate, mModelSampleRate);
    return std::clamp(modelFrames, 1, mMaxModelBlockSize);
  }

  void ProcessFallbackModels(int numSamples)
  {
    int modelFrames = numSamples;
    if (mResamplingActive)
    {
      modelFrames = GetModelFrameCount(numSamples);
      mInputResampler.ProcessFixedOutput(mInputBufferL.data(), numSamples, mFallbackInputBufferL.data(), modelFrames);
      mInputResampler.ProcessFixedOutput(mInputBufferR.data(), numSamples, mFallbackInputBufferR.data(), modelFrames);
    }
    else
    {
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        mFallbackInputBufferL[sampleIndex] = static_cast<NAM_SAMPLE>(mInputBufferL[sampleIndex]);
        mFallbackInputBufferR[sampleIndex] = static_cast<NAM_SAMPLE>(mInputBufferR[sampleIndex]);
      }
    }

    NAM_SAMPLE* inputPtrL = mFallbackInputBufferL.data();
    NAM_SAMPLE* outputPtrL = mFallbackOutputBufferL.data();
    NAM_SAMPLE* inputPtrsL[1] = { inputPtrL };
    NAM_SAMPLE* outputPtrsL[1] = { outputPtrL };
    ProcessActiveModel(mModelLeft, inputPtrL, outputPtrL, modelFrames);

    NAM_SAMPLE* inputPtrR = mFallbackInputBufferR.data();
    NAM_SAMPLE* outputPtrR = mFallbackOutputBufferR.data();
    NAM_SAMPLE* inputPtrsR[1] = { inputPtrR };
    NAM_SAMPLE* outputPtrsR[1] = { outputPtrR };
    ProcessActiveModel(mModelRight, inputPtrR, outputPtrR, modelFrames);

    if (mResamplingActive)
    {
      mOutputResampler.ProcessFixedOutput(mFallbackOutputBufferL.data(), modelFrames, mOutputBufferL.data(), numSamples);
      mOutputResampler.ProcessFixedOutput(mFallbackOutputBufferR.data(), modelFrames, mOutputBufferR.data(), numSamples);
    }
    else
    {
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        mOutputBufferL[sampleIndex] = static_cast<float>(mFallbackOutputBufferL[sampleIndex]);
        mOutputBufferR[sampleIndex] = static_cast<float>(mFallbackOutputBufferR[sampleIndex]);
      }
    }
  }

  void ProcessFallbackModelMono(int numSamples)
  {
    int modelFrames = numSamples;
    if (mResamplingActive)
    {
      modelFrames = GetModelFrameCount(numSamples);
      mInputResampler.ProcessFixedOutput(mInputBufferL.data(), numSamples, mFallbackInputBufferL.data(), modelFrames);
    }
    else
    {
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        mFallbackInputBufferL[sampleIndex] = static_cast<NAM_SAMPLE>(mInputBufferL[sampleIndex]);
      }
    }

    NAM_SAMPLE* inputPtr = mFallbackInputBufferL.data();
    NAM_SAMPLE* outputPtr = mFallbackOutputBufferL.data();
    NAM_SAMPLE* inputPtrs[1] = { inputPtr };
    NAM_SAMPLE* outputPtrs[1] = { outputPtr };
    ProcessActiveModel(mModelLeft, inputPtr, outputPtr, modelFrames);

    if (mResamplingActive)
    {
      mOutputResampler.ProcessFixedOutput(mFallbackOutputBufferL.data(), modelFrames, mOutputBufferL.data(), numSamples);
    }
    else
    {
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        mOutputBufferL[sampleIndex] = static_cast<float>(mFallbackOutputBufferL[sampleIndex]);
      }
    }
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

  static bool HasActiveModel(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) { return static_cast<bool>(ptr); }, model);
  }

  void ClearActiveModels()
  {
    mModelLeft = FallbackModelPtr{};
    mModelRight = FallbackModelPtr{};
  }

  static void ApplySlimmableToActiveModel(ActiveModel& model)
  {
    std::visit([](auto& ptr)
    {
      if constexpr (std::is_same_v<std::decay_t<decltype(ptr)>, FallbackModelPtr>)
      {
        ApplyGlobalNamSlimmableSize(ptr.get());
      }
    }, model);
  }

  void ResetActiveModel(ActiveModel& model)
  {
    std::visit([this](auto& ptr)
    {
      if (ptr)
        ptr->Reset(mModelSampleRate, mMaxModelBlockSize);
    }, model);
  }

  static double ActiveModelGetExpectedSampleRate(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> double
    {
      return ptr ? ptr->GetExpectedSampleRate() : -1.0;
    }, model);
  }

  static bool ActiveModelHasInputLevel(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> bool
    {
      return ptr && ptr->HasInputLevel();
    }, model);
  }

  static double ActiveModelGetInputLevel(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> double
    {
      return ptr ? ptr->GetInputLevel() : 0.0;
    }, model);
  }

  static bool ActiveModelHasOutputLevel(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> bool
    {
      return ptr && ptr->HasOutputLevel();
    }, model);
  }

  static double ActiveModelGetOutputLevel(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> double
    {
      return ptr ? ptr->GetOutputLevel() : 0.0;
    }, model);
  }

  static bool ActiveModelHasLoudness(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> bool
    {
      return ptr && ptr->HasLoudness();
    }, model);
  }

  static double ActiveModelGetLoudness(const ActiveModel& model)
  {
    return std::visit([](const auto& ptr) -> double
    {
      return ptr ? ptr->GetLoudness() : 0.0;
    }, model);
  }

  void ProcessActiveModel(ActiveModel& model, NAM_SAMPLE* input, NAM_SAMPLE* output, int frames)
  {
    std::visit([this, input, output, frames](auto& ptr)
    {
      if (!ptr)
        return;

      if constexpr (std::is_same_v<std::decay_t<decltype(ptr)>, FallbackModelPtr>)
      {
        NAM_SAMPLE* inputPtrs[1] = { input };
        NAM_SAMPLE* outputPtrs[1] = { output };
        ptr->process(inputPtrs, outputPtrs, frames);
      }
      else
      {
        if (frames <= 0)
          return;

        const std::size_t frameCount = static_cast<std::size_t>(frames);
        if (mOptimizedInputScratch.size() < frameCount)
          mOptimizedInputScratch.resize(frameCount);
        if (mOptimizedOutputScratch.size() < frameCount)
          mOptimizedOutputScratch.resize(frameCount);

        for (int i = 0; i < frames; ++i)
          mOptimizedInputScratch[static_cast<std::size_t>(i)] = static_cast<float>(input[i]);

        ptr->process(mOptimizedInputScratch.data(), mOptimizedOutputScratch.data(), frames);

        for (int i = 0; i < frames; ++i)
          output[i] = static_cast<NAM_SAMPLE>(mOptimizedOutputScratch[static_cast<std::size_t>(i)]);
      }
    }, model);
  }
};

inline void RegisterOptimizedNAMAmpEffect()
{
  EffectTypeInfo info;
  info.type = EffectGuids::kAmpNamOptimized;
  info.aliases = {"amp_nam_optimized", "amp_nam", EffectGuids::kAmpNam};
  info.displayName = "Neural Amp (NAM)";
  info.category = "amp";
  info.description = "Neural Amp Modeler (NAM)";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.resourceFilterHint = {"amp", "full-rig"};
  info.parameters = {
    {"inputGain",             "Input",              0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"useNamInputMetadata",   "Use NAM Input Metadata", 0.0, 0.0,   1.0,  "toggle", "Advanced", true},
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
