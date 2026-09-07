#pragma once

#include "presets/PresetTypes.h"
#include "dsp/EffectProcessor.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/SignalTelemetry.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/RealtimeParallel.h"

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
#include <thread>
#include <limits>
#include <utility>
#include <vector>

namespace guitarfx
{
class ResourceLibrary;

/**
 * Central DSP manager that runs multiple presets in parallel and mixes their outputs.
 * Supports per-preset mix level, mute/solo, stereo panning, and per-preset global FX.
 * Also handles global input settings like auto-level and mono/stereo mode.
 */
class MultiPresetMixer
{
  public:
    struct InstanceConfig
    {
        std::string id;   // Stable preset instance ID (e.g., "p1")
        std::string name; // Display name
        double mix = 1.0; // Linear gain [0.0, 1.0]
        bool mute = false;
        bool solo = false;
        double pan = 0.0; // [-1.0, 1.0] equal-power pan
    };

    // Tuner result data
    struct TunerResult
    {
        std::string noteName;      // e.g., "E", "A#/Bb"
        int octave = 0;            // Octave number (e.g., 2 for low E on guitar)
        double frequency = 0.0;    // Detected frequency in Hz
        double centOffset = 0.0;   // Cents deviation from perfect pitch (-50 to +50)
        double confidence = 0.0;   // Detection confidence (0.0 to 1.0)
        bool detected = false;     // Whether a valid pitch was detected
        double debugRms = 0.0;     // Debug: RMS of input signal
        double debugRawFreq = 0.0; // Debug: Raw detected frequency before note mapping
    };

    struct SignalLevelStats
    {
        double peak = 0.0;
        double rms = 0.0;
        int clipCount = 0;
    };

    struct NodeSignalLevel
    {
        using AnalyzerTelemetry = guitarfx::AnalyzerTelemetry;

        std::string scope; // pre, post, preset
        std::string presetId;
        std::string nodeId;
        std::string nodeType;
        int channelCount = 0;
        SignalLevelStats levels;
        std::optional<AnalyzerTelemetry> analyzer;
    };

    struct SignalDiagnosticsSnapshot
    {
        SignalLevelStats rawInput; // Before any gain/trim/mono processing
        SignalLevelStats input;
        SignalLevelStats output;
        std::vector<NodeSignalLevel> nodes;
    };

    using TunerCallback = std::function<void(const TunerResult&)>;

    MultiPresetMixer() = default;
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

    // Rebuilds a single already-active instance's executor graph in place (e.g. after a
    // scene switch or in-place edit of a preset that happens to be one of several active
    // mixer slots), preserving that slot's mix/pan/mute/solo. Unlike PreparePresetSwap()/
    // CommitPresetSwap(), this does NOT touch any other active instance. Returns false
    // (no-op) if presetId is not currently active.
    bool ReplaceActivePresetInPlace(const Preset& preset, const std::string& presetId, const std::string& name);

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
    // PresetInstance::canRingOut), and it is dropped early the moment it goes quiet — so
    // an ordinary preset costs no more than the plain crossfade did.
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
        return mTailSpillSeconds;
    }

    // Ceiling on the above. A runaway feedback delay never decays on its own, so the hold
    // is what stops one ringing under the next three songs.
    static constexpr double kMaxPresetSwapTailSeconds = 20.0;

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
        mGlobalChainConfig.limiterEnabled = enabled;
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

    // Global input/output settings
    void SetAutoLevelInput(bool enabled)
    {
        mAutoLevelInput = enabled;
    }

    void SetAutoLevelOutput(bool enabled)
    {
        mAutoLevelOutput = enabled;
    }

    [[nodiscard]] bool GetAutoLevelInput() const
    {
        return mAutoLevelInput;
    }

