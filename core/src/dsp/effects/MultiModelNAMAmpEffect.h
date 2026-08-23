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
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/NamModelCache.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/effects/NAMSampleRate.h"
#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/NAMSlimmableSettings.h"
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
    mPrepared = true;

    mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
    mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferL.resize(static_cast<size_t>(maxBlockSize));
    mDryBufferR.resize(static_cast<size_t>(maxBlockSize));

    for (auto& model : mModels)
    {
      ResizeModelBuffers(model, maxBlockSize);
    }

    UpdateLatencyAlignment();
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
    mDryDelayLeft.Reset();
    mDryDelayRight.Reset();
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

    const float inputGain = static_cast<float>(mInputGain);
    const float outputGain = static_cast<float>(mOutputGain);
    const float wetMix = static_cast<float>(mMix);
    const float dryMix = 1.0f - wetMix;

    for (int i = 0; i < numSamples; ++i)
    {
      mInputBufferL[i] *= inputGain;
      mInputBufferR[i] *= inputGain;
    }

    mDryDelayLeft.Process(mDryBufferL.data(), numSamples);
    mDryDelayRight.Process(mDryBufferR.data(), numSamples);

    if (selection.upperIndex == selection.lowerIndex)
    {
      auto& model = mModels[selection.lowerIndex];
      if (rtparallel::ShouldParallelizeStereoWork(numSamples))
      {
        const bool ran = rtparallel::DualLaneExecutor::Instance().Run(
          [&]() { ProcessModel(model, mInputBufferR.data(), model.outputBufferR.data(), numSamples, 1); },
          [&]() { ProcessModel(model, mInputBufferL.data(), model.outputBufferL.data(), numSamples, 0); });
        if (!ran)
        {
          ProcessModel(model, mInputBufferL.data(), model.outputBufferL.data(), numSamples, 0);
          ProcessModel(model, mInputBufferR.data(), model.outputBufferR.data(), numSamples, 1);
        }
      }
      else
      {
        ProcessModel(model, mInputBufferL.data(), model.outputBufferL.data(), numSamples, 0);
        ProcessModel(model, mInputBufferR.data(), model.outputBufferR.data(), numSamples, 1);
      }
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

    for (int i = 0; i < numSamples; ++i)
    {
      const float in = input[i];
      mDryBufferL[i] = in;
      mInputBufferL[i] = in;
    }

    if (mModels.empty() || !mEnabled)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        output[i] = mInputBufferL[i];
      }
      return;
    }

    const BlendSelection selection = SelectBlendModels();

    const float inputGain = static_cast<float>(mInputGain);
    const float outputGain = static_cast<float>(mOutputGain);
    const float wetMix = static_cast<float>(mMix);
    const float dryMix = 1.0f - wetMix;

    for (int i = 0; i < numSamples; ++i)
    {
      mInputBufferL[i] *= inputGain;
    }

    mDryDelayLeft.Process(mDryBufferL.data(), numSamples);

    if (selection.upperIndex == selection.lowerIndex)
    {
      auto &model = mModels[selection.lowerIndex];
      ProcessModel(model, mInputBufferL.data(), model.outputBufferL.data(), numSamples, 0);
      for (int i = 0; i < numSamples; ++i)
        output[i] = mDryBufferL[i] * dryMix + model.outputBufferL[i] * outputGain * wetMix;
      return;
    }

    auto &modelA = mModels[selection.lowerIndex];
    auto &modelB = mModels[selection.upperIndex];

    ProcessModel(modelA, mInputBufferL.data(), modelA.outputBufferL.data(), numSamples, 0);
    ProcessModel(modelB, mInputBufferL.data(), modelB.outputBufferL.data(), numSamples, 0);

    const float weightA = static_cast<float>(selection.weightLower);
    const float weightB = static_cast<float>(selection.weightUpper);

    for (int i = 0; i < numSamples; ++i)
    {
      const float mixed = modelA.outputBufferL[i] * weightA + modelB.outputBufferL[i] * weightB;
      output[i] = mDryBufferL[i] * dryMix + mixed * outputGain * wetMix;
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
    else if (key == "useCalibration")
    {
      mUseCalibration = value > 0.5;
    }
    else if (key == "calibrationInputLevel")
    {
      if (std::isfinite(value))
        mCalibrationInputLevel = value;
      else
        mCalibrationInputLevel.reset();
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
    else if (key == "slimmableSize")
    {
      if (const auto parsed = ParseDouble(value); parsed.has_value())
        mSlimmableSize = SanitizeNamSlimmableSize(*parsed);

      for (auto& model : mModels)
      {
        ApplyNamSlimmableSize(model.fallbackLeft.get(), mSlimmableSize);
        ApplyNamSlimmableSize(model.fallbackRight.get(), mSlimmableSize);
      }
    }
    else if (key == "oversampling" || key == "antiAliasPhase")
    {
      // Per-instance quality settings delivered as node config. Both change the
      // rendering rate or the AA filter, so models already prepared for a
      // different tier have to be re-prepared.
      const auto parsed = ParseDouble(value);
      if (!parsed.has_value())
        return;

      const int requestedOversampling = key == "oversampling"
        ? SanitizeNamOversamplingIndex(*parsed)
        : mOversamplingIndex;
      const int requestedPhase = key == "antiAliasPhase"
        ? SanitizeNamAntiAliasPhaseIndex(*parsed)
        : mAntiAliasPhaseIndex;

      if (requestedOversampling == mOversamplingIndex && requestedPhase == mAntiAliasPhaseIndex)
        return;

      mOversamplingIndex = requestedOversampling;
      mAntiAliasPhaseIndex = requestedPhase;
      ReconfigureModelProcessing();
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
    if (key == "useCalibration")
      return mUseCalibration ? 1.0 : 0.0;
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
    UpdateLatencyAlignment();
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

      mModels.push_back(std::move(instance));
    }

    if (mModels.empty())
      return false;

    std::sort(mModels.begin(), mModels.end(), [](const ModelInstance& a, const ModelInstance& b) {
      return a.parameterValue < b.parameterValue;
    });

    UpdateLatencyAlignment();
    UpdateAutoGains(SelectBlendModels());
    return true;
  }

  [[nodiscard]] bool HasResource() const override { return !mModels.empty(); }

  [[nodiscard]] std::string GetType() const override { return "amp_nam_blend"; }
  [[nodiscard]] std::string GetCategory() const override { return "amp"; }
  [[nodiscard]] int GetLatencySamples() const override { return mLatencySamples; }

private:
  struct ModelInstance
  {
    std::filesystem::path path;
    std::string parameterId;
    double parameterValue = 0.0;
    std::map<std::string, double> parameters;

    std::unique_ptr<::nam::DSP> fallbackLeft;
    std::unique_ptr<::nam::DSP> fallbackRight;

    std::vector<float> outputBufferL;
    std::vector<float> outputBufferR;
    std::vector<NAM_SAMPLE> fallbackInputL;
    std::vector<NAM_SAMPLE> fallbackInputR;
    std::vector<NAM_SAMPLE> fallbackOutputL;
    std::vector<NAM_SAMPLE> fallbackOutputR;

    NamOversamplingProcessor oversamplingLeft;
    NamOversamplingProcessor oversamplingRight;
    NamDryDelay wetDelayLeft;
    NamDryDelay wetDelayRight;

    std::optional<double> inputLevel;
    std::optional<double> outputLevel;
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
  bool mUseCalibration = true;
  bool mEnabled = true;
  bool mPrepared = false;
  std::string mParameterId;
  std::uint64_t mLevelTargetsRevision = 0;
  // Per-node quality settings, delivered as node config by PluginController and
  // seeded on newly built nodes from SignalGraphExecutor's type defaults. These
  // are deliberately not process-global: separate plugin instances in one DAW
  // project each run at their own tier.
  int mOversamplingIndex = kNamOversamplingIndexDefault;
  int mAntiAliasPhaseIndex = kNamAntiAliasPhaseIndexDefault;
  double mSlimmableSize = kNamSlimmableSizeDefault;
  int mLatencySamples = 0;
  NamDryDelay mDryDelayLeft;
  NamDryDelay mDryDelayRight;

  std::optional<double> mCalibrationInputLevel;

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
      // Shared parse behind two per-channel model instances. See dsp/NamModelCache.h.
      instance.fallbackLeft = nammodelcache::GetModel(instance.path);
      instance.fallbackRight = nammodelcache::GetModel(instance.path);
      if (instance.fallbackLeft && instance.fallbackRight)
      {
        ApplyNamSlimmableSize(instance.fallbackLeft.get(), mSlimmableSize);
        ApplyNamSlimmableSize(instance.fallbackRight.get(), mSlimmableSize);

        instance.inputLevel = instance.fallbackLeft->HasInputLevel()
          ? std::optional<double>(instance.fallbackLeft->GetInputLevel()) : std::nullopt;
        instance.outputLevel = instance.fallbackLeft->HasOutputLevel()
          ? std::optional<double>(instance.fallbackLeft->GetOutputLevel()) : std::nullopt;
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
    const int hostBlockSize = std::max(1, maxBlockSize);
    instance.outputBufferL.resize(static_cast<size_t>(hostBlockSize));
    instance.outputBufferR.resize(static_cast<size_t>(hostBlockSize));
    instance.fallbackInputL.resize(static_cast<size_t>(hostBlockSize));
    instance.fallbackInputR.resize(static_cast<size_t>(hostBlockSize));
    instance.fallbackOutputL.resize(static_cast<size_t>(hostBlockSize));
    instance.fallbackOutputR.resize(static_cast<size_t>(hostBlockSize));

    if (!mPrepared || !instance.fallbackLeft || !instance.fallbackRight)
      return;

    const double modelSampleRate = ResolveInstanceSampleRate(instance);
    const int factor = NamOversamplingFactorFromIndex(mOversamplingIndex);
    const auto filterPhase = NamAntiAliasPhaseFromIndex(mAntiAliasPhaseIndex);
    instance.oversamplingLeft.Prepare(
      *instance.fallbackLeft, mSampleRate, modelSampleRate, hostBlockSize, factor, filterPhase);
    instance.oversamplingRight.Prepare(
      *instance.fallbackRight, mSampleRate, modelSampleRate, hostBlockSize, factor, filterPhase);
  }

  void ResetModel(ModelInstance& instance, double sampleRate, int maxBlockSize)
  {
    (void)sampleRate;
    (void)maxBlockSize;
    if (instance.fallbackLeft && instance.fallbackRight)
    {
      instance.oversamplingLeft.Reset(*instance.fallbackLeft);
      instance.oversamplingRight.Reset(*instance.fallbackRight);
      instance.wetDelayLeft.Reset();
      instance.wetDelayRight.Reset();
    }
  }

  void ProcessModel(ModelInstance& instance, float* input, float* output, int numSamples, int channel)
  {
    if (instance.fallbackLeft && instance.fallbackRight)
    {
      auto& fallbackInput = channel == 0 ? instance.fallbackInputL : instance.fallbackInputR;
      auto& fallbackOutput = channel == 0 ? instance.fallbackOutputL : instance.fallbackOutputR;
      auto* fallback = channel == 0 ? instance.fallbackLeft.get() : instance.fallbackRight.get();
      auto& oversampling = channel == 0 ? instance.oversamplingLeft : instance.oversamplingRight;
      auto& wetDelay = channel == 0 ? instance.wetDelayLeft : instance.wetDelayRight;
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        fallbackInput[sampleIndex] = static_cast<NAM_SAMPLE>(input[sampleIndex]);
      }
      oversampling.Process(*fallback, fallbackInput.data(), fallbackOutput.data(), numSamples);
      for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
      {
        output[sampleIndex] = static_cast<float>(fallbackOutput[sampleIndex]);
      }
      wetDelay.Process(output, numSamples);
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

    if (mModels.empty() || !mUseCalibration)
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

    // Input: delta = calibrationInputLevel(dBu) - model.inputLevel(dBu)
    // Requires calibrationInputLevel to be set by controller.
    if (blendedInputLevel.has_value() && mCalibrationInputLevel.has_value())
    {
      const double raw = *mCalibrationInputLevel - *blendedInputLevel;
      const double deltaDb = std::clamp(raw, -24.0, 24.0);
      mAutoInputGain = std::pow(10.0, deltaDb / 20.0);
    }

    // Output: delta = model.outputLevel(dBu) - calibrationInputLevel(dBu)
    if (blendedOutputLevel.has_value() && mCalibrationInputLevel.has_value())
    {
      const double raw = *blendedOutputLevel - *mCalibrationInputLevel;
      const double deltaDb = std::clamp(raw, -24.0, 24.0);
      mAutoOutputGain = std::pow(10.0, deltaDb / 20.0);
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

  void ReconfigureModelProcessing()
  {
    if (!mPrepared)
      return;

    for (auto& model : mModels)
      ResizeModelBuffers(model, mMaxBlockSize);

    UpdateLatencyAlignment();
  }

  void UpdateLatencyAlignment()
  {
    mLatencySamples = 0;
    for (const auto& model : mModels)
    {
      mLatencySamples = std::max(mLatencySamples, model.oversamplingLeft.GetLatencySamples());
      mLatencySamples = std::max(mLatencySamples, model.oversamplingRight.GetLatencySamples());
    }

    for (auto& model : mModels)
    {
      model.wetDelayLeft.Prepare(
        mLatencySamples - model.oversamplingLeft.GetLatencySamples(), mMaxBlockSize);
      model.wetDelayRight.Prepare(
        mLatencySamples - model.oversamplingRight.GetLatencySamples(), mMaxBlockSize);
    }

    mDryDelayLeft.Prepare(mLatencySamples, mMaxBlockSize);
    mDryDelayRight.Prepare(mLatencySamples, mMaxBlockSize);
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
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Advanced", true},
    {"useCalibration", "Use Calibration", 1.0, 0.0, 1.0, "toggle", "Advanced", true}
  };

  EffectRegistry::Instance().Register(info.type, info, []()
  {
    return std::make_unique<MultiModelNAMAmpEffect>();
  });
}

} // namespace guitarfx
