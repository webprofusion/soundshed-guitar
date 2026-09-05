#pragma once

#include "presets/PresetTypes.h"
#include "dsp/SignalTelemetry.h"
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

/**
 * Executes a signal graph by processing audio through nodes in topological order.
 */
class SignalGraphExecutor
{
  public:
    struct DSPPerformanceStats
    {
        double totalProcessingTimeUs = 0.0;                        // Total time in microseconds
        double realTimeUs = 0.0;                                   // Real-time equivalent in microseconds
        double dspLoadPercent = 0.0;                               // % of real-time
        std::map<std::string, double> nodeProcessingTimesUs;       // Per-node times
        std::map<std::string, double> scopedNodeProcessingTimesUs; // Optional scoped keys for UI correlation
        std::map<std::string, int> nodeLatencySamples;             // Per-node algorithmic latency
        std::map<std::string, int> scopedNodeLatencySamples;       // Optional scoped keys for UI correlation
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

    // Queries
    [[nodiscard]] std::string FindFirstNodeOfType(const std::string& type) const;
    [[nodiscard]] std::vector<std::string> FindNodesOfType(const std::string& type, bool includeDisabled = true) const;
    [[nodiscard]] std::string FindFirstNodeOfTypes(const std::vector<std::string>& types) const;
    [[nodiscard]] std::vector<std::string> GetNodeTypes() const;

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
    struct NodeState
    {
        std::string id;
        std::string type;
        std::string category;
        std::unique_ptr<EffectProcessor> processor;
        std::vector<float> bufferLeft;
        std::vector<float> bufferRight;
        bool hasInput = false;
        bool hasStereoSignal = false;
        std::atomic<double> peak{0.0};
        std::atomic<double> rms{0.0};
        std::atomic<int> clipCount{0};
        // Last block's processing time, or NaN when the node did not run. Published here
        // rather than into a map so the audio thread never allocates; GetPerformanceStats()
        // collects it on the message thread, the same way node latency already works. NaN
        // (not 0) marks "did not run" so a bypassed node stays absent from the stats map.
        std::atomic<double> processingTimeUs{std::numeric_limits<double>::quiet_NaN()};
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
    };

    /// Memoises 10^(dB/20). The trim almost never changes, but std::pow was being
    /// called twice per block per graph regardless.
    class DbToLinear
    {
      public:
        [[nodiscard]] float Get(double db);

      private:
        double mDb = std::numeric_limits<double>::quiet_NaN();
        float mLinear = 1.0f;
    };

    void BuildExecutionOrder();
    void BuildExecutionLevels();
    void BuildExecutionPlan();
    /// Pushes mAppliedTempoBpm to every processor in mTempoAwareProcessors.
    void ApplyTempoToProcessors();
    void CreateProcessors();
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState* FindNodeState(const std::string& id);
    [[nodiscard]] const NodeState* FindNodeState(const std::string& id) const;
    void ProcessPlannedNode(PlannedNode& planned, int numSamples, bool diagnosticsEnabled, bool collectLevels);
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

    // Temporary buffers for mixing
    std::vector<float> mTempLeftBuffer;
    std::vector<float> mTempRightBuffer;
};
} // namespace guitarfx
