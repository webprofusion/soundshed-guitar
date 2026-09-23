#include "dsp/MultiPresetMixer.h"
#include "dsp/GlobalChainEditor.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
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

/// Can this graph still make a sound worth keeping once its input is cut? Only the
/// time-based families can, so only they earn a ring-out. Everything else — an amp, a cab,
/// an EQ, a synth voice with nothing left to track — is silent or, worse, still playing
/// within a few milliseconds of the switch, and running it on is the "old preset hangs on"
/// complaint rather than a tail.
///
/// Message thread only: it asks the registry for each type's category.
bool GraphCanRingOut(const std::vector<std::string>& nodeTypes)
{
    const auto& registry = EffectRegistry::Instance();

    for (const auto& type : nodeTypes)
    {
        // Opaque wrappers: the graph cannot see what a third-party plugin or a WASM module
        // does, and one of them being a reverb is likelier than the cost of being wrong.
        if (type == EffectGuids::kPluginHost || type == EffectGuids::kWasmHost)
        {
            return true;
        }

        const auto info = registry.GetTypeInfo(registry.Resolve(type));

        if (info && (info->category == "delay" || info->category == "reverb"))
        {
            return true;
        }
    }

    return false;
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

ExecutorSetup MultiPresetMixer::MakeExecutorSetup() const
{
    ExecutorSetup setup;
    setup.resourceLibrary = mResourceLibrary;
    setup.nodeTypeConfigDefaults = &mNodeTypeConfigDefaults;
    setup.signalDiagnostics = mTelemetry.IsEnabled();
    setup.prepared = mPrepared;
    setup.sampleRate = mSampleRate;
    setup.maxBlockSize = mMaxBlockSize;
    return setup;
}

GlobalChainEditor MultiPresetMixer::EditGlobalChain()
{
    return GlobalChainEditor(mGlobalChain.Config(), mGlobalChain.Pre(), mGlobalChain.Post());
}

std::unique_ptr<PresetInstance> MultiPresetMixer::BuildInstance(const Preset& preset, const std::string& id,
                                                                const std::string& name) const
{
    auto inst = std::make_unique<PresetInstance>();
    inst->cfg.id = id;
    inst->cfg.name = name;

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    inst->executor.SetResourceLibrary(mResourceLibrary);
    inst->executor.SeedNodeTypeConfigDefaults(mNodeTypeConfigDefaults);
    inst->executor.SetGraph(normalizedPreset.graph); // CreateProcessors + LoadResources here
    inst->executor.SetSignalDiagnosticsEnabled(mTelemetry.IsEnabled());
    inst->executor.SetNamInputModeMono(mMonoMode);
    inst->complexityScore = EstimateGraphComplexityScore(inst->executor.GetNodeTypes());
    inst->canRingOut = GraphCanRingOut(inst->executor.GetNodeTypesDeep());

    if (mPrepared)
    {
        inst->executor.Prepare(mSampleRate, mMaxBlockSize); // Effect Prepare() (NAM init, IR load) here
    }

    inst->ResizeBuffers(mMaxBlockSize);
    return inst;
}

bool MultiPresetMixer::AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name)
{
    // Avoid duplicate IDs. Instances that are fading out after a swap do not count —
    // their ID may legitimately match the one being added back.
    if (mVoices.Find(presetId) != nullptr)
    {
        return false;
    }

    mVoices.Install(BuildInstance(preset, presetId, name));
    return true;
}

