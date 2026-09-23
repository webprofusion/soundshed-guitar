#pragma once

#include "presets/PresetTypes.h"
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

    SignalGraphExecutor(const SignalGraphExecutor&) = delete;
    SignalGraphExecutor& operator=(const SignalGraphExecutor&) = delete;
    SignalGraphExecutor(SignalGraphExecutor&& other) noexcept;
    SignalGraphExecutor& operator=(SignalGraphExecutor&& other) noexcept;

    // Setup
    void SetGraph(const SignalGraph& graph);

    void SetResourceLibrary(ResourceLibrary* library)
    {
        mResourceLibrary = library;
    }

    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float** inputs, float** outputs, int numSamples);

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
    [[nodiscard]] std::string GetNodeConfig(const std::string& nodeId, const std::string& key) const;
    [[nodiscard]] EffectProcessor* GetNodeProcessor(const std::string& nodeId);
    [[nodiscard]] const EffectProcessor* GetNodeProcessor(const std::string& nodeId) const;

    // By-type automation. MIDI and DAW automation apply on the audio thread, under the DSP lock,
    // so none of these allocate. Each takes a type already resolved (EffectRegistry::Resolve).

    /// A node automation drives, as FindAutomationTarget found it. Valid until the graph changes.
    struct AutomationTarget
    {
        EffectProcessor* processor = nullptr;
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

    void SetNamInputModeMono(bool mono)
    {
        mNamInputModeMono = mono;
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
        std::vector<float> bufferLeft;
        std::vector<float> bufferRight;
        bool hasInput = false;
        bool hasStereoSignal = false;
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
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
        bool mayProduceStereo = false;
        bool isNam = false;
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

    void BuildExecutionOrder();
    void BuildExecutionLevels();
    void BuildExecutionPlan();
    /// Finds each note player's upstream note sources, once the plan's edges are resolved.
    void ResolveNoteRouting();
    /// Hands `planned`, a note player about to run, the sources that ran this block.
    static void HandNotesTo(PlannedNode& planned);
    /// Pushes mAppliedTempoBpm to every processor in mTempoAwareProcessors.
    void ApplyTempoToProcessors();
    void CreateProcessors();
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState* FindNodeState(const std::string& id);
    [[nodiscard]] const NodeState* FindNodeState(const std::string& id) const;
    void ProcessPlannedNode(PlannedNode& planned, int numSamples, bool diagnosticsEnabled, bool collectLevels);
    /// Points the watched node, and only that node, at mSpectrumTap. Rerun whenever node
    /// states are created, since a new one starts out untapped.
    void ApplySpectrumWatch();
    void StartWorkers(int count);
    void StopWorkers();
    void WorkerLoop();

    SignalGraph mGraph;
    ResourceLibrary* mResourceLibrary = nullptr;

    std::map<std::string, NodeState> mNodeStates;
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
    /// Processors whose type declares requiresTempo, resolved once per plan build.
    /// SetTempo() runs on the audio thread every block, and asking the registry which
    /// nodes are tempo-aware there meant a string copy and an EffectTypeInfo copy — a
    /// deep one, parameter and preset vectors included — per node per block.
    std::vector<EffectProcessor*> mTempoAwareProcessors;
    /// Last tempo pushed to those processors. Tracked so an unchanged tempo — every block
    /// but the handful where it actually moves — costs a comparison instead of a SetParam
    /// walk through each effect's string-keyed parameter dispatch.
    double mAppliedTempoBpm = 0.0;
    /// The nodes literally named "__input__"/"__output__", which carry the trim gains.
    /// Distinct from the plan nodes above: a preset can have an input-*typed* node under
    /// a different id, and the trim only ever came from the well-known ids.
    const GraphNode* mInputTrimNode = nullptr;
    const GraphNode* mOutputTrimNode = nullptr;
    DbToLinear mInputGainCache;
    DbToLinear mOutputGainCache;

    /// Counts down to the next block that refreshes the per-node level meters. They are
    /// read at kSignalDiagnosticsRateHz, so computing them every block just overwrote the
    /// previous block's numbers dozens of times before anything looked at them.
    int mMeteringCountdownSamples = 0;
    std::vector<int> mExecutionLevelScores;
    std::map<std::string, int> mIncomingEdgeCount;
    // Precomputed per-node incoming edge index lists (into mGraph.edges) for O(1) lookup in Process()
    std::map<std::string, std::vector<std::size_t>> mIncomingEdgesByNode;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    double mInputTrim = 0.0;
    double mOutputTrim = 0.0;
    bool mIsValid = false;
    bool mPrepared = false;

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
    bool mNamInputModeMono = false;

    // Parallel node processing within one graph level.
    static constexpr int kMaxParallelWorkers = 7;
    static constexpr int kMaxParallelWorkItems = 128;

    struct ParallelWorkItem
    {
        int planIndex = -1;
        int numSamples = 0;
        bool diagnosticsEnabled = false;
        bool collectLevels = false;
    };

    std::array<ParallelWorkItem, kMaxParallelWorkItems> mWorkItems{};
    std::atomic<int> mParallelTaskHead{0};
    std::atomic<int> mParallelTaskCount{0};
    std::atomic<int> mParallelDoneCount{0};
    std::atomic<uint32_t> mParallelGeneration{0};
    std::atomic<bool> mParallelQuit{false};
    std::mutex mParallelMutex;
    std::condition_variable mParallelCv;
    std::vector<std::thread> mWorkerThreads;
    bool mUseParallelLevels = false;

    /// Created on the first watch and kept for the executor's lifetime, so the audio thread
    /// can never be left holding a pointer to a freed tap.
    std::unique_ptr<SpectrumTap> mSpectrumTap;
    std::string mSpectrumWatchNodeId;

    // Temporary buffers for mixing
    std::vector<float> mTempLeftBuffer;
    std::vector<float> mTempRightBuffer;
};
} // namespace guitarfx
