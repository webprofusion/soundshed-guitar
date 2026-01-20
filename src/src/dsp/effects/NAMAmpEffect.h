#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/simd/SimdMath.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <filesystem>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cmath>

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
  /**
   * Neural Amp Modeler effect - wrapper around NAM DSP library.
   * Requires a .nam model file to be loaded as a resource.
   */
  class NAMAmpEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      mInputBuffer.resize(static_cast<size_t>(maxBlockSize));
      mOutputBuffer.resize(static_cast<size_t>(maxBlockSize));

      if (mModel)
      {
        mModel->Reset(sampleRate, maxBlockSize);
      }
    }

    void Reset() override
    {
      if (mModel)
      {
        mModel->Reset(mSampleRate, mMaxBlockSize);
      }

      std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
      std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!inputs[0] && !inputs[1])
      {
        if (outputs[0])
          std::fill_n(outputs[0], numSamples, 0.0f);
        if (outputs[1])
          std::fill_n(outputs[1], numSamples, 0.0f);
        return;
      }

      // Sum to mono for NAM processing (NAM models are mono)
      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : inL;
        mInputBuffer[i] = (inL + inR) * 0.5f * static_cast<float>(mInputGain);
      }

      if (mModel && mEnabled)
      {
        NAM_SAMPLE* inputPtr = mInputBuffer.data();
        NAM_SAMPLE* outputPtr = mOutputBuffer.data();
        NAM_SAMPLE* inputPtrs[1] = { inputPtr };
        NAM_SAMPLE* outputPtrs[1] = { outputPtr };
        mModel->process(inputPtrs, outputPtrs, numSamples);

        // Output to both channels
        for (int i = 0; i < numSamples; ++i)
        {
          float out = mOutputBuffer[i] * static_cast<float>(mOutputGain);
          if (outputs[0])
            outputs[0][i] = out;
          if (outputs[1])
            outputs[1][i] = out;
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

    void SetParam(const std::string &key, double value) override
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
      else if (key == "enabled")
      {
        mEnabled = value > 0.5;
      }
    }

    void SetConfig(const std::string &key, const std::string &value) override
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
    }

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "inputGain")
        return 20.0 * std::log10(mUserInputGain);
      if (key == "outputGain")
        return 20.0 * std::log10(mUserOutputGain);
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      try
      {
        auto model = nam::get_dsp(resourcePath);
        if (!model)
          return false;

        model->Reset(mSampleRate, mMaxBlockSize);
        mModel = std::move(model);
        mModelPath = resourcePath;

        if (mModel)
        {
          mModelInputLevel = mModel->HasInputLevel() ? std::optional<double>(mModel->GetInputLevel()) : std::nullopt;
          mModelOutputLevel = mModel->HasOutputLevel() ? std::optional<double>(mModel->GetOutputLevel()) : std::nullopt;
          mModelLoudness = mModel->HasLoudness() ? std::optional<double>(mModel->GetLoudness()) : std::nullopt;
          RecalculateAutoGains();
        }
        return true;
      }
      catch (...)
      {
        return false;
      }
    }

    [[nodiscard]] bool HasResource() const override { return mModel != nullptr; }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mModelPath; }

    [[nodiscard]] std::string GetType() const override { return "amp_nam"; }
    [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  private:
    std::unique_ptr<nam::DSP> mModel;
    std::filesystem::path mModelPath;

    std::vector<NAM_SAMPLE> mInputBuffer;
    std::vector<NAM_SAMPLE> mOutputBuffer;

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
    bool mEnabled = true;

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

      const auto inputLevel = mCalibrationInputLevel.has_value() ? mCalibrationInputLevel : mModelInputLevel;
      const auto outputLevel = mCalibrationOutputLevel.has_value() ? mCalibrationOutputLevel : mModelOutputLevel;

      if (mAutoLevelInput && inputLevel.has_value())
      {
        const double deltaDb = std::clamp(kTargetInputLeveldBu - *inputLevel, -24.0, 24.0);
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

    static bool ParseBool(const std::string &value)
    {
      return value == "1" || value == "true" || value == "True" || value == "TRUE";
    }
  };

  inline void RegisterNAMAmpEffect()
  {

    // Ensure NAM factory registrations are not optimized out by the linker
    nam::factory::ForceFactoryRegistration();

    EffectTypeInfo info;
    info.type = "amp_nam";
    info.displayName = "Neural Amp Model";
    info.category = "amp";
    info.description = "Neural Amp Modeler - AI-trained amp simulation";
    info.requiresResource = true;
    info.resourceType = "nam"; // .nam model files
    info.parameters = {
      {"inputGain", "Input Gain", 0.0, -24.0, 24.0, "dB"},
      {"outputGain", "Output Gain", 0.0, -24.0, 24.0, "dB"},
      {"autoLevelInput", "Auto Level Input", 1.0, 0.0, 1.0, ""},
      {"autoLevelOutput", "Auto Level Output", 1.0, 0.0, 1.0, ""},
      {"calibrationInputLevel", "Calibration Input", -18.0, -60.0, 24.0, "dB"},
      {"calibrationOutputLevel", "Calibration Output", -18.0, -60.0, 24.0, "dB"}};

    EffectRegistry::Instance().Register("amp_nam", info, []()
                                        { return std::make_unique<NAMAmpEffect>(); });
  }

} // namespace guitarfx
