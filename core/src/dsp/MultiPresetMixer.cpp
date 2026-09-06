#include "dsp/MultiPresetMixer.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectGuids.h"
#include "resources/ResourceLibrary.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string_view>

namespace guitarfx
{
namespace
{
constexpr float kInputAutoLevelTargetPeak = 0.7f;
constexpr float kInputAutoLevelMaxGain = 4.0f;
constexpr float kAutoLevelAttackMix = 0.01f;
constexpr float kAutoLevelReleaseMultiplier = 1.0001f;

bool GraphHasNodeType(const SignalGraph& graph, const std::string& type)
{
    for (const auto& node : graph.nodes)
    {
        if (node.type == type)
        {
            return true;
        }
    }

    return false;
}

GraphNode* FindNodeByIdOrType(SignalGraph& graph, const std::string& id, const std::string& type)
{
    if (auto* node = graph.FindNode(id))
    {
        return node;
    }

    for (auto& node : graph.nodes)
    {
        if (node.type == type)
        {
            return &node;
        }
    }

    return nullptr;
}

// Note names for pitch detection
constexpr std::array<const char*, 12> kNoteNames = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Alternative note names (flats)
constexpr std::array<const char*, 12> kNoteNamesFlat = {"C",  "Db", "D",  "Eb", "E",  "F",
                                                        "Gb", "G",  "Ab", "A",  "Bb", "B"};

MultiPresetMixer::SignalLevelStats ComputeLevelStats(const float* left, const float* right, int numSamples)
{
    MultiPresetMixer::SignalLevelStats stats;

    if (numSamples <= 0)
    {
        return stats;
    }

    double sumSquares = 0.0;
    std::size_t sampleCount = 0;

    if (left)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float value = left[i];
            const float absValue = std::abs(value);
            stats.peak = std::max(stats.peak, static_cast<double>(absValue));
            sumSquares += static_cast<double>(value) * static_cast<double>(value);

            if (absValue > 1.0f)
            {
                stats.clipCount++;
            }
        }

        sampleCount += static_cast<std::size_t>(numSamples);
    }

    if (right)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float value = right[i];
            const float absValue = std::abs(value);
            stats.peak = std::max(stats.peak, static_cast<double>(absValue));
            sumSquares += static_cast<double>(value) * static_cast<double>(value);

            if (absValue > 1.0f)
            {
                stats.clipCount++;
            }
        }

        sampleCount += static_cast<std::size_t>(numSamples);
    }

    if (sampleCount > 0)
    {
        stats.rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
    }

    return stats;
}

static inline void CpuRelax() noexcept
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
    __asm volatile("pause" ::: "memory");
    #elif defined(__aarch64__) || defined(__arm__)
    __asm volatile("yield" ::: "memory");
    #endif
#endif
}

int ScoreNodeTypeForParallelWork(std::string_view type)
{
    // Heuristic weights for per-node CPU cost in realtime processing.
    if (type == EffectGuids::kAmpNam || type == EffectGuids::kAmpNamOptimized || type == EffectGuids::kAmpNamBlend ||
        type == EffectGuids::kFxNam)
    {
        return 14;
    }

    if (type == EffectGuids::kCabIr || type == EffectGuids::kReverbIr)
    {
        return 12;
    }

    if (type == EffectGuids::kReverbAdvanced || type == EffectGuids::kReverbAmbient ||
        type == EffectGuids::kReverbRoom || type == EffectGuids::kReverbSpring)
    {
        return 6;
    }

    if (type == EffectGuids::kDelayDigital || type == EffectGuids::kDelayDoubler || type == EffectGuids::kEqParametric)
    {
        return 3;
    }

    if (type == EffectGuids::kGain)
    {
        return 1;
    }

    return 2;
}

int EstimateGraphComplexityScore(const std::vector<std::string>& nodeTypes)
{
    int score = 0;

    for (const auto& type : nodeTypes)
    {
        score += ScoreNodeTypeForParallelWork(type);
    }

    return std::max(1, score);
}

bool ShouldUseParallelPresetDispatch(bool multiThreadingEnabled, int activeCount, int totalWorkUnits,
                                     bool workersAvailable)
{
    if (!multiThreadingEnabled || !workersAvailable)
    {
        return false;
    }

    if (activeCount < 2)
    {
        return false;
    }

    // Avoid parallel fan-out for tiny blocks/light chains where scheduling cost dominates.
    constexpr int kMinParallelWorkUnits = 9000;
    return activeCount >= 3 || totalWorkUnits >= kMinParallelWorkUnits;
}
} // namespace

bool MultiPresetMixer::AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name)
{
    // Avoid duplicate IDs. Instances that are fading out after a swap do not count —
    // their ID may legitimately match the one being added back.
    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.id == presetId)
        {
            return false;
        }
    }

    auto inst = std::make_unique<PresetInstance>();
    inst->cfg.id = presetId;
    inst->cfg.name = name;

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    inst->executor.SetResourceLibrary(mResourceLibrary);
    inst->executor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    inst->executor.SetGraph(normalizedPreset.graph);
    inst->executor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    inst->executor.SetNamInputModeMono(mMonoMode);
    inst->complexityScore = EstimateGraphComplexityScore(inst->executor.GetNodeTypes());

    if (mPrepared)
    {
        inst->executor.Prepare(mSampleRate, mMaxBlockSize);
        AllocateInstanceBuffers(*inst, mMaxBlockSize);
    }

    inst->outL.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);
    inst->outR.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);

    mInstances.push_back(std::move(inst));
    return true;
}

float MultiPresetMixer::PresetInstance::CurrentFadeGain() const
{
    if (phase == InstancePhase::Active || fadeTotalSamples <= 0)
    {
        return 1.0f;
    }

    const float fraction = static_cast<float>(fadeSamplesRemaining) / static_cast<float>(fadeTotalSamples);
    return (phase == InstancePhase::FadingOut) ? fraction : (1.0f - fraction);
}

void MultiPresetMixer::PresetInstance::BeginFadeOut(int fadeSamples)
{
    // Resume the ramp from the gain we are actually at, so a switch landing mid-fade-in
    // continues smoothly downward instead of jumping to unity first.
    const float gain = CurrentFadeGain();
    phase = InstancePhase::FadingOut;
    fadeTotalSamples = std::max(1, fadeSamples);
    fadeSamplesRemaining =
        std::clamp(static_cast<int>(std::lround(gain * static_cast<double>(fadeTotalSamples))), 0, fadeTotalSamples);
}

void MultiPresetMixer::PresetInstance::GetFadeGains(int numSamples, float& startGain, float& endGain) const
{
    if (phase == InstancePhase::Active || fadeTotalSamples <= 0)
    {
        startGain = 1.0f;
        endGain = 1.0f;
        return;
    }

    const float total = static_cast<float>(fadeTotalSamples);
    const int endRemaining = std::max(0, fadeSamplesRemaining - numSamples);
    const float startFraction = static_cast<float>(fadeSamplesRemaining) / total;
    const float endFraction = static_cast<float>(endRemaining) / total;

    if (phase == InstancePhase::FadingOut)
    {
        // remaining/total: 1 -> 0
        startGain = startFraction;
        endGain = endFraction;
    }
    else
    {
        // 1 - remaining/total: 0 -> 1
        startGain = 1.0f - startFraction;
        endGain = 1.0f - endFraction;
    }
}

MultiPresetMixer::MultiPresetMixer(MultiPresetMixer&& other) noexcept
{
    *this = std::move(other);
}

