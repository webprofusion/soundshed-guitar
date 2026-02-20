#pragma once

#include "presets/PresetTypes.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>

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
    };

    struct NodeSignalLevel
    {
      std::string nodeId;
      std::string nodeType;
      double peak = 0.0;
      double rms = 0.0;
      int clipCount = 0;
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
    bool LoadNodeResource(const std::string &nodeId, const ResourceRef &ref);

    // Queries
    [[nodiscard]] std::string FindFirstNodeOfType(const std::string &type) const;
    [[nodiscard]] std::string FindFirstNodeOfTypes(const std::vector<std::string> &types) const;
    [[nodiscard]] std::vector<std::string> GetNodeTypes() const;

    // Global settings
    void SetInputTrim(double dB) { mInputTrim = dB; }
    void SetOutputTrim(double dB) { mOutputTrim = dB; }

    // Signal level diagnostics (optional)
    void SetSignalDiagnosticsEnabled(bool enabled) { mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release); }
    [[nodiscard]] bool IsSignalDiagnosticsEnabled() const { return mSignalDiagnosticsEnabled.load(std::memory_order_acquire); }
    [[nodiscard]] std::vector<NodeSignalLevel> GetNodeSignalLevels() const;

    // Queries
    [[nodiscard]] bool IsValid() const { return mIsValid; }
    [[nodiscard]] std::vector<std::string> GetExecutionOrder() const { return mExecutionOrder; }
    [[nodiscard]] DSPPerformanceStats GetPerformanceStats() const;

  private:
    struct NodeState
    {
      std::string id;
      std::string type;
      std::unique_ptr<EffectProcessor> processor;
      std::vector<float> bufferLeft;
      std::vector<float> bufferRight;
      bool hasInput = false;
      std::atomic<double> peak{0.0};
      std::atomic<double> rms{0.0};
      std::atomic<int> clipCount{0};
    };

    void BuildExecutionOrder();
    void CreateProcessors();
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState *FindNodeState(const std::string &id);

    SignalGraph mGraph;
    ResourceLibrary *mResourceLibrary = nullptr;

    std::map<std::string, NodeState> mNodeStates;
    std::vector<std::string> mExecutionOrder;
    std::map<std::string, int> mIncomingEdgeCount;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    double mInputTrim = 0.0;
    double mOutputTrim = 0.0;
    bool mIsValid = false;
    bool mPrepared = false;

    DSPPerformanceStats mLastPerformanceStats;
    mutable std::mutex mPerformanceStatsMutex;

    std::atomic<bool> mSignalDiagnosticsEnabled{false};

    // Temporary buffers for mixing
    std::vector<float> mTempLeftBuffer;
    std::vector<float> mTempRightBuffer;
  };

} // namespace guitarfx
