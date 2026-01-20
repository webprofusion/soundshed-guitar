#pragma once

/**
 * Multi-model NAM blend effect.
 *
 * Loads multiple NAM models and blends between them based on a normalized blend
 * parameter (0..1). Intended for mapping a physical parameter (gain/warp/etc.)
 * to discrete captures, using audio mixing (primary) with optional experimental
 * weight interpolation in the future.
 */

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/simd/OptimizedNAM.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

class MultiModelNAMAmpEffect : public EffectProcessor
{
public:
  void Prepare(double sampleRate, int maxBlockSize) override
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    mInputBuffer.resize(static_cast<size_t>(maxBlockSize));

    for (auto& model : mModels)
    {
      ResizeModelBuffers(model, maxBlockSize);
      ResetModel(model, sampleRate, maxBlockSize);
    }
  }

  void Reset() override
  {
    for (auto& model : mModels)
    {
      ResetModel(model, mSampleRate, mMaxBlockSize);
    }

    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
    mCachedAutoInputGain = 1.0;
    mCachedAutoOutputGain = 1.0;
  }

  void Process(float** inputs, float** outputs, int numSamples) override
  {
    if (!inputs[0] && !inputs[1])
    {
      if (outputs[0])
        std::fill_n(outputs[0], numSamples, 0.0f);
      if (outputs[1])
        std::fill_n(outputs[1], numSamples, 0.0f);
      return;
    }

    // Sum to mono input
    for (int i = 0; i < numSamples; ++i)
    {
      float inL = inputs[0] ? inputs[0][i] : 0.0f;
      float inR = inputs[1] ? inputs[1][i] : inL;
      mInputBuffer[i] = (inL + inR) * 0.5f;
    }

    if (mModels.empty() || !mEnabled)
    {
      // Passthrough
      for (int i = 0; i < numSamples; ++i)
      {
        float out = mInputBuffer[i];
        if (outputs[0])
          outputs[0][i] = out;
        if (outputs[1])
          outputs[1][i] = out;
      }
      return;
    }

    const BlendSelection selection = SelectBlendModels();
    UpdateAutoGains(selection);

    const float inputGain = static_cast<float>(mInputGain);
    const float outputGain = static_cast<float>(mOutputGain);

    // Apply input gain
    for (int i = 0; i < numSamples; ++i)
    {
      mInputBuffer[i] *= inputGain;
    }

    if (selection.upperIndex == selection.lowerIndex)
    {
      auto& model = mModels[selection.lowerIndex];
      ProcessModel(model, mInputBuffer.data(), model.outputBuffer.data(), numSamples);
      WriteOutputs(model.outputBuffer.data(), outputs, numSamples, outputGain);
      return;
    }

    auto& modelA = mModels[selection.lowerIndex];
    auto& modelB = mModels[selection.upperIndex];

    ProcessModel(modelA, mInputBuffer.data(), modelA.outputBuffer.data(), numSamples);
    ProcessModel(modelB, mInputBuffer.data(), modelB.outputBuffer.data(), numSamples);

    const float weightA = static_cast<float>(selection.weightLower);
    const float weightB = static_cast<float>(selection.weightUpper);

    for (int i = 0; i < numSamples; ++i)
    {
      float mixed = modelA.outputBuffer[i] * weightA + modelB.outputBuffer[i] * weightB;
      mixed *= outputGain;
      if (outputs[0])
        outputs[0][i] = mixed;
      if (outputs[1])
        outputs[1][i] = mixed;
    }
  }

  void SetParam(const std::string& key, double value) override
  {
    if (key == "inputGain")
    {
      mUserInputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
    }
    else if (key == "outputGain")
    {
      mUserOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
    }
    else if (key == "blend" || key == "gain")
    {
      mBlend = std::clamp(value, 0.0, 1.0);
    }
    else if (key == "enabled")
    {
      mEnabled = value > 0.5;
    }

    UpdateEffectiveGains();
  }

  void SetConfig(const std::string& key, const std::string& value) override
  {
    if (key == "parameterId")
    {
      mParameterId = value;
    }
    else if (key == "blendMode")
    {
      mSnapBlend = (value == "snap");
    }
  }

  [[nodiscard]] double GetParam(const std::string& key) const override
  {
    if (key == "inputGain")
      return 20.0 * std::log10(mUserInputGain);
    if (key == "outputGain")
      return 20.0 * std::log10(mUserOutputGain);
    if (key == "blend" || key == "gain")
      return mBlend;
    if (key == "enabled")
      return mEnabled ? 1.0 : 0.0;
    return 0.0;
  }

  bool LoadResources(const std::vector<ResourceRef>& refs,
                     const std::vector<std::filesystem::path>& paths) override
  {
    mModels.clear();
    if (refs.empty() || paths.empty())
      return false;

    const std::size_t count = std::min(refs.size(), paths.size());
    mModels.reserve(count);

    for (std::size_t i = 0; i < count; ++i)
    {
      const auto& ref = refs[i];
      const auto& path = paths[i];

      if (!mParameterId.empty() && !ref.parameterId.empty() && ref.parameterId != mParameterId)
      {
        continue;
      }

      ModelInstance instance;
      instance.path = path;
      instance.parameterId = ref.parameterId;
      instance.parameterValue = ref.parameterValue.value_or(static_cast<double>(i));

      if (!LoadModelInstance(instance))
      {
        continue;
      }

      ResizeModelBuffers(instance, mMaxBlockSize);
      ResetModel(instance, mSampleRate, mMaxBlockSize);

      mModels.push_back(std::move(instance));
    }

    if (mModels.empty())
      return false;

    std::sort(mModels.begin(), mModels.end(), [](const ModelInstance& a, const ModelInstance& b) {
      return a.parameterValue < b.parameterValue;
    });

    UpdateEffectiveGains();
    return true;
  }

  [[nodiscard]] bool HasResource() const override { return !mModels.empty(); }

  [[nodiscard]] std::string GetType() const override { return "amp_nam_blend"; }
  [[nodiscard]] std::string GetCategory() const override { return "amp"; }