MultiPresetMixer::MultiPresetMixer() : mTuner(std::make_unique<TunerEngine>())
{
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

    // The reaper thread and its retire queues stay with the object that owns them (as do the
    // parallel worker threads); only the DSP state moves.
    mResourceLibrary = other.mResourceLibrary;
    mVoices.TakeStateFrom(other.mVoices);
    mSampleRate = other.mSampleRate;
    mMaxBlockSize = other.mMaxBlockSize;
    mPrepared = other.mPrepared;
    mMixGainDb = other.mMixGainDb;
    mMixGain = other.mMixGain;
    mMasterGain = other.mMasterGain;
    mLimiterEnabled = other.mLimiterEnabled;
    mUserInputCalibrationGainDb = other.mUserInputCalibrationGainDb;
    mUserInputCalibrationGainLinear = other.mUserInputCalibrationGainLinear;
    mMonoMode = other.mMonoMode;
    mInputChannel = other.mInputChannel;
    mHostControlledInput = other.mHostControlledInput;
    mTempInL = std::move(other.mTempInL);
    mTempInR = std::move(other.mTempInR);
    mPreChainOutL = std::move(other.mPreChainOutL);
    mPreChainOutR = std::move(other.mPreChainOutR);
    mPostChainOutL = std::move(other.mPostChainOutL);
    mPostChainOutR = std::move(other.mPostChainOutR);
    mGlobalChain.TakeStateFrom(other.mGlobalChain);
    mTuner = std::move(other.mTuner);

    mTelemetry.CopyFrom(other.mTelemetry);

    return *this;
}

void MultiPresetMixer::SetUserInputCalibrationGainDb(double dB)
{
    const double clamped = IsFinite(dB) ? std::clamp(dB, -24.0, 24.0) : 0.0;
    mUserInputCalibrationGainDb = clamped;
    mUserInputCalibrationGainLinear = static_cast<float>(std::pow(10.0, clamped / 20.0));
}

void MultiPresetMixer::RemoveActivePreset(const std::string& presetId)
{
    mVoices.Remove(presetId);
}

bool MultiPresetMixer::RenameActivePreset(const std::string& oldId, const std::string& newId, const std::string& name)
{
    return mVoices.Rename(oldId, newId, name);
}

void MultiPresetMixer::PreparePresetSwap(const Preset& preset, const std::string& id, const std::string& name)
{
    // Build the new instance off the DSP lock. The audio thread continues processing the
    // current instances untouched while this runs.
    mVoices.Stage(BuildInstance(preset, id, name));
}

bool MultiPresetMixer::ReplaceActivePresetInPlace(const Preset& preset, const std::string& presetId,
                                                  const std::string& name)
{
    // Convenience for callers without concurrent audio. The controller uses the two
    // phases directly so preparation never holds its DSP lock.
    if (!mVoices.Find(presetId))
    {
        return false;
    }

    PreparePresetSwap(preset, presetId, name);
    return CommitPresetReplacement(presetId);
}

bool MultiPresetMixer::CommitPresetReplacement(const std::string& presetId)
{
    return mVoices.CommitReplacement(presetId);
}

bool MultiPresetMixer::CommitPresetAddition(const std::string& presetId)
{
    return mVoices.CommitAddition(presetId);
}

void MultiPresetMixer::SetPresetSwapTailSeconds(double seconds)
{
    mVoices.SetTailSeconds(seconds);
}

void MultiPresetMixer::CommitPresetSwap()
{
    // A swap plays one preset on its own. The Multi-Rig mix level belongs to the mix that
    // just went away, so a lone preset must not keep playing through its trim.
    if (mVoices.CommitSwap())
    {
        SetMixGainDb(0.0);
    }
}

void MultiPresetMixer::SetPresetMix(const std::string& presetId, double value)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->cfg.mix = std::clamp(value, 0.0, 1.0);
    }
}

void MultiPresetMixer::SetPresetPan(const std::string& presetId, double pan)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->cfg.pan = std::clamp(pan, -1.0, 1.0);
    }
}

void MultiPresetMixer::SetPresetMute(const std::string& presetId, bool mute)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->cfg.mute = mute;
    }
}

void MultiPresetMixer::SetPresetSolo(const std::string& presetId, bool solo)
{
    if (auto* inst = mVoices.Find(presetId))
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
    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetInputTrim(dB);
    }
}

void MultiPresetMixer::SetOutputTrim(double dB)
{
    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetOutputTrim(dB);
    }
}

void MultiPresetMixer::EnsureGlobalChainsUpToDate()
{
    if (mGlobalChain.EnsureUpToDate(MakeExecutorSetup()))
    {
        mMasterGain = std::pow(10.0, mGlobalChain.Config().outputGain / 20.0);
    }
}

// ==========================================================================
// Global Signal Chain Configuration
// ==========================================================================