    [[nodiscard]] bool GetAutoLevelOutput() const
    {
        return mAutoLevelOutput;
    }

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
        return mGlobalChainConfig;
    }

    // Global pre-chain controls (noise gate, transpose)
    void SetGlobalGateEnabled(bool enabled);
    void SetGlobalGateThreshold(double thresholdDb);
    void SetGlobalGateAttack(double attackMs);
    void SetGlobalGateHold(double holdMs);
    void SetGlobalGateRelease(double releaseMs);
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

    // Queries
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
        return mSignalDiagnosticsEnabled.load(std::memory_order_acquire);
    }

    /// Number of blocks received larger than the prepared block size. Any non-zero value
    /// means the host is overrunning what Prepare() was told; those blocks are split
    /// rather than truncated, but it is worth knowing about.
    [[nodiscard]] std::uint64_t GetOversizedBlockCount() const noexcept
    {
        return mOversizedBlockCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] SignalDiagnosticsSnapshot GetSignalDiagnosticsSnapshot() const;

    // Tuner functionality
    void SetTunerEnabled(bool enabled);

    [[nodiscard]] bool IsTunerEnabled() const noexcept
    {
        return mTunerEnabled;
    }

    void SetTunerCallback(TunerCallback callback);
    void SetTunerReferenceFrequency(double frequency);

    [[nodiscard]] double GetTunerReferenceFrequency() const noexcept
    {
        return mTunerReferenceFrequency;
    }

    void SetLiveTunerMode(bool enabled)
    {
        mLiveTunerMode = enabled;
    }

    [[nodiscard]] bool IsLiveTunerMode() const noexcept
    {
        return mLiveTunerMode;
    }

  private:
    /// Where an instance is in its lifecycle. Retiring instances stay in mInstances so the
    /// existing dispatch/mix paths process them unchanged, but they are hidden from every
    /// lookup and query so callers only ever see the live set.
    enum class InstancePhase
    {
        Active,    ///< Normal: full gain.
        FadingIn,  ///< Just installed, ramping 0 -> 1.
        Tailing,   ///< Superseded, input ramped to zero, output held while the tail rings out.
        FadingOut, ///< On its way out, ramping to 0; retired to the reaper when the ramp ends.
    };

    struct PresetInstance
    {
        InstanceConfig cfg;
        SignalGraphExecutor executor;
        std::vector<float> outL;
        std::vector<float> outR;
        /// The ramped-to-zero input a tailing instance is fed, in place of the shared
        /// pre-chain output every live instance reads. Per instance rather than one shared
        /// buffer because several may be tailing from different ramp positions at once,
        /// and the parallel dispatch runs them on different threads.
        std::vector<float> tailInL;
        std::vector<float> tailInR;
        int complexityScore = 1;
        /// Whether this graph has anything that could still sound once its input is cut —
        /// a delay, a reverb, or an opaque plugin/WASM node that might be either. Decided
        /// once when the instance is built; a graph with none of them is cut on the declick
        /// ramp as before rather than being run on in the hope that it decays.
        bool canRingOut = false;

        InstancePhase phase = InstancePhase::Active;
        int fadeSamplesRemaining = 0;
        int fadeTotalSamples = 0;

        // ── Tail spill state, all meaningful only while phase == Tailing ──
        /// Output gain held flat for the whole tail. Frozen at whatever the instance was
        /// at when the tail began, so an instance superseded mid-fade-in does not step up
        /// to unity on its way out.
        float tailGain = 1.0f;
        /// Input ramp 1 -> 0, so nothing new enters the chain but its state decays smoothly
        /// rather than being cut.
        int inputFadeSamplesRemaining = 0;
        int inputFadeTotalSamples = 0;
        /// What is left of the hold budget.
        int tailSamplesRemaining = 0;
        /// Consecutive samples the output has stayed under the silence floor. The tail is
        /// dropped early once this passes the mixer's quiet window.
        int quietSamples = 0;

        /// Sizes the per-block buffers. Not on the audio thread.
        void ResizeBuffers(int maxBlockSize);

        /// Gain multiplier at the start of a block of numSamples, and at its end.
        /// Linear (equal-gain) ramp: the outgoing and incoming chains carry the same source
        /// and are strongly correlated, so equal-power would overshoot by up to 3 dB.
        void GetFadeGains(int numSamples, float& startGain, float& endGain) const;

        /// The instance's current fade multiplier.
        [[nodiscard]] float CurrentFadeGain() const;

        /// Switch to fading out over `fadeSamples`, starting from whatever gain the instance
        /// is at right now. Switching again while an instance is still fading in must not
        /// snap it back to full gain — that step is exactly the click being designed out.
        void BeginFadeOut(int fadeSamples);

        /// Switch to ringing out: hold the current output gain for up to `holdSamples`
        /// while the input ramps away over `inputFadeSamples`.
        void BeginTail(int inputFadeSamples, int holdSamples);

        /// Writes this block's ramped-down input into tailInL/tailInR. Audio thread.
        void FillTailInput(const float* inL, const float* inR, int numSamples);

        /// Folds this block's output into the run of consecutive quiet samples.
        void ObserveTailDecay(int numSamples, float silencePeak);

        [[nodiscard]] bool IsRetiring() const
        {
            return phase == InstancePhase::FadingOut || phase == InstancePhase::Tailing;
        }

        PresetInstance() = default;
        PresetInstance(PresetInstance&&) noexcept = default;
        PresetInstance& operator=(PresetInstance&&) noexcept = default;
        PresetInstance(const PresetInstance&) = delete;
        PresetInstance& operator=(const PresetInstance&) = delete;
    };

    [[nodiscard]] PresetInstance* FindInstance(const std::string& id);
    [[nodiscard]] const PresetInstance* FindInstance(const std::string& id) const;
    void AllocateBuffers(int maxBlockSize);
    void AllocateInstanceBuffers(PresetInstance& inst, int maxBlockSize);
    static void ComputePanGains(double pan, float& gL, float& gR);
    void RebuildGlobalChains();
    void EnsureGlobalChainsUpToDate();
    /// Fill in a config's pre/post graphs where they are missing or malformed.
    static void NormalizeGlobalChainConfig(GlobalSignalChainConfig& config);
    /// Apply the non-graph parts of the global config (mono/auto-level/limiter/master gain).
    void ApplyGlobalChainScalars(const GlobalSignalChainConfig& config);

    // ---- Deferred destruction ------------------------------------------------------
    // Destroying an instance frees NAM models, convolver partition tables and node buffers.
    // Doing that on the message thread while holding the DSP lock stalls the audio thread
    // (which try_locks and outputs silence on failure), so retired instances are moved onto
    // a queue and destroyed on a background reaper thread instead. Moving is cheap and
    // allocation-free as long as the queue has spare capacity, which lets the audio thread
    // retire finished fade-outs itself via a try_lock.
    void StartReaper();
    void StopReaper();
    void ReaperLoop();
    /// Message-thread retire: always succeeds, may allocate, wakes the reaper.
    void RetireInstance(std::unique_ptr<PresetInstance> inst);
    /// Audio-thread retire: non-blocking and allocation-free. Returns false if the caller
    /// should keep the instance (already silent) and try again on the next block.
    [[nodiscard]] bool TryRetireInstanceRealtime(std::unique_ptr<PresetInstance>& inst);
    /// Drop every finished fade-out. Audio thread, end of Process().
    void CollectFinishedFadeOuts();

    // ---- Tail spill ----------------------------------------------------------------
    /// Retires a superseded instance the way the current settings say to: ringing out when
    /// the tail spill is on, plain declick fade otherwise. Nothing here asks whether the
    /// preset has anything to ring — that is what the silence floor answers, and it answers
    /// it for composites and hosted plugins too, which no graph inspection could.
    void RetireSupersededInstance(PresetInstance& inst);
    /// Pushes the oldest tails into their release once more than kMaxTailingInstances are
    /// ringing at once, so a run of fast switches cannot stack full chains without bound.
    void LimitTailingInstances();
    /// The hold budget a new tail starts with, in samples.
    [[nodiscard]] int TailHoldSamples() const;
    // Tuner processing (YIN-based pitch detection)
    void ProcessTuner(float** inputs, int numSamples);
    [[nodiscard]] double DetectPitch(const std::vector<double>& samples) const;
    [[nodiscard]] TunerResult FrequencyToNote(double frequency, double referenceFrequency) const;
    void StartTunerWorker();
    void StopTunerWorker();
    void TunerWorkerLoop();

    ResourceLibrary* mResourceLibrary = nullptr;
    // Per-instance node-type config (NAM quality), replayed onto every executor this
    // mixer builds — see SetNodeTypeConfigDefault().
    std::map<std::string, std::map<std::string, std::string>> mNodeTypeConfigDefaults;
    // Held by pointer so the audio thread can drop a finished fade-out with a pointer move:
    // moving a PresetInstance by value moves its SignalGraphExecutor, whose move-assignment
    // joins worker threads — not something that can happen on the realtime path.
    std::vector<std::unique_ptr<PresetInstance>> mInstances;

    // Staged instance built off the DSP lock by PreparePresetSwap(); committed by CommitPresetSwap().
    std::unique_ptr<PresetInstance> mPendingInstance;

    // Crossfade length for a preset swap, in samples (~21 ms at 48 kHz). This is a declick
    // ramp, not a musical crossfade; a user-configurable fade time arrives with the switching
    // settings in a later phase.
    static constexpr int kPresetFadeSamples = 1024;
    // Upper bound on simultaneously fading-out instances. Rapid successive switches
    // hard-drop the oldest rather than stacking unbounded CPU cost.
    static constexpr std::size_t kMaxFadingOutInstances = 3;

    // ── Tail spill tuning ────────────────────────────────────────────────────────────
    // How many outgoing presets may ring at once. A ringing instance is a whole chain still
    // being processed on the audio thread, so this is a CPU budget before it is a musical
    // choice — one, so a switch can never more than double the DSP load. A second switch
    // does not hard-drop the first tail, it starts its release early.
    static constexpr std::size_t kMaxTailingInstances = 1;
    // Release once the hold runs out. Long enough that cutting a still-loud feedback delay
    // reads as an ending rather than a chop — the 21 ms declick would be audible there.
    static constexpr double kTailReleaseSeconds = 0.25;
    // How long the output has to stay under the floor before the tail is called finished.
    // Long enough to ride over the zero crossings of a low note decaying underneath it.
    static constexpr double kTailQuietSeconds = 0.15;
    // The floor itself, ~-70 dBFS. Below anything a tail is audible at, and above the DC
    // trickle an amp model can settle on when its input goes to zero.
    static constexpr float kTailSilencePeak = 3.0e-4f;

    // Retired instances awaiting destruction on the reaper thread.
    std::vector<std::unique_ptr<PresetInstance>> mRetireQueue;
    std::vector<SignalGraphExecutor> mRetireExecutors;
    std::mutex mRetireMutex;
    std::condition_variable mRetireCv;
    std::thread mReaperThread;
    bool mReaperQuit = false;
    static constexpr std::size_t kRetireQueueCapacity = 16;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    bool mPrepared = false;
    // Tail spill: off until somebody sets a length. Prepare() resolves the two sample
    // counts below from the constants above, so they follow the sample rate.
    double mTailSpillSeconds = 0.0;
    int mTailReleaseSamples = kPresetFadeSamples;
    int mTailQuietSamples = 0;
    double mMixGainDb = 0.0;
    double mMixGain = 1.0;
    double mMasterGain = 1.0;
    bool mLimiterEnabled = false;
    std::atomic<bool> mMultiThreadedProcessingEnabled{rtparallel::kParallelDspSupported};

    // Global settings
    bool mAutoLevelInput = false;
    bool mAutoLevelOutput = false;
    double mUserInputCalibrationGainDb = 0.0;
    float mUserInputCalibrationGainLinear = 1.0f;
    bool mMonoMode = false;
    int mInputChannel = 0;             // 0=left, 1=right (for mono mode)
    bool mHostControlledInput = false; // true when a DAW host owns the input config

    // Auto-level gain state
    float mInputAutoLevelGain = 1.0f;
    float mOutputAutoLevelGain = 1.0f;

    // Temporary buffers for input processing
    std::vector<float> mTempInL, mTempInR;
    std::vector<float> mPreChainOutL, mPreChainOutR;
    std::vector<float> mPostChainOutL, mPostChainOutR;

    // Global signal chain configuration and executors
    GlobalSignalChainConfig mGlobalChainConfig;
    SignalGraphExecutor mPreChainExecutor;  // input → gate → transpose
    SignalGraphExecutor mPostChainExecutor; // eq → doubler → output
    std::atomic<bool> mGlobalChainNeedsRebuild{true};

    // Staged global chain built off the DSP lock by PrepareGlobalChainSwap(). The two
    // executor slots stay empty when the graphs were unchanged and only scalars need applying.
    std::optional<GlobalSignalChainConfig> mPendingGlobalChainConfig;
    std::optional<SignalGraphExecutor> mPendingPreChainExecutor;
    std::optional<SignalGraphExecutor> mPendingPostChainExecutor;

    // Tuner state
    bool mTunerEnabled = false;
    bool mLiveTunerMode = true; // When true, audio passes through DSP while tuning; when false, output is silent
    double mTunerReferenceFrequency = 440.0; // A4 reference pitch
    TunerCallback mTunerCallback;
    std::vector<double> mTunerBuffer;        // Circular buffer for pitch detection
    std::vector<double> mTunerOrderedBuffer; // Pre-allocated scratch for linearised reads (audio thread, no alloc)
    std::vector<double> mTunerAnalysisWriteBuffer; // Mailbox from audio thread to worker
    std::vector<double> mTunerAnalysisReadBuffer;  // Worker-owned snapshot outside the audio thread
    std::size_t mTunerBufferWriteIndex = 0;
    std::size_t mTunerSampleCounter = 0;                      // For throttling callback rate
    static constexpr std::size_t kTunerBufferSize = 4096;     // ~85ms at 48kHz for good low-frequency detection
    static constexpr std::size_t kTunerUpdateInterval = 2048; // Update every ~42ms at 48kHz
    std::mutex mTunerAnalysisMutex;
    std::condition_variable mTunerAnalysisCv;
    std::thread mTunerWorkerThread;
    bool mTunerWorkerQuit = false;
    bool mTunerAnalysisPending = false;
    double mTunerAnalysisReferenceFrequency = 440.0;
    std::uint64_t mTunerQueuedGeneration = 0;
    std::atomic<std::uint64_t> mTunerAnalysisGeneration{0};

    struct AtomicLevelStats
    {
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
    };

    // Counts blocks that arrived larger than the size we were prepared with (see Process).
    // Non-zero means the host is not honouring the prepared block size.
    std::atomic<std::uint64_t> mOversizedBlockCount{0};

    std::atomic<bool> mSignalDiagnosticsEnabled{true};
    AtomicLevelStats mRawInputLevels;
    AtomicLevelStats mInputLevels;
    AtomicLevelStats mOutputLevels;

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
