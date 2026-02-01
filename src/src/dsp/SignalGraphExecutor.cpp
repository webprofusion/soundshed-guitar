#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
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

    // Track incoming edge counts (used for multi-input summing)
    for (const auto &node : mGraph.nodes)
    {
      mIncomingEdgeCount[node.id] = 0;
    }
    for (const auto &edge : mGraph.edges)
    {
      mIncomingEdgeCount[edge.to] += 1;
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
      else if (node.type == kNodeTypeSplitter || node.type == kNodeTypeMixer)
      {
        // Splitter/mixer handled specially
        state.processor = std::make_unique<PassthroughProcessor>();
      }
      else
      {
        // Create from registry
        state.processor = registry.Create(node.type);
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
        /*else if (node.resource && node.resource->IsValid())
        {
          std::optional<std::filesystem::path> path;
          if (mResourceLibrary)
          {
            path = mResourceLibrary->ResolveResource(*node.resource);
          }
          if (path)
          {
            state.processor->LoadResource(*path);
          }
          else if (node.resource->IsFilePath())
          {
            state.processor->LoadResource(node.resource->filePath);
          }
        }*/
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

    mLastPerformanceStats.realTimeUs = realTimeUs;
    mLastPerformanceStats.nodeProcessingTimesUs.clear();

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

    // Apply input trim
    const float inputGain = static_cast<float>(std::pow(10.0, mInputTrim / 20.0));

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

      const auto *node = mGraph.FindNode(nodeId);
      if (!node)
        continue;

      // Skip input node (already handled)
      if (node->type == kNodeTypeInput || node->id == "__input__")
        continue;

      // Gather inputs from incoming edges
      const int incomingCount = mIncomingEdgeCount.count(nodeId) ? mIncomingEdgeCount[nodeId] : 0;
      const bool shouldAccumulate = (node->type == kNodeTypeMixer) || (incomingCount > 1);
      for (const auto &edge : mGraph.edges)
      {
        if (edge.to == nodeId)
        {
          auto *sourceState = FindNodeState(edge.from);
          if (sourceState && sourceState->hasInput)
          {
            const float gain = static_cast<float>(edge.gain);

            // Handle mixer or any multi-input node: accumulate inputs
            if (shouldAccumulate)
            {
              for (int i = 0; i < numSamples; ++i)
              {
                state->bufferLeft[static_cast<size_t>(i)] += sourceState->bufferLeft[static_cast<size_t>(i)] * gain;
                state->bufferRight[static_cast<size_t>(i)] += sourceState->bufferRight[static_cast<size_t>(i)] * gain;
              }
            }
            else
            {
              // Normal node: copy input (last edge wins for non-mixers)
              for (int i = 0; i < numSamples; ++i)
              {
                state->bufferLeft[static_cast<size_t>(i)] = sourceState->bufferLeft[static_cast<size_t>(i)] * gain;
                state->bufferRight[static_cast<size_t>(i)] = sourceState->bufferRight[static_cast<size_t>(i)] * gain;
              }
            }
            state->hasInput = true;
          }
        }
      }

      // Process the node
      if (state->processor && state->hasInput)
      {
        if (node->type == kNodeTypeSplitter || node->type == kNodeTypeMixer ||
            node->type == kNodeTypeOutput || node->id == "__output__")
        {
          // These nodes just pass through (routing handled above)
        }
        else if (state->processor->IsEnabled())
        {
          // Process effect
          float *inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
          float *outPtrs[2] = {mTempLeftBuffer.data(), mTempRightBuffer.data()};

          auto nodeStart = std::chrono::high_resolution_clock::now();
          state->processor->Process(inPtrs, outPtrs, numSamples);
          auto nodeEnd = std::chrono::high_resolution_clock::now();
          auto nodeDuration = std::chrono::duration_cast<std::chrono::microseconds>(nodeEnd - nodeStart);
          mLastPerformanceStats.nodeProcessingTimesUs[nodeId] = static_cast<double>(nodeDuration.count());

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
    const float outputGain = static_cast<float>(std::pow(10.0, mOutputTrim / 20.0));

    for (const auto &[id, state] : mNodeStates)
    {
      const auto *node = mGraph.FindNode(id);
      if (node && (node->type == kNodeTypeOutput || node->id == "__output__") && state.hasInput)
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
    auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(totalEnd - totalStart);
    mLastPerformanceStats.totalProcessingTimeUs = static_cast<double>(totalDuration.count());
    mLastPerformanceStats.dspLoadPercent = (mLastPerformanceStats.totalProcessingTimeUs / realTimeUs) * 100.0;
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
