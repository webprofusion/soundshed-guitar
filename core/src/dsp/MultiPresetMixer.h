#pragma once

#include "presets/PresetTypes.h"
#include "dsp/DspReaper.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/GlobalChainEngine.h"
#include "dsp/IdenticalInputDetector.h"
#include "dsp/PresetInstance.h"
#include "dsp/PresetVoicePool.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalTelemetry.h"
#include "dsp/SilenceSleepGate.h"
#include "dsp/MixerTelemetry.h"
#include "dsp/InputLevelHold.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/TunerEngine.h"
#include "dsp/looper/LooperEngine.h"

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

    /// Whether CollectRetiredMainThread() has a retired hosted chain to free. Any thread, one
    /// atomic load, for a caller polling at a timer's rate.
    [[nodiscard]] bool HasRetiredMainThreadWork() const noexcept
    {
        return mReaper.HasMainThreadWork();
    }

    // Message thread only, without the DSP lock, straight after a commit that replaced a running
    // slot: the hosted plugins of the chain just installed take over the instances their nodes
    // had in the chain it replaced (SignalGraphExecutor::SetHandOverSource). Until then those
    // nodes pass their input through, so a commit is followed by this as soon as the lock is
    // released. CollectRetiredMainThread() runs it too, for a commit that was not.
    void CompleteInstanceHandOvers();

    // Seamless preset swap: build the new executor off the DSP lock, then commit atomically.
    // PreparePresetSwap does the expensive work (effect creation, resource loading, Prepare).
    // CommitPresetSwap installs the pre-built instance, fades it in, and hands the outgoing
    // instance to the tail spill (or, with spill off, fades it out over the same window) so
    // the transition has no step discontinuity.
    // Pattern: call PreparePresetSwap() without holding the DSP lock, then hold the lock
    // and call CommitPresetSwap().
    void PreparePresetSwap(const Preset& preset, const std::string& id, const std::string& name);
    void CommitPresetSwap();

    /// A graph for `preset` built as a slot's is (the same resource library, NAM quality
    /// defaults and boundary nodes) but owned by the caller and never installed, for offline
    /// work that must leave the running slots alone (measurePresetLevel). Fed `inputLayout`,
    /// never dual mono, with no diagnostics or level-parallel fan-out. Not prepared. Message
    /// thread, without the DSP lock, as PreparePresetSwap is.
    [[nodiscard]] std::unique_ptr<SignalGraphExecutor> BuildDetachedExecutor(const Preset& preset,
                                                                             ChannelLayout inputLayout) const;

    /// The equal-power pan law a slot is mixed with: a centred slot is 3 dB down on each side.
    static void ComputePanGains(double pan, float& gL, float& gR);

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

    /// Makes the next PrepareGlobalChainSwap rebuild the executors even for an unchanged config,
    /// for a change that lives outside the config (the NAM engine).
    void MarkGlobalChainForRebuild() noexcept
    {
        mGlobalChain.MarkNeedsRebuild();
    }

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
    /// How long a muted (or soloed-out) rig keeps running once silent, so its delays and
    /// reverbs ring out unheard rather than freezing mid-tail for the unmute to replay.
    static constexpr double kMuteDrainSeconds = 2.0;

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

    /// The UI's output mute. Applied after the output gain rather than by zeroing it, because
    /// every global-chain rebuild re-derives the gain from the output setting and so undid a
    /// mute made through SetMasterGain.
    void SetOutputMuted(bool muted)
    {
        mOutputMuted.store(muted, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsOutputMuted() const
    {
        return mOutputMuted.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double GetMasterGain() const
    {
        return mMasterGain;
    }

    [[nodiscard]] bool IsLimiterEnabled() const
    {
        return mLimiterEnabled;
    }

    /// A mono output (a one-channel bus or device) gets the stereo mix folded, ½(L+R), and the
    /// rigs' equal-power pan has put a centred rig 3 dB down on each side, which nothing justifies
    /// once there is only one channel. On, the output stage makes those 3 dB up before the fold,
    /// so a centred rig reaches a mono output at its input level. Stereo outputs are untouched.
    /// Off by default; a plugin instance keeps its own value in the project (PluginController).
    void SetMonoOutputUnity(bool enabled) noexcept
    {
        mMonoOutputUnity.store(enabled, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsMonoOutputUnity() const noexcept
    {
        return mMonoOutputUnity.load(std::memory_order_relaxed);
    }

    void SetMultiThreadedProcessingEnabled(bool enabled);

    [[nodiscard]] bool IsMultiThreadedProcessingEnabled() const noexcept
    {
        return mMultiThreadedProcessingEnabled.load(std::memory_order_acquire);
    }

    /// Whether the chains stop running after a stretch of silence in and out (SilenceSleepGate).
    /// On by default; off for anything that needs every block rendered, such as a test.
    void SetSleepOnSilenceEnabled(bool enabled) noexcept
    {
        mSleepOnSilenceEnabled.store(enabled, std::memory_order_release);
    }

    /// Keeps the chains running through silence while set, whatever SetSleepOnSilenceEnabled
    /// says: a noise gate learning the noise floor has to hear it, and that noise is quiet
    /// enough to put the chains to sleep.
    void SetHoldAwake(bool holdAwake) noexcept
    {
        mHoldAwake.store(holdAwake, std::memory_order_release);
    }

    /// Audio thread, or a test between blocks: whether the last block skipped the chains.
    [[nodiscard]] bool IsSleepingOnSilence() const noexcept
    {
        return mSleepGate.IsAsleep();
    }

    /// The looper: a stage between the global post-chain and master gain, so no preset swap or chain
    /// rebuild touches a layer. dsp/looper/LooperEngine.h says which thread may call what.
    [[nodiscard]] looper::LooperEngine& Looper() noexcept
    {
        return mLooper;
    }

    // Global input settings
    void SetUserInputCalibrationGainDb(double dB);

    [[nodiscard]] double GetUserInputCalibrationGainDb() const
    {
        return mUserInputCalibrationGainDb;
    }

    // ── Input layout ────────────────────────────────────────────────────────────────
    // Whether the chains see one signal or two is configuration, never measured from the audio
    // (docs/signal-chain.md, "Channel layout"). In a DAW the bus decides; in the standalone app
    // the input mode does. Every setter below re-resolves the running graphs' layouts in place,
    // so a running mixer takes them under the DSP lock.

    /// What the chains are fed. Mono: one input, or both summed, on both channels. Stereo: both
    /// inputs, each kept to its side. DualMono: both inputs, with nothing crossing between the
    /// sides anywhere in a graph (it needs two outputs, and falls back to Mono summed without).
    enum class InputMode
    {
        Mono,
        Stereo,
        DualMono,
    };

    /// Input channel numbers for SetInputChannel: which input Mono takes, or both summed.
    static constexpr int kInputChannelLeft = 0;
    static constexpr int kInputChannelRight = 1;
    static constexpr int kInputChannelSum = 2;

    /// Standalone: Mono (true) or Stereo/DualMono (false). Ignored when the host controls the input.
    void SetMonoMode(bool mono);
    /// Standalone, and per instance in a DAW: process a stereo input as two mono chains.
    void SetDualMono(bool dualMono);
    /// Which input Mono takes: kInputChannelLeft, kInputChannelRight or kInputChannelSum.
    void SetInputChannel(int channel);

    [[nodiscard]] bool IsMonoMode() const
    {
        return mMonoMode;
    }

    [[nodiscard]] bool IsDualMono() const
    {
        return mDualMono;
    }

    [[nodiscard]] int GetInputChannel() const
    {
        return mInputChannel;
    }

    /// How many channels the input can supply and the output can reproduce: the DAW's main bus,
    /// or the standalone device's active channels. Read when the stream is prepared. One input
    /// forces Mono; one output folds the final mix to mono instead of dropping its right side.
    void SetAudioChannelCounts(int inputs, int outputs);

    [[nodiscard]] int GetInputChannelCount() const
    {
        return mInputChannelCount;
    }

    [[nodiscard]] int GetOutputChannelCount() const
    {
        return mOutputChannelCount;
    }

    /// Off the DSP lock, before dual mono is switched on: builds the second instances every
    /// running graph will need (SignalGraphExecutor::StageDualMonoTwins), so the switch itself,
    /// under the lock, only installs them. Cheap when they are already there.
    void StageDualMono();

    /// In dual mono, how many nodes across every running graph still run shared for want of a
    /// second instance: hosted plugins, which cannot be kept in step with their own editor.
    /// Under the DSP lock.
    [[nodiscard]] std::size_t CountDualMonoSharedNodes() const;

    /// The mode actually in force, after the host, the channel counts and the fallbacks.
    [[nodiscard]] InputMode GetEffectiveInputMode() const
    {
        return mEffectiveInputMode;
    }

    /// Whether a Stereo input whose two sides match runs the rigs on a mono layout, one model per
    /// amp instead of two (IdenticalInputDetector). On by default; off to compare against.
    void SetIdenticalInputFoldEnabled(bool enabled) noexcept
    {
        mIdenticalInputFoldEnabled.store(enabled, std::memory_order_release);
    }

    /// Whether the rigs are running mono on a matching stereo input right now.
    [[nodiscard]] bool IsInputFolded() const noexcept
    {
        return mInputFolded.load(std::memory_order_acquire);
    }

    /// What every rig's graph is fed: mono or stereo.
    [[nodiscard]] ChannelLayout GetRigInputLayout() const noexcept
    {
        return mRigInputLayout.load(std::memory_order_acquire);
    }

    /// When hosted in a DAW the host owns the input configuration: there is no Mono fold, and
    /// the bus's channel count says whether the input is stereo.
    void SetHostControlledInput(bool hostControlled);

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
    void SetGlobalPickupMatchEnabled(bool enabled);
    bool SetGlobalPickupMatchParam(const std::string& key, double value);
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
    void SetGlobalEQBandShape(int band, double shape);
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
    /// A node of the live global pre-chain (the global gate is "global_gate"). Valid while the
    /// caller holds the DSP lock, as GetNodeProcessor is.
    [[nodiscard]] EffectProcessor* GetGlobalPreNodeProcessor(const std::string& nodeId);
    bool LoadNodeResource(const std::string& presetId, const std::string& nodeId, const ResourceRef& ref);

    /// One running graph's deferred rebuilds (see DeferredRebuild).
    struct GraphRebuildWork
    {
        SignalGraphExecutor* executor = nullptr;
        std::unique_ptr<DeferredRebuild> work;
    };

    /// Under the DSP lock, message thread: the deferred rebuilds waiting in every running slot and
    /// the global chain, appended to `out`. A retiring slot's go with it.
    void TakeDeferredRebuilds(std::vector<GraphRebuildWork>& out);

    /// Under the DSP lock, message thread: hands what TakeDeferredRebuilds took back, once built,
    /// to each graph that is still running.
    void CommitDeferredRebuilds(std::vector<GraphRebuildWork>& rebuilds);

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

    // The same for one node, by id, as a per-preset mapping names it. Node ids are only unique
    // within a preset, so the preset playing as `presetId` is searched first; the other
    // instances only when no instance plays under that id. Allocates nothing.

    /// The node with this id, enabled or not, if it is of the type.
    [[nodiscard]] SignalGraphExecutor::AutomationTarget FindAutomationTargetById(const std::string& canonicalType,
                                                                                 const std::string& nodeId,
                                                                                 const std::string& presetId);

    /// Enables or bypasses the node with this id, if it is of the type. Returns its shared id,
    /// or null when there is no such node.
    const std::shared_ptr<const std::string>* SetAutomatedNodeEnabledById(const std::string& canonicalType,
                                                                          const std::string& nodeId,
                                                                          const std::string& presetId, bool enabled);

    /// Whether the node automation would switch is enabled: with a `nodeId` that node, without
    /// one the first node of the type in the first instance that has one. Nothing when there is
    /// no such node. Allocates nothing.
    [[nodiscard]] std::optional<bool> IsAutomatedNodeEnabled(const std::string& canonicalType,
                                                             const std::string& nodeId, const std::string& presetId);

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
    // ReadNodeSpectrum, ClearSpectrumTaps, ReadNodeLoudness, ClearLoudnessTaps,
    // ReadOutputLoudness, ClearOutputLoudnessTap, ReadNodeParamsForType, SetSignalDiagnosticsEnabled)
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

    /// The raw input's peak hold for input levelling training; armed and read by the controller.
    [[nodiscard]] InputLevelHold& GetInputLevelHold() noexcept
    {
        return mInputLevelHold;
    }

    /// Spectrum of one node's input, for an EQ display. `scope` is "pre", "post" or "preset",
    /// as in the diagnostics snapshot; `presetId` only matters for "preset", where empty means
    /// the first live instance. The node is tapped on first read and stays tapped -- including
    /// across a rebuild of its graph -- until ClearSpectrumTaps(). False when there is no such
    /// node. Message thread only.
    bool ReadNodeSpectrum(std::string_view scope, const std::string& presetId, const std::string& nodeId,
                          SpectrumTap::Bins& out);

    /// The same, with the output spectrum and band meters of a node that has them.
    bool ReadNodeSpectrum(std::string_view scope, const std::string& presetId, const std::string& nodeId,
                          WatchedSpectrum& out);

    /// Detaches every spectrum tap, in every executor including retiring ones.
    void ClearSpectrumTaps();

    /// Loudness of one node's output, for the DSP status readout: found and tapped as
    /// ReadNodeSpectrum does, and kept tapped until ClearLoudnessTaps(). Message thread only.
    bool ReadNodeLoudness(std::string_view scope, const std::string& presetId, const std::string& nodeId,
                          LoudnessTap::Reading& out);

    /// Detaches every loudness tap, in every executor including retiring ones.
    void ClearLoudnessTaps();

    /// Loudness of the app's output, for the output meter's readout: what the device is sent,
    /// after the output level and master volume, measured where the output meter is. Tapped on
    /// first read, restarting the measurement, until ClearOutputLoudnessTap(). Message thread.
    bool ReadOutputLoudness(LoudnessTap::Reading& out);
    void ClearOutputLoudnessTap();

    /// The highest DSP load any one block reached since the last call, as a percentage of the
    /// block's real time, and starts the next span. The load is GetPerformanceStats()'s: every
    /// chain's processing time that block, summed. Only blocks run with diagnostics on count.
    /// One consumer, the performance feed. Message thread.
    [[nodiscard]] double TakePeakDspLoadPercent() noexcept
    {
        return mPeakDspLoadPercent.exchange(0.0, std::memory_order_relaxed);
    }

    // Tuner functionality
    void SetTunerEnabled(bool enabled);

    [[nodiscard]] bool IsTunerEnabled() const noexcept
    {
        return mTuner->IsEnabled();
    }

    void SetTunerCallback(TunerCallback callback);
    void SetTunerReferenceFrequency(double frequency);

    /// The Scale Guide's LISTEN: tuner readings (with a chroma vector when `chroma` is set) while
    /// the tuner itself may be off, never muting the output.
    void SetTunerListening(bool listening, bool chroma)
    {
        mTuner->SetListening(listening, chroma);
    }

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
    /// The executor a telemetry tap addresses: "pre", "post", or "preset" with `presetId` (empty
    /// for the first live instance). Null when there is none. The caller holds a ReadScope.
    [[nodiscard]] SignalGraphExecutor* FindTapExecutor(std::string_view scope, const std::string& presetId);

    void AllocateBuffers(int maxBlockSize);
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
    // Audio thread: the gain the last block ended on. A change (the level knob, or the snap to
    // unity when a swap leaves the mix) ramps from here rather than stepping.
    float mMixGainApplied = 1.0f;
    double mMasterGain = 1.0;
    std::atomic<bool> mOutputMuted{false};
    std::atomic<bool> mMonoOutputUnity{false};
    bool mLimiterEnabled = false;
    std::atomic<bool> mMultiThreadedProcessingEnabled{rtparallel::kParallelDspSupported};
    std::atomic<bool> mSleepOnSilenceEnabled{true};
    std::atomic<bool> mHoldAwake{false};
    SilenceSleepGate mSleepGate; // audio thread, apart from Prepare()

    // Global settings
    double mUserInputCalibrationGainDb = 0.0;
    float mUserInputCalibrationGainLinear = 1.0f;
    bool mMonoMode = false;
    bool mDualMono = false;
    int mInputChannel = kInputChannelLeft; // which input Mono takes, or both summed
    bool mHostControlledInput = false;     // true when a DAW host owns the input config
    int mInputChannelCount = 2;
    int mOutputChannelCount = 2;
    // Resolved from the settings above by ApplyInputLayout(), under the DSP lock.
    InputMode mEffectiveInputMode = InputMode::Stereo;
    int mFoldChannel = kInputChannelLeft;
    // What every rig's graph is fed: the pre-chain's output layout. Read by BuildInstance(),
    // which runs off the DSP lock, hence atomic.
    std::atomic<ChannelLayout> mRigInputLayout{ChannelLayout::Stereo};
    // Whether every graph runs dual mono, read by BuildInstance() off the lock likewise.
    std::atomic<bool> mGraphsDualMono{false};
    // A Stereo input whose sides match, run mono. The detector is the audio thread's, reset under
    // the DSP lock by ApplyInputLayout() and Prepare(); the flag is written under the lock and
    // read off it by MakeExecutorSetup(), hence atomic.
    IdenticalInputDetector mIdenticalInput;
    std::atomic<bool> mInputFolded{false};
    std::atomic<bool> mIdenticalInputFoldEnabled{true};

    /// Resolves the effective input mode and fold, and hands each graph its input layout:
    /// the pre-chain the input's, every rig the pre-chain's output, the post-chain stereo.
    void ApplyInputLayout();

    /// The input layout the pre-chain gets: Mono for the Mono mode or a folded Stereo input.
    [[nodiscard]] ChannelLayout CurrentInputLayout() const noexcept
    {
        return (mEffectiveInputMode == InputMode::Mono || mInputFolded.load(std::memory_order_acquire))
                   ? ChannelLayout::Mono
                   : ChannelLayout::Stereo;
    }

    /// Audio thread: folds a matching Stereo input, or unfolds it, and keeps every graph's input
    /// layout in step with that, a rig built off the lock meanwhile included.
    void UpdateIdenticalInputFold(const float* left, const float* right, int numSamples);
    /// One block of at most mMaxBlockSize into a stereo pair (either output may be null).
    /// `outputGain` scales the final output stage with the master gain, ahead of the meters and
    /// the limiter: the mono fold's makeup (SetMonoOutputUnity), 1 otherwise.
    void ProcessStereoBlock(float** inputs, float** outputs, int numSamples, float outputGain = 1.0f);
    /// How ProcessStereoBlock left a block: chains run, asleep on silence, or muted by the tuner.
    enum class BlockExit
    {
        Processed,
        Asleep,
        TunerMuted,
    };
    /// Every block's end: the looper records and plays, then master gain, meters and limiter.
    void FinishOutputBlock(float** outputs, int numSamples, float outputGain, BlockExit exit, double blockDspUs,
                           bool diagnosticsEnabled);

    // Temporary buffers for input processing
    std::vector<float> mTempInL, mTempInR;
    std::vector<float> mPreChainOutL, mPreChainOutR;
    std::vector<float> mPostChainOutL, mPostChainOutR;
    // The final mix when the output is mono, before it is folded into the one channel.
    std::vector<float> mFoldOutL, mFoldOutR;

    // By pointer so the destructor can stop it first, ahead of everything it reports on.
    std::unique_ptr<TunerEngine> mTuner;
    looper::LooperEngine mLooper;

    MixerTelemetry mTelemetry;
    InputLevelHold mInputLevelHold;

    /// See TakePeakDspLoadPercent(). Written by the audio thread, taken by the message thread.
    std::atomic<double> mPeakDspLoadPercent{0.0};
    /// The output readout's tap, created on its first read and kept for the mixer's lifetime so
    /// the audio thread never holds a freed one; the audio thread pushes only while it is active.
    std::unique_ptr<LoudnessTap> mOutputLoudnessTap;
    std::atomic<LoudnessTap*> mActiveOutputLoudnessTap{nullptr};

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
    // Declared last, so destroyed first; ~MultiPresetMixer() stops it before anything else goes.
    rtparallel::RealtimeTaskPool mWorkerPool;

    /// Starts hardware_concurrency - 1 workers (at most kMaxParallelWorkers), leaving the
    /// audio thread's core uncontested.
    void StartWorkers();
};
} // namespace guitarfx
