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
#include "dsp/EffectRegistry.h"
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

    mInputBuffer.resize(static_cast<size_t>(maxBlockSize));
    mOutputBuffer.resize(static_cast<size_t>(maxBlockSize));
    mFallbackInputBuffer.resize(static_cast<size_t>(maxBlockSize));
    mFallbackOutputBuffer.resize(static_cast<size_t>(maxBlockSize));

    if (mOptimizedModel)
    {
      mOptimizedModel->Reset(sampleRate, maxBlockSize);
    }
    else if (mFallbackModel)
    {
      mFallbackModel->Reset(sampleRate, maxBlockSize);
    }
    CheckSampleRateMismatch();
    UpdateToneStack();
  }

  void Reset() override
  {
    if (mOptimizedModel)
    {
      mOptimizedModel->Reset(mSampleRate, mMaxBlockSize);
    }
    else if (mFallbackModel)
    {
      mFallbackModel->Reset(mSampleRate, mMaxBlockSize);
    }

    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    std::fill(mFallbackInputBuffer.begin(), mFallbackInputBuffer.end(), static_cast<NAM_SAMPLE>(0.0));
    std::fill(mFallbackOutputBuffer.begin(), mFallbackOutputBuffer.end(), static_cast<NAM_SAMPLE>(0.0));

    mBassFilter.Reset();
    mMidFilter.Reset();
    mTrebleFilter.Reset();
    mPresenceFilter.Reset();
  }

  void Process(float** inputs, float** outputs, int numSamples) override
  {
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

    // Sum to mono for NAM processing (NAM models are mono)
    const float inputGainF = static_cast<float>(mInputGain);
    for (int i = 0; i < numSamples; ++i)
    {
      float inL = inputs[0] ? inputs[0][i] : 0.0f;
      float inR = inputs[1] ? inputs[1][i] : inL;
      mInputBuffer[i] = (inL + inR) * 0.5f * inputGainF;
    }

    bool hasModel = mOptimizedModel || mFallbackModel;

    if (hasModel && mEnabled)
    {
      // Use optimized path if available, otherwise fallback
      if (mOptimizedModel)
      {
        mOptimizedModel->process(mInputBuffer.data(), mOutputBuffer.data(), numSamples);

        // Apply tone stack EQ, then output to both channels
        const float outputGainF = static_cast<float>(mOutputGain);
        for (int i = 0; i < numSamples; ++i)
        {
          float out = mOutputBuffer[i];
          out = mBassFilter.Process(out);
          out = mMidFilter.Process(out);
          out = mTrebleFilter.Process(out);
          out = mPresenceFilter.Process(out);
          out *= outputGainF;
          if (outputs[0])
            outputs[0][i] = out;
          if (outputs[1])
            outputs[1][i] = out;
        }
      }
      else
      {
        // Fallback uses NAM_SAMPLE (double) buffers
        for (int i = 0; i < numSamples; ++i)
        {
          mFallbackInputBuffer[i] = static_cast<NAM_SAMPLE>(mInputBuffer[i]);
        }

        NAM_SAMPLE* inputPtr = mFallbackInputBuffer.data();
        NAM_SAMPLE* outputPtr = mFallbackOutputBuffer.data();
        NAM_SAMPLE* inputPtrs[1] = { inputPtr };
        NAM_SAMPLE* outputPtrs[1] = { outputPtr };
        mFallbackModel->process(inputPtrs, outputPtrs, numSamples);

        // Apply tone stack EQ, then output to both channels
        const float outputGainF = static_cast<float>(mOutputGain);
        for (int i = 0; i < numSamples; ++i)
        {
          float out = static_cast<float>(mFallbackOutputBuffer[i]);
          out = mBassFilter.Process(out);
          out = mMidFilter.Process(out);
          out = mTrebleFilter.Process(out);
          out = mPresenceFilter.Process(out);
          out *= outputGainF;
          if (outputs[0])
            outputs[0][i] = out;
          if (outputs[1])
            outputs[1][i] = out;
        }
      }
    }
    else
    {
      // Bypass - pass through mono signal
      for (int i = 0; i < numSamples; ++i)
      {
        float out = mInputBuffer[i];
        if (outputs[0])
          outputs[0][i] = out;
        if (outputs[1])
          outputs[1][i] = out;
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
      mCalibrationInputLevel = value;
      RecalculateAutoGains();
    }
    else if (key == "calibrationOutputLevel")
    {
      mCalibrationOutputLevel = value;
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
    else if (key == "interfaceCalibrationEnabled")
    {
      mInterfaceCalibrationEnabled = ParseBool(value);
      RecalculateAutoGains();
    }
    else if (key == "interfaceCalibrationReferenceDbu")
    {
      if (auto parsed = ParseDouble(value))
      {
        mInterfaceReferenceDbu = *parsed;
        RecalculateAutoGains();
      }
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

  bool LoadResource(const std::filesystem::path& resourcePath) override
  {
    try
    {
      // Reset existing models
      mOptimizedModel.reset();
      mFallbackModel.reset();

      bool loaded = false;

      // Try optimized path first if preferred
      if (mPreferOptimized)
      {
        auto optimized = nam::LoadOptimizedModelWrapper(resourcePath);
        if (optimized && optimized->IsValid())
        {
          mOptimizedModel = std::move(optimized);
          mOptimizedModel->Reset(mSampleRate, mMaxBlockSize);
          loaded = true;
          mUsingOptimized = true;
        }
      }

      // Fall back to original NAM library
      if (!loaded)
      {
        auto model = ::nam::get_dsp(resourcePath);
        if (!model)
          return false;

        model->Reset(mSampleRate, mMaxBlockSize);
        mFallbackModel = std::move(model);
        loaded = true;
        mUsingOptimized = false;
      }

      if (!loaded)
        return false;

      mModelPath = resourcePath;
      CheckSampleRateMismatch();

      // Extract metadata
      if (mOptimizedModel)
      {
        mModelInputLevel = mOptimizedModel->HasInputLevel()
          ? std::optional<double>(mOptimizedModel->GetInputLevel())
          : std::nullopt;
        mModelOutputLevel = mOptimizedModel->HasOutputLevel()
          ? std::optional<double>(mOptimizedModel->GetOutputLevel())
          : std::nullopt;
        mModelLoudness = mOptimizedModel->HasLoudness()
          ? std::optional<double>(mOptimizedModel->GetLoudness())
          : std::nullopt;
      }
      else if (mFallbackModel)
      {
        mModelInputLevel = mFallbackModel->HasInputLevel()
          ? std::optional<double>(mFallbackModel->GetInputLevel())
          : std::nullopt;
        mModelOutputLevel = mFallbackModel->HasOutputLevel()
          ? std::optional<double>(mFallbackModel->GetOutputLevel())
          : std::nullopt;
        mModelLoudness = mFallbackModel->HasLoudness()
          ? std::optional<double>(mFallbackModel->GetLoudness())
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

  [[nodiscard]] bool HasResource() const override
  {
    return mOptimizedModel || mFallbackModel;
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
  // Model storage - one or the other will be used
  std::unique_ptr<nam::OptimizedDSPWrapper> mOptimizedModel;
  std::unique_ptr<::nam::DSP> mFallbackModel;

  std::filesystem::path mModelPath;
  bool mUsingOptimized = false;
  bool mPreferOptimized = true;
  bool mSampleRateMismatch = false;  // Default to preferring optimized

  std::vector<float> mInputBuffer;
  std::vector<float> mOutputBuffer;
  // Separate buffers for fallback model (uses NAM_SAMPLE which is double by default)
  std::vector<NAM_SAMPLE> mFallbackInputBuffer;
  std::vector<NAM_SAMPLE> mFallbackOutputBuffer;

  double mUserInputGain = 1.0;
  double mUserOutputGain = 1.0;
  double mAutoInputGain = 1.0;
  double mAutoOutputGain = 1.0;
  double mInputGain = 1.0;
  double mOutputGain = 1.0;
  bool mAutoLevelInput = true;
  bool mAutoLevelOutput = true;
  std::optional<double> mModelInputLevel;
  std::optional<double> mModelOutputLevel;
  std::optional<double> mModelLoudness;
  std::optional<double> mCalibrationInputLevel;
  std::optional<double> mCalibrationOutputLevel;
  bool mInterfaceCalibrationEnabled = true;
  double mInterfaceReferenceDbu = 12.0;
  bool mEnabled = true;

  // Tone stack EQ parameters (dB, -10..+10, default 0)
  double mBassDb     = 0.0; // Low shelf  ~100 Hz
  double mMidDb      = 0.0; // Peak EQ    ~500 Hz
  double mTrebleDb   = 0.0; // High shelf ~3200 Hz
  double mPresenceDb = 0.0; // Peak EQ    ~6300 Hz

  AmpToneBiquad mBassFilter;
  AmpToneBiquad mMidFilter;
  AmpToneBiquad mTrebleFilter;
  AmpToneBiquad mPresenceFilter;

  void UpdateToneStack()
  {
    if (mSampleRate <= 0.0)
      return;
    mBassFilter.SetLowShelf(100.0,  mBassDb,     mSampleRate);
    mMidFilter.SetPeaking  (500.0,  mMidDb, 1.0, mSampleRate);
    mTrebleFilter.SetHighShelf(3200.0, mTrebleDb, mSampleRate);
    mPresenceFilter.SetPeaking(6300.0, mPresenceDb, 1.5, mSampleRate);
  }

  void UpdateEffectiveGains()
  {
    mInputGain = mUserInputGain * mAutoInputGain;
    mOutputGain = mUserOutputGain * mAutoOutputGain;
  }

  void RecalculateAutoGains()
  {
    static constexpr double kTargetInputLeveldBu = -18.0;
    static constexpr double kTargetOutputLeveldB = -18.0;

    mAutoInputGain = 1.0;
    mAutoOutputGain = 1.0;

    const bool hasCalibrationInput = mCalibrationInputLevel.has_value();
    const auto inputLevel = hasCalibrationInput ? mCalibrationInputLevel : mModelInputLevel;
    const auto outputLevel = mCalibrationOutputLevel.has_value() ? mCalibrationOutputLevel : mModelOutputLevel;

    if (mAutoLevelInput && inputLevel.has_value())
    {
      double effectiveInputLevel = *inputLevel;
      if (!hasCalibrationInput && mInterfaceCalibrationEnabled && mModelInputLevel.has_value())
      {
        effectiveInputLevel = *mModelInputLevel - mInterfaceReferenceDbu;
      }
      const double deltaDb = std::clamp(kTargetInputLeveldBu - effectiveInputLevel, -24.0, 24.0);
      mAutoInputGain = std::pow(10.0, deltaDb / 20.0);
    }

    if (mAutoLevelOutput)
    {
      if (outputLevel.has_value())
      {
        const double deltaDb = std::clamp(kTargetOutputLeveldB - *outputLevel, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
      else if (mModelLoudness.has_value())
      {
        const double deltaDb = std::clamp(kTargetOutputLeveldB - *mModelLoudness, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
    }

    UpdateEffectiveGains();
  }

  void CheckSampleRateMismatch()
  {
    double expectedSR = -1.0;
    if (mOptimizedModel)
      expectedSR = mOptimizedModel->GetExpectedSampleRate();
    else if (mFallbackModel)
      expectedSR = mFallbackModel->GetExpectedSampleRate();

    const bool mismatch = (expectedSR > 0.0 && std::abs(expectedSR - mSampleRate) > 1.0);
    if (mismatch && !mSampleRateMismatch)
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
  info.type = "amp_nam_optimized";
  info.displayName = "Neural Amp (NAM)";
  info.category = "amp";
  info.description = "Neural Amp Modeler (NAM) with SIMD-optimized processing";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.resourceFilterHint = {"amp", "full-rig"};
  info.parameters = {
    {"inputGain",             "Input Gain",         0.0,   -24.0, 24.0,  "dB"},
    {"bass",                  "Bass",                0.0,   -10.0, 10.0,  "dB"},
    {"mid",                   "Mid",                 0.0,   -10.0, 10.0,  "dB"},
    {"treble",                "Treble",              0.0,   -10.0, 10.0,  "dB"},
    {"presence",              "Presence",            0.0,   -10.0, 10.0,  "dB"},
    {"outputGain",            "Output Gain",         0.0,   -24.0, 24.0,  "dB"},
    {"autoLevelInput",        "Auto Level Input",    1.0,    0.0,   1.0,  ""},
    {"autoLevelOutput",       "Auto Level Output",   1.0,    0.0,   1.0,  ""},
    {"calibrationInputLevel", "Calibration Input",  -18.0, -60.0, 24.0,  "dB"},
    {"calibrationOutputLevel","Calibration Output", -18.0, -60.0, 24.0,  "dB"}
  };

  EffectRegistry::Instance().Register("amp_nam_optimized", info, []()
  {
    return std::make_unique<OptimizedNAMAmpEffect>();
  });
}

} // namespace guitarfx