void MultiPresetMixer::ApplyGlobalChainScalars(const GlobalSignalChainConfig& config)
{
    mMonoMode = mHostControlledInput ? false : config.monoMode;
    mInputChannel = config.inputChannel;
    mLimiterEnabled = config.limiterEnabled;
    mMasterGain = std::pow(10.0, config.outputGain / 20.0);
    mGlobalChain.Pre().SetInputTrim(config.inputGain);
}

void MultiPresetMixer::SetGlobalChainConfig(const GlobalSignalChainConfig& config)
{
    GlobalSignalChainConfig normalized = config;
    GlobalChainEditor::NormalizeConfig(normalized);

    mGlobalChain.Adopt(std::move(normalized));
    ApplyGlobalChainScalars(mGlobalChain.Config());
    EnsureGlobalChainsUpToDate();
}

bool MultiPresetMixer::PrepareGlobalChainSwap(const GlobalSignalChainConfig& config)
{
    GlobalSignalChainConfig normalized = config;
    GlobalChainEditor::NormalizeConfig(normalized);
    return mGlobalChain.PrepareSwap(std::move(normalized), MakeExecutorSetup());
}

void MultiPresetMixer::CommitGlobalChainSwap()
{
    // After the swap so the input trim lands on the executor that is now live.
    if (mGlobalChain.CommitSwap())
    {
        ApplyGlobalChainScalars(mGlobalChain.Config());
    }
}

void MultiPresetMixer::SetGlobalGateEnabled(bool enabled)
{
    EditGlobalChain().SetGateEnabled(enabled);
}

void MultiPresetMixer::SetGlobalGateThreshold(double thresholdDb)
{
    EditGlobalChain().SetGateThreshold(thresholdDb);
}

void MultiPresetMixer::SetGlobalGateAttack(double attackMs)
{
    EditGlobalChain().SetGateAttack(attackMs);
}

void MultiPresetMixer::SetGlobalGateHold(double holdMs)
{
    EditGlobalChain().SetGateHold(holdMs);
}

void MultiPresetMixer::SetGlobalGateRelease(double releaseMs)
{
    EditGlobalChain().SetGateRelease(releaseMs);
}

void MultiPresetMixer::SetGlobalGateHysteresis(double hysteresisDb)
{
    EditGlobalChain().SetGateHysteresis(hysteresisDb);
}

void MultiPresetMixer::SetGlobalGateRange(double rangeDb)
{
    EditGlobalChain().SetGateRange(rangeDb);
}

void MultiPresetMixer::SetGlobalGateStereoLink(bool linked)
{
    EditGlobalChain().SetGateStereoLink(linked);
}

void MultiPresetMixer::SetGlobalTransposeEnabled(bool enabled)
{
    EditGlobalChain().SetTransposeEnabled(enabled);
}

void MultiPresetMixer::SetGlobalTranspose(int semitones)
{
    EditGlobalChain().SetTranspose(semitones);
}

void MultiPresetMixer::SetGlobalEQEnabled(bool enabled)
{
    EditGlobalChain().SetEQEnabled(enabled);
}

void MultiPresetMixer::SetGlobalEQBandGain(int band, double dB)
{
    EditGlobalChain().SetEQBandGain(band, dB);
}

void MultiPresetMixer::SetGlobalEQBandFrequency(int band, double freq)
{
    EditGlobalChain().SetEQBandFrequency(band, freq);
}

void MultiPresetMixer::SetGlobalEQBandQ(int band, double q)
{
    EditGlobalChain().SetEQBandQ(band, q);
}

void MultiPresetMixer::SetGlobalDoublerEnabled(bool enabled)
{
    EditGlobalChain().SetDoublerEnabled(enabled);
}

void MultiPresetMixer::SetGlobalDoublerDelay(double delayMs)
{
    EditGlobalChain().SetDoublerDelay(delayMs);
}

void MultiPresetMixer::SetGlobalDoublerMix(double mix)
{
    EditGlobalChain().SetDoublerMix(mix);
}

void MultiPresetMixer::SetGlobalDoublerDetune(double cents)
{
    EditGlobalChain().SetDoublerDetune(cents);
}

