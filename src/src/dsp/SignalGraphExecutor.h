#pragma once

#include "presets/PresetTypes.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

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
    SignalGraphExecutor();
    ~SignalGraphExecutor();

    SignalGraphExecutor(const SignalGraphExecutor &) = delete;
    SignalGraphExecutor &operator=(const SignalGraphExecutor &) = delete;
    SignalGraphExecutor(SignalGraphExecutor &&) noexcept = default;
    SignalGraphExecutor &operator=(SignalGraphExecutor &&) noexcept = default;

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
    bool LoadNodeResource(const std::string &nodeId, const ResourceRef &ref);

    // Queries
    [[nodiscard]] std::string FindFirstNodeOfType(const std::string &type) const;
    [[nodiscard]] std::vector<std::string> GetNodeTypes() const;

    // Global settings
    void SetInputTrim(double dB) { mInputTrim = dB; }
    void SetOutputTrim(double dB) { mOutputTrim = dB; }

    // Queries
    [[nodiscard]] bool IsValid() const { return mIsValid; }
    [[nodiscard]] std::vector<std::string> GetExecutionOrder() const { return mExecutionOrder; }

  private:
    struct NodeState
    {
      std::string id;
      std::string type;
      std::unique_ptr<EffectProcessor> processor;
      std::vector<float> bufferLeft;
      std::vector<float> bufferRight;
      bool hasInput = false;
    };

    void BuildExecutionOrder();
    void CreateProcessors();
    void AllocateBuffers(int maxBlockSize);
    [[nodiscard]] NodeState *FindNodeState(const std::string &id);

    SignalGraph mGraph;
    ResourceLibrary *mResourceLibrary = nullptr;

    std::map<std::string, NodeState> mNodeStates;
    std::vector<std::string> mExecutionOrder;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    double mInputTrim = 0.0;
    double mOutputTrim = 0.0;
    bool mIsValid = false;
    bool mPrepared = false;

    // Temporary buffers for mixing
    std::vector<float> mTempLeftBuffer;
    std::vector<float> mTempRightBuffer;
  };

} // namespace guitarfx
