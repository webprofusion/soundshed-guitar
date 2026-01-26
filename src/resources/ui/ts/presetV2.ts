/**
 * Preset V2 TypeScript Utilities
 * 
 * This module provides utilities for working with the new V2 preset format
 * that supports signal graphs with arbitrary effect types.
 */

import type { Preset, GraphNode, GraphEdge, ResourceRef } from "./types.js";

export interface ParameterDef {
  key: string;
  name: string;
  default: number;
  min: number;
  max: number;
  unit: string;
  step?: number;
  labels?: string[];
}

export interface EffectTypeInfo {
  type: string;
  displayName: string;
  category: string;
  requiresResource: boolean;
  resourceType?: string;
  parameters: ParameterDef[];
}

/**
 * Effect type registry - mirrors the C++ EffectRegistry
 */
class EffectRegistry {
  private types = new Map<string, EffectTypeInfo>();

  register(type: string, info: EffectTypeInfo): void {
    this.types.set(type, info);
  }

  get(type: string): EffectTypeInfo | undefined {
    return this.types.get(type);
  }

  getByCategory(category: string): EffectTypeInfo[] {
    return Array.from(this.types.values()).filter(t => t.category === category);
  }

  getAll(): EffectTypeInfo[] {
    return Array.from(this.types.values());
  }
}

export const EffectTypeRegistry = new EffectRegistry();

