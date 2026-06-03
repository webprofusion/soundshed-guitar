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
#include "dsp/BlockSincResampler.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/NAMSampleRate.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{

class MultiModelNAMAmpEffect : public EffectProcessor
{
public:
  void Prepare(double sampleRate, int maxBlockSize) override
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferL.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferR.resize(static_cast<size_t>(maxBlockSize));

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

    std::fill(mInputBufferL.begin(), mInputBufferL.end(), 0.0f);
    std::fill(mInputBufferR.begin(), mInputBufferR.end(), 0.0f);
    std::fill(mDryBufferL.begin(), mDryBufferL.end(), 0.0f);
    std::fill(mDryBufferR.begin(), mDryBufferR.end(), 0.0f);
    mCachedAutoInputGain = 1.0;
    mCachedAutoOutputGain = 1.0;
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

    for (int i = 0; i < numSamples; ++i)
    {
      float inL = inputs[0] ? inputs[0][i] : 0.0f;
      float inR = inputs[1] ? inputs[1][i] : inL;
      mDryBufferL[i] = inL;
      mDryBufferR[i] = inR;
      mInputBufferL[i] = inL;
      mInputBufferR[i] = inR;
    }

    if (mModels.empty() || !mEnabled)
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
      return;
    }

    const BlendSelection selection = SelectBlendModels();
    UpdateAutoGains(selection);

    const float inputGain = static_cast<float>(mInputGain);
    const float outputGain = static_cast<float>(mOutputGain);
    const float wetMix = static_cast<float>(mMix);
    const float dryMix = 1.0f - wetMix;

    for (int i = 0; i < numSamples; ++i)
    {
      mInputBufferL[i] *= inputGain;
      mInputBufferR[i] *= inputGain;
    }

    if (selection.upperIndex == selection.lowerIndex)
    {
      auto& model = mModels[selection.lowerIndex];
      ProcessModel(model, mInputBufferL.data(), model.outputBufferL.data(), numSamples, 0);
      ProcessModel(model, mInputBufferR.data(), model.outputBufferR.data(), numSamples, 1);
      WriteOutputs(model.outputBufferL.data(), model.outputBufferR.data(),
                   mDryBufferL.data(), mDryBufferR.data(),
                   outputs, numSamples, outputGain, wetMix, dryMix);
      return;
    }

    auto& modelA = mModels[selection.lowerIndex];
    auto& modelB = mModels[selection.upperIndex];

    ProcessModel(modelA, mInputBufferL.data(), modelA.outputBufferL.data(), numSamples, 0);
    ProcessModel(modelA, mInputBufferR.data(), modelA.outputBufferR.data(), numSamples, 1);
    ProcessModel(modelB, mInputBufferL.data(), modelB.outputBufferL.data(), numSamples, 0);
    ProcessModel(modelB, mInputBufferR.data(), modelB.outputBufferR.data(), numSamples, 1);

    const float weightA = static_cast<float>(selection.weightLower);
    const float weightB = static_cast<float>(selection.weightUpper);

    for (int i = 0; i < numSamples; ++i)
    {
      float mixedL = modelA.outputBufferL[i] * weightA + modelB.outputBufferL[i] * weightB;
      float mixedR = modelA.outputBufferR[i] * weightA + modelB.outputBufferR[i] * weightB;
      mixedL = mDryBufferL[i] * dryMix + mixedL * outputGain * wetMix;
      mixedR = mDryBufferR[i] * dryMix + mixedR * outputGain * wetMix;
      if (outputs[0])
        outputs[0][i] = mixedL;
      if (outputs[1])
        outputs[1][i] = mixedR;
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
    else if (key == "mix")
    {
      mMix = std::clamp(value, 0.0, 1.0);
    }
    else if (key == "autoLevelInput")
    {
      mAutoLevelInput = value > 0.5;
    }
    else if (key == "autoLevelOutput")
    {
      mAutoLevelOutput = value > 0.5;
    }
    else if (key == "calibrationInputLevel" || key == "calibrationOutputLevel")
    {
      // Legacy no-op: per-model calibration overrides are retired.
    }
    else if (key == "blend")
    {
      mBlend = std::clamp(value, 0.0, 1.0);
    }
    else if (key == "enabled")
    {
      mEnabled = value > 0.5;
    }
    else if (!key.empty())
    {
      mTargetParams[key] = value;
    }

    if (mModels.empty())
      UpdateEffectiveGains();
    else
      UpdateAutoGains(SelectBlendModels());
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
    if (key == "mix")
      return mMix;
    if (key == "blend")
      return mBlend;
    if (key == "enabled")
      return mEnabled ? 1.0 : 0.0;
    const auto it = mTargetParams.find(key);
    if (it != mTargetParams.end())
      return it->second;
    return 0.0;
  }

  bool LoadResources(const std::vector<ResourceRef>& refs,
                     const std::vector<std::filesystem::path>& paths) override
  {
    mModels.clear();
    mHasModelParameters = false;
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
      instance.parameters = ref.parameters;
      instance.normalizationGainDb = ReadResourceMetadataDouble(ref, "normalizationGainDb");
      if (instance.parameters.empty() && !ref.parameterId.empty() && ref.parameterValue.has_value())
      {
        instance.parameters[ref.parameterId] = *ref.parameterValue;
      }
      if (!instance.parameters.empty())
      {
        mHasModelParameters = true;
      }

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
    std::map<std::string, double> parameters;

    std::unique_ptr<::nam::DSP> fallbackLeft;
    std::unique_ptr<::nam::DSP> fallbackRight;
    bool resamplingActive = false;
    double processingSampleRate = 44100.0;
    int maxProcessingBlockSize = 512;

    std::vector<float> outputBufferL;
    std::vector<float> outputBufferR;
    std::vector<float> modelInputL;
    std::vector<float> modelInputR;
    std::vector<float> modelOutputL;
    std::vector<float> modelOutputR;
    std::vector<NAM_SAMPLE> fallbackInputL;
    std::vector<NAM_SAMPLE> fallbackInputR;
    std::vector<NAM_SAMPLE> fallbackOutputL;
    std::vector<NAM_SAMPLE> fallbackOutputR;

    BlockSincResampler inputResampler;
    BlockSincResampler outputResampler;

    std::optional<double> inputLevel;
    std::optional<double> outputLevel;
    std::optional<double> loudness;
    std::optional<double> normalizationGainDb;
  };

  struct BlendSelection
  {
    std::size_t lowerIndex = 0;
    std::size_t upperIndex = 0;
    double weightLower = 1.0;
    double weightUpper = 0.0;
  };

  std::vector<ModelInstance> mModels;
  std::vector<float> mInputBufferL;
  std::vector<float> mInputBufferR;
  std::vector<float> mDryBufferL;
  std::vector<float> mDryBufferR;

  double mUserInputGain = 1.0;
  double mUserOutputGain = 1.0;
  double mAutoInputGain = 1.0;
  double mAutoOutputGain = 1.0;
  double mInputGain = 1.0;
  double mOutputGain = 1.0;
  double mMix = 1.0;
  double mBlend = 0.0;
  std::map<std::string, double> mTargetParams;
  bool mHasModelParameters = false;
  bool mAutoLevelInput = false;
  bool mAutoLevelOutput = true;
  bool mEnabled = true;
  std::string mParameterId;
  std::uint64_t mLevelTargetsRevision = 0;

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

  static std::optional<double> ReadResourceMetadataDouble(const ResourceRef &ref, const std::string &key)
  {
    const auto it = ref.metadata.find(key);
    if (it == ref.metadata.end())
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

  bool LoadModelInstance(ModelInstance& instance)
  {
    try
    {
      instance.fallbackLeft = ::nam::get_dsp(instance.path);
      instance.fallbackRight = ::nam::get_dsp(instance.path);
      if (instance.fallbackLeft && instance.fallbackRight)
      {
        instance.inputLevel = instance.fallbackLeft->HasInputLevel()
          ? std::optional<double>(instance.fallbackLeft->GetInputLevel()) : std::nullopt;
        instance.outputLevel = instance.fallbackLeft->HasOutputLevel()
          ? std::optional<double>(instance.fallbackLeft->GetOutputLevel()) : std::nullopt;
        instance.loudness = instance.fallbackLeft->HasLoudness()
          ? std::optional<double>(instance.fallbackLeft->GetLoudness()) : std::nullopt;
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
    instance.processingSampleRate = ResolveInstanceSampleRate(instance);
    instance.resamplingActive = std::abs(instance.processingSampleRate - mSampleRate) > 1.0;
    instance.maxProcessingBlockSize = instance.resamplingActive
      ? BlockSincResampler::ComputeMaxOutputFrameCount(maxBlockSize, mSampleRate, instance.processingSampleRate)
      : maxBlockSize;
    instance.maxProcessingBlockSize = std::max(1, instance.maxProcessingBlockSize);

    instance.outputBufferL.resize(static_cast<size_t>(maxBlockSize));
    instance.outputBufferR.resize(static_cast<size_t>(maxBlockSize));
    instance.modelInputL.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.modelInputR.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.modelOutputL.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.modelOutputR.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.fallbackInputL.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.fallbackInputR.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.fallbackOutputL.resize(static_cast<size_t>(instance.maxProcessingBlockSize));
    instance.fallbackOutputR.resize(static_cast<size_t>(instance.maxProcessingBlockSize));

    instance.inputResampler.Prepare(mSampleRate, instance.processingSampleRate, maxBlockSize);
    instance.outputResampler.Prepare(instance.processingSampleRate, mSampleRate, instance.maxProcessingBlockSize);
  }

  void ResetModel(ModelInstance& instance, double sampleRate, int maxBlockSize)
  {
    (void)sampleRate;
    (void)maxBlockSize;
    if (instance.fallbackLeft && instance.fallbackRight)
    {
      instance.fallbackLeft->Reset(instance.processingSampleRate, instance.maxProcessingBlockSize);
      instance.fallbackRight->Reset(instance.processingSampleRate, instance.maxProcessingBlockSize);
    }
  }

  void ProcessModel(ModelInstance& instance, float* input, float* output, int numSamples, int channel)
  {
    if (instance.fallbackLeft && instance.fallbackRight)
    {
      auto& fallbackInput = channel == 0 ? instance.fallbackInputL : instance.fallbackInputR;
      auto& fallbackOutput = channel == 0 ? instance.fallbackOutputL : instance.fallbackOutputR;
      auto* fallback = channel == 0 ? instance.fallbackLeft.get() : instance.fallbackRight.get();
      int modelFrames = numSamples;
      if (instance.resamplingActive)
      {
        modelFrames = GetModelFrameCount(instance, numSamples);
        instance.inputResampler.ProcessFixedOutput(input, numSamples, fallbackInput.data(), modelFrames);
      }
      else
      {
        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
          fallbackInput[sampleIndex] = static_cast<NAM_SAMPLE>(input[sampleIndex]);
        }
      }
      NAM_SAMPLE* inputPtr = fallbackInput.data();
      NAM_SAMPLE* outputPtr = fallbackOutput.data();
      NAM_SAMPLE* inputPtrs[1] = { inputPtr };
      NAM_SAMPLE* outputPtrs[1] = { outputPtr };
      fallback->process(inputPtrs, outputPtrs, modelFrames);
      if (instance.resamplingActive)
      {
        instance.outputResampler.ProcessFixedOutput(fallbackOutput.data(), modelFrames, output, numSamples);
      }
      else
      {
        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
          output[sampleIndex] = static_cast<float>(fallbackOutput[sampleIndex]);
        }
      }
      return;
    }

    std::fill_n(output, numSamples, 0.0f);
  }

  void WriteOutputs(const float* left,
                    const float* right,
                    const float* dryLeft,
                    const float* dryRight,
                    float** outputs,
                    int numSamples,
                    float gain,
                    float wetMix,
                    float dryMix)
  {
    for (int i = 0; i < numSamples; ++i)
    {
      const float outL = dryLeft[i] * dryMix + left[i] * gain * wetMix;
      const float outR = dryRight[i] * dryMix + right[i] * gain * wetMix;
      if (outputs[0])
        outputs[0][i] = outL;
      if (outputs[1])
        outputs[1][i] = outR;
    }
  }

  BlendSelection SelectBlendModels() const
  {
    if (ShouldUseParamSelection())
    {
      return SelectBlendModelsByParams();
    }

    return SelectBlendModelsByBlend();
  }

  bool ShouldUseParamSelection() const
  {
    return !mTargetParams.empty() && mHasModelParameters;
  }

  BlendSelection SelectBlendModelsByParams() const
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

    std::size_t bestIndex = 0;
    std::size_t secondIndex = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    double secondDist = std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < mModels.size(); ++i)
    {
      const auto& model = mModels[i];
      double dist = 0.0;
      bool anyMatched = false;
      for (const auto& [paramId, targetValue] : mTargetParams)
      {
        const auto it = model.parameters.find(paramId);
        if (it == model.parameters.end())
        {
          dist += 4.0;
          continue;
        }
        const double delta = it->second - targetValue;
        dist += delta * delta;
        anyMatched = true;
      }

      if (!anyMatched)
      {
        dist += 9.0;
      }

      if (dist < bestDist)
      {
        secondDist = bestDist;
        secondIndex = bestIndex;
        bestDist = dist;
        bestIndex = i;
      }
      else if (dist < secondDist)
      {
        secondDist = dist;
        secondIndex = i;
      }
    }

    if (mSnapBlend || !std::isfinite(secondDist))
    {
      selection.lowerIndex = bestIndex;
      selection.upperIndex = bestIndex;
      selection.weightLower = 1.0;
      selection.weightUpper = 0.0;
      return selection;
    }

    const double eps = 1e-6;
    const double w1 = 1.0 / std::max(bestDist, eps);
    const double w2 = 1.0 / std::max(secondDist, eps);
    const double denom = std::max(w1 + w2, eps);

    selection.lowerIndex = bestIndex;
    selection.upperIndex = secondIndex;
    selection.weightLower = w1 / denom;
    selection.weightUpper = w2 / denom;
    return selection;
  }

  BlendSelection SelectBlendModelsByBlend() const
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
    const auto blendedNormalizationGainDb = BlendOptional(modelA->normalizationGainDb, modelB->normalizationGainDb,
      selection.weightLower, selection.weightUpper);

    if (mAutoLevelOutput)
    {
      if (blendedNormalizationGainDb.has_value())
      {
        const double deltaDb = std::clamp(*blendedNormalizationGainDb, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
      else if (blendedLoudness.has_value())
      {
        const double deltaDb = std::clamp(GetNominalOperatingLevelDbfs() - *blendedLoudness, -24.0, 24.0);
        mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
      }
    }

    mLevelTargetsRevision = GetLevelTargetsRevision();
    UpdateEffectiveGains();
  }

  void EnsureLevelTargetsCurrent()
  {
    const auto revision = GetLevelTargetsRevision();
    if (revision == mLevelTargetsRevision)
      return;

    if (mModels.empty())
      UpdateEffectiveGains();
    else
      UpdateAutoGains(SelectBlendModels());

    mLevelTargetsRevision = revision;
  }

  bool mSnapBlend = false;

  static double GetInstanceExpectedSampleRate(const ModelInstance& instance)
  {
    if (instance.fallbackLeft)
      return instance.fallbackLeft->GetExpectedSampleRate();
    return -1.0;
  }

  double ResolveInstanceSampleRate(const ModelInstance& instance) const
  {
    const double expectedSR = GetInstanceExpectedSampleRate(instance);
    return ResolveNamModelProcessingSampleRate(expectedSR, mSampleRate);
  }

  int GetModelFrameCount(const ModelInstance& instance, int numSamples) const
  {
    int modelFrames = BlockSincResampler::ComputeOutputFrameCount(numSamples, mSampleRate, instance.processingSampleRate);
    return std::clamp(modelFrames, 1, instance.maxProcessingBlockSize);
  }
};

inline void RegisterMultiModelNAMAmpEffect()
{
  EffectTypeInfo info;
  info.type = EffectGuids::kAmpNamBlend;
  info.aliases = {"amp_nam_blend"};
  info.displayName = "NAM Blend";
  info.category = "amp";
  info.description = "Blend between multiple NAM models";
  info.requiresResource = true;
  info.resourceType = "nam";
  info.parameters = {
    {"blend", "Blend", 0.0, 0.0, 1.0, "amount"},
    {"inputGain", "Input", 0.0, -24.0, 24.0, "dB"},
    {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Advanced", true}
  };

  EffectRegistry::Instance().Register(info.type, info, []()
  {
    return std::make_unique<MultiModelNAMAmpEffect>();
  });
}

} // namespace guitarfx
