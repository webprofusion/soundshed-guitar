#pragma once

#include "presets/PresetTypes.h"
#include "dsp/DspReaper.h"
#include "dsp/EffectProcessor.h"
#include "dsp/GlobalChainEngine.h"
#include "dsp/PresetInstance.h"
#include "dsp/PresetVoicePool.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalTelemetry.h"
#include "dsp/MixerTelemetry.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/TunerEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <limits>
#include <utility>
#include <vector>

namespace guitarfx
{
class GlobalChainEditor;
class ResourceLibrary;

/**
 * Central DSP manager that runs multiple presets in parallel and mixes their outputs.
 * Supports per-preset mix level, mute/solo, stereo panning, and per-preset global FX.
 * Also handles the global input settings: mono/stereo mode, input channel and calibration gain.
 *
 * The parts with a lifetime of their own are separate classes it composes: PresetVoicePool
 * (preset instances from install to retirement, and the swap rules), GlobalChainEngine (the
 * global pre/post executors, rebuilt and swapped), DspReaper (where retired chains are
 * destroyed), TunerEngine and MixerTelemetry. What is left here is the per-block signal
 * flow, the mixer's scalar settings, and the routing of node edits to the right executor.
 */
class MultiPresetMixer
{
  public:
    using InstanceConfig = PresetInstanceConfig;

    using TunerResult = TunerEngine::Result;

    using SignalLevelStats = MixerTelemetry::LevelStats;
    using NodeSignalLevel = MixerTelemetry::NodeSignalLevel;
    using SignalDiagnosticsSnapshot = MixerTelemetry::Snapshot;

    using TunerCallback = TunerEngine::Callback;

    MultiPresetMixer();
    ~MultiPresetMixer();
    MultiPresetMixer(const MultiPresetMixer&) = delete;
    MultiPresetMixer& operator=(const MultiPresetMixer&) = delete;
    MultiPresetMixer(MultiPresetMixer&& other) noexcept;
    MultiPresetMixer& operator=(MultiPresetMixer&& other) noexcept;

    void SetResourceLibrary(ResourceLibrary* library)
    {
        mResourceLibrary = library;
    }

    [[nodiscard]] ResourceLibrary* GetResourceLibrary() const
    {
        return mResourceLibrary;
    }

    // Add/Remove instances
    bool AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name);
    void RemoveActivePreset(const std::string& presetId);

    // Re-keys an already-active slot, e.g. after "save as" mints a new preset id for the
    // preset a slot is already running. Metadata only — the executor, its processors and
    // every hosted plugin instance keep running untouched. Returns false if oldId is not
    // active or newId is already taken. A no-op (true) when the ids are equal.
    bool RenameActivePreset(const std::string& oldId, const std::string& newId, const std::string& name);

    // Single-threaded convenience: rebuilds a single already-active instance (e.g. after a
    // scene switch or in-place edit of a preset that happens to be one of several active
    // mixer slots), preserving that slot's mix/pan/mute/solo. Concurrent audio callers use
    // PreparePresetSwap()/CommitPresetReplacement() instead. Unlike PreparePresetSwap()/
    // CommitPresetSwap(), this does NOT touch any other active instance. Returns false
    // (no-op) if presetId is not currently active.
    bool ReplaceActivePresetInPlace(const Preset& preset, const std::string& presetId, const std::string& name);
    // Two-phase in-place replacement: PreparePresetSwap off the DSP lock, then this
    // commit under it. Mixer controls are read at commit time; other slots stay live.
    bool CommitPresetReplacement(const std::string& presetId);
    // Two-phase AddActivePreset: PreparePresetSwap off the DSP lock, then this commit under
    // it installs the staged instance as one more slot alongside the others, at full gain as
    // AddActivePreset does. False, with the instance left staged, if it was not staged for
    // presetId or a live slot already answers to that id.
    bool CommitPresetAddition(const std::string& presetId);