MultiPresetMixer& MultiPresetMixer::operator=(MultiPresetMixer&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    // The reaper thread and its retire queue stay with the object that owns them (as do the
    // parallel worker threads); only the DSP state moves.
    mResourceLibrary = other.mResourceLibrary;
    mInstances = std::move(other.mInstances);
    mSampleRate = other.mSampleRate;
    mMaxBlockSize = other.mMaxBlockSize;
    mPrepared = other.mPrepared;
    mMixGainDb = other.mMixGainDb;
    mMixGain = other.mMixGain;
    mMasterGain = other.mMasterGain;
    mLimiterEnabled = other.mLimiterEnabled;
    mAutoLevelInput = other.mAutoLevelInput;
    mAutoLevelOutput = other.mAutoLevelOutput;
    mUserInputCalibrationGainDb = other.mUserInputCalibrationGainDb;
    mUserInputCalibrationGainLinear = other.mUserInputCalibrationGainLinear;
    mMonoMode = other.mMonoMode;
    mInputChannel = other.mInputChannel;
    mHostControlledInput = other.mHostControlledInput;
    mInputAutoLevelGain = other.mInputAutoLevelGain;
    mOutputAutoLevelGain = other.mOutputAutoLevelGain;
    mTempInL = std::move(other.mTempInL);
    mTempInR = std::move(other.mTempInR);
    mPreChainOutL = std::move(other.mPreChainOutL);
    mPreChainOutR = std::move(other.mPreChainOutR);
    mPostChainOutL = std::move(other.mPostChainOutL);
    mPostChainOutR = std::move(other.mPostChainOutR);
    mGlobalChainConfig = std::move(other.mGlobalChainConfig);
    mPreChainExecutor = std::move(other.mPreChainExecutor);
    mPostChainExecutor = std::move(other.mPostChainExecutor);
    mGlobalChainNeedsRebuild.store(other.mGlobalChainNeedsRebuild.load(std::memory_order_acquire),
                                   std::memory_order_release);
    mTunerEnabled = other.mTunerEnabled;
    mLiveTunerMode = other.mLiveTunerMode;
    mTunerReferenceFrequency = other.mTunerReferenceFrequency;
    mTunerCallback = std::move(other.mTunerCallback);
    mTunerBuffer = std::move(other.mTunerBuffer);
    mTunerOrderedBuffer = std::move(other.mTunerOrderedBuffer);
    mTunerAnalysisWriteBuffer = std::move(other.mTunerAnalysisWriteBuffer);
    mTunerAnalysisReadBuffer = std::move(other.mTunerAnalysisReadBuffer);
    mTunerBufferWriteIndex = other.mTunerBufferWriteIndex;
    mTunerSampleCounter = other.mTunerSampleCounter;
    mTunerWorkerQuit = other.mTunerWorkerQuit;
    mTunerAnalysisPending = other.mTunerAnalysisPending;
    mTunerAnalysisReferenceFrequency = other.mTunerAnalysisReferenceFrequency;
    mTunerQueuedGeneration = other.mTunerQueuedGeneration;
    mTunerAnalysisGeneration.store(other.mTunerAnalysisGeneration.load(std::memory_order_acquire),
                                   std::memory_order_release);

    mSignalDiagnosticsEnabled.store(other.mSignalDiagnosticsEnabled.load(std::memory_order_acquire),
                                    std::memory_order_release);
    mRawInputLevels.peak.store(other.mRawInputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mRawInputLevels.rms.store(other.mRawInputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mRawInputLevels.clipCount.store(other.mRawInputLevels.clipCount.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    mInputLevels.peak.store(other.mInputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mInputLevels.rms.store(other.mInputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mInputLevels.clipCount.store(other.mInputLevels.clipCount.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    mOutputLevels.peak.store(other.mOutputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mOutputLevels.rms.store(other.mOutputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mOutputLevels.clipCount.store(other.mOutputLevels.clipCount.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);

    return *this;
}

void MultiPresetMixer::SetUserInputCalibrationGainDb(double dB)
{
    const double clamped = std::isfinite(dB) ? std::clamp(dB, -24.0, 24.0) : 0.0;
    mUserInputCalibrationGainDb = clamped;
    mUserInputCalibrationGainLinear = static_cast<float>(std::pow(10.0, clamped / 20.0));
}

void MultiPresetMixer::RemoveActivePreset(const std::string& presetId)
{
    for (auto it = mInstances.begin(); it != mInstances.end(); ++it)
    {
        if (!(*it)->IsRetiring() && (*it)->cfg.id == presetId)
        {
            RetireInstance(std::move(*it));
            mInstances.erase(it);
            break;
        }
    }
}

bool MultiPresetMixer::RenameActivePreset(const std::string& oldId, const std::string& newId, const std::string& name)
{
    if (oldId.empty() || newId.empty())
    {
        return false;
    }

    if (oldId == newId)
    {
        if (auto* inst = FindInstance(newId))
        {
            inst->cfg.name = name;
            return true;
        }

        return false;
    }

    // Refuse rather than produce two slots answering to the same id: FindInstance returns
    // the first match, so a duplicate would silently route half the updates to the wrong
    // chain. Retiring instances are excluded from both lookups, so an outgoing instance
    // still carrying oldId does not block the rename.
    if (FindInstance(newId) != nullptr)
    {
        return false;
    }

    auto* inst = FindInstance(oldId);

    if (inst == nullptr)
    {
        return false;
    }

    inst->cfg.id = newId;
    inst->cfg.name = name;
    return true;
}

void MultiPresetMixer::PreparePresetSwap(const Preset& preset, const std::string& id, const std::string& name)
{
    // Build the new PresetInstance off the DSP lock. This includes effect processor
    // creation and resource loading (e.g. NAM model loading from disk) which can take
    // hundreds of milliseconds. The audio thread continues processing the current
    // instance in mInstances untouched while this runs.
    auto inst = std::make_unique<PresetInstance>();
    inst->cfg.id = id;
    inst->cfg.name = name;

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    inst->executor.SetResourceLibrary(mResourceLibrary);
    inst->executor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    inst->executor.SetGraph(normalizedPreset.graph); // CreateProcessors + LoadResources here
    inst->executor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    inst->executor.SetNamInputModeMono(mMonoMode);
    inst->complexityScore = EstimateGraphComplexityScore(inst->executor.GetNodeTypes());

    if (mPrepared)
    {
        inst->executor.Prepare(mSampleRate, mMaxBlockSize); // Effect Prepare() (NAM init, IR load) here
        AllocateInstanceBuffers(*inst, mMaxBlockSize);
    }

    inst->outL.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);
    inst->outR.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);

    mPendingInstance = std::move(inst);
}

bool MultiPresetMixer::ReplaceActivePresetInPlace(const Preset& preset, const std::string& presetId,
                                                  const std::string& name)
{
    auto existing =
        std::find_if(mInstances.begin(), mInstances.end(), [&](const std::unique_ptr<PresetInstance>& candidate) {
            return !candidate->IsRetiring() && candidate->cfg.id == presetId;
        });

    if (existing == mInstances.end())
    {
        return false;
    }

    // Preserve this slot's mixer-level settings across the rebuild.
    const InstanceConfig savedCfg = (*existing)->cfg;

    auto inst = std::make_unique<PresetInstance>();
    inst->cfg = savedCfg;
    inst->cfg.name = name;

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    inst->executor.SetResourceLibrary(mResourceLibrary);
    inst->executor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    inst->executor.SetGraph(normalizedPreset.graph);
    inst->executor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    inst->executor.SetNamInputModeMono(mMonoMode);
    inst->complexityScore = EstimateGraphComplexityScore(inst->executor.GetNodeTypes());

    if (mPrepared)
    {
        inst->executor.Prepare(mSampleRate, mMaxBlockSize);
        AllocateInstanceBuffers(*inst, mMaxBlockSize);
    }

    inst->outL.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);
    inst->outR.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);

    // Crossfade rather than cutting: leave the old slot in place, ramping down, and add
    // the replacement alongside it ramping up. Lookups skip retiring instances, so the
    // shared preset ID still resolves to the new slot from here on.
    inst->phase = InstancePhase::FadingIn;
    inst->fadeTotalSamples = kPresetFadeSamples;
    inst->fadeSamplesRemaining = kPresetFadeSamples;

    if ((*existing)->cfg.mute)
    {
        // Nothing to fade; retire it outright rather than running a silent chain.
        RetireInstance(std::move(*existing));
        *existing = std::move(inst);
    }
    else
    {
        auto& outgoing = **existing;
        outgoing.BeginFadeOut(kPresetFadeSamples);
        outgoing.cfg.solo = false;
        mInstances.push_back(std::move(inst));
    }

    return true;
}

void MultiPresetMixer::CommitPresetSwap()
{
    // Fast swap: install the pre-built instance and start fading the old ones out.
    // Must be called while holding the DSP lock. Everything here is O(instances) pointer
    // work — no allocation beyond the vector push, no resource loading, no destruction.
    if (!mPendingInstance)
    {
        return;
    }

    // Start (or continue) a fade-out on everything currently live. An instance already
    // fading from an earlier switch keeps its remaining count so it carries on from where
    // it is rather than jumping back to full gain.
    for (auto it = mInstances.begin(); it != mInstances.end();)
    {
        auto& inst = **it;

        if (inst.cfg.mute)
        {
            // Contributes nothing to fade out; retire it outright.
            RetireInstance(std::move(*it));
            it = mInstances.erase(it);
            continue;
        }

        if (!inst.IsRetiring())
        {
            inst.BeginFadeOut(kPresetFadeSamples);
            // A retiring instance must not influence the live solo decision.
            inst.cfg.solo = false;
        }

        ++it;
    }

    // Bound simultaneous fade-outs: rapid successive switches drop the oldest stragglers
    // (nearest the end of their ramp, so quietest) rather than stacking a full chain's
    // CPU cost per switch.
    while (mInstances.size() > kMaxFadingOutInstances)
    {
        RetireInstance(std::move(mInstances.front()));
        mInstances.erase(mInstances.begin());
    }

    mPendingInstance->phase = InstancePhase::FadingIn;
    mPendingInstance->fadeTotalSamples = kPresetFadeSamples;
    mPendingInstance->fadeSamplesRemaining = kPresetFadeSamples;

    mInstances.push_back(std::move(mPendingInstance));
    mPendingInstance.reset();

    // A swap plays one preset on its own. The Multi-Rig mix level belongs to the mix that
    // just went away, so a lone preset must not keep playing through its trim.
    SetMixGainDb(0.0);
}

void MultiPresetMixer::SetPresetMix(const std::string& presetId, double value)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->cfg.mix = std::clamp(value, 0.0, 1.0);
    }
}

void MultiPresetMixer::SetPresetPan(const std::string& presetId, double pan)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->cfg.pan = std::clamp(pan, -1.0, 1.0);
    }
}

void MultiPresetMixer::SetPresetMute(const std::string& presetId, bool mute)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->cfg.mute = mute;
    }
}

void MultiPresetMixer::SetPresetSolo(const std::string& presetId, bool solo)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->cfg.solo = solo;
    }
}

