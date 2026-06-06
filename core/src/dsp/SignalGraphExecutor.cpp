#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
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

    ResourceRef HydrateResolvedResourceRef(const ResourceRef &ref, const ResourceLibrary *resourceLibrary)
    {
      ResourceRef hydrated = ref;
      if (!resourceLibrary || !ref.IsLibraryRef())
      {
        return hydrated;
      }

      auto resource = resourceLibrary->LookupResource(ref.resourceType, ref.resourceId);
      if (!resource)
      {
        return hydrated;
      }

      hydrated.metadata = resource->metadata;
      if (!resource->hash.empty() && !hydrated.metadata.count("resourceHash"))
      {
        hydrated.metadata["resourceHash"] = resource->hash;
      }

      return hydrated;
    }

    std::optional<std::filesystem::path> ResolveResourcePath(const ResourceRef &ref, const ResourceLibrary *resourceLibrary)
    {
      if (resourceLibrary)
      {
        if (auto path = resourceLibrary->ResolveResource(ref))
        {
          return path;
        }
      }

      if (ref.IsFilePath())
      {
        return ref.filePath;
      }

      return std::nullopt;
    }

    bool BuffersAreEffectivelyMono(const float *left, const float *right, int numSamples)
    {
      if (!left || !right)
      {
        return true;
      }
      if (left == right)
      {
        return true;
      }

      // Tight epsilon keeps this focused on true mono/dual-mono blocks.
      constexpr float kEpsilon = 1.0e-8f;
      for (int i = 0; i < numSamples; ++i)
      {
        if (std::abs(left[i] - right[i]) > kEpsilon)
        {
          return false;
        }
      }
      return true;
    }

    bool NodeMayProduceStereo(const std::string &type, const std::string &category)
    {
      if (type == kNodeTypeInput || type == kNodeTypeOutput ||
          type == kNodeTypeSplitter || type == kNodeTypeMixer)
      {
        return false;
      }

      return category == "mod" || category == "delay" || category == "reverb";
    }

    int ScoreNodeTypeForParallelWork(const std::string &type)
    {
      if (type == EffectGuids::kAmpNam || type == EffectGuids::kAmpNamOptimized || type == EffectGuids::kAmpNamBlend || type == EffectGuids::kFxNam)
        return 14;
      if (type == EffectGuids::kCabIr || type == EffectGuids::kReverbIr)
        return 12;
      if (type == EffectGuids::kReverbAdvanced || type == EffectGuids::kReverbAmbient || type == EffectGuids::kReverbRoom || type == EffectGuids::kReverbSpring)
        return 6;
      if (type == EffectGuids::kDelayDigital || type == EffectGuids::kDelayDoubler || type == EffectGuids::kEqParametric)
        return 3;

      if (type == kNodeTypeInput || type == kNodeTypeOutput || type == kNodeTypeSplitter)
        return 0;
      if (type == kNodeTypeMixer)
        return 1;
      if (type == EffectGuids::kGain)
        return 1;

      return 2;
    }

    bool ShouldUseParallelLevel(int levelCount,
                                int levelScore,
                                int numSamples,
                                bool executorParallelEnabled,
                                bool workersAvailable)
    {
      if (!executorParallelEnabled || !workersAvailable)
        return false;
      if (levelCount < 2)
        return false;

      // Keep level parallelization for blocks/levels with enough expected CPU work.
      constexpr int kMinLevelParallelWorkUnits = 1800;
      const int totalWorkUnits = levelScore * numSamples;
      return totalWorkUnits >= kMinLevelParallelWorkUnits;
    }

  }

  SignalGraphExecutor::SignalGraphExecutor() = default;
  SignalGraphExecutor::~SignalGraphExecutor()
  {
    StopWorkers();
  }

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

    StopWorkers();

    mGraph = std::move(other.mGraph);
    mResourceLibrary = other.mResourceLibrary;
    mNodeStates = std::move(other.mNodeStates);
    mExecutionOrder = std::move(other.mExecutionOrder);
    mIncomingEdgeCount = std::move(other.mIncomingEdgeCount);
    mExecutionLevels = std::move(other.mExecutionLevels);
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
    mUseParallelLevels = other.mUseParallelLevels;

    return *this;
  }

  void SignalGraphExecutor::SetGraph(const SignalGraph &graph)
  {
    mGraph = graph;
    mIsValid = false;
    mPrepared = false;
    mNodeStates.clear();
    mExecutionOrder.clear();
    mExecutionLevelScores.clear();
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
    BuildExecutionLevels();
    CreateProcessors();

    if (mPrepared)
    {
      Prepare(mSampleRate, mMaxBlockSize);
    }
  }

  void SignalGraphExecutor::BuildExecutionOrder()
  {
    // Topological sort using Kahn's algorithm. A valid graph processes every
    // node once; fewer processed nodes means a cycle exists and audio is muted.
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

  void SignalGraphExecutor::BuildExecutionLevels()
  {
    mExecutionLevels.clear();
    mExecutionLevelScores.clear();
    if (!mIsValid)
      return;

    std::map<std::string, int> inDegree;
    std::map<std::string, std::vector<std::string>> adjacency;

    for (const auto &node : mGraph.nodes)
    {
      inDegree[node.id] = 0;
      adjacency[node.id] = {};
    }

    for (const auto &edge : mGraph.edges)
    {
      adjacency[edge.from].push_back(edge.to);
      inDegree[edge.to]++;
    }

    std::vector<std::string> frontier;
    frontier.reserve(mGraph.nodes.size());
    for (const auto &[id, degree] : inDegree)
    {
      if (degree == 0)
        frontier.push_back(id);
    }

    std::size_t processed = 0;
    while (!frontier.empty())
    {
      mExecutionLevels.push_back(frontier);
      int levelScore = 0;
      for (const auto &nodeId : frontier)
      {
        const auto *node = mGraph.FindNode(nodeId);
        if (node)
          levelScore += ScoreNodeTypeForParallelWork(node->type);
      }
      mExecutionLevelScores.push_back(levelScore);
      processed += frontier.size();

      std::vector<std::string> next;
      for (const auto &id : frontier)
      {
        for (const auto &neighbor : adjacency[id])
        {
          auto it = inDegree.find(neighbor);
          if (it == inDegree.end())
            continue;
          it->second -= 1;
          if (it->second == 0)
            next.push_back(neighbor);
        }
      }

      frontier = std::move(next);
    }

    if (processed != mGraph.nodes.size())
    {
      mExecutionLevels.clear();
      mExecutionLevelScores.clear();
    }
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
      state.category = node.category;

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
        if (!node.resources.empty())
        {
          std::vector<ResourceRef> resolvedRefs;
          std::vector<std::filesystem::path> resolvedPaths;
          resolvedRefs.reserve(node.resources.size());
          resolvedPaths.reserve(node.resources.size());

          for (std::size_t resourceIndex = 0; resourceIndex < node.resources.size(); ++resourceIndex)
          {
            const auto& res = node.resources[resourceIndex];
            if (!res.IsValid())
              continue;

            ResourceRef hydratedRef = HydrateResolvedResourceRef(res, mResourceLibrary);
            hydratedRef.metadata["resourceSlotIndex"] = std::to_string(resourceIndex);
            auto path = ResolveResourcePath(hydratedRef, mResourceLibrary);
            if (path)
            {
              resolvedRefs.push_back(hydratedRef);
              resolvedPaths.push_back(*path);
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

  const SignalGraphExecutor::NodeState *SignalGraphExecutor::FindNodeState(const std::string &id) const
  {
    auto it = mNodeStates.find(id);
    return it != mNodeStates.end() ? &it->second : nullptr;
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

    std::size_t maxLevelWidth = 0;
    int maxLevelScore = 0;
    for (std::size_t i = 0; i < mExecutionLevels.size(); ++i)
    {
      maxLevelWidth = std::max(maxLevelWidth, mExecutionLevels[i].size());
      if (i < mExecutionLevelScores.size())
        maxLevelScore = std::max(maxLevelScore, mExecutionLevelScores[i]);
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const int workerCount = static_cast<int>(hw > 1 ? hw - 1 : 0);
    constexpr int kMinLevelParallelWorkUnits = 1800;
    const bool graphHasMeaningfulParallelLevel = (maxLevelScore * maxBlockSize) >= kMinLevelParallelWorkUnits;
    mUseParallelLevels = maxLevelWidth > 1 && workerCount > 0 && graphHasMeaningfulParallelLevel;
    if (mUseParallelLevels)
      StartWorkers(workerCount);
    else
      StopWorkers();
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
      state.hasStereoSignal = false;
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
        const bool inputEnabled = !state.processor || state.processor->IsEnabled();
        if (inputEnabled && inputs[0])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            state.bufferLeft[static_cast<size_t>(i)] = inputs[0][i] * inputGain;
          }
        }
        if (inputEnabled && inputs[1])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            state.bufferRight[static_cast<size_t>(i)] = inputs[1][i] * inputGain;
          }
        }
        state.hasInput = true;
        const bool leftLive = (inputs[0] != nullptr);
        const bool rightLive = (inputs[1] != nullptr);
        state.hasStereoSignal = leftLive && rightLive && !BuffersAreEffectivelyMono(inputs[0], inputs[1], numSamples);
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

    std::mutex statsMutex;
    for (std::size_t levelIndex = 0; levelIndex < mExecutionLevels.size(); ++levelIndex)
    {
      const auto &level = mExecutionLevels[levelIndex];
      const int levelCount = static_cast<int>(level.size());
      const int levelScore = (levelIndex < mExecutionLevelScores.size()) ? mExecutionLevelScores[levelIndex] : 0;
      const bool useParallelLevel = ShouldUseParallelLevel(
        levelCount,
        levelScore,
        numSamples,
        mUseParallelLevels,
        !mWorkerThreads.empty());

      if (useParallelLevel)
      {
        const int wi = std::min(levelCount, kMaxParallelWorkItems);
        for (int i = 0; i < wi; ++i)
        {
          auto &item = mWorkItems[static_cast<size_t>(i)];
          item.nodeId = &level[static_cast<size_t>(i)];
          item.numSamples = numSamples;
          item.stats = &localStats;
          item.statsMutex = &statsMutex;
          item.diagnosticsEnabled = diagnosticsEnabled;
        }

        {
          std::lock_guard<std::mutex> lock(mParallelMutex);
          mParallelTaskHead.store(0, std::memory_order_relaxed);
          mParallelDoneCount.store(0, std::memory_order_relaxed);
          mParallelTaskCount.store(wi, std::memory_order_relaxed);
          mParallelGeneration.fetch_add(1, std::memory_order_relaxed);
        }

        const int workersNeeded = std::min<int>(std::max(0, wi - 1), static_cast<int>(mWorkerThreads.size()));
        for (int n = 0; n < workersNeeded; ++n)
          mParallelCv.notify_one();

        while (true)
        {
          const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);
          if (idx >= wi)
            break;
          ProcessNodeById(*mWorkItems[static_cast<size_t>(idx)].nodeId,
                          numSamples,
                          localStats,
                          &statsMutex,
                          diagnosticsEnabled);
          mParallelDoneCount.fetch_add(1, std::memory_order_release);
        }

        while (mParallelDoneCount.load(std::memory_order_acquire) < wi)
          std::this_thread::yield();

        for (int i = wi; i < levelCount; ++i)
          ProcessNodeById(level[static_cast<size_t>(i)], numSamples, localStats, nullptr, diagnosticsEnabled);
      }
      else
      {
        for (const auto &nodeId : level)
          ProcessNodeById(nodeId, numSamples, localStats, nullptr, diagnosticsEnabled);
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
        const bool outputEnabled = !state.processor || state.processor->IsEnabled();
        if (outputs[0])
        {
          for (int i = 0; i < numSamples; ++i)
          {
            outputs[0][i] = outputEnabled ? (state.bufferLeft[static_cast<size_t>(i)] * outputGain) : 0.0f;
          }
        }
        if (outputs[1])
        {
          if (outputEnabled && !state.hasStereoSignal && outputs[0])
          {
            for (int i = 0; i < numSamples; ++i)
            {
              outputs[1][i] = outputs[0][i];
            }
          }
          else
          {
            for (int i = 0; i < numSamples; ++i)
            {
              outputs[1][i] = outputEnabled ? (state.bufferRight[static_cast<size_t>(i)] * outputGain) : 0.0f;
            }
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

  void SignalGraphExecutor::ProcessNodeById(const std::string &nodeId,
                                            int numSamples,
                                            DSPPerformanceStats &stats,
                                            std::mutex *statsMutex,
                                            bool diagnosticsEnabled)
  {
    thread_local std::vector<float> tempLeft;
    thread_local std::vector<float> tempRight;
    if (static_cast<int>(tempLeft.size()) < numSamples)
    {
      tempLeft.resize(static_cast<size_t>(numSamples), 0.0f);
      tempRight.resize(static_cast<size_t>(numSamples), 0.0f);
    }

    auto *state = FindNodeState(nodeId);
    if (!state)
      return;

    const std::string &nodeType = state->type;
    if (nodeType == kNodeTypeInput)
      return;

    const auto inEdgesIt = mIncomingEdgesByNode.find(nodeId);
    const int incomingCount = (inEdgesIt != mIncomingEdgesByNode.end())
      ? static_cast<int>(inEdgesIt->second.size()) : 0;
    const bool isMixer = (nodeType == kNodeTypeMixer);
    const bool shouldAccumulate = isMixer || (incomingCount > 1);
    bool incomingStereoSignal = false;

    MixerEffect *mixerEffect = nullptr;
    if (isMixer && state->processor)
      mixerEffect = dynamic_cast<MixerEffect *>(state->processor.get());

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
          incomingStereoSignal = incomingStereoSignal || sourceState->hasStereoSignal;

          if (isMixer && mixerEffect)
          {
            if (mixerEffect->IsInputMuted(inputPort))
            {
              state->hasInput = true;
              continue;
            }

            const float level = mixerEffect->GetInputLevel(inputPort);
            const float panL = mixerEffect->GetInputPanL(inputPort);
            const float panR = mixerEffect->GetInputPanR(inputPort);
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
            for (int i = 0; i < numSamples; ++i)
            {
              state->bufferLeft[static_cast<size_t>(i)] += sourceState->bufferLeft[static_cast<size_t>(i)] * edgeGain;
              state->bufferRight[static_cast<size_t>(i)] += sourceState->bufferRight[static_cast<size_t>(i)] * edgeGain;
            }
          }
          else
          {
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

    if (state->hasInput)
      state->hasStereoSignal = incomingStereoSignal;

    if (state->processor && state->hasInput)
    {
      const bool nodeCanMono = !incomingStereoSignal && state->processor->SupportsMonoProcessing() && !NodeMayProduceStereo(state->type, state->category);

      if (nodeType == kNodeTypeSplitter || nodeType == kNodeTypeOutput)
      {
        if (nodeType == kNodeTypeOutput && !state->processor->IsEnabled())
        {
          std::fill(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples, 0.0f);
          std::fill(state->bufferRight.begin(), state->bufferRight.begin() + numSamples, 0.0f);
          state->hasStereoSignal = false;
        }
      }
      else if (nodeType == kNodeTypeMixer)
      {
        if (state->processor->IsEnabled())
        {
          float *inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
          float *outPtrs[2] = {tempLeft.data(), tempRight.data()};
          auto nodeStart = std::chrono::high_resolution_clock::now();
          state->processor->Process(inPtrs, outPtrs, numSamples);
          auto nodeEnd = std::chrono::high_resolution_clock::now();
          const std::chrono::duration<double, std::micro> nodeDuration(nodeEnd - nodeStart);
          if (statsMutex)
          {
            std::lock_guard<std::mutex> lock(*statsMutex);
            stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
            stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
          }
          else
          {
            stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
            stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
          }
          std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
          std::copy(tempRight.begin(), tempRight.begin() + numSamples, state->bufferRight.begin());
          if (!incomingStereoSignal && !NodeMayProduceStereo(state->type, state->category))
          {
            std::copy(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples, state->bufferRight.begin());
            state->hasStereoSignal = false;
          }
          else
          {
            state->hasStereoSignal = incomingStereoSignal || NodeMayProduceStereo(state->type, state->category);
          }
        }
      }
      else if (state->processor->IsEnabled() && nodeCanMono)
      {
        auto nodeStart = std::chrono::high_resolution_clock::now();
        state->processor->ProcessMono(state->bufferLeft.data(), tempLeft.data(), numSamples);
        auto nodeEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::micro> nodeDuration(nodeEnd - nodeStart);
        if (statsMutex)
        {
          std::lock_guard<std::mutex> lock(*statsMutex);
          stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
          stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
        }
        else
        {
          stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
          stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
        }
        std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
        std::copy(state->bufferLeft.begin(), state->bufferLeft.begin() + numSamples, state->bufferRight.begin());
        state->hasStereoSignal = false;
      }
      else if (state->processor->IsEnabled())
      {
        float *inPtrs[2] = {state->bufferLeft.data(), state->bufferRight.data()};
        float *outPtrs[2] = {tempLeft.data(), tempRight.data()};
        auto nodeStart = std::chrono::high_resolution_clock::now();
        state->processor->Process(inPtrs, outPtrs, numSamples);
        auto nodeEnd = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::micro> nodeDuration(nodeEnd - nodeStart);
        if (statsMutex)
        {
          std::lock_guard<std::mutex> lock(*statsMutex);
          stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
          stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
        }
        else
        {
          stats.nodeProcessingTimesUs[nodeId] = nodeDuration.count();
          stats.scopedNodeProcessingTimesUs[nodeId] = nodeDuration.count();
        }
        std::copy(tempLeft.begin(), tempLeft.begin() + numSamples, state->bufferLeft.begin());
        std::copy(tempRight.begin(), tempRight.begin() + numSamples, state->bufferRight.begin());
        state->hasStereoSignal = incomingStereoSignal || NodeMayProduceStereo(state->type, state->category);
      }
      else
      {
        state->hasStereoSignal = incomingStereoSignal;
      }
    }

    if (diagnosticsEnabled && state->hasInput)
    {
      const auto levelStats = ComputeLevelStats(state->bufferLeft.data(), state->bufferRight.data(), numSamples);
      state->peak.store(levelStats.peak, std::memory_order_relaxed);
      state->rms.store(levelStats.rms, std::memory_order_relaxed);
      state->clipCount.store(levelStats.clipCount, std::memory_order_relaxed);
    }
  }

  void SignalGraphExecutor::StartWorkers(int count)
  {
    StopWorkers();

    {
      std::lock_guard<std::mutex> lock(mParallelMutex);
      mParallelQuit.store(false, std::memory_order_relaxed);
      mParallelGeneration.store(0, std::memory_order_relaxed);
    }

    const int numWorkers = std::min(count, kMaxParallelWorkers);
    mWorkerThreads.reserve(static_cast<size_t>(numWorkers));
    for (int i = 0; i < numWorkers; ++i)
      mWorkerThreads.emplace_back([this]() { WorkerLoop(); });
  }

  void SignalGraphExecutor::StopWorkers()
  {
    if (mWorkerThreads.empty())
      return;

    {
      std::lock_guard<std::mutex> lock(mParallelMutex);
      mParallelQuit.store(true, std::memory_order_relaxed);
    }
    mParallelCv.notify_all();
    for (auto &thread : mWorkerThreads)
    {
      if (thread.joinable())
        thread.join();
    }
    mWorkerThreads.clear();
  }

  void SignalGraphExecutor::WorkerLoop()
  {
    uint32_t lastGeneration = 0;
    while (true)
    {
      {
        std::unique_lock<std::mutex> lock(mParallelMutex);
        mParallelCv.wait(lock, [&]()
        {
          return mParallelQuit.load(std::memory_order_relaxed)
              || mParallelGeneration.load(std::memory_order_relaxed) != lastGeneration;
        });
      }

      if (mParallelQuit.load(std::memory_order_acquire))
        break;

      lastGeneration = mParallelGeneration.load(std::memory_order_acquire);
      const int total = mParallelTaskCount.load(std::memory_order_acquire);
      while (true)
      {
        const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= total)
          break;

        const auto &item = mWorkItems[static_cast<size_t>(idx)];
        if (item.nodeId && item.stats && item.statsMutex)
        {
          ProcessNodeById(*item.nodeId, item.numSamples, *item.stats, item.statsMutex, item.diagnosticsEnabled);
        }
        mParallelDoneCount.fetch_add(1, std::memory_order_release);
      }
    }
  }

  SignalGraphExecutor::DSPPerformanceStats SignalGraphExecutor::GetPerformanceStats() const
  {
    std::lock_guard<std::mutex> lock(mPerformanceStatsMutex);
    auto stats = mLastPerformanceStats;
    for (const auto& [nodeId, state] : mNodeStates)
    {
      const int latencySamples = (state.processor && state.processor->IsEnabled())
        ? state.processor->GetLatencySamples()
        : 0;
      stats.nodeLatencySamples[nodeId] = latencySamples;
      stats.scopedNodeLatencySamples[nodeId] = latencySamples;
    }
    return stats;
  }

  int SignalGraphExecutor::GetTotalLatencySamples() const
  {
    std::map<std::string, int> cumulativeLatencyByNode;
    for (const auto& nodeId : mExecutionOrder)
    {
      int maxIncomingLatency = 0;
      if (auto incomingIt = mIncomingEdgesByNode.find(nodeId); incomingIt != mIncomingEdgesByNode.end())
      {
        for (const auto edgeIndex : incomingIt->second)
        {
          if (edgeIndex >= mGraph.edges.size())
            continue;
          const auto& edge = mGraph.edges[edgeIndex];
          auto sourceLatencyIt = cumulativeLatencyByNode.find(edge.from);
          if (sourceLatencyIt != cumulativeLatencyByNode.end())
            maxIncomingLatency = std::max(maxIncomingLatency, sourceLatencyIt->second);
        }
      }

      int ownLatency = 0;
      auto it = mNodeStates.find(nodeId);
      if (it != mNodeStates.end() && it->second.processor && it->second.processor->IsEnabled())
        ownLatency = it->second.processor->GetLatencySamples();

      cumulativeLatencyByNode[nodeId] = maxIncomingLatency + ownLatency;
    }

    if (auto outputIt = cumulativeLatencyByNode.find("__output__"); outputIt != cumulativeLatencyByNode.end())
      return outputIt->second;

    int maxLatency = 0;
    for (const auto& [_, latency] : cumulativeLatencyByNode)
      maxLatency = std::max(maxLatency, latency);
    return maxLatency;
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
      entry.stereoActive = state.hasStereoSignal;
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

  void SignalGraphExecutor::SetTempo(double bpm)
  {
    auto &registry = EffectRegistry::Instance();
    for (auto &[id, state] : mNodeStates)
    {
      if (!state.processor)
        continue;
      const auto resolvedType = registry.Resolve(state.type);
      const auto typeInfo = registry.GetTypeInfo(resolvedType);
      if (typeInfo && typeInfo->requiresTempo)
      {
        state.processor->SetParam("bpm", bpm);
      }
    }
  }

  void SignalGraphExecutor::SetNodeConfig(const std::string &nodeId, const std::string &key, const std::string &value)
  {
    const bool transientCommand = key == "showPluginEditor" || key == "openPluginEditor";
    if (!transientCommand)
    {
      if (auto* node = mGraph.FindNode(nodeId))
      {
        node->config[key] = value;
      }
    }

    auto *state = FindNodeState(nodeId);
    if (state && state->processor)
    {
      state->processor->SetConfig(key, value);
    }
  }

  std::string SignalGraphExecutor::GetNodeConfig(const std::string &nodeId, const std::string &key) const
  {
    const auto *state = FindNodeState(nodeId);
    if (state && state->processor)
      return state->processor->GetConfig(key);

    if (const auto* node = mGraph.FindNode(nodeId))
    {
      const auto it = node->config.find(key);
      if (it != node->config.end())
        return it->second;
    }

    return {};
  }

  EffectProcessor *SignalGraphExecutor::GetNodeProcessor(const std::string &nodeId)
  {
    auto *state = FindNodeState(nodeId);
    return state ? state->processor.get() : nullptr;
  }

  const EffectProcessor *SignalGraphExecutor::GetNodeProcessor(const std::string &nodeId) const
  {
    const auto *state = FindNodeState(nodeId);
    return state ? state->processor.get() : nullptr;
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

    const ResourceRef hydratedRef = HydrateResolvedResourceRef(ref, mResourceLibrary);
    if (auto path = ResolveResourcePath(hydratedRef, mResourceLibrary))
    {
      return state->processor->LoadResources({hydratedRef}, {*path});
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