    // Message thread only, without the DSP lock. Hosted processors (including composites)
    // must be destroyed here rather than on a reaper that may wait for their editor UI.
    void CollectRetiredMainThread();

    // Seamless preset swap: build the new executor off the DSP lock, then commit atomically.
    // PreparePresetSwap does the expensive work (effect creation, resource loading, Prepare).
    // CommitPresetSwap installs the pre-built instance, fades it in, and hands the outgoing
    // instance to the tail spill (or, with spill off, fades it out over the same window) so
    // the transition has no step discontinuity.
    // Pattern: call PreparePresetSwap() without holding the DSP lock, then hold the lock
    // and call CommitPresetSwap().
    void PreparePresetSwap(const Preset& preset, const std::string& id, const std::string& name);
    void CommitPresetSwap();

    // ── Tail spill ──────────────────────────────────────────────────────────────────
    // How long an outgoing preset may keep ringing after a swap, in seconds. The outgoing
    // chain has its *input* ramped to zero over the declick window and its output left
    // alone, so nothing new enters it but whatever is already circulating in its delay
    // lines and reverb tanks rings out over the top of the incoming preset. Only a graph
    // that could still sound with its input cut is offered a ring-out at all (see
    // PresetInstance::canRingOut). Output silence alone cannot prove its delay lines are
    // empty, so a tail-capable chain runs until the budget and release end.
    //
    // The trade-off, deliberate: for the length of that input ramp the outgoing output is not
    // attenuated, so a chain that compresses hard does not fall away as fast as its input does
    // and the two presets can briefly sum a little above unity. On a linear chain they sum to
    // unity exactly. A shorter ramp trades that against the click the ramp exists to prevent.
    //
    // 0 disables the whole thing and restores the plain declick crossfade. The caller sets
    // this from the user's setting and the current tempo before every swap, which is what
    // keeps "two bars" meaning two bars at the tempo the player is actually on.
    void SetPresetSwapTailSeconds(double seconds);

    [[nodiscard]] double GetPresetSwapTailSeconds() const noexcept
    {
        return mVoices.GetTailSeconds();
    }

    // Ceiling on the above. A runaway feedback delay never decays on its own, so the hold
    // is what stops one ringing under the next three songs.
    static constexpr double kMaxPresetSwapTailSeconds = PresetVoicePool::kMaxTailSeconds;

    // Global chain swap, same two-phase pattern as the preset swap above.
    // PrepareGlobalChainSwap normalizes the config and — only if it actually differs from the
    // live one — builds replacement pre/post executors off the DSP lock. CommitGlobalChainSwap
    // installs them and applies the scalar input/output settings. Both are cheap no-ops when
    // the incoming config matches what is already running, which is the common case: global
    // settings do not come from presets, so most preset loads pass an identical config.
    // Returns true if a rebuild was staged.
    bool PrepareGlobalChainSwap(const GlobalSignalChainConfig& config);
    void CommitGlobalChainSwap();

    // Per-preset mixing controls
    void SetPresetMix(const std::string& presetId, double value);
    void SetPresetPan(const std::string& presetId, double pan);
    void SetPresetMute(const std::string& presetId, bool mute);
    void SetPresetSolo(const std::string& presetId, bool solo);

    // The Multi-Rig's own level: applied to the summed preset mix ahead of the global
    // post-chain and the global output stage, and saved with the mix. Independent of the
    // global output gain, which SetGlobalOutputGain()/SetMasterGain() drive.
    void SetMixGainDb(double dB)
    {
        mMixGainDb = std::clamp(dB, kMinMixGainDb, kMaxMixGainDb);
        mMixGain = std::pow(10.0, mMixGainDb / 20.0);
    }

    [[nodiscard]] double GetMixGainDb() const
    {
        return mMixGainDb;
    }

    // Engine-side safety bounds, deliberately wider than the knob's own -24..+12 dB range
    // (MIX_GAIN_MIN_DB/MIX_GAIN_MAX_DB in multiPresetMixerSupport.ts): these only exist to
    // reject a nonsense value arriving from a file or a message.
    static constexpr double kMinMixGainDb = -60.0;
    static constexpr double kMaxMixGainDb = 24.0;