void MultiPresetMixer::SetMultiThreadedProcessingEnabled(bool enabled)
{
    // Platforms that cannot schedule the helpers safely stay single-threaded
    // whatever the caller asks for; see RealtimeParallel.h.
    enabled = enabled && rtparallel::kParallelDspSupported;

    const bool previous = mMultiThreadedProcessingEnabled.exchange(enabled, std::memory_order_acq_rel);

    if (previous == enabled)
    {
        return;
    }

    if (!enabled)
    {
        StopWorkers();
        return;
    }

    if (!mPrepared)
    {
        return;
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const int workerCount = static_cast<int>(hw > 1 ? hw - 1 : 0);

    if (workerCount > 0)
    {
        StartWorkers(workerCount);
    }
}

void MultiPresetMixer::SetInputTrim(double dB)
{
    for (auto& inst : mInstances)
    {
        inst->executor.SetInputTrim(dB);
    }
}

void MultiPresetMixer::SetOutputTrim(double dB)
{
    for (auto& inst : mInstances)
    {
        inst->executor.SetOutputTrim(dB);
    }
}

void MultiPresetMixer::RebuildGlobalChains()
{
    if (!mPrepared)
    {
        return;
    }

    mPreChainExecutor.Reset();
    mPostChainExecutor.Reset();

    mPreChainExecutor.SetResourceLibrary(mResourceLibrary);
    mPreChainExecutor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    auto preGraph = mGlobalChainConfig.BuildPreChainGraph();

    if (preGraph.nodes.empty() && preGraph.edges.empty())
    {
        preGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
        mGlobalChainConfig.preChainGraph = preGraph;
    }

    mPreChainExecutor.SetGraph(preGraph);
    mPreChainExecutor.SetInputTrim(mGlobalChainConfig.inputGain);
    mPreChainExecutor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    mPreChainExecutor.Prepare(mSampleRate, mMaxBlockSize);

    mPostChainExecutor.SetResourceLibrary(mResourceLibrary);
    mPostChainExecutor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    auto postGraph = mGlobalChainConfig.BuildPostChainGraph();

    if (postGraph.nodes.empty() && postGraph.edges.empty())
    {
        postGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
        mGlobalChainConfig.postChainGraph = postGraph;
    }

    mPostChainExecutor.SetGraph(postGraph);
    mPostChainExecutor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    mPostChainExecutor.Prepare(mSampleRate, mMaxBlockSize);

    mMasterGain = std::pow(10.0, mGlobalChainConfig.outputGain / 20.0);

    mGlobalChainNeedsRebuild.store(false, std::memory_order_release);
}

void MultiPresetMixer::EnsureGlobalChainsUpToDate()
{
    if (mPrepared && mGlobalChainNeedsRebuild.load(std::memory_order_acquire))
    {
        RebuildGlobalChains();
    }
}

// ==========================================================================
// Global Signal Chain Configuration
// ==========================================================================

void MultiPresetMixer::NormalizeGlobalChainConfig(GlobalSignalChainConfig& config)
{
    if ((config.preChainGraph.nodes.empty() && config.preChainGraph.edges.empty()) ||
        !GraphHasNodeType(config.preChainGraph, EffectGuids::kDynamicsGate) ||
        !GraphHasNodeType(config.preChainGraph, EffectGuids::kTranspose))
    {
        config.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if ((config.postChainGraph.nodes.empty() && config.postChainGraph.edges.empty()) ||
        !GraphHasNodeType(config.postChainGraph, EffectGuids::kEqParametric) ||
        !GraphHasNodeType(config.postChainGraph, EffectGuids::kDelayDoubler))
    {
        config.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
}

void MultiPresetMixer::ApplyGlobalChainScalars(const GlobalSignalChainConfig& config)
{
    mAutoLevelInput = config.autoLevelInput;
    mAutoLevelOutput = config.autoLevelOutput;
    mMonoMode = mHostControlledInput ? false : config.monoMode;
    mInputChannel = config.inputChannel;
    mLimiterEnabled = config.limiterEnabled;
    mMasterGain = std::pow(10.0, config.outputGain / 20.0);
    mPreChainExecutor.SetInputTrim(config.inputGain);
}

void MultiPresetMixer::SetGlobalChainConfig(const GlobalSignalChainConfig& config)
{
    GlobalSignalChainConfig normalized = config;
    NormalizeGlobalChainConfig(normalized);

    // Rebuilding tears down and recreates both global executors — construction, resource
    // loading and allocation. Skip it entirely when the graphs are unchanged, which is the
    // common case: global settings are per-instance state and do not come from presets, so
    // most preset loads pass through a config identical to the one already running.
    const bool graphsChanged = normalized.preChainGraph != mGlobalChainConfig.preChainGraph ||
                               normalized.postChainGraph != mGlobalChainConfig.postChainGraph;

    mGlobalChainConfig = std::move(normalized);
    ApplyGlobalChainScalars(mGlobalChainConfig);

    if (graphsChanged)
    {
        mGlobalChainNeedsRebuild.store(true, std::memory_order_release);
    }

    EnsureGlobalChainsUpToDate();
}

bool MultiPresetMixer::PrepareGlobalChainSwap(const GlobalSignalChainConfig& config)
{
    GlobalSignalChainConfig normalized = config;
    NormalizeGlobalChainConfig(normalized);

    const bool graphsChanged = normalized.preChainGraph != mGlobalChainConfig.preChainGraph ||
                               normalized.postChainGraph != mGlobalChainConfig.postChainGraph;
    const bool rebuildNeeded = mPrepared && (graphsChanged || mGlobalChainNeedsRebuild.load(std::memory_order_acquire));

    mPendingPreChainExecutor.reset();
    mPendingPostChainExecutor.reset();
    mPendingGlobalChainConfig = normalized;

    if (!rebuildNeeded)
    {
        return false;
    }

    // Expensive part: runs on the caller's thread with no DSP lock held.
    const bool diagnostics = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    SignalGraphExecutor preChain;
    preChain.SetResourceLibrary(mResourceLibrary);
    preChain.SetGraph(normalized.preChainGraph);
    preChain.SetInputTrim(normalized.inputGain);
    preChain.SetSignalDiagnosticsEnabled(diagnostics);
    preChain.Prepare(mSampleRate, mMaxBlockSize);

    SignalGraphExecutor postChain;
    postChain.SetResourceLibrary(mResourceLibrary);
    postChain.SetGraph(normalized.postChainGraph);
    postChain.SetSignalDiagnosticsEnabled(diagnostics);
    postChain.Prepare(mSampleRate, mMaxBlockSize);

    mPendingPreChainExecutor.emplace(std::move(preChain));
    mPendingPostChainExecutor.emplace(std::move(postChain));
    return true;
}

void MultiPresetMixer::CommitGlobalChainSwap()
{
    if (!mPendingGlobalChainConfig.has_value())
    {
        return;
    }

    mGlobalChainConfig = std::move(*mPendingGlobalChainConfig);
    mPendingGlobalChainConfig.reset();

    if (mPendingPreChainExecutor.has_value() && mPendingPostChainExecutor.has_value())
    {
        // Hand the outgoing executors to the reaper rather than destroying them here: the
        // audio thread try_locks the DSP mutex and outputs silence when it cannot take it,
        // so freeing node state under that lock is an audible dropout.
        //
        // Residual: the move-assignment below stops any worker threads the outgoing executor
        // owned, and SignalGraphExecutor's move does not transfer them, so that join happens
        // under the caller's DSP lock. It is a wake-and-join of parked threads (bounded, tens
        // of microseconds) and is zero for the linear default chains, which never start
        // workers. Preset instances avoid this entirely by being held via unique_ptr.
        {
            std::lock_guard<std::mutex> lock(mRetireMutex);
            mRetireExecutors.push_back(std::move(mPreChainExecutor));
            mRetireExecutors.push_back(std::move(mPostChainExecutor));
        }
        mRetireCv.notify_one();

        mPreChainExecutor = std::move(*mPendingPreChainExecutor);
        mPostChainExecutor = std::move(*mPendingPostChainExecutor);
        mGlobalChainNeedsRebuild.store(false, std::memory_order_release);
    }

    mPendingPreChainExecutor.reset();
    mPendingPostChainExecutor.reset();

    // After the swap so the input trim lands on the executor that is now live.
    ApplyGlobalChainScalars(mGlobalChainConfig);
}

void MultiPresetMixer::SetGlobalGateEnabled(bool enabled)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
        node->enabled = enabled;
        mPreChainExecutor.SetNodeEnabled(node->id, enabled);
    }
}

void MultiPresetMixer::SetGlobalGateThreshold(double thresholdDb)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
        node->params["threshold"] = thresholdDb;
        mPreChainExecutor.SetNodeParam(node->id, "threshold", thresholdDb);
    }
}

void MultiPresetMixer::SetGlobalGateAttack(double attackMs)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
        node->params["attack"] = attackMs;
        mPreChainExecutor.SetNodeParam(node->id, "attack", attackMs);
    }
}

void MultiPresetMixer::SetGlobalGateHold(double holdMs)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
        node->params["hold"] = holdMs;
        mPreChainExecutor.SetNodeParam(node->id, "hold", holdMs);
    }
}

void MultiPresetMixer::SetGlobalGateRelease(double releaseMs)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
        node->params["release"] = releaseMs;
        mPreChainExecutor.SetNodeParam(node->id, "release", releaseMs);
    }
}

void MultiPresetMixer::SetGlobalTransposeEnabled(bool enabled)
{
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_transpose", EffectGuids::kTranspose))
    {
        node->enabled = enabled;
        mPreChainExecutor.SetNodeEnabled(node->id, enabled);
    }
}

void MultiPresetMixer::SetGlobalTranspose(int semitones)
{
    const double value = static_cast<double>(std::clamp(semitones, -12, 12));
    const bool enabled = (value != 0.0);

    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
        mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_transpose", EffectGuids::kTranspose))
    {
        node->enabled = enabled;
        node->params["semitones"] = value;
        mPreChainExecutor.SetNodeEnabled(node->id, enabled);
        mPreChainExecutor.SetNodeParam(node->id, "semitones", value);
    }
}

void MultiPresetMixer::SetGlobalEQEnabled(bool enabled)
{
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
        node->enabled = enabled;
        mPostChainExecutor.SetNodeEnabled(node->id, enabled);
    }
}

void MultiPresetMixer::SetGlobalEQBandGain(int band, double dB)
{
    static const char* kParamNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};

    if (band < 0 || band > 3)
    {
        return;
    }

    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = dB;
        mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], dB);
    }
}

void MultiPresetMixer::SetGlobalEQBandFrequency(int band, double freq)
{
    static const char* kParamNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};

    if (band < 0 || band > 3)
    {
        return;
    }

    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = freq;
        mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], freq);
    }
}

void MultiPresetMixer::SetGlobalEQBandQ(int band, double q)
{
    static const char* kParamNames[] = {"", "lowMidQ", "highMidQ", ""};

    if (band < 1 || band > 2)
    {
        return;
    }

    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = q;
        mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], q);
    }
}

void MultiPresetMixer::SetGlobalDoublerEnabled(bool enabled)
{
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node =
            FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
        node->enabled = enabled;
        mPostChainExecutor.SetNodeEnabled(node->id, enabled);
    }
}

void MultiPresetMixer::SetGlobalDoublerDelay(double delayMs)
{
    const double clamped = std::clamp(delayMs, 0.5, 100.0);

    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node =
            FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["time"] = clamped;
        mPostChainExecutor.SetNodeParam(node->id, "time", clamped);
    }
}

