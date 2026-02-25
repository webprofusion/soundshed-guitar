#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/MixerEffect.h"
#include "dsp/effects/CompositeEffectProcessor.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <set>
#include <chrono>
#include <tuple>

namespace guitarfx
{
  namespace
  {
    struct LevelStats
    {
      double peak = 0.0;
      double rms = 0.0;
      int clipCount = 0;
    };

    LevelStats ComputeLevelStats(const float *left, const float *right, int numSamples)
    {
      LevelStats stats;
      if (numSamples <= 0)
      {
        return stats;
      }

      double sumSquares = 0.0;
      std::size_t sampleCount = 0;

      if (left)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          const float value = left[i];
          const float absValue = std::abs(value);
          stats.peak = std::max(stats.peak, static_cast<double>(absValue));
          sumSquares += static_cast<double>(value) * static_cast<double>(value);
          if (absValue > 1.0f)
          {
            stats.clipCount++;
          }
        }
        sampleCount += static_cast<std::size_t>(numSamples);
      }

      if (right)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          const float value = right[i];
          const float absValue = std::abs(value);
          stats.peak = std::max(stats.peak, static_cast<double>(absValue));
          sumSquares += static_cast<double>(value) * static_cast<double>(value);
          if (absValue > 1.0f)
          {
            stats.clipCount++;
          }
        }
        sampleCount += static_cast<std::size_t>(numSamples);
      }

      if (sampleCount > 0)
      {
        stats.rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
      }

      return stats;
    }
  }

  SignalGraphExecutor::SignalGraphExecutor() = default;
  SignalGraphExecutor::~SignalGraphExecutor() = default;

  SignalGraphExecutor::SignalGraphExecutor(SignalGraphExecutor &&other) noexcept
  {
    *this = std::move(other);
  }

  SignalGraphExecutor &SignalGraphExecutor::operator=(SignalGraphExecutor &&other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    mGraph = std::move(other.mGraph);
    mResourceLibrary = other.mResourceLibrary;
    mNodeStates = std::move(other.mNodeStates);
    mExecutionOrder = std::move(other.mExecutionOrder);
    mIncomingEdgeCount = std::move(other.mIncomingEdgeCount);
    mIncomingEdgesByNode = std::move(other.mIncomingEdgesByNode);
    mSampleRate = other.mSampleRate;
    mMaxBlockSize = other.mMaxBlockSize;
    mInputTrim = other.mInputTrim;
    mOutputTrim = other.mOutputTrim;
    mIsValid = other.mIsValid;
    mPrepared = other.mPrepared;
    mLastPerformanceStats = std::move(other.mLastPerformanceStats);
    mTempLeftBuffer = std::move(other.mTempLeftBuffer);
    mTempRightBuffer = std::move(other.mTempRightBuffer);
    mSignalDiagnosticsEnabled.store(other.mSignalDiagnosticsEnabled.load(std::memory_order_acquire), std::memory_order_release);

    return *this;
  }

  void SignalGraphExecutor::SetGraph(const SignalGraph &graph)
  {
    mGraph = graph;
    mIsValid = false;
    mPrepared = false;
    mNodeStates.clear();
    mExecutionOrder.clear();
    mIncomingEdgeCount.clear();

    // Add implicit input/output nodes if they're referenced in edges but not in nodes
    bool hasInputNode = false;
    bool hasOutputNode = false;

    for (const auto &node : mGraph.nodes)
    {
      if (node.id == "__input__" || node.type == kNodeTypeInput)
        hasInputNode = true;
      if (node.id == "__output__" || node.type == kNodeTypeOutput)
        hasOutputNode = true;
    }

    // Check if edges reference __input__ or __output__
    bool edgesReferenceInput = false;
    bool edgesReferenceOutput = false;
    for (const auto &edge : mGraph.edges)
    {
      if (edge.from == "__input__")
        edgesReferenceInput = true;
      if (edge.to == "__output__")
        edgesReferenceOutput = true;
    }

    // Add implicit nodes if needed
    if (edgesReferenceInput && !hasInputNode)
    {
      GraphNode inputNode;
      inputNode.id = "__input__";
      inputNode.type = kNodeTypeInput;
      inputNode.enabled = true;
      mGraph.nodes.insert(mGraph.nodes.begin(), inputNode);
    }

    if (edgesReferenceOutput && !hasOutputNode)
    {
      GraphNode outputNode;
      outputNode.id = "__output__";
      outputNode.type = kNodeTypeOutput;
      outputNode.enabled = true;
      mGraph.nodes.push_back(outputNode);
    }

    // Track incoming edge counts and precompute per-node incoming edge index lists
    mIncomingEdgesByNode.clear();
    for (const auto &node : mGraph.nodes)
    {
      mIncomingEdgeCount[node.id] = 0;
      mIncomingEdgesByNode[node.id] = {};
    }
    for (std::size_t i = 0; i < mGraph.edges.size(); ++i)
    {
      const auto &edge = mGraph.edges[i];
      mIncomingEdgeCount[edge.to] += 1;
      mIncomingEdgesByNode[edge.to].push_back(i);
    }

    BuildExecutionOrder();
    CreateProcessors();

    if (mPrepared)
    {
      Prepare(mSampleRate, mMaxBlockSize);
    }
  }

  void SignalGraphExecutor::BuildExecutionOrder()
  {
    // Topological sort using Kahn's algorithm
    std::map<std::string, int> inDegree;
    std::map<std::string, std::vector<std::string>> adjacency;

    // Initialize
    for (const auto &node : mGraph.nodes)
    {
      inDegree[node.id] = 0;
      adjacency[node.id] = {};
    }

    // Build adjacency and in-degree
    for (const auto &edge : mGraph.edges)
    {
      adjacency[edge.from].push_back(edge.to);
      inDegree[edge.to]++;
    }

    // Find all nodes with no incoming edges
    std::queue<std::string> queue;
    for (const auto &[id, degree] : inDegree)
    {
      if (degree == 0)
      {
        queue.push(id);
      }
    }

    // Process
    mExecutionOrder.clear();
    while (!queue.empty())
    {
      std::string current = queue.front();
      queue.pop();
      mExecutionOrder.push_back(current);

      for (const auto &neighbor : adjacency[current])
      {
        inDegree[neighbor]--;
        if (inDegree[neighbor] == 0)
        {
          queue.push(neighbor);
        }
      }
    }

    // Check if we processed all nodes (no cycles)
    mIsValid = (mExecutionOrder.size() == mGraph.nodes.size());
  }

  void SignalGraphExecutor::CreateProcessors()
  {
    auto &registry = EffectRegistry::Instance();

    for (const auto &node : mGraph.nodes)
    {
      auto [it, inserted] = mNodeStates.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node.id),
        std::forward_as_tuple());
      NodeState &state = it->second;
      state.id = node.id;
      state.type = node.type;

      // Create processor based on type
      if (node.type == kNodeTypeInput || node.type == kNodeTypeOutput)
      {
        // Input/output are handled specially, use passthrough
        state.processor = std::make_unique<PassthroughProcessor>();
      }
      else if (node.type == kNodeTypeSplitter)
      {
        // Splitter is handled specially with passthrough
        state.processor = std::make_unique<PassthroughProcessor>();
      }
      else if (node.type == kNodeTypeMixer)
      {
        // Mixer uses MixerEffect for per-input control (level, pan, delay)
        state.processor = registry.Create(node.type);
        if (!state.processor)
        {
          state.processor = std::make_unique<PassthroughProcessor>();
        }
      }
      else
      {
        // Create from registry
        state.processor = registry.Create(node.type);
      }

      // If this is a composite effect, pass the resource library to its inner executor
      if (state.processor && mResourceLibrary)
      {
        auto *composite = dynamic_cast<CompositeEffectProcessor *>(state.processor.get());
        if (composite)
        {
          composite->SetResourceLibrary(mResourceLibrary);
        }
      }

      // Apply parameters
      if (state.processor)
      {
        state.processor->SetEnabled(node.enabled);

        for (const auto &[key, value] : node.params)
        {
          state.processor->SetParam(key, value);
        }

        for (const auto &[key, value] : node.config)
        {
          state.processor->SetConfig(key, value);
        }

        // Load resources if needed
        if (!node.resources.empty() && mResourceLibrary)
        {
          std::vector<ResourceRef> resolvedRefs;
          std::vector<std::filesystem::path> resolvedPaths;
          resolvedRefs.reserve(node.resources.size());
          resolvedPaths.reserve(node.resources.size());

          for (const auto& res : node.resources)
          {
            if (!res.IsValid())
              continue;
            auto path = mResourceLibrary->ResolveResource(res);
            if (path)
            {
              resolvedRefs.push_back(res);
              resolvedPaths.push_back(*path);
            }
            else if (res.IsFilePath())
            {
              resolvedRefs.push_back(res);
              resolvedPaths.push_back(res.filePath);
            }
          }

          if (!resolvedPaths.empty())
          {
            state.processor->LoadResources(resolvedRefs, resolvedPaths);
          }
        }
      }

    }
  }

  std::vector<std::string> SignalGraphExecutor::GetNodeTypes() const
  {
    std::vector<std::string> types;
    types.reserve(mNodeStates.size());
    for (const auto &entry : mNodeStates)
    {
      types.push_back(entry.second.type);
    }
    return types;
  }

  void SignalGraphExecutor::Prepare(double sampleRate, int maxBlockSize)
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    AllocateBuffers(maxBlockSize);

    for (auto &[id, state] : mNodeStates)
    {
      if (state.processor)
      {
        state.processor->Prepare(sampleRate, maxBlockSize);
      }
    }
  }

  void SignalGraphExecutor::Reset()
  {
    for (auto &[id, state] : mNodeStates)
    {
      if (state.processor)
      {
        state.processor->Reset();
      }
    }
  }

  void SignalGraphExecutor::AllocateBuffers(int maxBlockSize)
  {
    for (auto &[id, state] : mNodeStates)
    {
      state.bufferLeft.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      state.bufferRight.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    }

    mTempLeftBuffer.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTempRightBuffer.resize(static_cast<size_t>(maxBlockSize), 0.0f);
  }

  void SignalGraphExecutor::Process(float **inputs, float **outputs, int numSamples)
  {
    auto totalStart = std::chrono::high_resolution_clock::now();

    const bool diagnosticsEnabled = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    // Clamp to allocated buffer size to prevent out-of-bounds writes
    numSamples = std::min(numSamples, mMaxBlockSize);

    if (!mIsValid || !mPrepared || !inputs || !outputs)
    {
      // Output silence if not ready
      if (outputs)
      {
        if (outputs[0])
          std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
        if (outputs[1])
          std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
      }
      return;
    }

    // Calculate real-time for this block
    double realTimeSeconds = static_cast<double>(numSamples) / mSampleRate;
    double realTimeUs = realTimeSeconds * 1e6;

    DSPPerformanceStats localStats;
    localStats.realTimeUs = realTimeUs;

    // Clear all buffers and reset input flags
    for (auto &[id, state] : mNodeStates)
    {
      std::fill(state.bufferLeft.begin(), state.bufferLeft.begin() + numSamples, 0.0f);
      std::fill(state.bufferRight.begin(), state.bufferRight.begin() + numSamples, 0.0f);
      state.hasInput = false;
      if (diagnosticsEnabled)
      {
        state.peak.store(0.0, std::memory_order_relaxed);
        state.rms.store(0.0, std::memory_order_relaxed);
        state.clipCount.store(0, std::memory_order_relaxed);
      }
    }

    // Apply input trim (global + input node gain)
    const auto* inputNode = mGraph.FindNode("__input__");
    const double inputNodeGainDb = (inputNode && inputNode->params.count("gainDb"))
      ? inputNode->params.at("gainDb")
      : 0.0;
    const float inputGain = static_cast<float>(std::pow(10.0, (mInputTrim + inputNodeGainDb) / 20.0));

    // Find input node and copy input
    for (auto &[id, state] : mNodeStates)
    {
      const auto *node = mGraph.FindNode(id);
      if (node && (node->type == kNodeTypeInput || node->id == "__input__"))
      {
        if (inputs[0])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            state.bufferLeft[static_cast<size_t>(i)] = inputs[0][i] * inputGain;
          }
        }
        if (inputs[1])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            state.bufferRight[static_cast<size_t>(i)] = inputs[1][i] * inputGain;
          }
        }
        state.hasInput = true;
        if (diagnosticsEnabled)
        {
          const auto stats = ComputeLevelStats(state.bufferLeft.data(), state.bufferRight.data(), numSamples);
          state.peak.store(stats.peak, std::memory_order_relaxed);
          state.rms.store(stats.rms, std::memory_order_relaxed);
          state.clipCount.store(stats.clipCount, std::memory_order_relaxed);
        }
        break;
      }
    }

    // Process nodes in topological order
    for (const auto &nodeId : mExecutionOrder)
    {
      auto *state = FindNodeState(nodeId);
      if (!state)
        continue;

      // Use cached type from NodeState — avoids O(N) FindNode scan in hot path
      const std::string &nodeType = state->type;

      // Skip canonical input routing node (already fed from host input)
      if (nodeType == kNodeTypeInput)
        continue;

      // Gather inputs from incoming edges using precomputed index list
      const auto inEdgesIt = mIncomingEdgesByNode.find(nodeId);
      const int incomingCount = (inEdgesIt != mIncomingEdgesByNode.end())
        ? static_cast<int>(inEdgesIt->second.size()) : 0;
      const bool isMixer = (nodeType == kNodeTypeMixer);
      const bool shouldAccumulate = isMixer || (incomingCount > 1);

      // Get MixerEffect if this is a mixer node
      MixerEffect *mixerEffect = nullptr;
      if (isMixer && state->processor)
      {
        mixerEffect = dynamic_cast<MixerEffect *>(state->processor.get());
      }

      if (inEdgesIt != mIncomingEdgesByNode.end())
      {
        for (std::size_t edgeIdx : inEdgesIt->second)
        {
          const auto &edge = mGraph.edges[edgeIdx];
          auto *sourceState = FindNodeState(edge.from);
          if (sourceState && sourceState->hasInput)
          {
            const float edgeGain = static_cast<float>(edge.gain);
            const int inputPort = edge.toPort;

            // Handle mixer with per-input processing
            if (isMixer && mixerEffect)
            {
              // Check if this input is muted
              if (mixerEffect->IsInputMuted(inputPort))
              {
                // Skip muted inputs
                continue;
              }

              // Get per-input coefficients from MixerEffect
              const float level = mixerEffect->GetInputLevel(inputPort);
              const float panL = mixerEffect->GetInputPanL(inputPort);
              const float panR = mixerEffect->GetInputPanR(inputPort);

              // Apply level and pan
              const float gainL = edgeGain * level * panL;
              const float gainR = edgeGain * level * panR;

              for (int i = 0; i < numSamples; ++i)
              {
                state->bufferLeft[static_cast<size_t>(i)] += sourceState->bufferLeft[static_cast<size_t>(i)] * gainL;
                state->bufferRight[static_cast<size_t>(i)] += sourceState->bufferRight[static_cast<size_t>(i)] * gainR;
              }
              state->hasInput = true;
            }
            else if (shouldAccumulate)
            {
              // Non-mixer multi-input: simple accumulation
              for (int i = 0; i < numSamples; ++i)
              {
                state->bufferLeft[static_cast<size_t>(i)] += sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                state->bufferRight[static_cast<size_t>(i)] += sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
              }
            }
            else
            {
              // Normal node: copy input (last edge wins for non-mixers)
              for (int i = 0; i < numSamples; ++i)
              {
                state->bufferLeft[static_cast<size_t>(i)] = sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
                state->bufferRight[static_cast<size_t>(i)] = sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
              }
            }
            state->hasInput = true;
          }
        }
      }

      // Process the node
      if (state->processor && state->hasInput)
      {
        if (nodeType == kNodeTypeSplitter || nodeType == kNodeTypeOutput)
        {
          // These nodes just pass through (routing handled above)
        }
        else if (nodeType == kNodeTypeMixer)
        {
          // Mixer: apply master gain via Process()
          if (state->processor->IsEnabled())
          {
            float *inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
            float *outPtrs[2] = {mTempLeftBuffer.data(), mTempRightBuffer.data()};
            state->processor->Process(inPtrs, outPtrs, numSamples);
            std::copy(mTempLeftBuffer.begin(), mTempLeftBuffer.begin() + numSamples, state->bufferLeft.begin());
            std::copy(mTempRightBuffer.begin(), mTempRightBuffer.begin() + numSamples, state->bufferRight.begin());
          }
        }
        else if (state->processor->IsEnabled())
        {
          // Process effect
          float *inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
          float *outPtrs[2] = {mTempLeftBuffer.data(), mTempRightBuffer.data()};

          auto nodeStart = std::chrono::high_resolution_clock::now();
          state->processor->Process(inPtrs, outPtrs, numSamples);
          auto nodeEnd = std::chrono::high_resolution_clock::now();
          const std::chrono::duration<double, std::micro> nodeDuration(nodeEnd - nodeStart);
          localStats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();

          // Copy back
          std::copy(mTempLeftBuffer.begin(), mTempLeftBuffer.begin() + numSamples, state->bufferLeft.begin());
          std::copy(mTempRightBuffer.begin(), mTempRightBuffer.begin() + numSamples, state->bufferRight.begin());
        }
      }

      if (diagnosticsEnabled && state->hasInput)
      {
        const auto stats = ComputeLevelStats(state->bufferLeft.data(), state->bufferRight.data(), numSamples);
        state->peak.store(stats.peak, std::memory_order_relaxed);
        state->rms.store(stats.rms, std::memory_order_relaxed);
        state->clipCount.store(stats.clipCount, std::memory_order_relaxed);
      }
    }

    // Find output node and copy to output
    const auto* outputNode = mGraph.FindNode("__output__");
    const double outputNodeGainDb = (outputNode && outputNode->params.count("gainDb"))
      ? outputNode->params.at("gainDb")
      : 0.0;
    const float outputGain = static_cast<float>(std::pow(10.0, (mOutputTrim + outputNodeGainDb) / 20.0));

    for (const auto &[id, state] : mNodeStates)
    {
      if ((state.type == kNodeTypeOutput || id == "__output__") && state.hasInput)
      {
        if (outputs[0])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            outputs[0][i] = state.bufferLeft[static_cast<size_t>(i)] * outputGain;
          }
        }
        if (outputs[1])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            outputs[1][i] = state.bufferRight[static_cast<size_t>(i)] * outputGain;
          }
        }
        break;
      }
    }

    auto totalEnd = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double, std::micro> totalDuration(totalEnd - totalStart);
    localStats.totalProcessingTimeUs = totalDuration.count();
    if (realTimeUs > 0.0)
    {
      localStats.dspLoadPercent = (localStats.totalProcessingTimeUs / realTimeUs) * 100.0;
    }

    std::unique_lock<std::mutex> lock(mPerformanceStatsMutex, std::try_to_lock);
    if (lock.owns_lock())
    {
      mLastPerformanceStats = std::move(localStats);
    }
  }

  SignalGraphExecutor::DSPPerformanceStats SignalGraphExecutor::GetPerformanceStats() const
  {
    std::lock_guard<std::mutex> lock(mPerformanceStatsMutex);
    return mLastPerformanceStats;
  }

  int SignalGraphExecutor::GetTotalLatencySamples() const
  {
    int total = 0;
    for (const auto& nodeId : mExecutionOrder)
    {
      auto it = mNodeStates.find(nodeId);
      if (it != mNodeStates.end() && it->second.processor)
        total += it->second.processor->GetLatencySamples();
    }
    return total;
  }

  std::vector<SignalGraphExecutor::NodeSignalLevel> SignalGraphExecutor::GetNodeSignalLevels() const
  {
    std::vector<NodeSignalLevel> result;
    result.reserve(mNodeStates.size());

    for (const auto &[id, state] : mNodeStates)
    {
      NodeSignalLevel entry;
      entry.nodeId = state.id;
      entry.nodeType = state.type;
      entry.peak = state.peak.load(std::memory_order_relaxed);
      entry.rms = state.rms.load(std::memory_order_relaxed);
      entry.clipCount = state.clipCount.load(std::memory_order_relaxed);
      result.push_back(std::move(entry));
    }

    return result;
  }

  void SignalGraphExecutor::SetNodeEnabled(const std::string &nodeId, bool enabled)
  {
    auto *state = FindNodeState(nodeId);
    if (state && state->processor)
    {
      state->processor->SetEnabled(enabled);
    }
  }

  void SignalGraphExecutor::SetNodeParam(const std::string &nodeId, const std::string &key, double value)
  {
    if (auto* node = mGraph.FindNode(nodeId))
    {
      node->params[key] = value;
    }

    auto *state = FindNodeState(nodeId);
    if (state && state->processor)
    {
      state->processor->SetParam(key, value);
    }
  }

  void SignalGraphExecutor::SetNodeConfig(const std::string &nodeId, const std::string &key, const std::string &value)
  {
    auto *state = FindNodeState(nodeId);
    if (state && state->processor)
    {
      state->processor->SetConfig(key, value);
    }
  }

  void SignalGraphExecutor::SetNodeConfigForType(const std::string &type, const std::string &key, const std::string &value)
  {
    for (auto &[id, state] : mNodeStates)
    {
      if (state.type == type && state.processor)
      {
        state.processor->SetConfig(key, value);
      }
    }
  }

  bool SignalGraphExecutor::LoadNodeResource(const std::string &nodeId, const ResourceRef &ref)
  {
    auto *state = FindNodeState(nodeId);
    if (!state || !state->processor)
    {
      return false;
    }

    if (mResourceLibrary)
    {
      auto path = mResourceLibrary->ResolveResource(ref);
      if (path)
      {
        return state->processor->LoadResource(*path);
      }
    }

    // Try direct file path
    if (ref.IsFilePath())
    {
      return state->processor->LoadResource(ref.filePath);
    }

    return false;
  }

  std::string SignalGraphExecutor::FindFirstNodeOfType(const std::string &type) const
  {
    for (const auto &node : mGraph.nodes)
    {
      if (node.type == type)
      {
        return node.id;
      }
    }
    return {};
  }

  std::string SignalGraphExecutor::FindFirstNodeOfTypes(const std::vector<std::string> &types) const
  {
    for (const auto &nodeId : mExecutionOrder)
    {
      auto it = mNodeStates.find(nodeId);
      if (it != mNodeStates.end())
      {
        const auto &nodeType = it->second.type;
        if (std::find(types.begin(), types.end(), nodeType) != types.end())
          return nodeId;
      }
    }
    return {};
  }

  SignalGraphExecutor::NodeState *SignalGraphExecutor::FindNodeState(const std::string &id)
  {
    auto it = mNodeStates.find(id);
    if (it != mNodeStates.end())
    {
      return &it->second;
    }
    return nullptr;
  }

} // namespace guitarfx
