#pragma once

#include "presets/PresetTypes.h"
#include "dsp/ChannelLayout.h"
#include "dsp/DeferredRebuild.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/SignalTelemetry.h"
#include "dsp/SpectrumTap.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace guitarfx
{
class EffectProcessor;
class MixerEffect;
class ResourceLibrary;
struct NoteBlock;

/**
 * Executes a signal graph by processing audio through nodes in topological order.
 *
 * Note routing: a node that makes notes (EffectProcessor::GetNoteOutput(), Guitar to MIDI) feeds
 * every node downstream of it that plays them (AcceptsNoteInput(), the Plugin Host), however
 * many nodes lie between. Downstream means reachable along the graph's edges, so a player in a
 * parallel branch that does not pass through the source hears nothing from it; and since a
 * source always sits in an earlier level than its players, it has finished its block before any
 * of them start theirs, parallel levels included. A player is handed the sources that ran this
 * block and only those: a bypassed source, or one with no input, counts as holding nothing, which
 * is how the player knows to let its notes go. Routing stops at a composite's edge.
 *
 * Channel layout: whether each connection carries one signal on both channels (mono) or two
 * (stereo) is resolved when the plan is built, from the executor's input layout and the effect
 * types along the path, never from the audio and never from an effect's current settings. The
 * input node has the input layout; a node's input is stereo if any connection into it is; its
 * output is stereo if its input is or its type CanWiden(). A node runs its mono path only on a
 * mono input, and only when its type cannot widen. Since everything after a node that can widen
 * already runs stereo, a user turning a pan or a width up is heard at once, with nothing to
 * switch. A mono connection always holds the same samples on both channels.
 *
 * Dual mono (SetDualMono): the input is stereo and nothing crosses between the sides anywhere.
 * A node whose type keeps its channels apart (EffectProcessor::KeepsChannelsSeparate) runs as
 * usual, with SetDualMono(true) switching off whatever it links. Any other node gets a second
 * instance: the primary runs on the left input on both channels and keeps its left output, the
 * second on the right input and keeps its right, so each side hears exactly what a mono input of
 * its own would make. Every operation on a node reaches both instances. Hosted plugins, which
 * cannot be kept in step with their own editor, run shared.
 */
class SignalGraphExecutor
{
  public:
    /// What the UI's performance panel and per-node readouts are drawn from. Nothing
    /// else consumes it, and nothing outside the app sees it.
    ///
    /// The per-node maps are keyed by bare node id here, which is all one executor knows.
    /// A node id is only unique *within* an executor -- every executor has an `__input__`,
    /// and the same preset loaded into two mixer slots repeats every id it has -- so
    /// MultiPresetMixer::GetPerformanceStats() rewrites them to `<scope>::<nodeId>` as it
    /// merges. That is the form the UI sees; see TelemetryPublisher.
    struct DSPPerformanceStats
    {
        double totalProcessingTimeUs = 0.0;                  // Total time in microseconds
        double realTimeUs = 0.0;                             // Real-time equivalent in microseconds
        double dspLoadPercent = 0.0;                         // % of real-time
        std::map<std::string, double> nodeProcessingTimesUs; // Per-node times
        std::map<std::string, int> nodeLatencySamples;       // Per-node algorithmic latency
    };

    struct NodeSignalLevel
    {
        using AnalyzerTelemetry = guitarfx::AnalyzerTelemetry;

        std::string nodeId;
        std::string nodeType;
        double peak = 0.0;
        double rms = 0.0;
        int clipCount = 0;
        int channelCount = 0;
        std::optional<AnalyzerTelemetry> analyzer;
    };

    SignalGraphExecutor();
    ~SignalGraphExecutor();

    /// Neither copyable nor movable: the plan points into the node states, and the worker
    /// threads are bound to this address. An owner that replaces one (GlobalChainEngine,
    /// PresetVoicePool) holds it by pointer and hands the old one to the DspReaper whole.
    SignalGraphExecutor(const SignalGraphExecutor&) = delete;
    SignalGraphExecutor& operator=(const SignalGraphExecutor&) = delete;

    // Setup

    /// Builds the processors and the execution plan for `graph`. Leaves the executor
    /// unprepared: call Prepare() before Process() runs again.
    void SetGraph(const SignalGraph& graph);

    void SetResourceLibrary(ResourceLibrary* library)
    {
        mResourceLibrary = library;
    }

    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float** inputs, float** outputs, int numSamples);

    /// The layout of what Process() is given. Set it before SetGraph() so the plan is built for
    /// it; setting it on a built graph re-resolves the layout in place, which is cheap and does
    /// not allocate, but changes what runs, so a running graph takes it under the DSP lock. A
    /// mono input is read from the left channel alone. Defaults to stereo: a caller that never
    /// says otherwise gets both channels processed, which can cost CPU but never drops audio.
    void SetInputLayout(ChannelLayout layout);

    [[nodiscard]] ChannelLayout GetInputLayout() const noexcept
    {
        return mInputLayout;
    }

    /// Whether what reaches the output is stereo, as resolved: a stereo input, or a node on the
    /// way that can widen. Fixed until the graph or its input layout changes.
    [[nodiscard]] bool OutputIsStereo() const noexcept
    {
        return mOutputStereo;
    }

    /// Dual mono on or off (see the class comment). Before SetGraph(), or on a graph not yet
    /// prepared, the second instances are built here. On a running graph this runs under the DSP
    /// lock and only installs what StageDualMonoTwins() built beforehand, off the lock; a coupled
    /// node without one runs shared until it has one.
    void SetDualMono(bool dualMono);

    [[nodiscard]] bool IsDualMono() const noexcept
    {
        return mDualMono;
    }

    /// Off the DSP lock: builds, loads and prepares a second instance for every coupled node that
    /// has none, composites' inner nodes included, for SetDualMono(true) to install.
    void StageDualMonoTwins();

    /// Coupled nodes running shared in dual mono for want of a second instance (a hosted plugin,
    /// or one not staged yet). For the UI to flag, and for tests.
    [[nodiscard]] std::vector<std::string> DualMonoSharedNodes() const;

    /// Whether any node in the graph can widen (EffectProcessor::CanWiden), composites' inner
    /// nodes included. What a composite reports for itself.
    [[nodiscard]] bool AnyNodeCanWiden() const;

    // Node control
    void SetNodeEnabled(const std::string& nodeId, bool enabled);
    void SetNodeParam(const std::string& nodeId, const std::string& key, double value);
    void SetNodeConfig(const std::string& nodeId, const std::string& key, const std::string& value);

    /// SetNodeConfig without the SetConfig call: records `value` in the graph (unless `key` is
    /// a transient command) and returns the node's processor, or nullptr, for the caller to
    /// apply it to. For a caller that must apply the value outside the lock it looked up under.
    [[nodiscard]] EffectProcessor* RecordNodeConfig(const std::string& nodeId, const std::string& key,
                                                    const std::string& value);
    void SetNodeConfigForType(const std::string& type, const std::string& key, const std::string& value);

    /**
     * Record a config value that every node of `type` should adopt, including nodes
     * built later by CreateProcessors(). Existing nodes are updated immediately.
     *
     * This is how per-instance settings that are not preset parameters (NAM quality:
     * oversampling, antiAliasPhase, slimmableSize) reach their nodes. It replaces the
     * process-wide globals those settings used to live in, which forced one value on
     * every plugin instance sharing a DAW's process. A node's own `config` entry still
     * wins, since CreateProcessors() applies these defaults first.
     */
    void SetNodeTypeConfigDefault(const std::string& type, const std::string& key, const std::string& value);

    /// Type defaults recorded so far, for seeding a nested or newly created executor.
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::string>>& GetNodeTypeConfigDefaults() const
    {
        return mNodeTypeConfigDefaults;
    }

    /// Copy another executor's type defaults wholesale (does not touch existing nodes).
    void SeedNodeTypeConfigDefaults(const std::map<std::string, std::map<std::string, std::string>>& defaults);
    bool LoadNodeResource(const std::string& nodeId, const ResourceRef& ref);

    /// Under the DSP lock, message thread: the deferred rebuilds (see DeferredRebuild) this graph's
    /// nodes have waiting, composites' inner nodes included, as one piece of work to build off the
    /// lock, or nullptr when there are none.
    [[nodiscard]] std::unique_ptr<DeferredRebuild> TakeDeferredRebuilds();

    /// Under the DSP lock, message thread: hands each node its built rebuild back, if the node
    /// still runs the processor it was taken from. `work` is what TakeDeferredRebuilds returned.
    void CommitDeferredRebuilds(DeferredRebuild& work);

    [[nodiscard]] std::string GetNodeConfig(const std::string& nodeId, const std::string& key) const;
    [[nodiscard]] EffectProcessor* GetNodeProcessor(const std::string& nodeId);
    [[nodiscard]] const EffectProcessor* GetNodeProcessor(const std::string& nodeId) const;

    // By-type automation. MIDI and DAW automation apply on the audio thread, under the DSP lock,
    // so none of these allocate. Each takes a type already resolved (EffectRegistry::Resolve).

    /// A node automation drives, as FindAutomationTarget found it. Valid until the graph changes.
    struct AutomationTarget
    {
        EffectProcessor* processor = nullptr;
        /// The node's second instance in dual mono, or null.
        EffectProcessor* twin = nullptr;
        /// This executor's own copy of the node.
        GraphNode* node = nullptr;
        /// The node's id, shared so that a notification naming the node can outlive the graph.
        const std::shared_ptr<const std::string>* id = nullptr;

        explicit operator bool() const
        {
            return id != nullptr;
        }
    };

    /// The first enabled node of the type, in execution order: FindNodesOfType(type, false).front().
    [[nodiscard]] AutomationTarget FindAutomationTarget(const std::string& canonicalType);

    /// SetNodeParam on a found target. The executor's copy of the graph only takes the value for a
    /// key the node already holds, since inserting one allocates. Nothing reads a value back from
    /// that copy except the Input and Output nodes' gain, which SetGraph makes sure they hold.
    static void SetAutomationTargetParam(const AutomationTarget& target, const std::string& key, double value);

    /// SetNodeEnabled on every node of the type, enabled or not. Returns whether there was one.
    bool SetAutomatedNodesEnabled(const std::string& canonicalType, bool enabled);

    // Queries
    [[nodiscard]] std::string FindFirstNodeOfType(const std::string& type) const;
    [[nodiscard]] std::vector<std::string> FindNodesOfType(const std::string& type, bool includeDisabled = true) const;
    [[nodiscard]] std::string FindFirstNodeOfTypes(const std::vector<std::string>& types) const;
    [[nodiscard]] std::vector<std::string> GetNodeTypes() const;

    /// The same, plus every node type inside any composite this graph contains. A composite
    /// is one node from the outside, so anything asking "does this graph contain an X" has
    /// to look through it. Walks processors, so message thread only.
    [[nodiscard]] std::vector<std::string> GetNodeTypesDeep() const;

    /// True if any node in this graph must be loaded and prepared on the main thread
    /// (plugin hosts marshalling through JUCE's MessageManager). Lets a composite that
    /// wraps such a node report the same requirement to its parent graph, so it is never
    /// dispatched to a worker thread.
    [[nodiscard]] bool AnyNodeRequiresMainThreadLoad() const;

    // Global settings
    void SetInputTrim(double dB)
    {
        mInputTrim = dB;
    }

    void SetOutputTrim(double dB)
    {
        mOutputTrim = dB;
    }

    // Push the current tempo (BPM) to all nodes that have requiresTempo == true.
    // Call this once per audio block before Process().
    void SetTempo(double bpm);

    // Signal level diagnostics (optional)
    void SetSignalDiagnosticsEnabled(bool enabled)
    {
        mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    }

    [[nodiscard]] bool IsSignalDiagnosticsEnabled() const
    {
        return mSignalDiagnosticsEnabled.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<NodeSignalLevel> GetNodeSignalLevels() const;

    // Spectrum of one node's input, drawn behind an EQ curve. At most one node per executor
    // is tapped, and only while the UI is showing it. Message thread only.

    /// Taps `nodeId`'s input, moving the tap off any other node. Returns false, and taps
    /// nothing, when there is no such node. Cheap to repeat for the node already tapped.
    bool WatchNodeSpectrum(const std::string& nodeId);
    void ClearSpectrumWatch();

    /// The tapped node's spectrum as of `now`. False when nothing is tapped.
    bool ReadWatchedSpectrum(SpectrumTap::Bins& out,
                             std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Runtime control for intra-graph parallel processing.
    void SetParallelLevelsEnabled(bool enabled)
    {
        mParallelLevelsEnabled.store(enabled, std::memory_order_release);
    }

    [[nodiscard]] bool IsParallelLevelsEnabled() const
    {
        return mParallelLevelsEnabled.load(std::memory_order_acquire);
    }

    // Queries
    [[nodiscard]] bool IsValid() const
    {
        return mIsValid;
    }

    [[nodiscard]] std::vector<std::string> GetExecutionOrder() const
    {
        return mExecutionOrder;
    }

    [[nodiscard]] DSPPerformanceStats GetPerformanceStats() const;
    /// Returns the enabled-node longest-path latency through the graph.
    [[nodiscard]] int GetTotalLatencySamples() const;

  private:
    /// NodeState::processingTimeUs for a node that did not run in the last block.
    static constexpr double kNodeDidNotRunUs = -1.0;

    struct NodeState
    {
        std::string id;
        std::string type;
        std::string category;
        /// `type` resolved from any alias, for by-type automation to compare without resolving.
        std::string canonicalType;
        /// `id` again, shared, for AutomationTarget.
        std::shared_ptr<const std::string> sharedId;
        std::unique_ptr<EffectProcessor> processor;
        /// Dual mono: the second instance, which runs the right side. Only ever installed or
        /// replaced under the DSP lock, so the audio thread may read it.
        std::unique_ptr<EffectProcessor> twin;
        /// Built off the lock by StageDualMonoTwins(), waiting for SetDualMono(true) to install it.
        /// The audio thread never reads it.
        std::unique_ptr<EffectProcessor> pendingTwin;
        /// The node's signal this block: its gathered input until it has run, its output after.
        std::vector<float> bufferLeft;
        std::vector<float> bufferRight;
        /// Where the node writes its output, swapped with the pair above once it has run, so
        /// no effect processes in place and nothing is copied back.
        std::vector<float> scratchLeft;
        std::vector<float> scratchRight;
        /// Where the second instance writes, in dual mono.
        std::vector<float> twinScratchLeft;
        std::vector<float> twinScratchRight;
        bool hasInput = false;
        /// The resolved layout of what reaches this node and of what it puts out (see the class
        /// comment). Written only when the layout is resolved, under the DSP lock once running.
        bool inputStereo = false;
        bool outputStereo = false;
        /// The node's channel mode (GraphNode::channelMode): None follows its input; the others
        /// fold a stereo input to one channel, run it mono and put out mono.
        enum class MonoFold : std::uint8_t
        {
            None,
            Sum,
            Left,
            Right,
        };
        MonoFold monoFold = MonoFold::None;
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
        /// Channels the node carried last block: 0 for a node that had no input, else 1 or 2 by
        /// its resolved output layout. Published every block, so the message thread never reads
        /// hasInput.
        std::atomic<int> channelCount{0};
        // Last block's processing time, or kNodeDidNotRunUs when the node did not run. Published
        // here rather than into a map so the audio thread never allocates; GetPerformanceStats()
        // collects it on the message thread, the same way node latency already works. "Did not
        // run" has to stay distinct from any real time so a bypassed node is left out of the stats
        // map. It used to be NaN, but the Release builds' fast floating-point semantics fold
        // std::isfinite to true, so the NaN went out as a time. One atomic rather than a time and a
        // flag, so a reader can never pair halves from two different blocks.
        std::atomic<double> processingTimeUs{kNodeDidNotRunUs};
        /// The executor's spectrum tap while this is the watched node, else null. The tap
        /// outlives every node that points at it; see mSpectrumTap.
        std::atomic<SpectrumTap*> spectrumTap{nullptr};
        /// For a note source: whether it ran this block, and whether it ran the one before.
        /// Written by whichever thread runs the node; read by its players, which always run in a
        /// later level, after the level barrier.
        bool notesThisBlock = false;
        bool notesLastBlock = false;
    };

    /// One resolved incoming connection.
    struct PlannedEdge
    {
        NodeState* source = nullptr;
        float gain = 1.0f;
        int toPort = 0;
    };

    /// A node with everything Process() used to re-derive per block already resolved.
    ///
    /// The execution order only changes when the graph does, but the hot loop was
    /// rediscovering it every block: a std::map<std::string, NodeState> lookup per node,
    /// another for its incoming edge list, one more per edge for the source, a handful of
    /// string comparisons to classify the node type, and a dynamic_cast for mixers. That
    /// was roughly a fifth of the audio thread on a light chain, all of it answering
    /// questions whose answers had not changed since the graph was built.
    struct PlannedNode
    {
        NodeState* state = nullptr;
        std::vector<PlannedEdge> incoming;
        /// Non-null only for mixer nodes; resolved once instead of dynamic_cast per block.
        MixerEffect* mixer = nullptr;
        bool isInput = false;
        bool isOutput = false;
        bool isSplitter = false;
        bool isMixer = false;
        /// Its input is mono (or folded to mono by its channel mode), it has a mono path, and its
        /// type cannot widen: it runs ProcessMono on the left channel and the result is copied to
        /// the right. Set by the layout pass.
        bool runMono = false;
        /// Channel mode Mono on a stereo input: fold the gathered input before the node runs.
        bool foldInput = false;
        /// Channel mode Mono on a type that can widen: sum what it puts out to one channel.
        bool foldOutput = false;
        /// Dual mono: the second instance that runs the right side, or null.
        EffectProcessor* twin = nullptr;
        /// Mixer, or more than one incoming edge: inputs sum rather than overwrite.
        bool accumulateInputs = false;
        /// Note routing (see the class comment). The notes this node makes, if it makes any...
        const NoteBlock* noteOutput = nullptr;
        /// ...and, for a node that plays notes, the sources upstream of it, in plan order, with
        /// room reserved to collect those that ran this block without allocating.
        bool acceptsNotes = false;
        std::vector<const PlannedNode*> noteSources;
        std::vector<const NoteBlock*> liveNotes;
    };

    /// Memoises 10^(dB/20). The trim almost never changes, but std::pow was being
    /// called twice per block per graph regardless.
    ///
    /// It starts out already holding the answer for 0 dB rather than a NaN "not computed
    /// yet" sentinel. The core builds with fast floating-point semantics, where comparing
    /// against NaN is not guaranteed to come out unordered: this one came out equal, so
    /// the first lookup never computed, every later one returned the initial 1.0, and the
    /// Input/Output node gains and the trims did nothing.
    class DbToLinear
    {
      public:
        [[nodiscard]] float Get(double db);

      private:
        double mDb = 0.0;
        float mLinear = 1.0f;
    };

    /// Sorts the graph into levels of independent nodes, which in order are the execution
    /// order, and decides whether it is valid.
    /// A node's resources, resolved by CreateProcessors() and loaded by LoadPendingResources().
    struct PendingResourceLoad
    {
        EffectProcessor* processor = nullptr;
        std::vector<ResourceRef> refs;
        std::vector<std::filesystem::path> paths;
    };

    void BuildExecutionLevels();
    void BuildExecutionPlan();
    /// Resolves every node's layout from mInputLayout, in execution order, and tells each
    /// processor its input layout: every one when `notifyAll` (a freshly built plan), else only
    /// those whose input layout changed. No allocation, so a running graph can redo it.
    void ResolveChannelLayout(bool notifyAll);
    /// Finds each note player's upstream note sources, once the plan's edges are resolved.
    void ResolveNoteRouting();
    /// Hands `planned`, a note player about to run, the sources that ran this block.
    static void HandNotesTo(PlannedNode& planned);
    /// Pushes mAppliedTempoBpm to every node in mTempoAwareStates, second instances included.
    void ApplyTempoToProcessors();
    /// Creates every node's processor and applies its params and config; the resource loads it
    /// resolves wait in mPendingResourceLoads for LoadPendingResources().
    void CreateProcessors();
    /// Loads what CreateProcessors() queued. After the plan is built, so each node already knows
    /// its input layout: a NAM node on a mono connection loads one model, not two.
    void LoadPendingResources();
    /// Applies a node's enabled state, params, config and resources to `processor`: queued in
    /// `loads`, or loaded at once when it is null.
    void ConfigureNodeProcessor(EffectProcessor& processor, const GraphNode& node,
                                std::vector<PendingResourceLoad>* loads);
    [[nodiscard]] static bool NeedsDualMonoTwin(const NodeState& state);
    /// A second instance of `node`, configured as the first was.
    std::unique_ptr<EffectProcessor> CreateDualMonoTwin(const GraphNode& node, std::vector<PendingResourceLoad>* loads);
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState* FindNodeState(const std::string& id);
    [[nodiscard]] const NodeState* FindNodeState(const std::string& id) const;
    void ProcessPlannedNode(PlannedNode& planned, int numSamples, bool diagnosticsEnabled, bool collectLevels);
    /// Points the watched node, and only that node, at mSpectrumTap. Rerun whenever node
    /// states are created, since a new one starts out untapped.
    void ApplySpectrumWatch();

    SignalGraph mGraph;
    ResourceLibrary* mResourceLibrary = nullptr;

    std::map<std::string, NodeState> mNodeStates;
    std::vector<PendingResourceLoad> mPendingResourceLoads;

    // Config applied to every node of a given type at creation — see SetNodeTypeConfigDefault().
    std::map<std::string, std::map<std::string, std::string>> mNodeTypeConfigDefaults;
    std::vector<std::string> mExecutionOrder;
    std::vector<std::vector<std::string>> mExecutionLevels;

    // Resolved form of the above, rebuilt by BuildExecutionPlan() whenever the graph or
    // its processors change. mExecutionLevelPlan holds indices into mPlan, so publishing
    // work to the parallel workers costs an int rather than a string pointer.
    std::vector<PlannedNode> mPlan;
    std::vector<std::vector<int>> mExecutionLevelPlan;
    PlannedNode* mInputPlanNode = nullptr;
    /// Output candidates in id order; Process() takes the first one that ran this block.
    std::vector<PlannedNode*> mOutputPlanNodes;

    /// Every node in execution order with its entry in mGraph, for by-type automation, which
    /// otherwise found them with a map lookup by id and resolved each one's type as it went.
    struct AutomationNode
    {
        NodeState* state = nullptr;
        GraphNode* node = nullptr;
    };

    std::vector<AutomationNode> mAutomationNodes;
    /// Nodes whose type declares requiresTempo, resolved once per plan build. SetTempo() runs
    /// on the audio thread every block, and asking the registry which nodes are tempo-aware
    /// there meant a string copy and an EffectTypeInfo copy — a deep one, parameter and preset
    /// vectors included — per node per block. A node, not a processor: a dual-mono second
    /// instance installed later hears the tempo too.
    std::vector<NodeState*> mTempoAwareStates;
    /// Last tempo pushed to those processors. Tracked so an unchanged tempo — every block
    /// but the handful where it actually moves — costs a comparison instead of a SetParam
    /// walk through each effect's string-keyed parameter dispatch.
    double mAppliedTempoBpm = 0.0;
    /// The Input and Output nodes' own gains in dB: the kBoundaryGainParam entries of the
    /// nodes literally named "__input__"/"__output__", which SetGraph makes sure they hold,
    /// or null without such a node. Distinct from the plan nodes above: a preset can have an
    /// input-*typed* node under a different id, and the trim only ever came from the
    /// well-known ids. Resolved once per plan rather than looked up per block: a std::map
    /// entry keeps its address, and automation writes the value in place.
    const double* mInputNodeGainDb = nullptr;
    const double* mOutputNodeGainDb = nullptr;
    DbToLinear mInputGainCache;
    DbToLinear mOutputGainCache;

    /// Counts down to the next block that refreshes the per-node level meters. They are
    /// read at kSignalDiagnosticsRateHz, so computing them every block just overwrote the
    /// previous block's numbers dozens of times before anything looked at them.
    int mMeteringCountdownSamples = 0;
    std::vector<int> mExecutionLevelScores;
    /// Each node's incoming edges, as indices into mGraph.edges, for the plan and the latency walk.
    std::map<std::string, std::vector<std::size_t>> mIncomingEdgesByNode;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    double mInputTrim = 0.0;
    double mOutputTrim = 0.0;
    bool mIsValid = false;
    bool mPrepared = false;
    ChannelLayout mInputLayout = ChannelLayout::Stereo;
    bool mOutputStereo = false;
    bool mDualMono = false;

    // Last block's totals, published by the audio thread and read by the message thread.
    //
    // Three atomics rather than a DSPPerformanceStats behind a mutex: that struct carries
    // four std::maps, and MSVC's std::map allocates a sentinel node in its default
    // constructor -- so building one per block put a heap allocation and a free on the
    // audio thread, which is both a realtime-safety violation and, on a light chain,
    // around 15% of that thread's time. The maps were never filled here in any case;
    // GetPerformanceStats() assembles them on the message thread.
    std::atomic<double> mLastTotalProcessingTimeUs{0.0};
    std::atomic<double> mLastRealTimeUs{0.0};
    std::atomic<double> mLastDspLoadPercent{0.0};

    std::atomic<bool> mSignalDiagnosticsEnabled{true};
    std::atomic<bool> mParallelLevelsEnabled{true};

    // Parallel node processing within one graph level. Prepare() starts the workers, sized to
    // the widest level, and only for a graph with a level worth fanning out.
    static constexpr int kMaxParallelWorkers = 7;
    rtparallel::RealtimeTaskPool mWorkerPool;
    bool mUseParallelLevels = false;

    /// Created on the first watch and kept for the executor's lifetime, so the audio thread
    /// can never be left holding a pointer to a freed tap.
    std::unique_ptr<SpectrumTap> mSpectrumTap;
    std::string mSpectrumWatchNodeId;
};
} // namespace guitarfx