void MultiPresetMixer::SetGlobalDoublerMix(double mix)
{
    const double clamped = std::clamp(mix, 0.0, 1.0);

    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node =
            FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["mix"] = clamped;
        mPostChainExecutor.SetNodeParam(node->id, "mix", clamped);
    }
}

void MultiPresetMixer::SetGlobalDoublerDetune(double cents)
{
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
        mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    if (auto* node =
            FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["detune"] = cents;
        mPostChainExecutor.SetNodeParam(node->id, "detune", cents);
    }
}

void MultiPresetMixer::SetGlobalInputGain(double dB)
{
    mGlobalChainConfig.inputGain = dB;
    // Input gain applied via pre-chain input trim
    mPreChainExecutor.SetInputTrim(dB);
}

void MultiPresetMixer::SetGlobalOutputGain(double dB)
{
    mGlobalChainConfig.outputGain = dB;
    // Convert dB to linear for master gain
    mMasterGain = std::pow(10.0, dB / 20.0);
}

// Node-level control methods
void MultiPresetMixer::SetNodeEnabled(const std::string& presetId, const std::string& nodeId, bool enabled)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->executor.SetNodeEnabled(nodeId, enabled);
    }
}

void MultiPresetMixer::SetNodeParam(const std::string& presetId, const std::string& nodeId, const std::string& key,
                                    double value)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->executor.SetNodeParam(nodeId, key, value);
    }
}

void MultiPresetMixer::SetNodeConfig(const std::string& presetId, const std::string& nodeId, const std::string& key,
                                     const std::string& value)
{
    if (auto* inst = FindInstance(presetId))
    {
        inst->executor.SetNodeConfig(nodeId, key, value);
    }
}

void MultiPresetMixer::SetNodeConfigForType(const std::string& type, const std::string& key, const std::string& value)
{
    for (auto& inst : mInstances)
    {
        inst->executor.SetNodeConfigForType(type, key, value);
    }

    mPreChainExecutor.SetNodeConfigForType(type, key, value);
    mPostChainExecutor.SetNodeConfigForType(type, key, value);
}

void MultiPresetMixer::SetNodeTypeConfigDefault(const std::string& type, const std::string& key,
                                                const std::string& value)
{
    mNodeTypeConfigDefaults[type][key] = value;

    for (auto& inst : mInstances)
    {
        inst->executor.SetNodeTypeConfigDefault(type, key, value);
    }

    mPreChainExecutor.SetNodeTypeConfigDefault(type, key, value);
    mPostChainExecutor.SetNodeTypeConfigDefault(type, key, value);
}

std::optional<std::pair<std::string, std::string>> MultiPresetMixer::FindFirstEnabledNodeOfType(
    const std::string& effectType) const
{
    for (const auto& inst : mInstances)
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        const auto nodeIds = inst->executor.FindNodesOfType(effectType, false);

        if (!nodeIds.empty())
        {
            return std::make_pair(inst->cfg.id, nodeIds.front());
        }
    }

    return std::nullopt;
}

std::vector<MultiPresetMixer::NodeReadout> MultiPresetMixer::ReadNodeParamsForType(
    const std::string& effectType, const std::vector<std::string>& paramIds) const
{
    std::vector<NodeReadout> readouts;

    if (effectType.empty() || paramIds.empty())
    {
        return readouts;
    }

    auto collect = [&](const SignalGraphExecutor& executor, const char* scope, const std::string& presetId) {
        for (const auto& nodeId : executor.FindNodesOfType(effectType, true))
        {
            const auto* processor = executor.GetNodeProcessor(nodeId);

            if (!processor)
            {
                continue;
            }

            NodeReadout readout;
            readout.scope = scope;
            readout.presetId = presetId;
            readout.nodeId = nodeId;
            readout.values.reserve(paramIds.size());

            for (const auto& paramId : paramIds)
            {
                readout.values.push_back(processor->GetParam(paramId));
            }

            readouts.push_back(std::move(readout));
        }
    };

    collect(mPreChainExecutor, "pre", std::string{});

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring())
        {
            collect(inst->executor, "preset", inst->cfg.id);
        }
    }

    collect(mPostChainExecutor, "post", std::string{});

    return readouts;
}

bool MultiPresetMixer::SetNodeEnabledByType(const std::string& effectType, bool enabled)
{
    bool updated = false;

    for (const auto& inst : mInstances)
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        const auto nodeIds = inst->executor.FindNodesOfType(effectType, true);

        for (const auto& nodeId : nodeIds)
        {
            SetNodeEnabled(inst->cfg.id, nodeId, enabled);
            updated = true;
        }
    }

    return updated;
}

bool MultiPresetMixer::SetNodeParamByType(const std::string& effectType, const std::string& paramId, double value)
{
    const auto found = FindFirstEnabledNodeOfType(effectType);

    if (!found)
    {
        return false;
    }

    SetNodeParam(found->first, found->second, paramId, value);
    return true;
}

std::string MultiPresetMixer::GetNodeConfig(const std::string& presetId, const std::string& nodeId,
                                            const std::string& key) const
{
    if (const auto* inst = FindInstance(presetId))
    {
        return inst->executor.GetNodeConfig(nodeId, key);
    }

    return {};
}

EffectProcessor* MultiPresetMixer::GetNodeProcessor(const std::string& presetId, const std::string& nodeId)
{
    if (auto* inst = FindInstance(presetId))
    {
        return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
}

const EffectProcessor* MultiPresetMixer::GetNodeProcessor(const std::string& presetId, const std::string& nodeId) const
{
    if (const auto* inst = FindInstance(presetId))
    {
        return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
}

void MultiPresetMixer::SetTempo(double bpm)
{
    for (auto& inst : mInstances)
    {
        inst->executor.SetTempo(bpm);
    }

    mPreChainExecutor.SetTempo(bpm);
    mPostChainExecutor.SetTempo(bpm);
}

bool MultiPresetMixer::LoadNodeResource(const std::string& presetId, const std::string& nodeId, const ResourceRef& ref)
{
    if (auto* inst = FindInstance(presetId))
    {
        return inst->executor.LoadNodeResource(nodeId, ref);
    }

    return false;
}

// ---------------------------------------------------------------------------
// Destructor / parallel worker lifecycle
// ---------------------------------------------------------------------------

MultiPresetMixer::~MultiPresetMixer()
{
    StopTunerWorker();
    StopWorkers();
    StopReaper();
}

// ---------------------------------------------------------------------------
// Deferred destruction (reaper)
// ---------------------------------------------------------------------------

void MultiPresetMixer::StartReaper()
{
    if (mReaperThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mRetireMutex);
        mReaperQuit = false;
        // Reserve up front so the audio thread's retire path never reallocates.
        mRetireQueue.reserve(kRetireQueueCapacity);
        mRetireExecutors.reserve(kRetireQueueCapacity);
    }

    mReaperThread = std::thread([this] { ReaperLoop(); });
}

void MultiPresetMixer::StopReaper()
{
    if (!mReaperThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mRetireMutex);
        mReaperQuit = true;
    }
    mRetireCv.notify_all();
    mReaperThread.join();

    // Destroy anything still queued now that the worker is gone.
    std::lock_guard<std::mutex> lock(mRetireMutex);
    mRetireQueue.clear();
    mRetireExecutors.clear();
}

void MultiPresetMixer::ReaperLoop()
{
    while (true)
    {
        std::vector<std::unique_ptr<PresetInstance>> instances;
        std::vector<SignalGraphExecutor> executors;

        {
            std::unique_lock<std::mutex> lock(mRetireMutex);
            // Poll rather than relying solely on notification: the audio thread retires
            // finished fade-outs without signalling the condition variable (notify_one is
            // not something we want on the realtime path), so a periodic wake is needed.
            mRetireCv.wait_for(lock, std::chrono::milliseconds(250),
                               [this] { return mReaperQuit || !mRetireQueue.empty() || !mRetireExecutors.empty(); });

            if (mReaperQuit)
            {
                return;
            }

            // Move the work out and clear in place — clear() keeps the reserved capacity the
            // audio thread's allocation-free retire path depends on. Destruction of the moved
            // elements happens below, outside the lock, so a producer never waits on it.
            instances.reserve(mRetireQueue.size());

            for (auto& inst : mRetireQueue)
            {
                instances.push_back(std::move(inst));
            }

            mRetireQueue.clear();

            executors.reserve(mRetireExecutors.size());

            for (auto& exec : mRetireExecutors)
            {
                executors.push_back(std::move(exec));
            }

            mRetireExecutors.clear();
        }

        // instances/executors destruct here, off both the audio and message threads.
    }
}

void MultiPresetMixer::RetireInstance(std::unique_ptr<PresetInstance> inst)
{
    if (!inst)
    {
        return;
    }

    StartReaper();
    {
        std::lock_guard<std::mutex> lock(mRetireMutex);
        mRetireQueue.push_back(std::move(inst));
    }
    mRetireCv.notify_one();
}

bool MultiPresetMixer::TryRetireInstanceRealtime(std::unique_ptr<PresetInstance>& inst)
{
    // Audio thread: never block, never allocate. If the reaper happens to be draining the
    // queue, or the queue is at capacity, leave the instance in place — it is fully faded
    // out by this point, so carrying it for another block costs a silent chain and nothing
    // audible. The next block tries again.
    std::unique_lock<std::mutex> lock(mRetireMutex, std::try_to_lock);

    if (!lock.owns_lock())
    {
        return false;
    }

    if (mRetireQueue.size() >= mRetireQueue.capacity())
    {
        return false;
    }

    mRetireQueue.push_back(std::move(inst));
    return true;
}

void MultiPresetMixer::CollectFinishedFadeOuts()
{
    for (auto it = mInstances.begin(); it != mInstances.end();)
    {
        if ((*it)->IsRetiring() && (*it)->fadeSamplesRemaining <= 0 && TryRetireInstanceRealtime(*it))
        {
            it = mInstances.erase(it); // unique_ptr moves only — no executor moves, no joins
        }
        else
        {
            ++it;
        }
    }
}

