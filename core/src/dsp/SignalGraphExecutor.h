#pragma once

#include "presets/PresetTypes.h"
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
  class ResourceLibrary;

  /**
   * Executes a signal graph by processing audio through nodes in topological order.
   */
  class SignalGraphExecutor
  {
  public:
    struct DSPPerformanceStats
    {
      double totalProcessingTimeUs = 0.0; // Total time in microseconds
      double realTimeUs = 0.0;             // Real-time equivalent in microseconds
      double dspLoadPercent = 0.0;         // % of real-time
      std::map<std::string, double> nodeProcessingTimesUs; // Per-node times
      std::map<std::string, double> scopedNodeProcessingTimesUs; // Optional scoped keys for UI correlation
      std::map<std::string, int> nodeLatencySamples; // Per-node algorithmic latency
      std::map<std::string, int> scopedNodeLatencySamples; // Optional scoped keys for UI correlation
    };

    struct NodeSignalLevel
    {
      struct AnalyzerTelemetry
      {
        double peakPercent = 0.0;
        double rmsPercent = 0.0;
        double rmsDbu = 0.0;
        double rmsDbv = 0.0;
        double rmsVolts = 0.0;
        bool loudnessValid = false;
        double momentaryLufs = -std::numeric_limits<double>::infinity();
        double shortTermLufs = -std::numeric_limits<double>::infinity();
        double integratedLufs = -std::numeric_limits<double>::infinity();
        bool stereo = false;
        int activeChannelCount = 0;
        std::vector<float> spectrogramBinsDb;
        double spectrogramMinDbfs = -120.0;
        double spectrogramMaxDbfs = 0.0;
        double spectrogramMinFrequencyHz = 20.0;
        double spectrogramMaxFrequencyHz = 20000.0;
        std::vector<float> barkBandsDb;
        double barkMinDbfs = -96.0;
        double barkMaxDbfs = 0.0;
        double barkMinFrequencyHz = 20.0;
        double barkMaxFrequencyHz = 15500.0;
        std::uint64_t generatedAtMs = 0;
      };

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

    SignalGraphExecutor(const SignalGraphExecutor &) = delete;
    SignalGraphExecutor &operator=(const SignalGraphExecutor &) = delete;
    SignalGraphExecutor(SignalGraphExecutor &&other) noexcept;
    SignalGraphExecutor &operator=(SignalGraphExecutor &&other) noexcept;

    // Setup
    void SetGraph(const SignalGraph &graph);
    void SetResourceLibrary(ResourceLibrary *library) { mResourceLibrary = library; }
    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float **inputs, float **outputs, int numSamples);

    // Node control
    void SetNodeEnabled(const std::string &nodeId, bool enabled);
    void SetNodeParam(const std::string &nodeId, const std::string &key, double value);
    void SetNodeConfig(const std::string &nodeId, const std::string &key, const std::string &value);
    void SetNodeConfigForType(const std::string &type, const std::string &key, const std::string &value);

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
    void SetNodeTypeConfigDefault(const std::string &type, const std::string &key, const std::string &value);

    /// Type defaults recorded so far, for seeding a nested or newly created executor.
    [[nodiscard]] const std::map<std::string, std::map<std::string, std::string>> &
    GetNodeTypeConfigDefaults() const { return mNodeTypeConfigDefaults; }

    /// Copy another executor's type defaults wholesale (does not touch existing nodes).
    void SeedNodeTypeConfigDefaults(const std::map<std::string, std::map<std::string, std::string>> &defaults);
    bool LoadNodeResource(const std::string &nodeId, const ResourceRef &ref);
    [[nodiscard]] std::string GetNodeConfig(const std::string &nodeId, const std::string &key) const;
    [[nodiscard]] EffectProcessor *GetNodeProcessor(const std::string &nodeId);
    [[nodiscard]] const EffectProcessor *GetNodeProcessor(const std::string &nodeId) const;

    // Queries
    [[nodiscard]] std::string FindFirstNodeOfType(const std::string &type) const;
    [[nodiscard]] std::vector<std::string> FindNodesOfType(const std::string &type, bool includeDisabled = true) const;
    [[nodiscard]] std::string FindFirstNodeOfTypes(const std::vector<std::string> &types) const;
    [[nodiscard]] std::vector<std::string> GetNodeTypes() const;

    /// True if any node in this graph must be loaded and prepared on the main thread
    /// (plugin hosts marshalling through JUCE's MessageManager). Lets a composite that
    /// wraps such a node report the same requirement to its parent graph, so it is never
    /// dispatched to a worker thread.
    [[nodiscard]] bool AnyNodeRequiresMainThreadLoad() const;

    // Global settings
    void SetInputTrim(double dB) { mInputTrim = dB; }
    void SetOutputTrim(double dB) { mOutputTrim = dB; }
    void SetNamInputModeMono(bool mono) { mNamInputModeMono = mono; }

    // Push the current tempo (BPM) to all nodes that have requiresTempo == true.
    // Call this once per audio block before Process().
    void SetTempo(double bpm);

    // Signal level diagnostics (optional)
    void SetSignalDiagnosticsEnabled(bool enabled) { mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release); }
    [[nodiscard]] bool IsSignalDiagnosticsEnabled() const { return mSignalDiagnosticsEnabled.load(std::memory_order_acquire); }
    [[nodiscard]] std::vector<NodeSignalLevel> GetNodeSignalLevels() const;

    // Runtime control for intra-graph parallel processing.
    void SetParallelLevelsEnabled(bool enabled) { mParallelLevelsEnabled.store(enabled, std::memory_order_release); }
    [[nodiscard]] bool IsParallelLevelsEnabled() const { return mParallelLevelsEnabled.load(std::memory_order_acquire); }

    // Queries
    [[nodiscard]] bool IsValid() const { return mIsValid; }
    [[nodiscard]] std::vector<std::string> GetExecutionOrder() const { return mExecutionOrder; }
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

    void BuildExecutionOrder();
    void BuildExecutionLevels();
    void CreateProcessors();
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState *FindNodeState(const std::string &id);
    [[nodiscard]] const NodeState *FindNodeState(const std::string &id) const;
    void ProcessNodeById(const std::string &nodeId,
               int numSamples,
               bool diagnosticsEnabled);
    void StartWorkers(int count);
    void StopWorkers();
    void WorkerLoop();

    SignalGraph mGraph;
    ResourceLibrary *mResourceLibrary = nullptr;

    std::map<std::string, NodeState> mNodeStates;
    // Config applied to every node of a given type at creation — see SetNodeTypeConfigDefault().
    std::map<std::string, std::map<std::string, std::string>> mNodeTypeConfigDefaults;
    std::vector<std::string> mExecutionOrder;
    std::vector<std::vector<std::string>> mExecutionLevels;
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

    DSPPerformanceStats mLastPerformanceStats;
    mutable std::mutex mPerformanceStatsMutex;

    std::atomic<bool> mSignalDiagnosticsEnabled{true};
    std::atomic<bool> mParallelLevelsEnabled{true};
    bool mNamInputModeMono = false;

    // Parallel node processing within one graph level.
    static constexpr int kMaxParallelWorkers = 7;
    static constexpr int kMaxParallelWorkItems = 128;
    struct ParallelWorkItem
    {
      const std::string *nodeId = nullptr;
      int numSamples = 0;
      bool diagnosticsEnabled = false;
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