    // Master/global controls
    void SetMasterGain(double value)
    {
        mMasterGain = value;
    }

    // The global output limiter. ApplyGlobalChainScalars() re-reads this out of the chain
    // config on every rebuild, so the config has to move with it — otherwise a preset load
    // snaps the limiter back to whatever the config was last built with.
    void SetLimiterEnabled(bool enabled)
    {
        mLimiterEnabled = enabled;
        mGlobalChain.Config().limiterEnabled = enabled;
    }

    [[nodiscard]] double GetMasterGain() const
    {
        return mMasterGain;
    }

    [[nodiscard]] bool IsLimiterEnabled() const
    {
        return mLimiterEnabled;
    }

    void SetMultiThreadedProcessingEnabled(bool enabled);

    [[nodiscard]] bool IsMultiThreadedProcessingEnabled() const noexcept
    {
        return mMultiThreadedProcessingEnabled.load(std::memory_order_acquire);
    }

    // Global input settings
    void SetUserInputCalibrationGainDb(double dB);

    [[nodiscard]] double GetUserInputCalibrationGainDb() const
    {
        return mUserInputCalibrationGainDb;
    }

    void SetMonoMode(bool mono)
    {
        mMonoMode = mHostControlledInput ? false : mono;
    }

    void SetInputChannel(int channel)
    {
        mInputChannel = std::clamp(channel, 0, 1);
    }

    [[nodiscard]] bool IsMonoMode() const
    {
        return mMonoMode;
    }

    [[nodiscard]] int GetInputChannel() const
    {
        return mInputChannel;
    }

    // When hosted in a DAW the host owns the input configuration: mono
    // folding/channel selection is disabled and the input is used as provided.
    void SetHostControlledInput(bool hostControlled)
    {
        mHostControlledInput = hostControlled;

        if (hostControlled)
        {
            mMonoMode = false;
        }
    }

    [[nodiscard]] bool IsHostControlledInput() const
    {
        return mHostControlledInput;
    }

    // Signal chain parameter routing (apply to all presets)
    void SetInputTrim(double dB);
    void SetOutputTrim(double dB);

    // Global signal chain configuration
    void SetGlobalChainConfig(const GlobalSignalChainConfig& config);

    [[nodiscard]] const GlobalSignalChainConfig& GetGlobalChainConfig() const
    {
        return mGlobalChain.Config();
    }

    // Global pre-chain controls (noise gate, transpose)
    void SetGlobalGateEnabled(bool enabled);
    void SetGlobalGateThreshold(double thresholdDb);
    void SetGlobalGateAttack(double attackMs);
    void SetGlobalGateHold(double holdMs);
    void SetGlobalGateRelease(double releaseMs);
    void SetGlobalGateHysteresis(double hysteresisDb);
    void SetGlobalGateRange(double rangeDb);
    void SetGlobalGateStereoLink(bool linked);
    void SetGlobalTransposeEnabled(bool enabled);
    void SetGlobalTranspose(int semitones);

    // Global post-chain controls (EQ, doubler)
    void SetGlobalEQEnabled(bool enabled);
    void SetGlobalEQBandGain(int band, double dB);
    void SetGlobalEQBandFrequency(int band, double freq);
    void SetGlobalEQBandQ(int band, double q);
    void SetGlobalDoublerEnabled(bool enabled);
    void SetGlobalDoublerDelay(double delayMs);
    void SetGlobalDoublerMix(double mix);
    void SetGlobalDoublerDetune(double cents);

    // Global input/output gain
    void SetGlobalInputGain(double dB);
    void SetGlobalOutputGain(double dB);