void MultiPresetMixer::StartTunerWorker()
{
    if (mTunerWorkerThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mTunerAnalysisMutex);
        mTunerWorkerQuit = false;
        mTunerAnalysisPending = false;
    }

    mTunerWorkerThread = std::thread([this] { TunerWorkerLoop(); });
}

void MultiPresetMixer::StopTunerWorker()
{
    if (!mTunerWorkerThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mTunerAnalysisMutex);
        mTunerWorkerQuit = true;
        mTunerAnalysisPending = false;
    }
    mTunerAnalysisCv.notify_all();

    if (mTunerWorkerThread.joinable())
    {
        mTunerWorkerThread.join();
    }
}

void MultiPresetMixer::TunerWorkerLoop()
{
    while (true)
    {
        double referenceFrequency = 440.0;
        std::uint64_t queuedGeneration = 0;
        TunerCallback callback;

        {
            std::unique_lock<std::mutex> lock(mTunerAnalysisMutex);
            mTunerAnalysisCv.wait(lock, [&] { return mTunerWorkerQuit || mTunerAnalysisPending; });

            if (mTunerWorkerQuit)
            {
                return;
            }

            std::swap(mTunerAnalysisReadBuffer, mTunerAnalysisWriteBuffer);
            referenceFrequency = mTunerAnalysisReferenceFrequency;
            queuedGeneration = mTunerQueuedGeneration;
            mTunerAnalysisPending = false;
            callback = mTunerCallback;
        }

        if (!callback || mTunerAnalysisReadBuffer.empty())
        {
            continue;
        }

        double sumSq = 0.0;

        for (const auto sample : mTunerAnalysisReadBuffer)
        {
            sumSq += sample * sample;
        }

        const double rms = std::sqrt(sumSq / static_cast<double>(mTunerAnalysisReadBuffer.size()));

        const double frequency = DetectPitch(mTunerAnalysisReadBuffer);
        TunerResult result = FrequencyToNote(frequency, referenceFrequency);
        result.debugRms = rms;
        result.debugRawFreq = frequency;

        if (queuedGeneration != mTunerAnalysisGeneration.load(std::memory_order_acquire))
        {
            continue;
        }

        callback(result);
    }
}

void MultiPresetMixer::StartWorkers(int count)
{
    StopWorkers();

    {
        std::lock_guard<std::mutex> lock(mParallelMutex);
        mParallelQuit.store(false, std::memory_order_relaxed);
        mParallelGeneration.store(0, std::memory_order_relaxed);
    }

    const int numWorkers = std::min(count, kMaxParallelWorkers);
    mWorkerThreads.reserve(static_cast<size_t>(numWorkers));

    for (int i = 0; i < numWorkers; ++i)
    {
        mWorkerThreads.emplace_back([this] { WorkerLoop(); });
    }
}

void MultiPresetMixer::StopWorkers()
{
    if (mWorkerThreads.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mParallelMutex);
        mParallelQuit.store(true, std::memory_order_relaxed);
    }
    mParallelCv.notify_all();

    for (auto& t : mWorkerThreads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    mWorkerThreads.clear();
}

void MultiPresetMixer::WorkerLoop()
{
    uint32_t lastGen = 0;

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(mParallelMutex);
            mParallelCv.wait(lock, [&] {
                return mParallelQuit.load(std::memory_order_relaxed) ||
                       mParallelGeneration.load(std::memory_order_relaxed) != lastGen;
            });
        }

        if (mParallelQuit.load(std::memory_order_acquire))
        {
            break;
        }

        lastGen = mParallelGeneration.load(std::memory_order_acquire);

        const int total = mParallelTaskCount.load(std::memory_order_acquire);

        while (true)
        {
            const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);

            if (idx >= total)
            {
                break;
            }

            const auto& wi = mWorkItems[static_cast<size_t>(idx)];
            float* ins[2] = {wi.preChainOutL, wi.preChainOutR};
            float* outs[2] = {wi.inst->outL.data(), wi.inst->outR.data()};
            wi.inst->executor.Process(ins, outs, wi.numSamples);
            mParallelDoneCount.fetch_add(1, std::memory_order_release);
        }
    }
}

void MultiPresetMixer::Prepare(double sampleRate, int maxBlockSize)
{
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    // Bring the reaper up before any swap can happen, so retiring never has to spawn a
    // thread while the DSP lock is held.
    StartReaper();

    // Allocate global temp buffers
    mTempInL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTempInR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTunerOrderedBuffer.resize(kTunerBufferSize, 0.0);
    mTunerAnalysisWriteBuffer.resize(kTunerBufferSize, 0.0);
    mTunerAnalysisReadBuffer.resize(kTunerBufferSize, 0.0);

    // Build and prepare global signal chains based on current config
    mGlobalChainNeedsRebuild.store(true, std::memory_order_release);
    EnsureGlobalChainsUpToDate();

    AllocateBuffers(maxBlockSize);

    for (auto& inst : mInstances)
    {
        inst->executor.Prepare(sampleRate, maxBlockSize);
        AllocateInstanceBuffers(*inst, maxBlockSize);
    }

    // Start worker threads for parallel preset processing.
    // Reserve hw_concurrency-1 threads so the audio thread's core is not contested.
    if (mMultiThreadedProcessingEnabled.load(std::memory_order_acquire))
    {
        const unsigned int hw = std::thread::hardware_concurrency();
        const int workerCount = static_cast<int>(hw > 1 ? hw - 1 : 0);

        if (workerCount > 0)
        {
            StartWorkers(workerCount);
        }
    }
    else
    {
        StopWorkers();
    }
}

void MultiPresetMixer::Reset()
{
    mPreChainExecutor.Reset();
    mPostChainExecutor.Reset();

    for (auto& inst : mInstances)
    {
        inst->executor.Reset();
    }
}

