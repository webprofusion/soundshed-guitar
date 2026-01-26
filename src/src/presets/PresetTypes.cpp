#include "presets/PresetTypes.h"

namespace guitarfx
{

SignalGraph GlobalSignalChainConfig::BuildPreChainGraph() const
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
  gateNode.enabled = preChain.gateEnabled;
  gateNode.params["threshold"] = preChain.gateThreshold;
  gateNode.params["attack"] = preChain.gateAttack;
  gateNode.params["hold"] = preChain.gateHold;
  gateNode.params["release"] = preChain.gateRelease;
  graph.nodes.push_back(gateNode);

  // Transpose (Resampled)
  GraphNode transposeNode;
  transposeNode.id = "global_transpose";
  transposeNode.type = "transpose";
  transposeNode.category = "modulation";
  transposeNode.label = "Transpose";
  transposeNode.enabled = preChain.transposeEnabled;
  transposeNode.params["semitones"] = static_cast<double>(preChain.transposeSemitones);
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

SignalGraph GlobalSignalChainConfig::BuildPostChainGraph() const
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
  eqNode.enabled = postChain.eqEnabled;
  eqNode.params["lowGain"] = postChain.eqLowGain;
  eqNode.params["lowFreq"] = postChain.eqLowFreq;
  eqNode.params["lowMidGain"] = postChain.eqLowMidGain;
  eqNode.params["lowMidFreq"] = postChain.eqLowMidFreq;
  eqNode.params["lowMidQ"] = postChain.eqLowMidQ;
  eqNode.params["highMidGain"] = postChain.eqHighMidGain;
  eqNode.params["highMidFreq"] = postChain.eqHighMidFreq;
  eqNode.params["highMidQ"] = postChain.eqHighMidQ;
  eqNode.params["highGain"] = postChain.eqHighGain;
  eqNode.params["highFreq"] = postChain.eqHighFreq;
  graph.nodes.push_back(eqNode);

  // Doubler
  GraphNode doublerNode;
  doublerNode.id = "global_doubler";
  doublerNode.type = "delay_doubler";
  doublerNode.category = "modulation";
  doublerNode.label = "Doubler";
  doublerNode.enabled = postChain.doublerEnabled;
  doublerNode.params["time"] = postChain.doublerDelay;
  doublerNode.params["mix"] = postChain.doublerMix;
  doublerNode.params["detune"] = postChain.doublerDetune;
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
  // All defaults are already set in the struct definition
  return config;
}

} // namespace guitarfx
