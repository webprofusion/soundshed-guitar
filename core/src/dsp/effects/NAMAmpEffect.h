#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/BlockSincResampler.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/NAMSampleRate.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#include "dsp/simd/SimdMath.h"
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

      mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mDryBufferL.resize(static_cast<size_t>(maxBlockSize));
      mDryBufferR.resize(static_cast<size_t>(maxBlockSize));

      ConfigureModelProcessing();
    }

    void Reset() override
    {
      if (mModelLeft)
      {
        mModelLeft->Reset(mModelSampleRate, mMaxModelBlockSize);
      }
      if (mModelRight)
      {
        mModelRight->Reset(mModelSampleRate, mMaxModelBlockSize);
      }

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
    }

    void Process(float **inputs, float **outputs, int numSamples) override
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

      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : inL;
        mDryBufferL[i] = inL;
        mDryBufferR[i] = inR;
        mInputBufferL[i] = static_cast<NAM_SAMPLE>(inL * static_cast<float>(mInputGain));
        mInputBufferR[i] = static_cast<NAM_SAMPLE>(inR * static_cast<float>(mInputGain));
      }

      if (mModelLeft && mModelRight && mEnabled)
      {
        const float wetMix = static_cast<float>(mMix);
        const float dryMix = 1.0f - wetMix;

        ProcessModels(numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
          const float wetL = static_cast<float>(mOutputBufferL[i]) * static_cast<float>(mOutputGain);
          const float wetR = static_cast<float>(mOutputBufferR[i]) * static_cast<float>(mOutputGain);
          const float outL = mDryBufferL[i] * dryMix + wetL * wetMix;
          const float outR = mDryBufferR[i] * dryMix + wetR * wetMix;
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
      else if (key == "slimmableSize")
      {
        if (const auto parsed = ParseDouble(value); parsed.has_value())
          SetGlobalNamSlimmableSize(*parsed);
        ApplyGlobalNamSlimmableSize(mModelLeft.get());
        ApplyGlobalNamSlimmableSize(mModelRight.get());
      }
    }

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "inputGain")
        return 20.0 * std::log10(mUserInputGain);
      if (key == "outputGain")
        return 20.0 * std::log10(mUserOutputGain);
      if (key == "mix")
        return mMix;
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

    bool LoadResource(const std::filesystem::path &resourcePath) override
    {
      return LoadModelResource(resourcePath, nullptr);
    }

    [[nodiscard]] bool HasResource() const override { return mModelLeft != nullptr && mModelRight != nullptr; }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mModelPath; }

    [[nodiscard]] std::string GetType() const override { return "amp_nam"; }
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

    bool LoadModelResource(const std::filesystem::path &resourcePath, const ResourceRef *ref)
    {
      try
      {
        if (!std::filesystem::exists(resourcePath))
        {
          std::cerr << "[NAMAmpEffect] ERROR: Model file not found: " << resourcePath << "\n";
          return false;
        }

        auto modelLeft = nam::get_dsp(resourcePath);
        auto modelRight = nam::get_dsp(resourcePath);
        if (!modelLeft || !modelRight)
        {
          std::cerr << "[NAMAmpEffect] ERROR: Failed to parse NAM model file: " << resourcePath << "\n";
          return false;
        }

        ApplyGlobalNamSlimmableSize(modelLeft.get());
        ApplyGlobalNamSlimmableSize(modelRight.get());

        mModelLeft = std::move(modelLeft);
        mModelRight = std::move(modelRight);
        mModelPath = resourcePath;
        mResourceNormalizationGainDb = ReadResourceMetadataDouble(ref, "normalizationGainDb");
        ConfigureModelProcessing();

        if (mModelLeft)
        {
          mModelInputLevel = mModelLeft->HasInputLevel() ? std::optional<double>(mModelLeft->GetInputLevel()) : std::nullopt;
          mModelOutputLevel = mModelLeft->HasOutputLevel() ? std::optional<double>(mModelLeft->GetOutputLevel()) : std::nullopt;
          mModelLoudness = mModelLeft->HasLoudness() ? std::optional<double>(mModelLeft->GetLoudness()) : std::nullopt;
          RecalculateAutoGains();
        }
        return true;
      }
      catch (const std::exception &e)
      {
        std::cerr << "[NAMAmpEffect] ERROR: Exception loading model " << resourcePath << ": " << e.what() << "\n";
        return false;
      }
      catch (...)
      {
        std::cerr << "[NAMAmpEffect] ERROR: Unknown exception loading model " << resourcePath << "\n";
        return false;
      }
    }

    std::unique_ptr<nam::DSP> mModelLeft;
    std::unique_ptr<nam::DSP> mModelRight;
    std::filesystem::path mModelPath;
    bool mResamplingActive = false;
    double mModelSampleRate = 44100.0;
    int mMaxModelBlockSize = 512;

    std::vector<NAM_SAMPLE> mInputBufferL;
    std::vector<NAM_SAMPLE> mInputBufferR;
    std::vector<NAM_SAMPLE> mOutputBufferL;
    std::vector<NAM_SAMPLE> mOutputBufferR;
    std::vector<NAM_SAMPLE> mModelInputBufferL;
    std::vector<NAM_SAMPLE> mModelInputBufferR;
    std::vector<NAM_SAMPLE> mModelOutputBufferL;
    std::vector<NAM_SAMPLE> mModelOutputBufferR;
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
    std::optional<double> mModelInputLevel;
    std::optional<double> mModelOutputLevel;
    std::optional<double> mModelLoudness;
    std::optional<double> mResourceNormalizationGainDb;
    std::optional<double> mCalibrationInputLevel;
    std::optional<double> mCalibrationOutputLevel;
    bool mEnabled = true;
    std::uint64_t mLevelTargetsRevision = 0;

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

    void ProcessModels(int numSamples)
    {
      if (!mResamplingActive)
      {
        NAM_SAMPLE* inputPtrL = mInputBufferL.data();
        NAM_SAMPLE* outputPtrL = mOutputBufferL.data();
        NAM_SAMPLE* inputPtrsL[1] = { inputPtrL };
        NAM_SAMPLE* outputPtrsL[1] = { outputPtrL };
        mModelLeft->process(inputPtrsL, outputPtrsL, numSamples);

        NAM_SAMPLE* inputPtrR = mInputBufferR.data();
        NAM_SAMPLE* outputPtrR = mOutputBufferR.data();
        NAM_SAMPLE* inputPtrsR[1] = { inputPtrR };
        NAM_SAMPLE* outputPtrsR[1] = { outputPtrR };
        mModelRight->process(inputPtrsR, outputPtrsR, numSamples);
        return;
      }

      int modelFrames = BlockSincResampler::ComputeOutputFrameCount(numSamples, mSampleRate, mModelSampleRate);
      modelFrames = std::clamp(modelFrames, 1, mMaxModelBlockSize);

      mInputResampler.ProcessFixedOutput(mInputBufferL.data(), numSamples, mModelInputBufferL.data(), modelFrames);
      mInputResampler.ProcessFixedOutput(mInputBufferR.data(), numSamples, mModelInputBufferR.data(), modelFrames);

      NAM_SAMPLE* inputPtrL = mModelInputBufferL.data();
      NAM_SAMPLE* outputPtrL = mModelOutputBufferL.data();
      NAM_SAMPLE* inputPtrsL[1] = { inputPtrL };
      NAM_SAMPLE* outputPtrsL[1] = { outputPtrL };
      mModelLeft->process(inputPtrsL, outputPtrsL, modelFrames);

      NAM_SAMPLE* inputPtrR = mModelInputBufferR.data();
      NAM_SAMPLE* outputPtrR = mModelOutputBufferR.data();
      NAM_SAMPLE* inputPtrsR[1] = { inputPtrR };
      NAM_SAMPLE* outputPtrsR[1] = { outputPtrR };
      mModelRight->process(inputPtrsR, outputPtrsR, modelFrames);

      mOutputResampler.ProcessFixedOutput(mModelOutputBufferL.data(), modelFrames, mOutputBufferL.data(), numSamples);
      mOutputResampler.ProcessFixedOutput(mModelOutputBufferR.data(), modelFrames, mOutputBufferR.data(), numSamples);
    }

    static bool ParseBool(const std::string &value)
    {
      return value == "1" || value == "true" || value == "True" || value == "TRUE";
    }

    static std::optional<double> ParseDouble(const std::string &value)
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

  inline void RegisterNAMAmpEffect()
  {

    // Ensure NAM factory registrations are not optimized out by the linker
    nam::factory::ForceFactoryRegistration();

    EffectTypeInfo info;
    info.type = EffectGuids::kAmpNam;
    info.aliases = {"amp_nam"};
    info.displayName = "Neural Amp";
    info.category = "amp";
    info.description = "Neural Amp Modeler - AI-trained amp simulation";
    info.requiresResource = true;
    info.resourceType = "nam"; // .nam model files
    info.parameters = {
      {"inputGain",             "Input",              0.0,   -24.0, 24.0,  "dB"},
      {"outputGain",            "Output",             0.0,   -24.0, 24.0,  "dB"},
      {"mix",                   "Mix",                1.0,    0.0,   1.0,  "amount", "Advanced", true},
      {"autoLevelOutput",       "Auto Level Output",  1.0,    0.0,   1.0,  "toggle", "Advanced", true}};

    EffectRegistry::Instance().Register(info.type, info, []()
                                        { return std::make_unique<NAMAmpEffect>(); });
  }

} // namespace guitarfx