void MultiPresetMixer::Process(float** inputs, float** outputs, int numSamples)
{
    if (!outputs || numSamples <= 0)
    {
        return;
    }

    // A host is supposed to respect the block size we were prepared with, but not all of
    // them do. Clamping alone kept us inside our buffers at the cost of leaving the tail of
    // the caller's output buffer untouched -- and since hosts reuse those buffers, the
    // stale previous block plays through as a click. Split the work instead so the whole
    // buffer is always written. mOversizedBlockCount makes it visible when this happens.
    if (mPrepared && mMaxBlockSize > 0 && numSamples > mMaxBlockSize)
    {
        mOversizedBlockCount.fetch_add(1, std::memory_order_relaxed);

        int offset = 0;

        while (offset < numSamples)
        {
            const int chunk = std::min(mMaxBlockSize, numSamples - offset);
            float* chunkIn[2] = {inputs && inputs[0] ? inputs[0] + offset : nullptr,
                                 inputs && inputs[1] ? inputs[1] + offset : nullptr};
            float* chunkOut[2] = {outputs[0] ? outputs[0] + offset : nullptr,
                                  outputs[1] ? outputs[1] + offset : nullptr};
            Process(chunkIn, chunkOut, chunk);
            offset += chunk;
        }

        return;
    }

    const bool diagnosticsEnabled = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    // NOTE: Do NOT call EnsureGlobalChainsUpToDate() here.
    // Rebuilding global chains allocates memory which is unsafe on the audio thread.
    // The chains are rebuilt from Prepare() and SetGlobalChainConfig() on the UI/main thread.

    // Safety check: ensure we're prepared before processing
    if (!mPrepared || mInstances.empty())
    {
        // Output silence if not ready
        if (outputs[0])
        {
            std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
        }

        if (outputs[1])
        {
            std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
        }

        return;
    }

    // Prepare input based on global mono/stereo settings
    float* processInL = inputs ? inputs[0] : nullptr;
    float* processInR = inputs ? inputs[1] : nullptr;

    if (diagnosticsEnabled)
    {
        const auto rawStats = ComputeLevelStats(processInL, processInR, numSamples);
        mRawInputLevels.peak.store(rawStats.peak, std::memory_order_relaxed);
        mRawInputLevels.rms.store(rawStats.rms, std::memory_order_relaxed);
        mRawInputLevels.clipCount.store(rawStats.clipCount, std::memory_order_relaxed);
    }

    if (mMonoMode && (processInL || processInR))
    {
        // Apply mono mode: produce dual-mono buffers even when only one live
        // hardware input channel is present.
        const bool standaloneInputPath = !mHostControlledInput;

        for (int i = 0; i < numSamples; ++i)
        {
            const float leftSample = processInL ? processInL[i] : 0.0f;
            const float rightSample = processInR ? processInR[i] : 0.0f;

            float monoSample = 0.0f;

            if (mInputChannel == 0)
            {
                monoSample = leftSample; // Left only
            }
            else if (mInputChannel == 1)
            {
                monoSample = rightSample; // Right only
            }
            else
            {
                // Match NAM Gateway standalone input semantics:
                // sum live inputs directly (no DAW-style averaging).
                monoSample = standaloneInputPath ? (leftSample + rightSample) : ((leftSample + rightSample) * 0.5f);
            }

            mTempInL[static_cast<std::size_t>(i)] = monoSample;
            mTempInR[static_cast<std::size_t>(i)] = monoSample;
        }

        processInL = mTempInL.data();
        processInR = mTempInR.data();
    }

    if (std::abs(mUserInputCalibrationGainLinear - 1.0f) > 1.0e-4f)
    {
        if (processInL)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                mTempInL[static_cast<std::size_t>(i)] = processInL[i] * mUserInputCalibrationGainLinear;
            }

            processInL = mTempInL.data();
        }

        if (processInR)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                mTempInR[static_cast<std::size_t>(i)] = processInR[i] * mUserInputCalibrationGainLinear;
            }

            processInR = mTempInR.data();
        }
    }

    // Apply auto-level input gain (simple peak detection and normalization)
    if (mAutoLevelInput && processInL && processInR)
    {
        // Find peak
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            peak = std::max(peak, std::abs(processInL[i]));
            peak = std::max(peak, std::abs(processInR[i]));
        }

        // Apply auto-level with smoothing
        if (peak > 0.001f)
        {
            const float targetGain = kInputAutoLevelTargetPeak / peak;
            const float limitedGain = std::min(targetGain, kInputAutoLevelMaxGain);
            mInputAutoLevelGain =
                mInputAutoLevelGain * (1.0f - kAutoLevelAttackMix) + limitedGain * kAutoLevelAttackMix;

            for (int i = 0; i < numSamples; ++i)
            {
                mTempInL[static_cast<std::size_t>(i)] = processInL[i] * mInputAutoLevelGain;
                mTempInR[static_cast<std::size_t>(i)] = processInR[i] * mInputAutoLevelGain;
            }

            processInL = mTempInL.data();
            processInR = mTempInR.data();
        }
    }

    if (diagnosticsEnabled)
    {
        const auto stats = ComputeLevelStats(processInL, processInR, numSamples);
        mInputLevels.peak.store(stats.peak, std::memory_order_relaxed);
        mInputLevels.rms.store(stats.rms, std::memory_order_relaxed);
        mInputLevels.clipCount.store(stats.clipCount, std::memory_order_relaxed);
    }

    // Process tuner FIRST (before any processing, uses raw input for accurate pitch detection)
    if (mTunerEnabled)
    {
        float* tunerInputs[2] = {processInL, processInR};
        ProcessTuner(tunerInputs, numSamples);

        // If not in live tuner mode, mute the output
        if (!mLiveTunerMode)
        {
            if (outputs[0])
            {
                std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
            }

            if (outputs[1])
            {
                std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
            }

            return;
        }
    }

    const bool namInputModeMono = mMonoMode;
    mPreChainExecutor.SetNamInputModeMono(namInputModeMono);
    mPostChainExecutor.SetNamInputModeMono(namInputModeMono);

    // ==========================================================================
    // GLOBAL PRE-CHAIN: Input → Noise Gate → Transpose
    // ==========================================================================
    float* preChainInputs[2] = {processInL, processInR};
    float* preChainOutputs[2] = {mPreChainOutL.data(), mPreChainOutR.data()};
    mPreChainExecutor.Process(preChainInputs, preChainOutputs, numSamples);

    // ==========================================================================
    // PRESET PROCESSING: Process each active preset and mix
    // ==========================================================================

    // Detect solo mode. Instances fading out after a swap never participate in the solo
    // decision — they are on their way out and must stay audible for the whole ramp.
    bool anySolo = false;

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.solo)
        {
            anySolo = true;
            break;
        }
    }

    const auto isAudible = [&](const PresetInstance& inst) {
        if (inst.cfg.mute)
        {
            return false;
        }

        if (inst.IsRetiring())
        {
            // Fully faded out but not yet handed to the reaper (the queue was busy last
            // block). It contributes nothing, so skip the chain entirely.
            return inst.fadeSamplesRemaining > 0;
        }

        return !anySolo || inst.cfg.solo;
    };

    // Mix one processed instance into the accumulator, applying its mixer gain, pan and
    // (when it is mid-swap) a linear fade ramp across the block.
    const auto mixInstance = [&](const PresetInstance& inst) {
        float gL = 1.0f, gR = 1.0f;
        ComputePanGains(inst.cfg.pan, gL, gR);
        const float baseL = static_cast<float>(inst.cfg.mix) * gL;
        const float baseR = static_cast<float>(inst.cfg.mix) * gR;

        if (inst.phase == InstancePhase::Active)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (outputs[0])
                {
                    outputs[0][i] += inst.outL[static_cast<size_t>(i)] * baseL;
                }

                if (outputs[1])
                {
                    outputs[1][i] += inst.outR[static_cast<size_t>(i)] * baseR;
                }
            }

            return;
        }

        float fadeStart = 1.0f, fadeEnd = 1.0f;
        inst.GetFadeGains(numSamples, fadeStart, fadeEnd);
        // Divide by numSamples, not numSamples-1: the last sample of this block lands one
        // step short of fadeEnd, which is exactly where the next block starts. Using
        // numSamples-1 would repeat that value and put a one-sample flat spot in the ramp.
        const float step = (fadeEnd - fadeStart) / static_cast<float>(numSamples);

        float fade = fadeStart;

        for (int i = 0; i < numSamples; ++i, fade += step)
        {
            if (outputs[0])
            {
                outputs[0][i] += inst.outL[static_cast<size_t>(i)] * baseL * fade;
            }

            if (outputs[1])
            {
                outputs[1][i] += inst.outR[static_cast<size_t>(i)] * baseR * fade;
            }
        }
    };

    // Clear preset mix accumulator (use outputs as accumulator)
    if (outputs[0])
    {
        std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
    }

    if (outputs[1])
    {
        std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
    }

    // Count active instances to decide whether to use parallel dispatch.
    int activeCount = 0;

    for (const auto& inst : mInstances)
    {
        if (isAudible(*inst))
        {
            ++activeCount;
        }
    }

    int totalWorkUnits = 0;

    if (activeCount >= 2)
    {
        for (const auto& inst : mInstances)
        {
            if (!isAudible(*inst))
            {
                continue;
            }

            totalWorkUnits += inst->complexityScore * numSamples;
        }
    }

    const bool useParallel =
        ShouldUseParallelPresetDispatch(mMultiThreadedProcessingEnabled.load(std::memory_order_acquire), activeCount,
                                        totalWorkUnits, !mWorkerThreads.empty());

    // Avoid nested parallelism: if mixer-level fan-out is active, run each preset graph serially.
    for (auto& inst : mInstances)
    {
        inst->executor.SetNamInputModeMono(namInputModeMono);
        inst->executor.SetParallelLevelsEnabled(!useParallel);
    }

    if (useParallel)
    {
        // Pack work items (up to kMaxWorkItems); any extras fall through to serial below.
        int wi = 0;

        for (auto& instPtr : mInstances)
        {
            auto& inst = *instPtr;

            if (!isAudible(inst))
            {
                continue;
            }

            if (wi < kMaxWorkItems)
            {
                mWorkItems[static_cast<size_t>(wi)] = {&inst, mPreChainOutL.data(), mPreChainOutR.data(), numSamples};
                ++wi;
            }
            else
            {
                // Overflow beyond kMaxWorkItems: process serially and mix immediately.
                float* presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
                inst.executor.Process(preChainOutputs, presetOutPtrs, numSamples);
                mixInstance(inst);
            }
        }

        // Publish tasks and wake only the workers we actually need.
        {
            std::lock_guard<std::mutex> lock(mParallelMutex);
            mParallelTaskHead.store(0, std::memory_order_relaxed);
            mParallelDoneCount.store(0, std::memory_order_relaxed);
            mParallelTaskCount.store(wi, std::memory_order_relaxed);
            mParallelGeneration.fetch_add(1, std::memory_order_relaxed);
        }
        const int workersNeeded = std::min<int>(std::max(0, wi - 1), static_cast<int>(mWorkerThreads.size()));

        for (int n = 0; n < workersNeeded; ++n)
        {
            mParallelCv.notify_one();
        }

        // Audio thread steals tasks alongside workers.
        while (true)
        {
            const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);

            if (idx >= wi)
            {
                break;
            }

            const auto& item = mWorkItems[static_cast<size_t>(idx)];
            float* ins[2] = {item.preChainOutL, item.preChainOutR};
            float* outs[2] = {item.inst->outL.data(), item.inst->outR.data()};
            item.inst->executor.Process(ins, outs, item.numSamples);
            mParallelDoneCount.fetch_add(1, std::memory_order_release);
        }

        // Spin-wait for all tasks to complete.
        while (mParallelDoneCount.load(std::memory_order_acquire) < wi)
        {
            CpuRelax();
        }

        // Mix all parallel outputs into the accumulator.
        for (int i = 0; i < wi; ++i)
        {
            mixInstance(*mWorkItems[static_cast<size_t>(i)].inst);
        }
    }
    else
    {
        // Serial path: single active preset or no worker threads available.
        for (auto& instPtr : mInstances)
        {
            auto& inst = *instPtr;

            if (!isAudible(inst))
            {
                continue;
            }

            float* presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
            inst.executor.Process(preChainOutputs, presetOutPtrs, numSamples);
            mixInstance(inst);
        }
    }

    // Advance swap ramps once per block, after every mix site, then drop any instance that
    // has finished fading out. Instances excluded from the mix above still advance so a
    // muted fade-out cannot get stuck holding its resources forever.
    for (auto& inst : mInstances)
    {
        if (inst->phase == InstancePhase::Active)
        {
            continue;
        }

        inst->fadeSamplesRemaining = std::max(0, inst->fadeSamplesRemaining - numSamples);

        if (inst->fadeSamplesRemaining == 0 && inst->phase == InstancePhase::FadingIn)
        {
            inst->phase = InstancePhase::Active;
            inst->fadeTotalSamples = 0;
        }
    }

    CollectFinishedFadeOuts();

    // ==========================================================================
    // MIX GAIN: the Multi-Rig's own level, applied to the summed preset mix ahead
    // of the global post-chain and the global output stage.
    // ==========================================================================
    const float mixGain = static_cast<float>(mMixGain);

    if (mixGain != 1.0f)
    {
        for (float* channel : {outputs[0], outputs[1]})
        {
            if (!channel)
            {
                continue;
            }

            for (int i = 0; i < numSamples; ++i)
            {
                channel[i] *= mixGain;
            }
        }
    }

    // ==========================================================================
    // GLOBAL POST-CHAIN: EQ → Doubler
    // ==========================================================================
    float* postChainOutputs[2] = {mPostChainOutL.data(), mPostChainOutR.data()};
    mPostChainExecutor.Process(outputs, postChainOutputs, numSamples);

    // Copy post-chain output back to main outputs
    if (outputs[0])
    {
        std::copy(mPostChainOutL.begin(), mPostChainOutL.begin() + numSamples, outputs[0]);
    }

    if (outputs[1])
    {
        std::copy(mPostChainOutR.begin(), mPostChainOutR.begin() + numSamples, outputs[1]);
    }

    // NOTE: preset swaps used to be masked by fading the master output up from zero here.
    // That could not hide the step down to silence when the outgoing chain was cut, and it
    // also ducked the global post-chain's own tail. The swap is now crossfaded per instance
    // in the preset mix above, so nothing is needed at this point.

    // ==========================================================================
    // FINAL OUTPUT STAGE: Master gain, auto-level, limiter
    // ==========================================================================

    // Apply master gain
    const float master = static_cast<float>(mMasterGain);

    if (master != 1.0f)
    {
        if (outputs[0])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outputs[0][i] *= master;
            }
        }

        if (outputs[1])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outputs[1][i] *= master;
            }
        }
    }

    // Apply auto-level output (simple peak limiting)
    if (mAutoLevelOutput)
    {
        const float outputProtectionCeilingLinear = static_cast<float>(GetOutputProtectionCeilingLinear());
        float peak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (outputs[0])
            {
                peak = std::max(peak, std::abs(outputs[0][i]));
            }

            if (outputs[1])
            {
                peak = std::max(peak, std::abs(outputs[1][i]));
            }
        }

        if (peak > outputProtectionCeilingLinear)
        {
            const float attenuation = outputProtectionCeilingLinear / peak;
            mOutputAutoLevelGain =
                mOutputAutoLevelGain * (1.0f - kAutoLevelAttackMix) + attenuation * kAutoLevelAttackMix;

            if (outputs[0])
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    outputs[0][i] *= mOutputAutoLevelGain;
                }
            }

            if (outputs[1])
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    outputs[1][i] *= mOutputAutoLevelGain;
                }
            }
        }
        else
        {
            // Slowly release gain reduction
            mOutputAutoLevelGain = std::min(1.0f, mOutputAutoLevelGain * kAutoLevelReleaseMultiplier);
        }
    }

    if (diagnosticsEnabled)
    {
        const auto stats =
            ComputeLevelStats(outputs ? outputs[0] : nullptr, outputs ? outputs[1] : nullptr, numSamples);
        mOutputLevels.peak.store(stats.peak, std::memory_order_relaxed);
        mOutputLevels.rms.store(stats.rms, std::memory_order_relaxed);
        mOutputLevels.clipCount.store(stats.clipCount, std::memory_order_relaxed);
    }

    // Optional simple limiter (clip)
    if (mLimiterEnabled)
    {
        const float outputProtectionCeilingLinear = static_cast<float>(GetOutputProtectionCeilingLinear());

        if (outputs[0])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outputs[0][i] =
                    std::clamp(outputs[0][i], -outputProtectionCeilingLinear, outputProtectionCeilingLinear);
            }
        }

        if (outputs[1])
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outputs[1][i] =
                    std::clamp(outputs[1][i], -outputProtectionCeilingLinear, outputProtectionCeilingLinear);
            }
        }
    }
}