// Built-in effect types definitions
export const BUILTIN_EFFECTS: EffectTypeInfo[] = [
  {
    type: "dynamics_gate",
    displayName: "Noise Gate",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "threshold", name: "Threshold", default: -60, min: -100, max: 0, unit: "dB" },
      { key: "attack", name: "Attack", default: 0.1, min: 0.01, max: 50, unit: "ms" },
      { key: "hold", name: "Hold", default: 50, min: 0, max: 500, unit: "ms" },
      { key: "release", name: "Release", default: 100, min: 10, max: 1000, unit: "ms" }
    ]
  },
  {
    type: "compressor_vca",
    displayName: "VCA Compressor",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "threshold", name: "Threshold", default: -20, min: -60, max: 0, unit: "dB" },
      { key: "ratio", name: "Ratio", default: 4, min: 1, max: 20, unit: ":1" },
      { key: "attack", name: "Attack", default: 10, min: 0.1, max: 500, unit: "ms" },
      { key: "release", name: "Release", default: 100, min: 10, max: 2000, unit: "ms" },
      { key: "knee", name: "Knee", default: 6, min: 0, max: 24, unit: "dB" },
      { key: "makeup", name: "Makeup", default: 0, min: 0, max: 24, unit: "dB" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "compressor_opto",
    displayName: "Opto Compressor",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "threshold", name: "Threshold", default: -20, min: -60, max: 0, unit: "dB" },
      { key: "ratio", name: "Ratio", default: 3, min: 1, max: 20, unit: ":1" },
      { key: "attack", name: "Attack", default: 20, min: 5, max: 200, unit: "ms" },
      { key: "release", name: "Release", default: 300, min: 50, max: 3000, unit: "ms" },
      { key: "makeup", name: "Makeup", default: 0, min: 0, max: 24, unit: "dB" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "overdrive",
    displayName: "Overdrive",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "drive", name: "Drive", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "tone", name: "Tone", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "level", name: "Level", default: 0, min: -12, max: 12, unit: "dB" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "distortion",
    displayName: "Distortion",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "drive", name: "Drive", default: 0.6, min: 0, max: 1, unit: "amount" },
      { key: "tone", name: "Tone", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "level", name: "Level", default: 0, min: -12, max: 12, unit: "dB" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "fuzz",
    displayName: "Fuzz",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "drive", name: "Drive", default: 0.7, min: 0, max: 1, unit: "amount" },
      { key: "tone", name: "Tone", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "level", name: "Level", default: 0, min: -12, max: 12, unit: "dB" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "limiter_brickwall",
    displayName: "Brickwall Limiter",
    category: "dynamics",
    requiresResource: false,
    parameters: [
      { key: "ceiling", name: "Ceiling", default: -0.1, min: -24, max: 0, unit: "dB" },
      { key: "release", name: "Release", default: 50, min: 1, max: 500, unit: "ms" }
    ]
  },
  {
    type: "amp_builtin",
    displayName: "Heavy American",
    category: "amp",
    requiresResource: false,
    parameters: [
      { key: "voice", name: "Voice", default: 0, min: 0, max: 1, unit: "toggle" },
      { key: "gain", name: "Gain", default: 0.45, min: 0, max: 1, unit: "amount" },
      { key: "stageCount", name: "Preamp Stages", default: 2, min: 1, max: 6, unit: "amount", step: 1 },
      { key: "bass", name: "Bass", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "middle", name: "Middle", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "treble", name: "Treble", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "contour", name: "Contour", default: 0.2, min: 0, max: 1, unit: "amount" },
      { key: "presence", name: "Presence", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "output", name: "Output", default: 0, min: -24, max: 24, unit: "dB" }
    ]
  },
  {
    type: "amp_nam",
    displayName: "NAM Amp Model",
    category: "amp",
    requiresResource: true,
    resourceType: "nam",
    parameters: [
      { key: "inputGain", name: "Input", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "outputGain", name: "Output", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "autoLevelInput", name: "Auto Level Input", default: 1, min: 0, max: 1, unit: "toggle" },
      { key: "autoLevelOutput", name: "Auto Level Output", default: 1, min: 0, max: 1, unit: "toggle" },
      { key: "calibrationInputLevel", name: "Cal Input", default: -18, min: -60, max: 24, unit: "dB" },
      { key: "calibrationOutputLevel", name: "Cal Output", default: -18, min: -60, max: 24, unit: "dB" }
    ]
  },
  {
    type: "amp_nam_optimized",
    displayName: "NAM Amp (SIMD)",
    category: "amp",
    requiresResource: true,
    resourceType: "nam",
    parameters: [
      { key: "inputGain", name: "Input", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "outputGain", name: "Output", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "autoLevelInput", name: "Auto Level Input", default: 1, min: 0, max: 1, unit: "toggle" },
      { key: "autoLevelOutput", name: "Auto Level Output", default: 1, min: 0, max: 1, unit: "toggle" },
      { key: "calibrationInputLevel", name: "Cal Input", default: -18, min: -60, max: 24, unit: "dB" },
      { key: "calibrationOutputLevel", name: "Cal Output", default: -18, min: -60, max: 24, unit: "dB" }
    ]
  },
  {
    type: "amp_nam_blend",
    displayName: "NAM Blend",
    category: "amp",
    requiresResource: true,
    resourceType: "nam",
    parameters: [
      { key: "blend", name: "Blend", default: 0, min: 0, max: 1, unit: "amount" },
      { key: "inputGain", name: "Input", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "outputGain", name: "Output", default: 0, min: -24, max: 24, unit: "dB" }
    ]
  },
  {
    type: "cab_ir",
    displayName: "IR Cabinet",
    category: "cab",
    requiresResource: true,
    resourceType: "ir",
    parameters: [
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" },
      { key: "outputGain", name: "Output", default: 0, min: -24, max: 24, unit: "dB" },
      { key: "air", name: "Air", default: 0, min: 0, max: 1, unit: "amount" },
      {
        key: "airMode",
        name: "Air Mode",
        default: 0,
        min: 0,
        max: 2,
        unit: "enum",
        step: 1,
        labels: ["Shelf", "Presence", "Shelf+Presence"]
      }
    ]
  },
  {
    type: "cab_simple",
    displayName: "Simple Cabinet",
    category: "cab",
    requiresResource: false,
    parameters: [
      { key: "bass", name: "Bass", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "presence", name: "Presence", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "brightness", name: "Brightness", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "eq_parametric",
    displayName: "Parametric EQ",
    category: "eq",
    requiresResource: false,
    parameters: [
      { key: "band0_gain", name: "Low Gain", default: 0, min: -18, max: 18, unit: "dB" },
      { key: "band0_freq", name: "Low Freq", default: 100, min: 20, max: 500, unit: "Hz" },
      { key: "band1_gain", name: "Low-Mid Gain", default: 0, min: -18, max: 18, unit: "dB" },
      { key: "band1_freq", name: "Low-Mid Freq", default: 400, min: 200, max: 2000, unit: "Hz" },
      { key: "band1_q", name: "Low-Mid Q", default: 1.0, min: 0.1, max: 10, unit: "amount" },
      { key: "band2_gain", name: "High-Mid Gain", default: 0, min: -18, max: 18, unit: "dB" },
      { key: "band2_freq", name: "High-Mid Freq", default: 2000, min: 1000, max: 8000, unit: "Hz" },
      { key: "band2_q", name: "High-Mid Q", default: 1.0, min: 0.1, max: 10, unit: "amount" },
      { key: "band3_gain", name: "High Gain", default: 0, min: -18, max: 18, unit: "dB" },
      { key: "band3_freq", name: "High Freq", default: 8000, min: 4000, max: 20000, unit: "Hz" }
    ]
  },
  {
    type: "gain",
    displayName: "Gain",
    category: "utility",
    requiresResource: false,
    parameters: [
      { key: "gain", name: "Gain", default: 0, min: -60, max: 24, unit: "dB" },
      { key: "polarity", name: "Invert", default: 0, min: 0, max: 1, unit: "toggle" }
    ]
  },
  {
    type: "splitter",
    displayName: "Splitter",
    category: "utility",
    requiresResource: false,
    parameters: []
  },
  {
    type: "delay_digital",
    displayName: "Digital Delay",
    category: "delay",
    requiresResource: false,
    parameters: [
      { key: "time", name: "Time", default: 300, min: 1, max: 2000, unit: "ms" },
      { key: "feedback", name: "Feedback", default: 0.4, min: 0, max: 0.95, unit: "amount" },
      { key: "mix", name: "Mix", default: 0.3, min: 0, max: 1, unit: "amount" },
      { key: "highCut", name: "High Cut", default: 8000, min: 500, max: 20000, unit: "Hz" }
    ]
  },
  {
    type: "delay_doubler",
    displayName: "Doubler",
    category: "delay",
    requiresResource: false,
    parameters: [
      { key: "time", name: "Delay Time", default: 6, min: 0, max: 100, unit: "ms" },
      { key: "mix", name: "Mix", default: 0.3, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "pitch_shift",
    displayName: "Pitch Shift",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "semitones", name: "Semitones", default: 0, min: -24, max: 24, unit: "st" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "transpose",
    displayName: "Transpose",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "semitones", name: "Semitones", default: 0, min: -36, max: 12, unit: "st", step: 1 },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "chorus",
    displayName: "Chorus",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "rate", name: "Rate", default: 1.2, min: 0.1, max: 10, unit: "Hz" },
      { key: "depth", name: "Depth", default: 12, min: 0, max: 20, unit: "ms" },
      { key: "delay", name: "Delay", default: 18, min: 1, max: 30, unit: "ms" },
      { key: "feedback", name: "Feedback", default: 0.1, min: 0, max: 0.95, unit: "amount" },
      { key: "mix", name: "Mix", default: 0.4, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "flanger",
    displayName: "Flanger",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "rate", name: "Rate", default: 0.25, min: 0.05, max: 5, unit: "Hz" },
      { key: "depth", name: "Depth", default: 2, min: 0, max: 5, unit: "ms" },
      { key: "delay", name: "Delay", default: 1, min: 0.1, max: 5, unit: "ms" },
      { key: "feedback", name: "Feedback", default: 0.2, min: 0, max: 0.95, unit: "amount" },
      { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "phaser",
    displayName: "Phaser",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "rate", name: "Rate", default: 0.4, min: 0.05, max: 8, unit: "Hz" },
      { key: "depth", name: "Depth", default: 0.8, min: 0, max: 1, unit: "amount" },
      { key: "feedback", name: "Feedback", default: 0.3, min: 0, max: 0.95, unit: "amount" },
      { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "tremolo",
    displayName: "Tremolo",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "rate", name: "Rate", default: 4, min: 0.1, max: 12, unit: "Hz" },
      { key: "depth", name: "Depth", default: 0.7, min: 0, max: 1, unit: "amount" },
      { key: "shape", name: "Shape", default: 0, min: 0, max: 1, unit: "amount" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "auto_wah",
    displayName: "Auto-Wah",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "sensitivity", name: "Sensitivity", default: 0.6, min: 0, max: 1, unit: "amount" },
      { key: "minFreq", name: "Min Freq", default: 300, min: 200, max: 1000, unit: "Hz" },
      { key: "maxFreq", name: "Max Freq", default: 2800, min: 800, max: 5000, unit: "Hz" },
      { key: "resonance", name: "Resonance", default: 2.5, min: 0.5, max: 10, unit: "Q" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "octave",
    displayName: "Octave",
    category: "modulation",
    requiresResource: false,
    parameters: [
      { key: "octaveUp", name: "Oct Up", default: 0.6, min: 0, max: 1, unit: "amount" },
      { key: "octaveDown", name: "Oct Down", default: 0.6, min: 0, max: 1, unit: "amount" },
      { key: "tone", name: "Tone", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }
    ]
  },
  {
    type: "reverb_room",
    displayName: "Room Reverb",
    category: "reverb",
    requiresResource: false,
    parameters: [
      { key: "decay", name: "Decay", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "damping", name: "Damping", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "mix", name: "Mix", default: 0.3, min: 0, max: 1, unit: "amount" }
    ]
  }
];

// Register all built-in effects
BUILTIN_EFFECTS.forEach(effect => EffectTypeRegistry.register(effect.type, effect));

/**
 * Create a new empty preset V2 structure
 */
export function createEmptyPresetV2(): Preset {
  return {
    id: crypto.randomUUID(),
    formatVersion: 2,
    name: "New Preset",
    category: "User",
    description: "",
    author: "",
    version: "1.0",
    createdAt: new Date().toISOString(),
    modifiedAt: new Date().toISOString(),
    globals: {
      inputTrim: 0,
      outputTrim: 0,
      masterVolume: 1,
      autoLevelInput: false,
      autoLevelOutput: false
    },
    graph: {
      nodes: [],
      edges: []
    },
    embeddedResources: []
  };
}

/**
 * Create a simple linear preset with amp and cab
 */
export function createSimplePresetV2(
  name: string,
  ampResource: ResourceRef | null = null,
  cabResource: ResourceRef | null = null
): Preset {
  const preset = createEmptyPresetV2();
  preset.name = name;
  
  let nodeId = 0;
  let prevNodeId = "__input__";

  // Add noise gate
  const gateNode: GraphNode = {
    id: `gate_${nodeId++}`,
    type: "dynamics_gate",
    displayName: "Gate",
    category: "dynamics",
    bypassed: false,
    params: { threshold: -60 },
    config: {}
  };
  preset.graph!.nodes.push(gateNode);
  preset.graph!.edges.push({
    from: prevNodeId,
    to: gateNode.id,
    fromPort: 0,
    toPort: 0,
    gain: 1
  });
  prevNodeId = gateNode.id;

  // Add amp if resource provided
  if (ampResource) {
    const ampNode: GraphNode = {
      id: `amp_${nodeId++}`,
      type: "amp_nam",
      displayName: "Amp",
      category: "amp",
      bypassed: false,
      params: { inputGain: 0, outputGain: 0 },
      config: {},
      resource: ampResource
    };
    preset.graph!.nodes.push(ampNode);
    preset.graph!.edges.push({
      from: prevNodeId,
      to: ampNode.id,
      fromPort: 0,
      toPort: 0,
      gain: 1
    });
    prevNodeId = ampNode.id;
  }

  // Add cab if resource provided
  if (cabResource) {
    const cabNode: GraphNode = {
      id: `cab_${nodeId++}`,
      type: "cab_ir",
      displayName: "Cab",
      category: "cab",
      bypassed: false,
      params: { mix: 1 },
      config: {},
      resource: cabResource
    };
    preset.graph!.nodes.push(cabNode);
    preset.graph!.edges.push({
      from: prevNodeId,
      to: cabNode.id,
      fromPort: 0,
      toPort: 0,
      gain: 1
    });
    prevNodeId = cabNode.id;
  }

  // Connect to output
  preset.graph!.edges.push({
    from: prevNodeId,
    to: "__output__",
    fromPort: 0,
    toPort: 0,
    gain: 1
  });

  return preset;
}

/**
 * Add a node to the preset's signal graph
 */
export function addNodeToGraph(
  preset: Preset,
  afterNodeId: string,
  nodeType: string,
  displayName: string | null = null
): GraphNode {
  if (!preset.graph) {
    throw new Error("Preset does not have a signal graph");
  }

  const typeInfo = EffectTypeRegistry.get(nodeType);
  if (!typeInfo) {
    throw new Error(`Unknown effect type: ${nodeType}`);
  }

  const newNode: GraphNode = {
    id: `${nodeType}_${Date.now()}`,
    type: nodeType,
    displayName: displayName || typeInfo.displayName,
    category: typeInfo.category,
    bypassed: false,
    params: {},
    config: {}
  };

  // Initialize default parameter values
  typeInfo.parameters.forEach(param => {
    newNode.params[param.key] = param.default;
  });

  // Find the edge to split
  const edgeIndex = preset.graph.edges.findIndex(e => e.from === afterNodeId);
  if (edgeIndex < 0) {
    throw new Error(`No outgoing edge from node: ${afterNodeId}`);
  }

  const oldEdge = preset.graph.edges[edgeIndex];
  const nextNodeId = oldEdge.to;

  // Update the edge to point to the new node
  oldEdge.to = newNode.id;

  // Add new edge from new node to the next node
  preset.graph.edges.push({
    from: newNode.id,
    to: nextNodeId,
    fromPort: 0,
    toPort: 0,
    gain: 1
  });

  // Add the node
  preset.graph.nodes.push(newNode);

  preset.modifiedAt = new Date().toISOString();

  return newNode;
}

/**
 * Remove a node from the preset's signal graph
 */
export function removeNodeFromGraph(preset: Preset, nodeId: string): void {
  if (!preset.graph) {
    throw new Error("Preset does not have a signal graph");
  }

  // Find incoming and outgoing edges
  const incomingEdge = preset.graph.edges.find(e => e.to === nodeId);
  const outgoingEdge = preset.graph.edges.find(e => e.from === nodeId);

  if (!incomingEdge || !outgoingEdge) {
    throw new Error(`Cannot remove node ${nodeId}: missing edges`);
  }

  // Reconnect: incoming -> outgoing
  incomingEdge.to = outgoingEdge.to;

  // Remove the outgoing edge
  preset.graph.edges = preset.graph.edges.filter(e => e.from !== nodeId);

  // Remove the node
  preset.graph.nodes = preset.graph.nodes.filter(n => n.id !== nodeId);

  preset.modifiedAt = new Date().toISOString();
}

/**
 * Update a node's parameter
 */
export function setNodeParam(preset: Preset, nodeId: string, key: string, value: number): void {
  if (!preset.graph) {
    throw new Error("Preset does not have a signal graph");
  }

  const node = preset.graph.nodes.find(n => n.id === nodeId);
  if (!node) {
    throw new Error(`Node not found: ${nodeId}`);
  }

  node.params[key] = value;
  preset.modifiedAt = new Date().toISOString();
}

/**
 * Bypass or enable a node
 */
export function setNodeBypassed(preset: Preset, nodeId: string, bypassed: boolean): void {
  if (!preset.graph) {
    throw new Error("Preset does not have a signal graph");
  }

  const node = preset.graph.nodes.find(n => n.id === nodeId);
  if (!node) {
    throw new Error(`Node not found: ${nodeId}`);
  }

  node.bypassed = bypassed;
  preset.modifiedAt = new Date().toISOString();
}

/**
 * Get the processing order of nodes (topological sort)
 */
export function getProcessingOrder(preset: Preset): GraphNode[] {
  if (!preset.graph) {
    return [];
  }

  const order: GraphNode[] = [];
  const visited = new Set<string>();
  const inDegree = new Map<string, number>();

  // Calculate in-degrees
  preset.graph.nodes.forEach(n => inDegree.set(n.id, 0));
  inDegree.set("__input__", 0);
  inDegree.set("__output__", 0);

  preset.graph.edges.forEach(e => {
    const current = inDegree.get(e.to) || 0;
    inDegree.set(e.to, current + 1);
  });

  // Start with nodes that have no incoming edges
  const queue = ["__input__"];

  while (queue.length > 0) {
    const nodeId = queue.shift()!;
    if (visited.has(nodeId)) continue;
    visited.add(nodeId);

    if (nodeId !== "__input__" && nodeId !== "__output__") {
      const node = preset.graph.nodes.find(n => n.id === nodeId);
      if (node) order.push(node);
    }

    // Find all outgoing edges
    preset.graph.edges
      .filter(e => e.from === nodeId)
      .forEach(e => {
        const deg = inDegree.get(e.to)! - 1;
        inDegree.set(e.to, deg);
        if (deg === 0) {
          queue.push(e.to);
        }
      });
  }

  return order;
}

export interface ValidationResult {
  valid: boolean;
  errors: string[];
}

/**
 * Validate a preset structure
 */
export function validatePresetV2(preset: Preset): ValidationResult {
  const errors: string[] = [];

  if (!preset.id) errors.push("Missing preset ID");
  if (!preset.name) errors.push("Missing preset name");
  if (preset.formatVersion !== 2) errors.push("Invalid format version");

  if (!preset.graph) {
    errors.push("Missing signal graph");
  } else {
    // Check for cycles
    try {
      const order = getProcessingOrder(preset);
      if (order.length !== preset.graph.nodes.length) {
        errors.push("Signal graph contains cycles or disconnected nodes");
      }
    } catch (e) {
      errors.push(`Graph validation error: ${(e as Error).message}`);
    }

    // Check that all edge endpoints exist
    const nodeIds = new Set(preset.graph.nodes.map(n => n.id));
    nodeIds.add("__input__");
    nodeIds.add("__output__");

    preset.graph.edges.forEach((edge, i) => {
      if (!nodeIds.has(edge.from)) {
        errors.push(`Edge ${i}: source node '${edge.from}' not found`);
      }
      if (!nodeIds.has(edge.to)) {
        errors.push(`Edge ${i}: target node '${edge.to}' not found`);
      }
    });

    // Check node types
    preset.graph.nodes.forEach(node => {
      if (!EffectTypeRegistry.get(node.type)) {
        errors.push(`Unknown effect type: ${node.type}`);
      }
    });
  }

  return {
    valid: errors.length === 0,
    errors
  };
}
