#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <set>

namespace guitarfx
{
  SignalGraphExecutor::SignalGraphExecutor() = default;
  SignalGraphExecutor::~SignalGraphExecutor() = default;

  void SignalGraphExecutor::SetGraph(const SignalGraph &graph)
  {
    mGraph = graph;
    mIsValid = false;
    mPrepared = false;
    mNodeStates.clear();
    mExecutionOrder.clear();

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
      NodeState state;
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

        // Load resource if needed
        if (node.resource && node.resource->IsValid() && mResourceLibrary)
        {
          auto path = mResourceLibrary->ResolveResource(*node.resource);
          if (path)
          {
            state.processor->LoadResource(*path);
          }
        }
      }

      mNodeStates[node.id] = std::move(state);
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
    if (!mIsValid || !mPrepared || !inputs || !outputs)
    {
      return;
    }

    // Clear all buffers and reset input flags
    for (auto &[id, state] : mNodeStates)
    {
      std::fill(state.bufferLeft.begin(), state.bufferLeft.begin() + numSamples, 0.0f);
      std::fill(state.bufferRight.begin(), state.bufferRight.begin() + numSamples, 0.0f);
      state.hasInput = false;
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
      for (const auto &edge : mGraph.edges)
      {
        if (edge.to == nodeId)
        {
          auto *sourceState = FindNodeState(edge.from);
          if (sourceState && sourceState->hasInput)
          {
            const float gain = static_cast<float>(edge.gain);

            // Handle mixer: accumulate inputs
            if (node->type == kNodeTypeMixer)
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

          state->processor->Process(inPtrs, outPtrs, numSamples);

          // Copy back
          std::copy(mTempLeftBuffer.begin(), mTempLeftBuffer.begin() + numSamples, state->bufferLeft.begin());
          std::copy(mTempRightBuffer.begin(), mTempRightBuffer.begin() + numSamples, state->bufferRight.begin());
        }
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