void MultiPresetMixer::SetSignalDiagnosticsEnabled(bool enabled)
{
    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPreChainExecutor.SetSignalDiagnosticsEnabled(enabled);
    mPostChainExecutor.SetSignalDiagnosticsEnabled(enabled);

    for (auto& inst : mInstances)
    {
        inst->executor.SetSignalDiagnosticsEnabled(enabled);
    }
}

MultiPresetMixer::SignalDiagnosticsSnapshot MultiPresetMixer::GetSignalDiagnosticsSnapshot() const
{
    const auto readLevels = [](const AtomicLevelStats& source) {
        SignalLevelStats stats;
        stats.peak = source.peak.load(std::memory_order_relaxed);
        stats.rms = source.rms.load(std::memory_order_relaxed);
        stats.clipCount = source.clipCount.load(std::memory_order_relaxed);

        return stats;
    };

    // One executor reading becomes one snapshot node. The analyzer payload is the shared
    // AnalyzerTelemetry, so it moves across whole rather than field by field.
    const auto toSnapshotNode = [](const SignalGraphExecutor::NodeSignalLevel& entry, std::string_view scope,
                                   const std::string& presetId) {
        NodeSignalLevel node;
        node.scope = scope;
        node.presetId = presetId;
        node.nodeId = entry.nodeId;
        node.nodeType = entry.nodeType;
        node.channelCount = entry.channelCount;
        node.levels.peak = entry.peak;
        node.levels.rms = entry.rms;
        node.levels.clipCount = entry.clipCount;
        node.analyzer = entry.analyzer;

        return node;
    };

    SignalDiagnosticsSnapshot snapshot;
    snapshot.rawInput = readLevels(mRawInputLevels);
    snapshot.input = readLevels(mInputLevels);
    snapshot.output = readLevels(mOutputLevels);

    const auto preLevels = mPreChainExecutor.GetNodeSignalLevels();
    const auto postLevels = mPostChainExecutor.GetNodeSignalLevels();

    snapshot.nodes.reserve(preLevels.size() + postLevels.size() + mInstances.size() * 8);

    for (const auto& entry : preLevels)
    {
        snapshot.nodes.push_back(toSnapshotNode(entry, "pre", {}));
    }

    for (const auto& inst : mInstances)
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        for (const auto& entry : inst->executor.GetNodeSignalLevels())
        {
            snapshot.nodes.push_back(toSnapshotNode(entry, "preset", inst->cfg.id));
        }
    }

    for (const auto& entry : postLevels)
    {
        snapshot.nodes.push_back(toSnapshotNode(entry, "post", {}));
    }

    return snapshot;
}

std::vector<std::string> MultiPresetMixer::GetActivePresetIds() const
{
    std::vector<std::string> ids;
    ids.reserve(mInstances.size());

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring())
        {
            ids.push_back(inst->cfg.id);
        }
    }

    return ids;
}

std::vector<std::string> MultiPresetMixer::GetPresetNodeTypes(const std::string& presetId) const
{
    const auto* inst = FindInstance(presetId);

    if (!inst)
    {
        return {};
    }

    return inst->executor.GetNodeTypes();
}

std::optional<MultiPresetMixer::InstanceConfig> MultiPresetMixer::GetPresetConfig(const std::string& presetId) const
{
    if (const auto* inst = FindInstance(presetId))
    {
        return inst->cfg;
    }

    return std::nullopt;
}

MultiPresetMixer::PresetInstance* MultiPresetMixer::FindInstance(const std::string& id)
{
    // Retiring instances are invisible to lookups. Their ID often matches the incoming
    // one (a scene switch reuses the preset ID), so returning one would route parameter
    // updates into the chain that is on its way out.
    for (auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.id == id)
        {
            return inst.get();
        }
    }

    return nullptr;
}

const MultiPresetMixer::PresetInstance* MultiPresetMixer::FindInstance(const std::string& id) const
{
    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring() && inst->cfg.id == id)
        {
            return inst.get();
        }
    }

    return nullptr;
}

size_t MultiPresetMixer::GetPresetCount() const
{
    size_t count = 0;

    for (const auto& inst : mInstances)
    {
        if (!inst->IsRetiring())
        {
            ++count;
        }
    }

    return count;
}

void MultiPresetMixer::AllocateBuffers(int maxBlockSize)
{
    for (auto& inst : mInstances)
    {
        inst->outL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
        inst->outR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
        AllocateInstanceBuffers(*inst, maxBlockSize);
    }
}

void MultiPresetMixer::AllocateInstanceBuffers(PresetInstance& inst, int maxBlockSize)
{
    // Output buffers only - gate/pitch/doubler are now signal chain nodes
    (void)inst; // Nothing to allocate per-instance anymore
    (void)maxBlockSize;
}

void MultiPresetMixer::ComputePanGains(double pan, float& gL, float& gR)
{
    // Equal-power pan law
    // pan in [-1, 1] maps to theta in [0, pi/2]
    constexpr double kPi = 3.14159265358979323846;
    const double theta = (pan + 1.0) * (kPi * 0.25); // (-1)->0, 0->pi/4, 1->pi/2
    gL = static_cast<float>(std::cos(theta));
    gR = static_cast<float>(std::sin(theta));
}

SignalGraphExecutor::DSPPerformanceStats MultiPresetMixer::GetPerformanceStats() const
{
    SignalGraphExecutor::DSPPerformanceStats aggregatedStats;

    // Node ids only distinguish nodes inside one executor, so every id is rewritten to
    // `<scope>::<nodeId>` on the way in. Merging them unscoped would fold every
    // executor's `__input__` into one entry, and two mixer slots running the same preset
    // would collide on every node it has.
    const auto mergeStats = [&aggregatedStats](const SignalGraphExecutor::DSPPerformanceStats& stats,
                                               const std::string& scopedPrefix) {
        aggregatedStats.totalProcessingTimeUs += stats.totalProcessingTimeUs;
        aggregatedStats.realTimeUs = std::max(aggregatedStats.realTimeUs, stats.realTimeUs);

        for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
        {
            aggregatedStats.nodeProcessingTimesUs[scopedPrefix + nodeId] += timeUs;
        }

        for (const auto& [nodeId, latencySamples] : stats.nodeLatencySamples)
        {
            auto& slot = aggregatedStats.nodeLatencySamples[scopedPrefix + nodeId];
            slot = std::max(slot, latencySamples);
        }
    };

    mergeStats(mPreChainExecutor.GetPerformanceStats(), "pre::");

    for (const auto& instance : mInstances)
    {
        if (!instance->IsRetiring())
        {
            mergeStats(instance->executor.GetPerformanceStats(), instance->cfg.id + "::");
        }
    }

    mergeStats(mPostChainExecutor.GetPerformanceStats(), "post::");

    if (aggregatedStats.realTimeUs > 0.0)
    {
        aggregatedStats.dspLoadPercent = (aggregatedStats.totalProcessingTimeUs / aggregatedStats.realTimeUs) * 100.0;
    }

    return aggregatedStats;
}