    // Node-level control (for signal chain editing)
    void SetNodeEnabled(const std::string& presetId, const std::string& nodeId, bool enabled);
    void SetNodeParam(const std::string& presetId, const std::string& nodeId, const std::string& key, double value);
    void SetNodeConfig(const std::string& presetId, const std::string& nodeId, const std::string& key,
                       const std::string& value);

    /// SetNodeConfig in two halves, for a value that must not be applied under the DSP lock
    /// the lookup needs (a hosted plugin loads, restores state and opens its editor under its
    /// own lock, and any of those can take seconds). Under the DSP lock: records `value` in
    /// the slot's graph and returns the node's processor, or nullptr, without calling it.
    /// The caller then calls SetConfig. The pointer stays valid on the message thread until
    /// that thread next rebuilds or retires the slot: the audio thread only ever drops
    /// instances that are already retiring, and a retired hosted processor is destroyed only
    /// by CollectRetiredMainThread().
    [[nodiscard]] EffectProcessor* RecordNodeConfig(const std::string& presetId, const std::string& nodeId,
                                                    const std::string& key, const std::string& value);
    void SetNodeConfigForType(const std::string& type, const std::string& key, const std::string& value);

    /**
     * Record a config value adopted by every node of `type` across every preset slot and
     * the global pre/post chains, including slots and nodes created later.
     *
     * Carries this instance's NAM quality settings (oversampling, antiAliasPhase,
     * slimmableSize), which are per plugin instance rather than process-wide.
     */
    void SetNodeTypeConfigDefault(const std::string& type, const std::string& key, const std::string& value);
    [[nodiscard]] std::string GetNodeConfig(const std::string& presetId, const std::string& nodeId,
                                            const std::string& key) const;
    [[nodiscard]] EffectProcessor* GetNodeProcessor(const std::string& presetId, const std::string& nodeId);
    [[nodiscard]] const EffectProcessor* GetNodeProcessor(const std::string& presetId, const std::string& nodeId) const;
    bool LoadNodeResource(const std::string& presetId, const std::string& nodeId, const ResourceRef& ref);