void MultiPresetMixer::SetGlobalInputGain(double dB)
{
    EditGlobalChain().SetInputGain(dB);
}

void MultiPresetMixer::SetGlobalOutputGain(double dB)
{
    mMasterGain = EditGlobalChain().SetOutputGain(dB);
}

// Node-level control methods
void MultiPresetMixer::SetNodeEnabled(const std::string& presetId, const std::string& nodeId, bool enabled)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->executor.SetNodeEnabled(nodeId, enabled);
    }
}

void MultiPresetMixer::SetNodeParam(const std::string& presetId, const std::string& nodeId, const std::string& key,
                                    double value)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->executor.SetNodeParam(nodeId, key, value);
    }
}

void MultiPresetMixer::SetNodeConfig(const std::string& presetId, const std::string& nodeId, const std::string& key,
                                     const std::string& value)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        inst->executor.SetNodeConfig(nodeId, key, value);
    }
}

EffectProcessor* MultiPresetMixer::RecordNodeConfig(const std::string& presetId, const std::string& nodeId,
                                                    const std::string& key, const std::string& value)
{
    auto* inst = mVoices.Find(presetId);
    return inst ? inst->executor.RecordNodeConfig(nodeId, key, value) : nullptr;
}

void MultiPresetMixer::SetNodeConfigForType(const std::string& type, const std::string& key, const std::string& value)
{
    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetNodeConfigForType(type, key, value);
    }

    mGlobalChain.Pre().SetNodeConfigForType(type, key, value);
    mGlobalChain.Post().SetNodeConfigForType(type, key, value);
}

void MultiPresetMixer::SetNodeTypeConfigDefault(const std::string& type, const std::string& key,
                                                const std::string& value)
{
    mNodeTypeConfigDefaults[type][key] = value;

    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetNodeTypeConfigDefault(type, key, value);
    }

    mGlobalChain.Pre().SetNodeTypeConfigDefault(type, key, value);
    mGlobalChain.Post().SetNodeTypeConfigDefault(type, key, value);
}

std::optional<std::pair<std::string, std::string>> MultiPresetMixer::FindFirstEnabledNodeOfType(
    const std::string& effectType) const
{
    for (const auto& inst : mVoices.Instances())
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

    const PresetVoicePool::ReadScope readScope(mVoices);

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

    collect(mGlobalChain.Pre(), "pre", std::string{});

    for (const auto& inst : mVoices.Instances())
    {
        if (!inst->IsRetiring())
        {
            collect(inst->executor, "preset", inst->cfg.id);
        }
    }

    collect(mGlobalChain.Post(), "post", std::string{});

    return readouts;
}

bool MultiPresetMixer::SetNodeEnabledByType(const std::string& effectType, bool enabled)
{
    return SetAutomatedNodesEnabled(EffectRegistry::Instance().Resolve(effectType), enabled);
}

bool MultiPresetMixer::SetNodeParamByType(const std::string& effectType, const std::string& paramId, double value)
{
    const auto target = FindAutomationTarget(EffectRegistry::Instance().Resolve(effectType));

    if (!target)
    {
        return false;
    }

    SignalGraphExecutor::SetAutomationTargetParam(target, paramId, value);
    return true;
}

SignalGraphExecutor::AutomationTarget MultiPresetMixer::FindAutomationTarget(const std::string& canonicalType)
{
    for (const auto& inst : mVoices.Instances())
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        if (const auto target = inst->executor.FindAutomationTarget(canonicalType))
        {
            return target;
        }
    }

    return {};
}

bool MultiPresetMixer::SetAutomatedNodesEnabled(const std::string& canonicalType, bool enabled)
{
    bool updated = false;

    for (const auto& inst : mVoices.Instances())
    {
        if (!inst->IsRetiring())
        {
            updated = inst->executor.SetAutomatedNodesEnabled(canonicalType, enabled) || updated;
        }
    }

    return updated;
}

std::string MultiPresetMixer::GetNodeConfig(const std::string& presetId, const std::string& nodeId,
                                            const std::string& key) const
{
    if (const auto* inst = mVoices.Find(presetId))
    {
        return inst->executor.GetNodeConfig(nodeId, key);
    }

    return {};
}