int MultiPresetMixer::GetTotalLatencySamples() const
{
    const int preChain = mPreChainExecutor.GetTotalLatencySamples();
    const int postChain = mPostChainExecutor.GetTotalLatencySamples();

    int instanceMax = 0;

    for (const auto& inst : mInstances)
    {
        // A fading-out instance's latency must not leak into the reported figure: it is
        // about to disappear, and reporting it would make the host renegotiate PDC twice.
        if (!inst->IsRetiring())
        {
            instanceMax = std::max(instanceMax, inst->executor.GetTotalLatencySamples());
        }
    }

    return preChain + instanceMax + postChain;
}

void MultiPresetMixer::SetTunerEnabled(bool enabled)
{
    mTunerEnabled = enabled;
    const std::uint64_t generation = mTunerAnalysisGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (enabled)
    {
        // Reset tuner state when enabled
        StartTunerWorker();
        mTunerBuffer.resize(kTunerBufferSize, 0.0);
        std::fill(mTunerBuffer.begin(), mTunerBuffer.end(), 0.0);
        mTunerOrderedBuffer.resize(kTunerBufferSize, 0.0);
        mTunerAnalysisWriteBuffer.resize(kTunerBufferSize, 0.0);
        mTunerAnalysisReadBuffer.resize(kTunerBufferSize, 0.0);
        mTunerBufferWriteIndex = 0;
        mTunerSampleCounter = 0;
    }

    std::lock_guard<std::mutex> lock(mTunerAnalysisMutex);
    mTunerAnalysisPending = false;
    mTunerQueuedGeneration = generation;
    mTunerAnalysisReferenceFrequency = mTunerReferenceFrequency;
}

void MultiPresetMixer::SetTunerCallback(TunerCallback callback)
{
    std::lock_guard<std::mutex> lock(mTunerAnalysisMutex);
    mTunerCallback = std::move(callback);
}

void MultiPresetMixer::SetTunerReferenceFrequency(double frequency)
{
    mTunerReferenceFrequency = std::clamp(frequency, 400.0, 480.0);
}

void MultiPresetMixer::ProcessTuner(float** inputs, int numSamples)
{
    // Use the main input channel setting (same as DSP processing)
    const int ch = mInputChannel;

    if (!mTunerEnabled || !inputs || !inputs[ch])
    {
        return;
    }

    // Buffers are provisioned when the tuner is enabled; skip this update rather
    // than allocating on the audio thread if the state is incomplete.
    if (mTunerBuffer.size() != kTunerBufferSize || mTunerOrderedBuffer.size() != kTunerBufferSize)
    {
        return;
    }

    // Fill the tuner buffer with input samples (mono - use selected channel)
    for (int i = 0; i < numSamples; ++i)
    {
        mTunerBuffer[mTunerBufferWriteIndex] = static_cast<double>(inputs[ch][i]);
        mTunerBufferWriteIndex = (mTunerBufferWriteIndex + 1) % kTunerBufferSize;
        ++mTunerSampleCounter;
    }

    // Update tuner at regular intervals
    if (mTunerSampleCounter >= kTunerUpdateInterval)
    {
        mTunerSampleCounter = 0;

        // Reorder buffer to be contiguous for pitch detection (uses pre-allocated member, no heap alloc)
        for (std::size_t i = 0; i < kTunerBufferSize; ++i)
        {
            mTunerOrderedBuffer[i] = mTunerBuffer[(mTunerBufferWriteIndex + i) % kTunerBufferSize];
        }

        bool queuedForAnalysis = false;
        {
            std::unique_lock<std::mutex> lock(mTunerAnalysisMutex, std::try_to_lock);

            if (lock.owns_lock() && mTunerAnalysisWriteBuffer.size() == kTunerBufferSize)
            {
                std::copy(mTunerOrderedBuffer.begin(), mTunerOrderedBuffer.end(), mTunerAnalysisWriteBuffer.begin());
                mTunerAnalysisReferenceFrequency = mTunerReferenceFrequency;
                mTunerQueuedGeneration = mTunerAnalysisGeneration.load(std::memory_order_acquire);
                mTunerAnalysisPending = true;
                queuedForAnalysis = true;
            }
        }

        if (queuedForAnalysis)
        {
            mTunerAnalysisCv.notify_one();
        }
    }
}

double MultiPresetMixer::DetectPitch(const std::vector<double>& samples) const
{
    // Autocorrelation-based pitch detection (YIN-inspired algorithm)
    const std::size_t n = samples.size();

    if (n < 2)
    {
        return 0.0;
    }

    // Calculate RMS to check if there's enough signal
    double sumSquares = 0.0;

    for (const auto& sample : samples)
    {
        sumSquares += sample * sample;
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(n));

    // If signal is too quiet, don't try to detect pitch
    if (rms < 0.003)
    {
        return 0.0;
    }

    // Define search range for guitar: 50Hz (low tunings) to 1500Hz (F#6)
    const int minPeriod = static_cast<int>(mSampleRate / 1500.0); // Highest frequency
    const int maxPeriod = static_cast<int>(mSampleRate / 50.0);   // Lowest frequency

    if (maxPeriod >= static_cast<int>(n / 2) || minPeriod < 2)
    {
        return 0.0;
    }

    // Calculate difference function (YIN step 2)
    std::vector<double> diff(static_cast<std::size_t>(maxPeriod) + 1, 0.0);

    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
        double sum = 0.0;

        for (std::size_t i = 0; i < n - static_cast<std::size_t>(tau); ++i)
        {
            const double delta = samples[i] - samples[i + tau];
            sum += delta * delta;
        }

        diff[static_cast<std::size_t>(tau)] = sum;
    }

    // Cumulative mean normalized difference function (YIN step 4)
    std::vector<double> cmndf(static_cast<std::size_t>(maxPeriod) + 1, 1.0);
    double runningSum = 0.0;

    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
        runningSum += diff[static_cast<std::size_t>(tau)];

        if (runningSum > 0.0)
        {
            cmndf[static_cast<std::size_t>(tau)] =
                diff[static_cast<std::size_t>(tau)] * static_cast<double>(tau) / runningSum;
        }
    }

    // Find the first minimum below threshold (YIN step 5)
    constexpr double threshold = 0.15;
    int bestPeriod = -1;

    for (int tau = minPeriod; tau < maxPeriod; ++tau)
    {
        if (cmndf[static_cast<std::size_t>(tau)] < threshold)
        {
            // Find the local minimum
            while (tau + 1 <= maxPeriod &&
                   cmndf[static_cast<std::size_t>(tau + 1)] < cmndf[static_cast<std::size_t>(tau)])
            {
                ++tau;
            }

            bestPeriod = tau;
            break;
        }
    }

    // If no period found below threshold, find the global minimum
    if (bestPeriod < 0)
    {
        double minVal = cmndf[static_cast<std::size_t>(minPeriod)];
        bestPeriod = minPeriod;

        for (int tau = minPeriod + 1; tau <= maxPeriod; ++tau)
        {
            if (cmndf[static_cast<std::size_t>(tau)] < minVal)
            {
                minVal = cmndf[static_cast<std::size_t>(tau)];
                bestPeriod = tau;
            }
        }

        // If the minimum is too high, no pitch detected
        if (minVal > 0.5)
        {
            return 0.0;
        }
    }

    // Parabolic interpolation for sub-sample accuracy (YIN step 6)
    double period = static_cast<double>(bestPeriod);

    if (bestPeriod > minPeriod && bestPeriod < maxPeriod)
    {
        const double s0 = cmndf[static_cast<std::size_t>(bestPeriod - 1)];
        const double s1 = cmndf[static_cast<std::size_t>(bestPeriod)];
        const double s2 = cmndf[static_cast<std::size_t>(bestPeriod + 1)];
        const double denom = 2.0 * (2.0 * s1 - s0 - s2);

        if (std::abs(denom) > 1e-10)
        {
            period += (s2 - s0) / denom;
        }
    }

    return mSampleRate / period;
}

MultiPresetMixer::TunerResult MultiPresetMixer::FrequencyToNote(double frequency, double referenceFrequency) const
{
    TunerResult result;

    if (frequency < 20.0 || frequency > 20000.0)
    {
        result.detected = false;
        return result;
    }

    result.frequency = frequency;
    result.detected = true;

    // Calculate the number of semitones from A4 (reference frequency, typically 440 Hz)
    const double semitonesFromA4 = 12.0 * std::log2(frequency / referenceFrequency);

    // Round to nearest semitone
    const int nearestSemitone = static_cast<int>(std::round(semitonesFromA4));

    // Calculate the exact frequency of the nearest note
    const double nearestFrequency = referenceFrequency * std::pow(2.0, nearestSemitone / 12.0);

    // Calculate cents offset from the nearest note
    result.centOffset = 1200.0 * std::log2(frequency / nearestFrequency);

    // Clamp to reasonable range
    result.centOffset = std::clamp(result.centOffset, -50.0, 50.0);

    // Calculate note index (A4 is note 9 in octave 4, i.e., index 57 from C0)
    // A4 = 440Hz, note index = 9 (0=C, 1=C#, ..., 9=A)
    // Total semitone from C0 = nearestSemitone + 57 (A4 is 57 semitones above C0)
    const int totalSemitones = nearestSemitone + 57;

    // Handle negative semitones
    const int noteIndex = ((totalSemitones % 12) + 12) % 12;
    result.octave = (totalSemitones / 12);

    if (totalSemitones < 0 && totalSemitones % 12 != 0)
    {
        result.octave -= 1;
    }

    // Get note name with both sharp and flat
    const char* sharpName = kNoteNames[static_cast<std::size_t>(noteIndex)];
    const char* flatName = kNoteNamesFlat[static_cast<std::size_t>(noteIndex)];

    // Use combined notation for accidentals (e.g., "D#/Eb")
    if (std::string(sharpName) != std::string(flatName))
    {
        result.noteName = std::string(sharpName) + "/" + std::string(flatName);
    }
    else
    {
        result.noteName = sharpName;
    }

    // Calculate confidence based on how close we are to the note
    result.confidence = 1.0 - std::abs(result.centOffset) / 50.0;
    result.confidence = std::clamp(result.confidence, 0.0, 1.0);

    return result;
}
} // namespace guitarfx