private:
  struct ModelInstance
  {
    std::filesystem::path path;
    std::string parameterId;
    double parameterValue = 0.0;

    std::unique_ptr<nam::OptimizedDSPWrapper> optimized;
    std::unique_ptr<::nam::DSP> fallback;
    bool usingOptimized = false;

    std::vector<float> outputBuffer;
    std::vector<NAM_SAMPLE> fallbackInput;
    std::vector<NAM_SAMPLE> fallbackOutput;

    std::optional<double> inputLevel;
    std::optional<double> outputLevel;
    std::optional<double> loudness;
  };

  struct BlendSelection
  {
    std::size_t lowerIndex = 0;
    std::size_t upperIndex = 0;
    double weightLower = 1.0;
    double weightUpper = 0.0;
  };

  std::vector<ModelInstance> mModels;
  std::vector<float> mInputBuffer;

  double mUserInputGain = 1.0;
  double mUserOutputGain = 1.0;
  double mAutoInputGain = 1.0;
  double mAutoOutputGain = 1.0;
  double mInputGain = 1.0;
  double mOutputGain = 1.0;
  double mBlend = 0.0;
  bool mAutoLevelInput = true;
  bool mAutoLevelOutput = true;
  bool mEnabled = true;
  std::string mParameterId;

  double mCachedAutoInputGain = 1.0;
  double mCachedAutoOutputGain = 1.0;
  std::optional<double> mCalibrationInputLevel;
  std::optional<double> mCalibrationOutputLevel;

  void UpdateEffectiveGains()
  {
    mInputGain = mUserInputGain * mAutoInputGain;
    mOutputGain = mUserOutputGain * mAutoOutputGain;
  }

  static bool ParseBool(const std::string& value)
  {
    return value == "1" || value == "true" || value == "True" || value == "TRUE";
  }

  bool LoadModelInstance(ModelInstance& instance)
  {
    try
    {
      instance.optimized = guitarfx::nam::LoadOptimizedModelWrapper(instance.path);
      if (instance.optimized && instance.optimized->IsValid())
      {
        instance.usingOptimized = true;
        instance.inputLevel = instance.optimized->HasInputLevel()
          ? std::optional<double>(instance.optimized->GetInputLevel()) : std::nullopt;
        instance.outputLevel = instance.optimized->HasOutputLevel()
          ? std::optional<double>(instance.optimized->GetOutputLevel()) : std::nullopt;
        instance.loudness = instance.optimized->HasLoudness()
          ? std::optional<double>(instance.optimized->GetLoudness()) : std::nullopt;
        return true;
      }

      instance.fallback = ::nam::get_dsp(instance.path);
      if (instance.fallback)
      {
        instance.usingOptimized = false;
        instance.inputLevel = instance.fallback->HasInputLevel()
          ? std::optional<double>(instance.fallback->GetInputLevel()) : std::nullopt;
        instance.outputLevel = instance.fallback->HasOutputLevel()
          ? std::optional<double>(instance.fallback->GetOutputLevel()) : std::nullopt;
        instance.loudness = instance.fallback->HasLoudness()
          ? std::optional<double>(instance.fallback->GetLoudness()) : std::nullopt;
        return true;
      }

      return false;
    }
    catch (...)
    {
      return false;
    }
  }

  void ResizeModelBuffers(ModelInstance& instance, int maxBlockSize)
  {
    instance.outputBuffer.resize(static_cast<size_t>(maxBlockSize));
    instance.fallbackInput.resize(static_cast<size_t>(maxBlockSize));
    instance.fallbackOutput.resize(static_cast<size_t>(maxBlockSize));
  }

  void ResetModel(ModelInstance& instance, double sampleRate, int maxBlockSize)
  {
    if (instance.usingOptimized && instance.optimized)
    {
      instance.optimized->Reset(sampleRate, maxBlockSize);
    }
    else if (instance.fallback)
    {
      instance.fallback->Reset(sampleRate, maxBlockSize);
    }
  }

  void ProcessModel(ModelInstance& instance, float* input, float* output, int numSamples)
  {
    if (instance.usingOptimized && instance.optimized)
    {
      instance.optimized->process(input, output, numSamples);
      return;
    }

    if (instance.fallback)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        instance.fallbackInput[i] = static_cast<NAM_SAMPLE>(input[i]);
      }
      NAM_SAMPLE* inputPtr = instance.fallbackInput.data();
      NAM_SAMPLE* outputPtr = instance.fallbackOutput.data();
      NAM_SAMPLE* inputPtrs[1] = { inputPtr };
      NAM_SAMPLE* outputPtrs[1] = { outputPtr };
      instance.fallback->process(inputPtrs, outputPtrs, numSamples);
      for (int i = 0; i < numSamples; ++i)
      {
        output[i] = static_cast<float>(instance.fallbackOutput[i]);
      }
      return;
    }

    std::fill_n(output, numSamples, 0.0f);
  }

  void WriteOutputs(const float* mono, float** outputs, int numSamples, float gain)
  {
    for (int i = 0; i < numSamples; ++i)
    {
      float out = mono[i] * gain;
      if (outputs[0])
        outputs[0][i] = out;
      if (outputs[1])
        outputs[1][i] = out;
    }
  }

  BlendSelection SelectBlendModels() const
  {
    BlendSelection selection;
    if (mModels.empty())
      return selection;

    if (mModels.size() == 1)
    {
      selection.lowerIndex = 0;
      selection.upperIndex = 0;
      selection.weightLower = 1.0;
      selection.weightUpper = 0.0;
      return selection;
    }

    const double minValue = mModels.front().parameterValue;
    const double maxValue = mModels.back().parameterValue;
    const double target = minValue + mBlend * (maxValue - minValue);

    if (target <= minValue)
    {
      selection.lowerIndex = 0;
      selection.upperIndex = 0;
      selection.weightLower = 1.0;
      selection.weightUpper = 0.0;
      return selection;
    }

    if (target >= maxValue)
    {
      selection.lowerIndex = mModels.size() - 1;
      selection.upperIndex = mModels.size() - 1;
      selection.weightLower = 1.0;
      selection.weightUpper = 0.0;
      return selection;
    }

    std::size_t upperIndex = 1;
    while (upperIndex < mModels.size() && mModels[upperIndex].parameterValue < target)
    {
      ++upperIndex;
    }

    std::size_t lowerIndex = (upperIndex == 0) ? 0 : upperIndex - 1;

    const double lowerValue = mModels[lowerIndex].parameterValue;
    const double upperValue = mModels[upperIndex].parameterValue;
    if (mSnapBlend)
    {
      const double lowerDist = std::abs(target - lowerValue);
      const double upperDist = std::abs(upperValue - target);
      const std::size_t chosen = lowerDist <= upperDist ? lowerIndex : upperIndex;
      selection.lowerIndex = chosen;
      selection.upperIndex = chosen;
      selection.weightLower = 1.0;
      selection.weightUpper = 0.0;
      return selection;
    }

    const double denom = std::max(upperValue - lowerValue, 1e-9);
    const double t = std::clamp((target - lowerValue) / denom, 0.0, 1.0);

    selection.lowerIndex = lowerIndex;
    selection.upperIndex = upperIndex;
    selection.weightLower = 1.0 - t;
    selection.weightUpper = t;
    return selection;
  }

  static std::optional<double> BlendOptional(const std::optional<double>& a,
                                             const std::optional<double>& b,
                                             double weightA,
                                             double weightB)
  {
    if (a.has_value() && b.has_value())
      return (*a) * weightA + (*b) * weightB;
    if (a.has_value())
      return *a;
    if (b.has_value())
      return *b;
    return std::nullopt;
  }

  void UpdateAutoGains(const BlendSelection& selection)
  {
    mAutoInputGain = 1.0;
    mAutoOutputGain = 1.0;

    if (mModels.empty())
    {
      UpdateEffectiveGains();
      return;
    }

    const ModelInstance* modelA = &mModels[selection.lowerIndex];
    const ModelInstance* modelB = &mModels[selection.upperIndex];

    const auto blendedInputLevel = BlendOptional(modelA->inputLevel, modelB->inputLevel,
      selection.weightLower, selection.weightUpper);
    const auto blendedOutputLevel = BlendOptional(modelA->outputLevel, modelB->outputLevel,
      selection.weightLower, selection.weightUpper);
    const auto blendedLoudness = BlendOptional(modelA->loudness, modelB->loudness,
      selection.weightLower, selection.weightUpper);

    static constexpr double kTargetInputLeveldBu = -18.0;
    static constexpr double kTargetOutputLeveldB = -18.0;

    const auto inputLevel = mCalibrationInputLevel.has_value() ? mCalibrationInputLevel : blendedInputLevel;
    const auto outputLevel = mCalibrationOutputLevel.has_value() ? mCalibrationOutputLevel : blendedOutputLevel;

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
      else if (blendedLoudness.has_value())
      {
        const double deltaDb = std::clamp(kTargetOutputLeveldB - *blendedLoudness, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
    }

    UpdateEffectiveGains();
  }
  bool mSnapBlend = false;
};

inline void RegisterMultiModelNAMAmpEffect()
{
  // Ensure NAM factory registrations are not optimized out by the linker
  ::nam::factory::ForceFactoryRegistration();

  EffectTypeInfo info;
  info.type = "amp_nam_blend";
  info.displayName = "NAM Blend";
  info.category = "amp";
  info.description = "Blend between multiple NAM models";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.parameters = {
    {"blend", "Blend", 0.0, 0.0, 1.0, ""},
    {"inputGain", "Input Gain", 0.0, -24.0, 24.0, "dB"},
    {"outputGain", "Output Gain", 0.0, -24.0, 24.0, "dB"}
  };

  EffectRegistry::Instance().Register("amp_nam_blend", info, []()
  {
    return std::make_unique<MultiModelNAMAmpEffect>();
  });
}

} // namespace guitarfx
