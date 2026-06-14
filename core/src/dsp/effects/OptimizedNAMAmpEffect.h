#pragma once

/**
 * Neural Amp Modeler effect used for the NAM amp type IDs.
 *
 * Processing relies entirely on upstream NeuralAmpModelerCore DSP, which is
 * already SIMD-optimized. This effect adds the tone stack, level staging and
 * runtime resampling around the core model.
 */

#include "dsp/EffectProcessor.h"
#include "dsp/BlockSincResampler.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/effects/NAMSampleRate.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cmath>

// Forward declare factory registration helper to avoid linker dead-stripping.
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

    ConfigureModelProcessing();
    UpdateToneStack();
  }

  void Reset() override
  {
    if (mModelLeft)
      mModelLeft->Reset(mModelSampleRate, mMaxModelBlockSize);
    if (mModelRight)
      mModelRight->Reset(mModelSampleRate, mMaxModelBlockSize);

    std::fill(mInputBufferL.begin(), mInputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mInputBufferR.begin(), mInputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mOutputBufferL.begin(), mOutputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mOutputBufferR.begin(), mOutputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mModelInputBufferL.begin(), mModelInputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mModelInputBufferR.begin(), mModelInputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mModelOutputBufferL.begin(), mModelOutputBufferL.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mModelOutputBufferR.begin(), mModelOutputBufferR.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mDryBufferL.begin(), mDryBufferL.end(), 0.0f);
    std::fill(mDryBufferR.begin(), mDryBufferR.end(), 0.0f);

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
      mInputBufferL[i] = static_cast<NAM_SAMPLE>(inL * inputGainF);
      mInputBufferR[i] = static_cast<NAM_SAMPLE>(inR * inputGainF);
    }

    if (mModelLeft && mModelRight && mEnabled)
    {
      const float wetMix = static_cast<float>(mMix);
      const float dryMix = 1.0f - wetMix;
      const bool wetOnly = wetMix >= 0.9999f;
      const bool toneNeutral = IsToneStackNeutral();

      ProcessModels(numSamples);

      const float outputGainF = static_cast<float>(mOutputGain);

      if (toneNeutral && wetOnly && NearlyEqual(mOutputGain, 1.0))
      {
        for (int i = 0; i < numSamples; ++i)
        {
          if (outputs[0])
            outputs[0][i] = static_cast<float>(mOutputBufferL[i]);
          if (outputs[1])
            outputs[1][i] = static_cast<float>(mOutputBufferR[i]);
        }
        return;
      }

      if (toneNeutral)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          float outL = static_cast<float>(mOutputBufferL[i]) * outputGainF;
          float outR = static_cast<float>(mOutputBufferR[i]) * outputGainF;
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
        float outL = static_cast<float>(mOutputBufferL[i]);
        outL = mBassFilter[0].Process(outL);
        outL = mMidFilter[0].Process(outL);
        outL = mTrebleFilter[0].Process(outL);
        outL = mPresenceFilter[0].Process(outL);
        outL *= outputGainF;
        if (!wetOnly)
          outL = mDryBufferL[i] * dryMix + outL * wetMix;

        float outR = static_cast<float>(mOutputBufferR[i]);
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
        const float outL = static_cast<float>(mInputBufferL[i]);
        const float outR = static_cast<float>(mInputBufferR[i]);
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
      mInputBufferL[i] = static_cast<NAM_SAMPLE>(in * inputGainF);
    }

    if (mModelLeft && mEnabled)
    {
      const float wetMix = static_cast<float>(mMix);
      const float dryMix = 1.0f - wetMix;
      const bool wetOnly = wetMix >= 0.9999f;
      const bool toneNeutral = IsToneStackNeutral();

      ProcessModelMono(numSamples);

      const float outputGainF = static_cast<float>(mOutputGain);

      if (toneNeutral && wetOnly && NearlyEqual(mOutputGain, 1.0))
      {
        for (int i = 0; i < numSamples; ++i)
          output[i] = static_cast<float>(mOutputBufferL[i]);
        return;
      }

      if (toneNeutral)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          float out = static_cast<float>(mOutputBufferL[i]) * outputGainF;
          if (!wetOnly)
            out = mDryBufferL[i] * dryMix + out * wetMix;
          output[i] = out;
        }
        return;
      }

      for (int i = 0; i < numSamples; ++i)
      {
        float out = static_cast<float>(mOutputBufferL[i]);
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
        output[i] = static_cast<float>(mInputBufferL[i]);
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
    else if (key == "clampAutoGain")
    {
      mClampAutoGain = value > 0.5;
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
      ApplyGlobalNamSlimmableSize(mModelLeft.get());
      ApplyGlobalNamSlimmableSize(mModelRight.get());
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
    if (key == "autoLevelOutput")
      return mAutoLevelOutput ? 1.0 : 0.0;
    if (key == "clampAutoGain")
      return mClampAutoGain ? 1.0 : 0.0;
    if (key == "useNamInputMetadata")
      return mUseNamInputMetadata ? 1.0 : 0.0;
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
    return mModelLeft != nullptr && mModelRight != nullptr;
  }

  [[nodiscard]] std::filesystem::path GetResourcePath() const override
  {
    return mModelPath;
  }

  [[nodiscard]] std::string GetType() const override { return "amp_nam_optimized"; }
  [[nodiscard]] std::string GetCategory() const override { return "amp"; }

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
      if (!std::filesystem::exists(resourcePath))
      {
        std::cerr << "[OptimizedNAMAmpEffect] ERROR: Model file not found: " << resourcePath << "\n";
        return false;
      }

      auto modelLeft = ::nam::get_dsp(resourcePath);
      auto modelRight = ::nam::get_dsp(resourcePath);
      if (!modelLeft || !modelRight)
      {
        std::cerr << "[OptimizedNAMAmpEffect] ERROR: Failed to parse NAM model file: " << resourcePath << "\n";
        return false;
      }

      ApplyGlobalNamSlimmableSize(modelLeft.get());
      ApplyGlobalNamSlimmableSize(modelRight.get());

      mModelLeft = std::move(modelLeft);
      mModelRight = std::move(modelRight);
      mModelPath = resourcePath;
      mResourceNormalizationGainDb = ReadResourceMetadataDouble(ref, "normalizationGainDb");
      ConfigureModelProcessing();

      mModelInputLevel = mModelLeft->HasInputLevel()
        ? std::optional<double>(mModelLeft->GetInputLevel()) : std::nullopt;
      mModelOutputLevel = mModelLeft->HasOutputLevel()
        ? std::optional<double>(mModelLeft->GetOutputLevel()) : std::nullopt;
      mModelLoudness = mModelLeft->HasLoudness()
        ? std::optional<double>(mModelLeft->GetLoudness()) : std::nullopt;

      RecalculateAutoGains();
      return true;
    }
    catch (const std::exception &e)
    {
      std::cerr << "[OptimizedNAMAmpEffect] ERROR: Exception loading model " << resourcePath << ": " << e.what() << "\n";
      return false;
    }
    catch (...)
    {
      std::cerr << "[OptimizedNAMAmpEffect] ERROR: Unknown exception loading model " << resourcePath << "\n";
      return false;
    }
  }

  std::unique_ptr<::nam::DSP> mModelLeft;
  std::unique_ptr<::nam::DSP> mModelRight;

  std::filesystem::path mModelPath;
  bool mResamplingActive = false;
  double mModelSampleRate = 44100.0;
  int mMaxModelBlockSize = 512;

  // Host-domain buffers (NAM_SAMPLE). Input holds post-input-gain signal;
  // output holds model output. When no resampling is needed the model
  // processes these in place.
  std::vector<NAM_SAMPLE> mInputBufferL;
  std::vector<NAM_SAMPLE> mInputBufferR;
  std::vector<NAM_SAMPLE> mOutputBufferL;
  std::vector<NAM_SAMPLE> mOutputBufferR;
  // Model-domain buffers, used only when runtime resampling is active.
  std::vector<NAM_SAMPLE> mModelInputBufferL;
  std::vector<NAM_SAMPLE> mModelInputBufferR;
  std::vector<NAM_SAMPLE> mModelOutputBufferL;
  std::vector<NAM_SAMPLE> mModelOutputBufferR;
  // Dry signal kept as float for the tone/mix stage.
  std::vector<float> mDryBufferL;
  std::vector<float> mDryBufferR;

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
  bool mUseNamInputMetadata = true;
  bool mClampAutoGain = true;
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

    // Input calibration: mirrors NeuralAmpModelerPlugin _SetInputGain().
    // delta = calibrationInputLevel(dBu) - model.inputLevel(dBu)
    // calibrationInputLevel is only provided for the first NAM in a chain (the
    // one whose input is a raw guitar signal at a known interface level).
    // Without it we cannot safely compute a correction — using 0 dBu as the
    // reference would give a -18 dB hit on a model trained at +18 dBu. Skip
    // the correction entirely if the interface level is unknown, matching the
    // reference plugin's behaviour when "CalibrateInput" is disabled.
    if (mAutoLevelInput && mUseNamInputMetadata
        && mModelInputLevel.has_value() && mCalibrationInputLevel.has_value())
    {
      const double raw = *mCalibrationInputLevel - *mModelInputLevel;
      const double deltaDb = mClampAutoGain ? std::clamp(raw, -24.0, 24.0) : raw;
      mAutoInputGain = std::pow(10.0, deltaDb / 20.0);
    }

    if (mAutoLevelOutput)
    {
      if (mResourceNormalizationGainDb.has_value())
      {
        // Library metadata override takes highest priority.
        const double raw = *mResourceNormalizationGainDb;
        const double deltaDb = mClampAutoGain ? std::clamp(raw, -24.0, 24.0) : raw;
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
      else if (mModelOutputLevel.has_value() && mCalibrationInputLevel.has_value())
      {
        // "Calibrated" output mode: mirrors NeuralAmpModelerPlugin _SetOutputGain() case 2.
        // gainDB += model.outputLevel(dBu) - calibrationInputLevel(dBu)
        // Only active when the interface calibration level is known (first NAM in chain).
        const double raw = *mModelOutputLevel - *mCalibrationInputLevel;
        const double deltaDb = mClampAutoGain ? std::clamp(raw, -24.0, 24.0) : raw;
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
      else if (mModelLoudness.has_value())
      {
        // "Normalized" output mode: mirrors NeuralAmpModelerPlugin _SetOutputGain() case 1.
        // gainDB += targetLoudness - model.loudness
        const double raw = GetNominalOperatingLevelDbfs() - *mModelLoudness;
        const double deltaDb = mClampAutoGain ? std::clamp(raw, -24.0, 24.0) : raw;
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
    if (!mModelLeft)
      return mSampleRate;

    const double expectedSR = mModelLeft->GetExpectedSampleRate();
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

    mInputResampler.Prepare(mSampleRate, mModelSampleRate, mMaxBlockSize, SampleRateConversionQuality::HighPerformance);
    mOutputResampler.Prepare(mModelSampleRate, mSampleRate, mMaxModelBlockSize, SampleRateConversionQuality::HighPerformance);

    if (mModelLeft)
      mModelLeft->Reset(mModelSampleRate, mMaxModelBlockSize);
    if (mModelRight)
      mModelRight->Reset(mModelSampleRate, mMaxModelBlockSize);
  }

  [[nodiscard]] int GetModelFrameCount(int numSamples) const
  {
    int modelFrames = BlockSincResampler::ComputeOutputFrameCount(numSamples, mSampleRate, mModelSampleRate);
    return std::clamp(modelFrames, 1, mMaxModelBlockSize);
  }

  void ProcessModels(int numSamples)
  {
    auto processLeft = [&](int frames)
    {
      NAM_SAMPLE* in = mResamplingActive ? mModelInputBufferL.data() : mInputBufferL.data();
      NAM_SAMPLE* out = mResamplingActive ? mModelOutputBufferL.data() : mOutputBufferL.data();
      NAM_SAMPLE* inputPtrs[1] = { in };
      NAM_SAMPLE* outputPtrs[1] = { out };
      mModelLeft->process(inputPtrs, outputPtrs, frames);
    };

    auto processRight = [&](int frames)
    {
      NAM_SAMPLE* in = mResamplingActive ? mModelInputBufferR.data() : mInputBufferR.data();
      NAM_SAMPLE* out = mResamplingActive ? mModelOutputBufferR.data() : mOutputBufferR.data();
      NAM_SAMPLE* inputPtrs[1] = { in };
      NAM_SAMPLE* outputPtrs[1] = { out };
      mModelRight->process(inputPtrs, outputPtrs, frames);
    };

    if (!mResamplingActive)
    {
      bool ranParallel = false;
      if (rtparallel::ShouldParallelizeStereoWork(numSamples))
      {
        ranParallel = rtparallel::DualLaneExecutor::Instance().Run(
          [&]() { processRight(numSamples); },
          [&]() { processLeft(numSamples); });
      }
      if (!ranParallel)
      {
        processLeft(numSamples);
        processRight(numSamples);
      }
      return;
    }

    const int modelFrames = GetModelFrameCount(numSamples);
    mInputResampler.ProcessFixedOutput(mInputBufferL.data(), numSamples, mModelInputBufferL.data(), modelFrames);
    mInputResampler.ProcessFixedOutput(mInputBufferR.data(), numSamples, mModelInputBufferR.data(), modelFrames);

    bool ranParallel = false;
    if (rtparallel::ShouldParallelizeStereoWork(modelFrames))
    {
      ranParallel = rtparallel::DualLaneExecutor::Instance().Run(
        [&]() { processRight(modelFrames); },
        [&]() { processLeft(modelFrames); });
    }
    if (!ranParallel)
    {
      processLeft(modelFrames);
      processRight(modelFrames);
    }

    mOutputResampler.ProcessFixedOutput(mModelOutputBufferL.data(), modelFrames, mOutputBufferL.data(), numSamples);
    mOutputResampler.ProcessFixedOutput(mModelOutputBufferR.data(), modelFrames, mOutputBufferR.data(), numSamples);
  }

  void ProcessModelMono(int numSamples)
  {
    if (!mResamplingActive)
    {
      NAM_SAMPLE* inputPtrs[1] = { mInputBufferL.data() };
      NAM_SAMPLE* outputPtrs[1] = { mOutputBufferL.data() };
      mModelLeft->process(inputPtrs, outputPtrs, numSamples);
      return;
    }

    const int modelFrames = GetModelFrameCount(numSamples);
    mInputResampler.ProcessFixedOutput(mInputBufferL.data(), numSamples, mModelInputBufferL.data(), modelFrames);

    NAM_SAMPLE* inputPtrs[1] = { mModelInputBufferL.data() };
    NAM_SAMPLE* outputPtrs[1] = { mModelOutputBufferL.data() };
    mModelLeft->process(inputPtrs, outputPtrs, modelFrames);

    mOutputResampler.ProcessFixedOutput(mModelOutputBufferL.data(), modelFrames, mOutputBufferL.data(), numSamples);
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
  // Ensure NAM model factories are linked in (prevents dead-stripping).
  ::nam::factory::ForceFactoryRegistration();

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
    {"bass",                  "Bass",               0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"mid",                   "Mid",                0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"treble",                "Treble",             0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"presence",              "Presence",           0.0,   -10.0, 10.0,  "dB",  "Tone"},
    {"outputGain",            "Output",             0.0,   -24.0, 24.0,  "dB",  "Level"},
    {"mix",                   "Mix",                1.0,    0.0,   1.0,  "amount", "Advanced", true}
  };

  EffectRegistry::Instance().Register(info.type, info, []()
  {
    return std::make_unique<OptimizedNAMAmpEffect>();
  });
}

} // namespace guitarfx
