#include "presets/PresetTypes.h"
#include "dsp/EffectRegistry.h"

#include <algorithm>
#include <tuple>

namespace guitarfx
{
namespace
{
  std::string BuildDefaultSceneId(std::size_t index)
  {
    return "scene-" + std::to_string(index + 1);
  }

  bool HasSignalGraphContent(const SignalGraph& graph)
  {
    return !graph.nodes.empty() || !graph.edges.empty();
  }

  void ApplyDefaultParamsFromRegistry(GraphNode& node)
  {
    auto info = EffectRegistry::Instance().GetTypeInfo(node.type);
    if (!info.has_value())
    {
      return;
    }

    for (const auto& param : info->parameters)
    {
      if (node.params.find(param.id) == node.params.end())
      {
        node.params[param.id] = param.defaultValue;
      }
    }
  }

  GraphNode* EnsureBoundaryNode(SignalGraph& graph,
                                const std::string& nodeId,
                                const std::string& nodeType)
  {
    if (auto* existing = graph.FindNode(nodeId))
    {
      existing->type = nodeType;
      existing->enabled = true;
      return existing;
    }

    for (auto& node : graph.nodes)
    {
      if (node.type == nodeType)
      {
        const std::string oldId = node.id;
        node.id = nodeId;
        node.enabled = true;

        if (oldId != nodeId)
        {
          for (auto& edge : graph.edges)
          {
            if (edge.from == oldId)
            {
              edge.from = nodeId;
            }
            if (edge.to == oldId)
            {
              edge.to = nodeId;
            }
          }
        }

        return &node;
      }
    }

    GraphNode node;
    node.id = nodeId;
    node.type = nodeType;
    node.enabled = true;
    graph.nodes.push_back(node);
    return &graph.nodes.back();
  }

}

void EnsurePresetBoundaryGainNodes(SignalGraph& graph)
{
  // Call both EnsureBoundaryNode before using any resulting pointer:
  // each call may push_back to graph.nodes, which would reallocate the
  // vector and invalidate a pointer returned by the first call.
  EnsureBoundaryNode(graph, "__input__",  kNodeTypeInput);
  EnsureBoundaryNode(graph, "__output__", kNodeTypeOutput);

  for (const char* id : {"__input__", "__output__"})
  {
    if (auto* node = graph.FindNode(id))
    {
      node->enabled = true;
      if (node->params.find("gainDb") == node->params.end())
        node->params["gainDb"] = 0.0;
    }
  }
}

void NormalizePresetScenes(Preset& preset)
{
  const bool hasLegacyGraph = HasSignalGraphContent(preset.graph);

  if (preset.scenes.empty())
  {
    PresetScene scene;
    scene.id = "scene-1";
    scene.title = "Scene 1";
    scene.graph = preset.graph;
    EnsurePresetBoundaryGainNodes(scene.graph);
    preset.scenes.push_back(std::move(scene));
  }
  else if (preset.scenes.size() == 1 && hasLegacyGraph && !HasSignalGraphContent(preset.scenes.front().graph))
  {
    preset.scenes.front().graph = preset.graph;
  }

  for (std::size_t index = 0; index < preset.scenes.size(); ++index)
  {
    auto& scene = preset.scenes[index];
    if (scene.id.empty())
      scene.id = BuildDefaultSceneId(index);
    if (scene.title.empty())
      scene.title = "Scene " + std::to_string(index + 1);
    EnsurePresetBoundaryGainNodes(scene.graph);
  }

  if (preset.graph.nodes.empty() && preset.graph.edges.empty() && !preset.scenes.empty())
    preset.graph = preset.scenes.front().graph;

  EnsurePresetBoundaryGainNodes(preset.graph);
}

PresetScene* FindPresetScene(Preset& preset, const std::string& sceneId)
{
  for (auto& scene : preset.scenes)
  {
    if (scene.id == sceneId)
      return &scene;
  }
  return nullptr;
}

const PresetScene* FindPresetScene(const Preset& preset, const std::string& sceneId)
{
  for (const auto& scene : preset.scenes)
  {
    if (scene.id == sceneId)
      return &scene;
  }
  return nullptr;
}

std::string GetDefaultPresetSceneId(const Preset& preset)
{
  if (!preset.scenes.empty())
    return preset.scenes.front().id;
  return "scene-1";
}

bool SetPresetActiveScene(Preset& preset, const std::string& sceneId, std::string* resolvedSceneId)
{
  NormalizePresetScenes(preset);

  auto* scene = FindPresetScene(preset, sceneId);
  if (!scene && !preset.scenes.empty())
    scene = &preset.scenes.front();
  if (!scene)
    return false;

  preset.graph = scene->graph;
  EnsurePresetBoundaryGainNodes(preset.graph);

  if (resolvedSceneId)
    *resolvedSceneId = scene->id;
  return true;
}

void SyncPresetSceneFromGraph(Preset& preset, const std::string& sceneId)
{
  NormalizePresetScenes(preset);

  auto* scene = FindPresetScene(preset, sceneId);
  if (!scene && !preset.scenes.empty())
    scene = &preset.scenes.front();
  if (!scene)
    return;

  EnsurePresetBoundaryGainNodes(preset.graph);
  scene->graph = preset.graph;
  EnsurePresetBoundaryGainNodes(scene->graph);
}

SignalGraph GlobalSignalChainConfig::BuildPreChainGraph() const
{
  return preChainGraph;
}

SignalGraph GlobalSignalChainConfig::BuildPostChainGraph() const
{
  return postChainGraph;
}

SignalGraph GlobalSignalChainConfig::BuildDefaultPreChainGraph()
{
  SignalGraph graph;

  // Input node
  GraphNode inputNode;
  inputNode.id = "__input__";
  inputNode.type = kNodeTypeInput;
  inputNode.enabled = true;
  graph.nodes.push_back(inputNode);

  // Noise Gate
  GraphNode gateNode;
  gateNode.id = "global_gate";
  gateNode.type = EffectGuids::kDynamicsGate;
  gateNode.category = "dynamics";
  gateNode.label = "Noise Gate";
  gateNode.enabled = false;
  ApplyDefaultParamsFromRegistry(gateNode);
  graph.nodes.push_back(gateNode);

  // Transpose (Resampled)
  GraphNode transposeNode;
  transposeNode.id = "global_transpose";
  transposeNode.type = EffectGuids::kTranspose;
  transposeNode.category = "modulation";
  transposeNode.label = "Transpose";
  transposeNode.enabled = false;
  ApplyDefaultParamsFromRegistry(transposeNode);
  graph.nodes.push_back(transposeNode);

  // Output node
  GraphNode outputNode;
  outputNode.id = "__output__";
  outputNode.type = kNodeTypeOutput;
  outputNode.enabled = true;
  graph.nodes.push_back(outputNode);

  // Edges: input → gate → transpose → output
  graph.edges.push_back({"__input__", "global_gate"});
  graph.edges.push_back({"global_gate", "global_transpose"});
  graph.edges.push_back({"global_transpose", "__output__"});

  return graph;
}

SignalGraph GlobalSignalChainConfig::BuildDefaultPostChainGraph()
{
  SignalGraph graph;

  // Input node
  GraphNode inputNode;
  inputNode.id = "__input__";
  inputNode.type = kNodeTypeInput;
  inputNode.enabled = true;
  graph.nodes.push_back(inputNode);

  // Parametric EQ
  GraphNode eqNode;
  eqNode.id = "global_eq";
  eqNode.type = EffectGuids::kEqParametric;
  eqNode.category = "eq";
  eqNode.label = "Global EQ";
  eqNode.enabled = false;
  ApplyDefaultParamsFromRegistry(eqNode);
  graph.nodes.push_back(eqNode);

  // Doubler
  GraphNode doublerNode;
  doublerNode.id = "global_doubler";
  doublerNode.type = EffectGuids::kDelayDoubler;
  doublerNode.category = "modulation";
  doublerNode.label = "Doubler";
  doublerNode.enabled = false;
  ApplyDefaultParamsFromRegistry(doublerNode);
  graph.nodes.push_back(doublerNode);

  // Output node
  GraphNode outputNode;
  outputNode.id = "__output__";
  outputNode.type = kNodeTypeOutput;
  outputNode.enabled = true;
  graph.nodes.push_back(outputNode);

  // Edges: input → eq → doubler → output
  graph.edges.push_back({"__input__", "global_eq"});
  graph.edges.push_back({"global_eq", "global_doubler"});
  graph.edges.push_back({"global_doubler", "__output__"});

  return graph;
}

GlobalSignalChainConfig GlobalSignalChainConfig::CreateDefault()
{
  GlobalSignalChainConfig config;
  config.preChainGraph = BuildDefaultPreChainGraph();
  config.postChainGraph = BuildDefaultPostChainGraph();
  return config;
}

} // namespace guitarfx