EffectProcessor* MultiPresetMixer::GetNodeProcessor(const std::string& presetId, const std::string& nodeId)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
}

const EffectProcessor* MultiPresetMixer::GetNodeProcessor(const std::string& presetId, const std::string& nodeId) const
{
    if (const auto* inst = mVoices.Find(presetId))
    {
        return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
}

void MultiPresetMixer::SetTempo(double bpm)
{
    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetTempo(bpm);
    }

    mGlobalChain.Pre().SetTempo(bpm);
    mGlobalChain.Post().SetTempo(bpm);
}

bool MultiPresetMixer::LoadNodeResource(const std::string& presetId, const std::string& nodeId, const ResourceRef& ref)
{
    if (auto* inst = mVoices.Find(presetId))
    {
        return inst->executor.LoadNodeResource(nodeId, ref);
    }

    return false;
}

void MultiPresetMixer::TakeDeferredRebuilds(std::vector<GraphRebuildWork>& out)
{
    for (auto& inst : mVoices.Instances())
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        if (auto work = inst->executor.TakeDeferredRebuilds())
        {
            out.push_back({&inst->executor, std::move(work)});
        }
    }

    for (SignalGraphExecutor* executor : {&mGlobalChain.Pre(), &mGlobalChain.Post()})
    {
        if (auto work = executor->TakeDeferredRebuilds())
        {
            out.push_back({executor, std::move(work)});
        }
    }
}

void MultiPresetMixer::CommitDeferredRebuilds(std::vector<GraphRebuildWork>& rebuilds)
{
    // Slots are only installed and retired by the message thread, which is the thread doing this,
    // so a graph is still running unless it started retiring. Each node checks it still runs the
    // processor its work came from.
    const auto isRunning = [this](const SignalGraphExecutor* executor) {
        if (executor == &mGlobalChain.Pre() || executor == &mGlobalChain.Post())
        {
            return true;
        }

        return std::any_of(mVoices.Instances().begin(), mVoices.Instances().end(),
                           [executor](const auto& inst) { return &inst->executor == executor && !inst->IsRetiring(); });
    };

    for (auto& rebuild : rebuilds)
    {
        if (isRunning(rebuild.executor))
        {
            rebuild.executor->CommitDeferredRebuilds(*rebuild.work);
        }
    }
}

// ---------------------------------------------------------------------------
// Destructor / parallel worker lifecycle
// ---------------------------------------------------------------------------

MultiPresetMixer::~MultiPresetMixer()
{
    // Stop tuner callbacks before any other mixer state begins tearing down.
    mTuner.reset();
    StopWorkers();
    mReaper.Stop();
}

