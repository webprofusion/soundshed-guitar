#pragma once

/**
 * Multi-model NAM blend effect.
 *
 * Loads several NAM models, captured at different settings of a real amp or pedal, and
 * plays one or two of them at a time. Which, and how much of each, comes from either
 *  - the settings each model was captured at (gain, bass, ...) matched against the node's
 *    values for those parameters: the two nearest by squared distance, weighted by inverse
 *    distance. Only parameters some model was captured at count; or
 *  - when the node sets none of those, the `blend` sweep (0..1) across the models'
 *    positions, crossfading the two either side.
 * In snap mode only the nearest model plays.
 *
 * Changes are never cut. A model that comes into the mix fades in over kRampSeconds and
 * one that leaves fades out. A model that has not been running first runs unheard for its
 * prewarm length, so its receptive field holds the current input rather than whatever it
 * last heard. Models out of the mix are not run at all.
 */

#include "dsp/EffectProcessor.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
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
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx
{
namespace detail
{
/// nam::DSP keeps PrewarmSamples() protected. Naming it through a derived class gives a
/// pointer to the member, which can then be called on any model; this type is never built.
struct NamPrewarmReader : ::nam::DSP
{
    [[nodiscard]] static int Read(::nam::DSP& model)
    {
        return (model.*(&NamPrewarmReader::PrewarmSamples))();
    }
};
} // namespace detail

class MultiModelNAMAmpEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    /// How long a model takes to fade fully in or out of the mix.
    static constexpr double kRampSeconds = 0.03;
    /// How long a model that was not running plays unheard before it is faded in, so its
    /// receptive field holds current input. Each model uses its own prewarm length where it
    /// reports one; slimmable and container models report none, and get this floor.
    static constexpr double kWarmupSeconds = 0.06;
    /// Longest warm-up, however deep the model: past this a knob feels unresponsive.
    static constexpr double kMaxWarmupSeconds = 0.25;
    /// A model asked for less than this share of the mix is left out, so a knob sitting on a
    /// captured setting runs one model rather than two.
    static constexpr double kMinAudibleWeight = 0.02;
    /// Models run at once while a fast sweep leaves several fading out.
    static constexpr std::size_t kMaxRunningModels = 4;

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        rtparallel::DualLaneExecutor::EnsureStarted(); // here, so Process() never starts its thread
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mPrepared = true;
        mRampStep = 1.0 / std::max(1.0, kRampSeconds * sampleRate);

        mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
        mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
        mDryBufferL.resize(static_cast<size_t>(maxBlockSize));
        mDryBufferR.resize(static_cast<size_t>(maxBlockSize));
        mMixBufferL.resize(static_cast<size_t>(maxBlockSize));
        mMixBufferR.resize(static_cast<size_t>(maxBlockSize));

        for (auto& model : mModels)
        {
            ResizeModelBuffers(model, maxBlockSize);
        }

        UpdateLatencyAlignment();
        mStartFresh = true;
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
        mStartFresh = true;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        EnsureLevelTargetsCurrent();

        // Clamp to allocated buffer size to prevent out-of-bounds writes
        numSamples = std::min(numSamples, mMaxBlockSize);

        if (numSamples <= 0)
        {
            return;
        }

        if (!inputs[0] && !inputs[1])
        {
            if (outputs[0])
            {
                std::fill_n(outputs[0], numSamples, 0.0f);
            }

            if (outputs[1])
            {
                std::fill_n(outputs[1], numSamples, 0.0f);
            }

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
                if (outputs[0])
                {
                    outputs[0][i] = mInputBufferL[i];
                }

                if (outputs[1])
                {
                    outputs[1][i] = mInputBufferR[i];
                }
            }

            return;
        }

        const OutputRamp ramp = RenderBlend(numSamples, true);

        for (int i = 0; i < numSamples; ++i)
        {
            const float gain = ramp.At(i);

            if (outputs[0])
            {
                outputs[0][i] = mDryBufferL[i] * ramp.dryMix + mMixBufferL[i] * gain;
            }

            if (outputs[1])
            {
                outputs[1][i] = mDryBufferR[i] * ramp.dryMix + mMixBufferR[i] * gain;
            }
        }
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        EnsureLevelTargetsCurrent();

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (!output || numSamples <= 0)
        {
            return;
        }

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
            std::copy_n(mInputBufferL.data(), numSamples, output);
            return;
        }

        const OutputRamp ramp = RenderBlend(numSamples, false);

        for (int i = 0; i < numSamples; ++i)
        {
            output[i] = mDryBufferL[i] * ramp.dryMix + mMixBufferL[i] * ramp.At(i);
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
            if (IsFinite(value))
            {
                mCalibrationInputLevel = value;
            }
            else
            {
                mCalibrationInputLevel.reset();
            }
        }
        else if (key == "calibrationInputLevelEnabled")
        {
            mCalibrationInputLevelEnabled = value > 0.5;
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
        {
            UpdateEffectiveGains();
        }
        else
        {
            UpdateAutoGains(SelectBlendModels());
        }
    }

    void SetConfig(const std::string& key, const std::string& value) override
    {
        if (key == "parameterId")
        {
            mParameterId = value;
        }
        else if (key == "blendMode")
        {
            // The blend definition's mode.
            mDefinitionSnap = (value == "snap");
        }
        else if (key == "blendModeOverride")
        {
            // This node's own choice, which wins over the definition's; empty follows it.
            mBlendModeOverride = (value == "snap" || value == "interpolate") ? value : std::string{};
        }
        else if (key == "slimmableSize")
        {
            if (const auto parsed = ParseDouble(value); parsed.has_value())
            {
                mSlimmableSize = SanitizeNamSlimmableSize(*parsed);
            }

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
            {
                return;
            }

            const int requestedOversampling =
                key == "oversampling" ? SanitizeNamOversamplingIndex(*parsed) : mOversamplingIndex;
            const int requestedPhase =
                key == "antiAliasPhase" ? SanitizeNamAntiAliasPhaseIndex(*parsed) : mAntiAliasPhaseIndex;

            if (requestedOversampling == mOversamplingIndex && requestedPhase == mAntiAliasPhaseIndex)
            {
                return;
            }

            mOversamplingIndex = requestedOversampling;
            mAntiAliasPhaseIndex = requestedPhase;
            ReconfigureModelProcessing();
        }
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "inputGain")
        {
            return 20.0 * std::log10(mUserInputGain);
        }

        if (key == "outputGain")
        {
            return 20.0 * std::log10(mUserOutputGain);
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "blend")
        {
            return mBlend;
        }

        if (key == "enabled")
        {
            return mEnabled ? 1.0 : 0.0;
        }

        if (key == "useCalibration")
        {
            return mUseCalibration ? 1.0 : 0.0;
        }

        const auto it = mTargetParams.find(key);

        if (it != mTargetParams.end())
        {
            return it->second;
        }

        return 0.0;
    }

    bool LoadResources(const std::vector<ResourceRef>& refs, const std::vector<std::filesystem::path>& paths) override
    {
        mModels.clear();
        mMappedParams.clear();
        mStartFresh = true;
        UpdateLatencyAlignment();

        if (refs.empty() || paths.empty())
        {
            return false;
        }

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

            if (!LoadModelInstance(instance))
            {
                continue;
            }

            for (const auto& [paramId, _] : instance.parameters)
            {
                mMappedParams.insert(paramId);
            }

            ResizeModelBuffers(instance, mMaxBlockSize);

            mModels.push_back(std::move(instance));
        }

        if (mModels.empty())
        {
            mMappedParams.clear();
            return false;
        }

        std::sort(mModels.begin(), mModels.end(),
                  [](const ModelInstance& a, const ModelInstance& b) { return a.parameterValue < b.parameterValue; });

        UpdateLatencyAlignment();
        UpdateAutoGains(SelectBlendModels());
        return true;
    }

    [[nodiscard]] bool HasResource() const override
    {
        return !mModels.empty();
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "amp_nam_blend";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "amp";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mLatencySamples;
    }

    /// Models run by the last block, heard or warming up. For tests and diagnostics.
    [[nodiscard]] std::size_t GetRunningModelCount() const
    {
        return static_cast<std::size_t>(
            std::count_if(mModels.begin(), mModels.end(), [](const ModelInstance& model) { return model.running; }));
    }

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

        // Mixing state, audio thread only.
        double desired = 0.0;    // share of the mix the selection asks for
        double target = 0.0;     // share it is heading for this block
        double gain = 0.0;       // share it has now
        bool running = false;    // processed this block, heard or not
        int warmupRemaining = 0; // samples left to run unheard before it may be faded in
        int warmupSamples = 0;   // host samples it runs unheard when brought back
    };

    struct BlendSelection
    {
        std::size_t lowerIndex = 0;
        std::size_t upperIndex = 0;
        double weightLower = 1.0;
        double weightUpper = 0.0;

        [[nodiscard]] static BlendSelection Only(std::size_t index)
        {
            BlendSelection selection;
            selection.lowerIndex = index;
            selection.upperIndex = index;
            return selection;
        }
    };

    /// The wet gain across a block, ramped from the last block's so a gain change is smooth.
    struct OutputRamp
    {
        float start = 1.0f;
        float step = 0.0f;
        float dryMix = 0.0f;

        [[nodiscard]] float At(int sample) const
        {
            return start + step * static_cast<float>(sample + 1);
        }
    };

    std::vector<ModelInstance> mModels;
    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mDryBufferL;
    std::vector<float> mDryBufferR;
    std::vector<float> mMixBufferL;
    std::vector<float> mMixBufferR;

    double mUserInputGain = 1.0;
    double mUserOutputGain = 1.0;
    double mAutoInputGain = 1.0;
    double mAutoOutputGain = 1.0;
    double mInputGain = 1.0;
    double mOutputGain = 1.0;
    double mAppliedInputGain = 1.0;
    double mAppliedOutputGain = 1.0;
    double mMix = 1.0;
    double mBlend = 0.0;
    std::map<std::string, double> mTargetParams;
    /// Every parameter some model was captured at. A target for anything else says nothing
    /// about which model to play, so selection ignores it.
    std::set<std::string> mMappedParams;
    bool mUseCalibration = true;
    bool mEnabled = true;
    bool mPrepared = false;
    bool mDefinitionSnap = false;
    std::string mBlendModeOverride;
    std::string mParameterId;
    std::uint64_t mLevelTargetsRevision = 0;
    double mRampStep = 1.0;
    /// After a load, Prepare() or Reset(): the next block takes the selection at once, since
    /// every model starts from a clean state and there is no earlier mix to fade from.
    bool mStartFresh = true;
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
    // Off while the controller has no interface level to give. See OptimizedNAMAmpEffect.
    bool mCalibrationInputLevelEnabled = true;

    void UpdateEffectiveGains()
    {
        mInputGain = mUserInputGain * mAutoInputGain;
        mOutputGain = mUserOutputGain * mAutoOutputGain;
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
                                          ? std::optional<double>(instance.fallbackLeft->GetInputLevel())
                                          : std::nullopt;
                instance.outputLevel = instance.fallbackLeft->HasOutputLevel()
                                           ? std::optional<double>(instance.fallbackLeft->GetOutputLevel())
                                           : std::nullopt;
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
        {
            return;
        }

        const double modelSampleRate = ResolveInstanceSampleRate(instance);
        const int factor = NamOversamplingFactorFromIndex(mOversamplingIndex);
        const auto filterPhase = NamAntiAliasPhaseFromIndex(mAntiAliasPhaseIndex);
        instance.oversamplingLeft.Prepare(*instance.fallbackLeft, mSampleRate, modelSampleRate, hostBlockSize, factor,
                                          filterPhase);
        instance.oversamplingRight.Prepare(*instance.fallbackRight, mSampleRate, modelSampleRate, hostBlockSize, factor,
                                           filterPhase);

        // The prewarm length is in samples at the model's own rate. Counting them at that rate
        // overestimates when the model runs oversampled, which only errs toward a clean start.
        const double expectedRate = GetInstanceExpectedSampleRate(instance);
        const double prewarmSeconds =
            expectedRate > 0.0 ? detail::NamPrewarmReader::Read(*instance.fallbackLeft) / expectedRate : 0.0;
        const double warmupSeconds = std::clamp(prewarmSeconds, kWarmupSeconds, kMaxWarmupSeconds);
        instance.warmupSamples = static_cast<int>(std::lround(warmupSeconds * mSampleRate));
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

    /// Runs the models in the mix into mMixBufferL (and R when `stereo`), with the input gain
    /// already applied, and returns the wet gain to apply over the block.
    OutputRamp RenderBlend(int numSamples, bool stereo)
    {
        UpdateModelTargets();

        const bool startFresh = std::exchange(mStartFresh, false);

        if (startFresh)
        {
            mAppliedInputGain = mInputGain;
            mAppliedOutputGain = mOutputGain;
        }

        // Input gain ramps across the block: calibration follows the selection, and a step
        // into a NAM model is a click.
        const double inputStep = (mInputGain - mAppliedInputGain) / numSamples;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = static_cast<float>(mAppliedInputGain + inputStep * (i + 1));
            mInputBufferL[i] *= gain;

            if (stereo)
            {
                mInputBufferR[i] *= gain;
            }
        }

        mAppliedInputGain = mInputGain;

        mDryDelayLeft.Process(mDryBufferL.data(), numSamples);

        if (stereo)
        {
            mDryDelayRight.Process(mDryBufferR.data(), numSamples);
        }

        const auto runLane = [this, numSamples](int channel) {
            float* input = channel == 0 ? mInputBufferL.data() : mInputBufferR.data();

            for (auto& model : mModels)
            {
                if (model.running)
                {
                    ProcessModel(model, input, channel == 0 ? model.outputBufferL.data() : model.outputBufferR.data(),
                                 numSamples, channel);
                }
            }
        };

        // Each channel has its own model instances, so the two lanes share nothing.
        const bool ranInParallel =
            stereo && rtparallel::ShouldParallelizeStereoWork(numSamples) &&
            rtparallel::DualLaneExecutor::Instance().Run([&]() { runLane(1); }, [&]() { runLane(0); });

        if (!ranInParallel)
        {
            runLane(0);

            if (stereo)
            {
                runLane(1);
            }
        }

        std::fill_n(mMixBufferL.data(), numSamples, 0.0f);

        if (stereo)
        {
            std::fill_n(mMixBufferR.data(), numSamples, 0.0f);
        }

        for (auto& model : mModels)
        {
            if (!model.running)
            {
                continue;
            }

            if (model.warmupRemaining > 0)
            {
                model.warmupRemaining = std::max(0, model.warmupRemaining - numSamples);
                continue;
            }

            double gain = model.gain;

            for (int i = 0; i < numSamples; ++i)
            {
                gain += std::clamp(model.target - gain, -mRampStep, mRampStep);
                const auto g = static_cast<float>(gain);
                mMixBufferL[i] += model.outputBufferL[i] * g;

                if (stereo)
                {
                    mMixBufferR[i] += model.outputBufferR[i] * g;
                }
            }

            model.gain = gain;
        }

        // A model out of the mix and fully faded stops running; bringing it back warms it up.
        for (auto& model : mModels)
        {
            if (model.running && model.desired <= 0.0 && model.gain <= 0.0)
            {
                model.running = false;
                model.warmupRemaining = 0;
            }
        }

        const double wetMix = mMix;
        const double outputStart = mAppliedOutputGain * wetMix;
        const double outputEnd = mOutputGain * wetMix;
        mAppliedOutputGain = mOutputGain;

        OutputRamp ramp;
        ramp.start = static_cast<float>(outputStart);
        ramp.step = static_cast<float>((outputEnd - outputStart) / numSamples);
        ramp.dryMix = static_cast<float>(1.0 - wetMix);
        return ramp;
    }

    /// Turns the selection into each model's share of the mix. A model coming in starts
    /// warming up; until one of the models asked for is ready, the current mix holds.
    void UpdateModelTargets()
    {
        const BlendSelection selection = SelectBlendModels();

        for (auto& model : mModels)
        {
            model.desired = 0.0;
        }

        double lower = selection.weightLower;
        double upper = selection.upperIndex != selection.lowerIndex ? selection.weightUpper : 0.0;

        if (upper < kMinAudibleWeight)
        {
            upper = 0.0;
        }
        else if (lower < kMinAudibleWeight)
        {
            lower = 0.0;
        }

        const double total = lower + upper;

        if (total > 0.0)
        {
            mModels[selection.lowerIndex].desired = lower / total;

            if (upper > 0.0)
            {
                mModels[selection.upperIndex].desired = upper / total;
            }
        }
        else
        {
            mModels[selection.lowerIndex].desired = 1.0;
        }

        if (mStartFresh)
        {
            for (auto& model : mModels)
            {
                model.running = model.desired > 0.0;
                model.gain = model.desired;
                model.target = model.desired;
                model.warmupRemaining = 0;
            }

            return;
        }

        for (auto& model : mModels)
        {
            if (model.desired > 0.0 && !model.running)
            {
                model.running = true;
                model.gain = 0.0;
                model.warmupRemaining = model.warmupSamples;
            }
        }

        double readyShare = 0.0;

        for (const auto& model : mModels)
        {
            if (model.running && model.warmupRemaining == 0)
            {
                readyShare += model.desired;
            }
        }

        for (auto& model : mModels)
        {
            if (readyShare > 0.0)
            {
                model.target = model.running && model.warmupRemaining == 0 ? model.desired / readyShare : 0.0;
            }
            else
            {
                model.target = model.gain;
            }
        }

        LimitRunningModels();
    }

    /// A fast sweep can leave several models fading out at once. Past kMaxRunningModels the
    /// quietest of those is dropped, which is the least audible cut available.
    void LimitRunningModels()
    {
        std::size_t running = GetRunningModelCount();

        while (running > kMaxRunningModels)
        {
            ModelInstance* quietest = nullptr;

            for (auto& model : mModels)
            {
                if (model.running && model.desired <= 0.0 && (!quietest || model.gain < quietest->gain))
                {
                    quietest = &model;
                }
            }

            if (!quietest)
            {
                return;
            }

            quietest->running = false;
            quietest->gain = 0.0;
            quietest->target = 0.0;
            quietest->warmupRemaining = 0;
            --running;
        }
    }

    [[nodiscard]] bool IsSnapMode() const
    {
        return mBlendModeOverride.empty() ? mDefinitionSnap : mBlendModeOverride == "snap";
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
        return std::any_of(mTargetParams.begin(), mTargetParams.end(),
                           [this](const auto& target) { return mMappedParams.contains(target.first); });
    }

    BlendSelection SelectBlendModelsByParams() const
    {
        if (mModels.size() < 2)
        {
            return BlendSelection::Only(0);
        }

        const auto distanceTo = [this](const ModelInstance& model) {
            double dist = 0.0;
            bool anyMatched = false;

            for (const auto& [paramId, targetValue] : mTargetParams)
            {
                // A parameter no model was captured at, such as one since dropped from the
                // blend, would add the same to every model and only flatten the weights.
                if (!mMappedParams.contains(paramId))
                {
                    continue;
                }

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

            return dist;
        };

        // Seeded from the first two models rather than from infinity, which the fast floating-point
        // Release builds assume never occurs. There are at least two models by this point.
        std::size_t bestIndex = 0;
        std::size_t secondIndex = 1;
        double bestDist = distanceTo(mModels[0]);
        double secondDist = distanceTo(mModels[1]);

        if (secondDist < bestDist)
        {
            std::swap(bestIndex, secondIndex);
            std::swap(bestDist, secondDist);
        }

        for (std::size_t i = 2; i < mModels.size(); ++i)
        {
            const double dist = distanceTo(mModels[i]);

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

        if (IsSnapMode())
        {
            return BlendSelection::Only(bestIndex);
        }

        const double eps = 1e-6;
        const double w1 = 1.0 / std::max(bestDist, eps);
        const double w2 = 1.0 / std::max(secondDist, eps);
        const double denom = std::max(w1 + w2, eps);

        BlendSelection selection;
        selection.lowerIndex = bestIndex;
        selection.upperIndex = secondIndex;
        selection.weightLower = w1 / denom;
        selection.weightUpper = w2 / denom;
        return selection;
    }

    BlendSelection SelectBlendModelsByBlend() const
    {
        if (mModels.size() < 2)
        {
            return BlendSelection::Only(0);
        }

        const double minValue = mModels.front().parameterValue;
        const double maxValue = mModels.back().parameterValue;
        const double target = minValue + mBlend * (maxValue - minValue);

        if (target <= minValue)
        {
            return BlendSelection::Only(0);
        }

        if (target >= maxValue)
        {
            return BlendSelection::Only(mModels.size() - 1);
        }

        std::size_t upperIndex = 1;

        while (upperIndex < mModels.size() && mModels[upperIndex].parameterValue < target)
        {
            ++upperIndex;
        }

        std::size_t lowerIndex = (upperIndex == 0) ? 0 : upperIndex - 1;

        const double lowerValue = mModels[lowerIndex].parameterValue;
        const double upperValue = mModels[upperIndex].parameterValue;

        if (IsSnapMode())
        {
            const double lowerDist = std::abs(target - lowerValue);
            const double upperDist = std::abs(upperValue - target);
            return BlendSelection::Only(lowerDist <= upperDist ? lowerIndex : upperIndex);
        }

        const double denom = std::max(upperValue - lowerValue, 1e-9);
        const double t = std::clamp((target - lowerValue) / denom, 0.0, 1.0);

        BlendSelection selection;
        selection.lowerIndex = lowerIndex;
        selection.upperIndex = upperIndex;
        selection.weightLower = 1.0 - t;
        selection.weightUpper = t;
        return selection;
    }

    static std::optional<double> BlendOptional(const std::optional<double>& a, const std::optional<double>& b,
                                               double weightA, double weightB)
    {
        if (a.has_value() && b.has_value())
        {
            return (*a) * weightA + (*b) * weightB;
        }

        if (a.has_value())
        {
            return *a;
        }

        if (b.has_value())
        {
            return *b;
        }

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

        const auto blendedInputLevel =
            BlendOptional(modelA->inputLevel, modelB->inputLevel, selection.weightLower, selection.weightUpper);
        const auto blendedOutputLevel =
            BlendOptional(modelA->outputLevel, modelB->outputLevel, selection.weightLower, selection.weightUpper);

        const std::optional<double> calibrationInputLevel =
            mCalibrationInputLevelEnabled ? mCalibrationInputLevel : std::nullopt;

        // Input: delta = calibrationInputLevel(dBu) - model.inputLevel(dBu)
        // Requires calibrationInputLevel to be set by controller.
        if (blendedInputLevel.has_value() && calibrationInputLevel.has_value())
        {
            const double raw = *calibrationInputLevel - *blendedInputLevel;
            const double deltaDb = std::clamp(raw, -24.0, 24.0);
            mAutoInputGain = std::pow(10.0, deltaDb / 20.0);
        }

        // Output: delta = model.outputLevel(dBu) - calibrationInputLevel(dBu)
        if (blendedOutputLevel.has_value() && calibrationInputLevel.has_value())
        {
            const double raw = *blendedOutputLevel - *calibrationInputLevel;
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
        {
            return;
        }

        if (mModels.empty())
        {
            UpdateEffectiveGains();
        }
        else
        {
            UpdateAutoGains(SelectBlendModels());
        }

        mLevelTargetsRevision = revision;
    }

    static double GetInstanceExpectedSampleRate(const ModelInstance& instance)
    {
        if (instance.fallbackLeft)
        {
            return instance.fallbackLeft->GetExpectedSampleRate();
        }

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
        {
            return;
        }

        for (auto& model : mModels)
        {
            ResizeModelBuffers(model, mMaxBlockSize);
        }

        UpdateLatencyAlignment();
        mStartFresh = true;
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
            model.wetDelayLeft.Prepare(mLatencySamples - model.oversamplingLeft.GetLatencySamples(), mMaxBlockSize);
            model.wetDelayRight.Prepare(mLatencySamples - model.oversamplingRight.GetLatencySamples(), mMaxBlockSize);
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
    info.parameters = {{"blend", "Blend", 0.0, 0.0, 1.0, "amount"},
                       {"inputGain", "Input", 0.0, -24.0, 24.0, "dB"},
                       {"outputGain", "Output", 0.0, -24.0, 24.0, "dB"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Advanced", true},
                       {"useCalibration", "Use Calibration", 1.0, 0.0, 1.0, "toggle", "Advanced", true}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<MultiModelNAMAmpEffect>(); });
}
} // namespace guitarfx
