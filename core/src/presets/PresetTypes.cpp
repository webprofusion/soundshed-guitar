#include "presets/PresetTypes.h"
#include "dsp/EffectRegistry.h"

namespace guitarfx
{
namespace
{
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
  gateNode.type = "dynamics_gate";
  gateNode.category = "dynamics";
  gateNode.label = "Noise Gate";
  gateNode.enabled = false;
  ApplyDefaultParamsFromRegistry(gateNode);
  graph.nodes.push_back(gateNode);

  // Transpose (Resampled)
  GraphNode transposeNode;
  transposeNode.id = "global_transpose";
  transposeNode.type = "transpose";
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
  eqNode.type = "eq_parametric";
  eqNode.category = "eq";
  eqNode.label = "Global EQ";
  eqNode.enabled = false;
  ApplyDefaultParamsFromRegistry(eqNode);
  graph.nodes.push_back(eqNode);

  // Doubler
  GraphNode doublerNode;
  doublerNode.id = "global_doubler";
  doublerNode.type = "delay_doubler";
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