void MultiPresetMixer::CollectRetiredMainThread()
{
    mReaper.CollectMainThread();
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

    mVoices.Prepare(sampleRate);

    // Bring the reaper up before any swap can happen, so retiring never has to spawn a
    // thread while the DSP lock is held.
    mReaper.Start();

    // Allocate global temp buffers
    mTempInL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTempInR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTuner->Prepare(sampleRate);

    // Build and prepare global signal chains based on current config
    mGlobalChain.MarkNeedsRebuild();
    EnsureGlobalChainsUpToDate();

    AllocateBuffers(maxBlockSize);

    for (auto& inst : mVoices.Instances())
    {
        inst->executor.Prepare(sampleRate, maxBlockSize);
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
    mGlobalChain.Pre().Reset();
    mGlobalChain.Post().Reset();

    for (auto& inst : mVoices.Instances())
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
    // buffer is always written. Telemetry counts these violations for the host.
    if (mPrepared && mMaxBlockSize > 0 && numSamples > mMaxBlockSize)
    {
        mTelemetry.NoteOversizedBlock();

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

    const bool diagnosticsEnabled = mTelemetry.IsEnabled();

    // NOTE: Do NOT call EnsureGlobalChainsUpToDate() here.
    // Rebuilding global chains allocates memory which is unsafe on the audio thread.
    // The chains are rebuilt from Prepare() and SetGlobalChainConfig() on the UI/main thread.

    // Safety check: ensure we're prepared before processing
    if (!mPrepared || mVoices.Instances().empty())
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
        mTelemetry.Record(MixerTelemetry::Stage::RawInput, processInL, processInR, numSamples);
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

    if (diagnosticsEnabled)
    {
        mTelemetry.Record(MixerTelemetry::Stage::Input, processInL, processInR, numSamples);
    }

    // Process tuner FIRST (before any processing, uses raw input for accurate pitch detection)
    if (mTuner->IsEnabled())
    {
        float* tunerInputs[2] = {processInL, processInR};
        mTuner->Process(tunerInputs[mInputChannel], numSamples);

        // If not in live tuner mode, mute the output
        if (!mTuner->IsLiveMode())
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
    mGlobalChain.Pre().SetNamInputModeMono(namInputModeMono);
    mGlobalChain.Post().SetNamInputModeMono(namInputModeMono);

    // ==========================================================================
    // GLOBAL PRE-CHAIN: Input → Noise Gate → Transpose
    // ==========================================================================
    float* preChainInputs[2] = {processInL, processInR};
    float* preChainOutputs[2] = {mPreChainOutL.data(), mPreChainOutR.data()};
    mGlobalChain.Pre().Process(preChainInputs, preChainOutputs, numSamples);

    // ==========================================================================
    // PRESET PROCESSING: Process each active preset and mix
    // ==========================================================================

    // Detect solo mode. Instances fading out after a swap never participate in the solo
    // decision — they are on their way out and must stay audible for the whole ramp.
    bool anySolo = false;

    for (const auto& inst : mVoices.Instances())
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

        if (inst.phase == InstancePhase::Tailing)
        {
            // Ringing out. Its own decay is what ends it, not a gain ramp.
            return true;
        }

        if (inst.IsRetiring())
        {
            // Fully faded out but not yet handed to the reaper (the queue was busy last
            // block). It contributes nothing, so skip the chain entirely.
            return inst.fadeSamplesRemaining > 0;
        }

        return !anySolo || inst.cfg.solo;
    };

    // What an instance reads this block. Everything live shares the pre-chain output; a
    // tailing instance gets its own copy, ramped to zero, so no new playing enters a chain
    // the player has already switched away from — only what is still circulating in it.
    const auto instanceInputs = [&](PresetInstance& inst) -> std::array<float*, 2> {
        if (!inst.tailInput)
        {
            return {mPreChainOutL.data(), mPreChainOutR.data()};
        }

        inst.FillTailInput(mPreChainOutL.data(), mPreChainOutR.data(), numSamples);
        return {inst.tailInL.data(), inst.tailInR.data()};
    };

    // Mix one processed instance into the accumulator, applying its mixer gain, pan and
    // (when it is mid-swap) a linear fade ramp across the block.
    const auto mixInstance = [&](PresetInstance& inst) {
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

    // Count live instances to decide whether to use parallel dispatch. Deliberately *not*
    // the ones on their way out: a tail is a passenger, not a reason to fan out, and letting
    // one flip a single-preset session onto the parallel path put a spin-wait on
    // normal-priority workers into every switch — with the DSP lock held, so a worker that
    // did not get scheduled promptly hung the whole app. A retiring instance still rides
    // along once the live set has earned the fan-out on its own.
    int liveCount = 0;

    for (const auto& inst : mVoices.Instances())
    {
        if (!inst->IsRetiring() && isAudible(*inst))
        {
            ++liveCount;
        }
    }

    int totalWorkUnits = 0;

    if (liveCount >= 2)
    {
        for (const auto& inst : mVoices.Instances())
        {
            if (inst->IsRetiring() || !isAudible(*inst))
            {
                continue;
            }

            totalWorkUnits += inst->complexityScore * numSamples;
        }
    }

    const bool useParallel =
        ShouldUseParallelPresetDispatch(mMultiThreadedProcessingEnabled.load(std::memory_order_acquire), liveCount,
                                        totalWorkUnits, !mWorkerThreads.empty());

    // Avoid nested parallelism: if mixer-level fan-out is active, run each preset graph serially.
    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetNamInputModeMono(namInputModeMono);
        inst->executor.SetParallelLevelsEnabled(!useParallel);
    }

    if (useParallel)
    {
        // Pack work items (up to kMaxWorkItems); any extras fall through to serial below.
        int wi = 0;

        for (auto& instPtr : mVoices.Instances())
        {
            auto& inst = *instPtr;

            if (!isAudible(inst))
            {
                continue;
            }

            // Resolved on this thread before the tasks are published: a worker must never
            // be the one writing a tail's ramped input.
            const auto ins = instanceInputs(inst);

            if (wi < kMaxWorkItems)
            {
                mWorkItems[static_cast<size_t>(wi)] = {&inst, ins[0], ins[1], numSamples};
                ++wi;
            }
            else
            {
                // Overflow beyond kMaxWorkItems: process serially and mix immediately.
                float* presetInPtrs[2] = {ins[0], ins[1]};
                float* presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
                inst.executor.Process(presetInPtrs, presetOutPtrs, numSamples);
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

        // Wait for all tasks to complete. A worker that has not been scheduled yet cannot be
        // waited out by spinning: this thread holds the DSP lock, so starving it against a
        // normal-priority worker hangs every message-thread handler behind the lock, not just
        // this block. Spin first — that is the whole point on the fast path — but back off to
        // a real yield once spinning has clearly not been enough, so the worker can run.
        constexpr int kSpinsBeforeYield = 10000;
        int spins = 0;

        while (mParallelDoneCount.load(std::memory_order_acquire) < wi)
        {
            if (++spins < kSpinsBeforeYield)
            {
                CpuRelax();
            }
            else
            {
                std::this_thread::yield();
            }
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
        for (auto& instPtr : mVoices.Instances())
        {
            auto& inst = *instPtr;

            if (!isAudible(inst))
            {
                continue;
            }

            const auto ins = instanceInputs(inst);
            float* presetInPtrs[2] = {ins[0], ins[1]};
            float* presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
            inst.executor.Process(presetInPtrs, presetOutPtrs, numSamples);
            mixInstance(inst);
        }
    }

    // Advance swap ramps once per block, after every mix site, then drop any instance that
    // has finished fading out.
    mVoices.AdvanceRamps(numSamples);
    mVoices.CollectFinishedFadeOuts();

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
    mGlobalChain.Post().Process(outputs, postChainOutputs, numSamples);

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
    // FINAL OUTPUT STAGE: Master gain, limiter
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

    if (diagnosticsEnabled)
    {
        mTelemetry.Record(MixerTelemetry::Stage::Output, outputs ? outputs[0] : nullptr,
                          outputs ? outputs[1] : nullptr, numSamples);
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
    const PresetVoicePool::ReadScope readScope(mVoices);
    mTelemetry.SetEnabled(enabled);
    mGlobalChain.Pre().SetSignalDiagnosticsEnabled(enabled);
    mGlobalChain.Post().SetSignalDiagnosticsEnabled(enabled);

    for (auto& inst : mVoices.Instances())
    {
        inst->executor.SetSignalDiagnosticsEnabled(enabled);
    }
}

MultiPresetMixer::SignalDiagnosticsSnapshot MultiPresetMixer::GetSignalDiagnosticsSnapshot() const
{
    const PresetVoicePool::ReadScope readScope(mVoices);
    SignalDiagnosticsSnapshot snapshot = mTelemetry.GetSnapshot();

    const auto preLevels = mGlobalChain.Pre().GetNodeSignalLevels();
    const auto postLevels = mGlobalChain.Post().GetNodeSignalLevels();

    snapshot.nodes.reserve(preLevels.size() + postLevels.size() + mVoices.Instances().size() * 8);

    for (const auto& entry : preLevels)
    {
        snapshot.nodes.push_back(MixerTelemetry::ToSnapshotNode(entry, "pre", {}));
    }

    for (const auto& inst : mVoices.Instances())
    {
        if (inst->IsRetiring())
        {
            continue;
        }

        for (const auto& entry : inst->executor.GetNodeSignalLevels())
        {
            snapshot.nodes.push_back(MixerTelemetry::ToSnapshotNode(entry, "preset", inst->cfg.id));
        }
    }

    for (const auto& entry : postLevels)
    {
        snapshot.nodes.push_back(MixerTelemetry::ToSnapshotNode(entry, "post", {}));
    }

    return snapshot;
}

bool MultiPresetMixer::ReadNodeSpectrum(std::string_view scope, const std::string& presetId, const std::string& nodeId,
                                        SpectrumTap::Bins& out)
{
    const PresetVoicePool::ReadScope readScope(mVoices);
    SignalGraphExecutor* executor = nullptr;

    if (scope == "pre")
    {
        executor = &mGlobalChain.Pre();
    }
    else if (scope == "post")
    {
        executor = &mGlobalChain.Post();
    }
    else if (scope == "preset")
    {
        for (auto& inst : mVoices.Instances())
        {
            if (!inst->IsRetiring() && (presetId.empty() || inst->cfg.id == presetId))
            {
                executor = &inst->executor;
                break;
            }
        }
    }

    // Watching again on every read is what carries the tap onto an executor built since the
    // last one -- a preset swap or a global chain rebuild replaces the executor outright.
    return executor && executor->WatchNodeSpectrum(nodeId) && executor->ReadWatchedSpectrum(out);
}

void MultiPresetMixer::ClearSpectrumTaps()
{
    const PresetVoicePool::ReadScope readScope(mVoices);
    mGlobalChain.Pre().ClearSpectrumWatch();
    mGlobalChain.Post().ClearSpectrumWatch();

    for (auto& inst : mVoices.Instances())
    {
        inst->executor.ClearSpectrumWatch();
    }
}

std::vector<std::string> MultiPresetMixer::GetActivePresetIds() const
{
    std::vector<std::string> ids;
    ids.reserve(mVoices.Instances().size());

    for (const auto& inst : mVoices.Instances())
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
    const auto* inst = mVoices.Find(presetId);

    if (!inst)
    {
        return {};
    }

    return inst->executor.GetNodeTypes();
}

std::optional<MultiPresetMixer::InstanceConfig> MultiPresetMixer::GetPresetConfig(const std::string& presetId) const
{
    if (const auto* inst = mVoices.Find(presetId))
    {
        return inst->cfg;
    }

    return std::nullopt;
}

size_t MultiPresetMixer::GetPresetCount() const
{
    return mVoices.LiveCount();
}

size_t MultiPresetMixer::GetRetiringPresetCount() const
{
    return mVoices.RetiringCount();
}

void MultiPresetMixer::AllocateBuffers(int maxBlockSize)
{
    for (auto& inst : mVoices.Instances())
    {
        inst->ResizeBuffers(maxBlockSize);
    }
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
    const PresetVoicePool::ReadScope readScope(mVoices);
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

    mergeStats(mGlobalChain.Pre().GetPerformanceStats(), "pre::");

    for (const auto& instance : mVoices.Instances())
    {
        if (!instance->IsRetiring())
        {
            mergeStats(instance->executor.GetPerformanceStats(), instance->cfg.id + "::");
        }
    }

    mergeStats(mGlobalChain.Post().GetPerformanceStats(), "post::");

    if (aggregatedStats.realTimeUs > 0.0)
    {
        aggregatedStats.dspLoadPercent = (aggregatedStats.totalProcessingTimeUs / aggregatedStats.realTimeUs) * 100.0;
    }

    return aggregatedStats;
}

int MultiPresetMixer::GetTotalLatencySamples() const
{
    const PresetVoicePool::ReadScope readScope(mVoices);
    const int preChain = mGlobalChain.Pre().GetTotalLatencySamples();
    const int postChain = mGlobalChain.Post().GetTotalLatencySamples();

    int instanceMax = 0;

    for (const auto& inst : mVoices.Instances())
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
    mTuner->SetEnabled(enabled);
}

void MultiPresetMixer::SetTunerCallback(TunerCallback callback)
{
    mTuner->SetCallback(std::move(callback));
}

void MultiPresetMixer::SetTunerReferenceFrequency(double frequency)
{
    mTuner->SetReferenceFrequency(frequency);
}

} // namespace guitarfx