    /// Find the first enabled node of the given effect type across all active preset instances
    /// (topological order within each instance, instances in insertion order).
    /// Returns (presetId, nodeId) or empty optional if not found.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> FindFirstEnabledNodeOfType(
        const std::string& effectType) const;

    /// The node by-type automation drives: FindFirstEnabledNodeOfType's choice, for a type
    /// already resolved (EffectRegistry::Resolve). Allocates nothing, since MIDI and DAW
    /// automation apply on the audio thread. Under the DSP lock, and good for as long as it is
    /// held; set its parameter with SignalGraphExecutor::SetAutomationTargetParam.
    [[nodiscard]] SignalGraphExecutor::AutomationTarget FindAutomationTarget(const std::string& canonicalType);

    /// SetNodeEnabledByType for a type already resolved, without allocating.
    bool SetAutomatedNodesEnabled(const std::string& canonicalType, bool enabled);

    /// A node's identity plus a snapshot of some of its parameters.
    struct NodeReadout
    {
        std::string scope;    ///< "pre", "preset" or "post"
        std::string presetId; ///< empty for the pre and post global chains
        std::string nodeId;
        std::vector<double> values; ///< one entry per requested parameter id, in order
    };

    /// Read a fixed set of parameters from every node of the given effect type, across the
    /// pre-chain, all preset instances and the post-chain.
    ///
    /// Intended for periodic UI telemetry: it returns values rather than processor pointers so
    /// callers cannot accidentally hold a reference across a graph rebuild. Parameters an effect
    /// does not implement read back as 0.
    [[nodiscard]] std::vector<NodeReadout> ReadNodeParamsForType(const std::string& effectType,
                                                                 const std::vector<std::string>& paramIds) const;

    /// Apply a parameter to the first enabled node of the given effect type across all active presets.
    /// Returns true if a matching node was found and updated.
    bool SetNodeParamByType(const std::string& effectType, const std::string& paramId, double value);

    /// Apply enabled/bypass state to all nodes of a given effect type across active presets.
    /// Returns true if at least one node was updated.
    bool SetNodeEnabledByType(const std::string& effectType, bool enabled);

    // Push the current tempo (BPM) to all tempo-aware nodes in every preset and global chain.
    // Call once per audio block before Process().
    void SetTempo(double bpm);

    // Lifecycle
    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float** inputs, float** outputs, int numSamples);

    // Queries. A walk of the instance list races the audio thread erasing finished fade-outs
    // at the end of Process(), so the caller holds the DSP lock. The periodic telemetry reads
    // (GetPresetCount through GetTotalLatencySamples, GetSignalDiagnosticsSnapshot,
    // ReadNodeSpectrum, ClearSpectrumTaps, ReadNodeParamsForType, SetSignalDiagnosticsEnabled)
    // hold the erase off themselves instead, so the message thread can call them without it:
    // taking the DSP lock 20-30 times a second would silence a block whenever one landed on an
    // audio callback. Slots are added, retired and re-keyed under the DSP lock on the message
    // thread, so from any other thread these still need it.
    [[nodiscard]] std::vector<std::string> GetActivePresetIds() const;
    [[nodiscard]] std::vector<std::string> GetPresetNodeTypes(const std::string& presetId) const;
    [[nodiscard]] std::optional<InstanceConfig> GetPresetConfig(const std::string& presetId) const;
    /// Live instance count. Instances still fading out after a swap are not counted.
    [[nodiscard]] size_t GetPresetCount() const;

    /// Instances that are on their way out — ringing out their tail or fading — and so are
    /// hidden from every other query while still costing a whole chain per block. This is
    /// what a switch's CPU cost is actually made of.
    [[nodiscard]] size_t GetRetiringPresetCount() const;
    /// Every executor's stats, merged. Unlike the per-executor form, the per-node maps
    /// here are keyed `<scope>::<nodeId>` — `pre::`, `post::`, or the preset id — because
    /// a bare node id does not identify a node across executors. This is the form the UI
    /// receives.
    [[nodiscard]] SignalGraphExecutor::DSPPerformanceStats GetPerformanceStats() const;
    /// Total algorithmic latency in samples: pre-chain + max(preset instances) + post-chain.
    [[nodiscard]] int GetTotalLatencySamples() const;

    // Signal diagnostics
    void SetSignalDiagnosticsEnabled(bool enabled);

    [[nodiscard]] bool IsSignalDiagnosticsEnabled() const noexcept
    {
        return mTelemetry.IsEnabled();
    }

    /// Number of blocks received larger than the prepared block size. Any non-zero value
    /// means the host is overrunning what Prepare() was told; those blocks are split
    /// rather than truncated, but it is worth knowing about.
    [[nodiscard]] std::uint64_t GetOversizedBlockCount() const noexcept
    {
        return mTelemetry.GetOversizedBlockCount();
    }

    [[nodiscard]] SignalDiagnosticsSnapshot GetSignalDiagnosticsSnapshot() const;

    /// Spectrum of one node's input, for an EQ display. `scope` is "pre", "post" or "preset",
    /// as in the diagnostics snapshot; `presetId` only matters for "preset", where empty means
    /// the first live instance. The node is tapped on first read and stays tapped -- including
    /// across a rebuild of its graph -- until ClearSpectrumTaps(). False when there is no such
    /// node. Message thread only.
    bool ReadNodeSpectrum(std::string_view scope, const std::string& presetId, const std::string& nodeId,
                          SpectrumTap::Bins& out);

    /// Detaches every spectrum tap, in every executor including retiring ones.
    void ClearSpectrumTaps();

    // Tuner functionality
    void SetTunerEnabled(bool enabled);

    [[nodiscard]] bool IsTunerEnabled() const noexcept
    {
        return mTuner->IsEnabled();
    }

    void SetTunerCallback(TunerCallback callback);
    void SetTunerReferenceFrequency(double frequency);

    [[nodiscard]] double GetTunerReferenceFrequency() const noexcept
    {
        return mTuner->GetReferenceFrequency();
    }

    void SetLiveTunerMode(bool enabled)
    {
        mTuner->SetLiveMode(enabled);
    }

    [[nodiscard]] bool IsLiveTunerMode() const noexcept
    {
        return mTuner->IsLiveMode();
    }

  private:
    /// A new instance for `preset`, built and prepared, ready to install or stage. Does the
    /// expensive work — effect creation, resource loading (a NAM model from disk can take
    /// hundreds of milliseconds), Prepare() — so a caller that can hold the DSP lock around
    /// the result calls this without it.
    [[nodiscard]] std::unique_ptr<PresetInstance> BuildInstance(const Preset& preset, const std::string& id,
                                                                const std::string& name) const;
    /// What every executor this mixer builds starts from.
    [[nodiscard]] ExecutorSetup MakeExecutorSetup() const;
    /// The edit rules for the live global chain. Defined in MultiPresetMixer.cpp.
    [[nodiscard]] GlobalChainEditor EditGlobalChain();

    void AllocateBuffers(int maxBlockSize);
    static void ComputePanGains(double pan, float& gL, float& gR);
    void EnsureGlobalChainsUpToDate();
    /// Apply the non-graph parts of the global config (mono/auto-level/limiter/master gain).
    void ApplyGlobalChainScalars(const GlobalSignalChainConfig& config);

    ResourceLibrary* mResourceLibrary = nullptr;
    // Per-instance node-type config (NAM quality), replayed onto every executor this
    // mixer builds — see SetNodeTypeConfigDefault().
    std::map<std::string, std::map<std::string, std::string>> mNodeTypeConfigDefaults;

    // Declared ahead of the voice pool and the global chain, which retire into it, so it is
    // constructed before and destroyed after both. The destructor stops it explicitly first.
    DspReaper mReaper;
    PresetVoicePool mVoices{mReaper};
    GlobalChainEngine mGlobalChain{mReaper};

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    bool mPrepared = false;
    double mMixGainDb = 0.0;
    double mMixGain = 1.0;
    double mMasterGain = 1.0;
    bool mLimiterEnabled = false;
    std::atomic<bool> mMultiThreadedProcessingEnabled{rtparallel::kParallelDspSupported};

    // Global settings
    double mUserInputCalibrationGainDb = 0.0;
    float mUserInputCalibrationGainLinear = 1.0f;
    bool mMonoMode = false;
    int mInputChannel = 0;             // 0=left, 1=right (for mono mode)
    bool mHostControlledInput = false; // true when a DAW host owns the input config

    // Temporary buffers for input processing
    std::vector<float> mTempInL, mTempInR;
    std::vector<float> mPreChainOutL, mPreChainOutR;
    std::vector<float> mPostChainOutL, mPostChainOutR;

    // Stable heap address keeps the tuner's worker bound to its owner across mixer moves.
    std::unique_ptr<TunerEngine> mTuner;

    MixerTelemetry mTelemetry;

    // ---- Parallel preset processing -----------------------------------------------
    static constexpr int kMaxParallelWorkers = 7;
    static constexpr int kMaxWorkItems = 16;

    struct ParallelWorkItem
    {
        PresetInstance* inst = nullptr;
        float* preChainOutL = nullptr;
        float* preChainOutR = nullptr;
        int numSamples = 0;
    };

    std::array<ParallelWorkItem, kMaxWorkItems> mWorkItems{};
    std::atomic<int> mParallelTaskHead{0};
    std::atomic<int> mParallelTaskCount{0};
    std::atomic<int> mParallelDoneCount{0};
    std::atomic<uint32_t> mParallelGeneration{0};
    std::atomic<bool> mParallelQuit{false};
    std::mutex mParallelMutex;
    std::condition_variable mParallelCv;
    std::vector<std::thread> mWorkerThreads;

    void StartWorkers(int count);
    void StopWorkers();
    void WorkerLoop();
};
} // namespace guitarfx
