/**
 * Preset V2 JavaScript Utilities
 * 
 * This module provides utilities for working with the new V2 preset format
 * that supports signal graphs with arbitrary effect types.
 */

// Effect type registry - mirrors the C++ EffectRegistry
const EffectTypeRegistry = {
  types: new Map(),

  register(type, info) {
    this.types.set(type, info);
  },

  get(type) {
    return this.types.get(type);
  },

  getByCategory(category) {
    return Array.from(this.types.values()).filter(t => t.category === category);
  },

  getAll() {
    return Array.from(this.types.values());
  }
};

// Register built-in effect types
const BUILTIN_EFFECTS = [
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
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "" }
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
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "" }
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
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "" },
      { key: "outputGain", name: "Output", default: 0, min: -24, max: 24, unit: "dB" }
    ]
  },
  {
    type: "cab_simple",
    displayName: "Simple Cabinet",
    category: "cab",
    requiresResource: false,
    parameters: [
      { key: "bass", name: "Bass", default: 0.5, min: 0, max: 1, unit: "" },
      { key: "presence", name: "Presence", default: 0.5, min: 0, max: 1, unit: "" },
      { key: "brightness", name: "Brightness", default: 0.5, min: 0, max: 1, unit: "" },
      { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "" }
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
      { key: "band1_q", name: "Low-Mid Q", default: 1.0, min: 0.1, max: 10, unit: "" },
      { key: "band2_gain", name: "High-Mid Gain", default: 0, min: -18, max: 18, unit: "dB" },
      { key: "band2_freq", name: "High-Mid Freq", default: 2000, min: 1000, max: 8000, unit: "Hz" },
      { key: "band2_q", name: "High-Mid Q", default: 1.0, min: 0.1, max: 10, unit: "" },
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
      { key: "polarity", name: "Invert", default: 0, min: 0, max: 1, unit: "" }
    ]
  },
  {
    type: "delay_digital",
    displayName: "Digital Delay",
    category: "delay",
    requiresResource: false,
    parameters: [
      { key: "time", name: "Time", default: 300, min: 1, max: 2000, unit: "ms" },
      { key: "feedback", name: "Feedback", default: 0.4, min: 0, max: 0.95, unit: "" },
      { key: "mix", name: "Mix", default: 0.3, min: 0, max: 1, unit: "" },
      { key: "highCut", name: "High Cut", default: 8000, min: 500, max: 20000, unit: "Hz" }
    ]
  },
  {
    type: "reverb_room",
    displayName: "Room Reverb",
    category: "reverb",
    requiresResource: false,
    parameters: [
      { key: "decay", name: "Decay", default: 0.5, min: 0, max: 1, unit: "" },
      { key: "damping", name: "Damping", default: 0.5, min: 0, max: 1, unit: "" },
      { key: "mix", name: "Mix", default: 0.3, min: 0, max: 1, unit: "" }
    ]
  }
];

// Register all built-in effects
BUILTIN_EFFECTS.forEach(effect => EffectTypeRegistry.register(effect.type, effect));

/**
 * Create a new empty preset V2 structure
 */
function createEmptyPresetV2() {
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
      masterVolume: 1
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
function createSimplePresetV2(name, ampResource = null, cabResource = null) {
  const preset = createEmptyPresetV2();
  preset.name = name;
  
  let nodeId = 0;
  let prevNodeId = "__input__";

  // Add noise gate
  const gateNode = {
    id: `gate_${nodeId++}`,
    type: "dynamics_gate",
    displayName: "Gate",
    category: "dynamics",
    bypassed: false,
    params: { threshold: -60 },
    config: {}
  };
  preset.graph.nodes.push(gateNode);
  preset.graph.edges.push({
    from: prevNodeId,
    to: gateNode.id,
    fromPort: 0,
    toPort: 0,
    gain: 1
  });
  prevNodeId = gateNode.id;

  // Add amp if resource provided
  if (ampResource) {
    const ampNode = {
      id: `amp_${nodeId++}`,
      type: "amp_nam",
      displayName: "Amp",
      category: "amp",
      bypassed: false,
      params: { inputGain: 0, outputGain: 0 },
      config: {},
      resource: ampResource
    };
    preset.graph.nodes.push(ampNode);
    preset.graph.edges.push({
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
    const cabNode = {
      id: `cab_${nodeId++}`,
      type: "cab_ir",
      displayName: "Cab",
      category: "cab",
      bypassed: false,
      params: { mix: 1 },
      config: {},
      resource: cabResource
    };
    preset.graph.nodes.push(cabNode);
    preset.graph.edges.push({
      from: prevNodeId,
      to: cabNode.id,
      fromPort: 0,
      toPort: 0,
      gain: 1
    });
    prevNodeId = cabNode.id;
  }

  // Connect to output
  preset.graph.edges.push({
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
function addNodeToGraph(preset, afterNodeId, nodeType, displayName = null) {
  const typeInfo = EffectTypeRegistry.get(nodeType);
  if (!typeInfo) {
    throw new Error(`Unknown effect type: ${nodeType}`);
  }

  const newNode = {
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
function removeNodeFromGraph(preset, nodeId) {
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
function setNodeParam(preset, nodeId, key, value) {
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
function setNodeBypassed(preset, nodeId, bypassed) {
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
function getProcessingOrder(preset) {
  const order = [];
  const visited = new Set();
  const inDegree = new Map();

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
    const nodeId = queue.shift();
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
        const deg = inDegree.get(e.to) - 1;
        inDegree.set(e.to, deg);
        if (deg === 0) {
          queue.push(e.to);
        }
      });
  }

  return order;
}

/**
 * Validate a preset structure
 */
function validatePresetV2(preset) {
  const errors = [];

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
      errors.push(`Graph validation error: ${e.message}`);
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

/**
 * Convert a V1 preset to V2 format (client-side migration)
 */
function migratePresetV1ToV2(v1Preset) {
  const v2 = createEmptyPresetV2();

  v2.id = v1Preset.id || crypto.randomUUID();
  v2.name = v1Preset.name || "Migrated Preset";
  v2.category = v1Preset.category || "User";
  v2.description = v1Preset.description || "";
  v2.author = "";
  v2.version = "1.0";

  // Migrate parameters to globals
  const params = {};
  (v1Preset.parameters || []).forEach(p => {
    params[p.id] = p.value;
  });

  v2.globals.inputTrim = params.inputTrim || params.input_trim || 0;
  v2.globals.outputTrim = params.outputTrim || params.output_trim || 0;

  let nodeId = 0;
  let prevNode = "__input__";

  const addNode = (type, displayName, extraParams = {}, resource = null) => {
    const node = {
      id: `${type}_${nodeId++}`,
      type,
      displayName,
      category: EffectTypeRegistry.get(type)?.category || "unknown",
      bypassed: false,
      params: { ...extraParams },
      config: {}
    };
    if (resource) {
      node.resource = resource;
    }
    v2.graph.nodes.push(node);
    v2.graph.edges.push({
      from: prevNode,
      to: node.id,
      fromPort: 0,
      toPort: 0,
      gain: 1
    });
    prevNode = node.id;
    return node;
  };

  // Add noise gate if enabled
  if (params.gateEnabled || params.gate_enabled) {
    addNode("dynamics_gate", "Gate", {
      threshold: params.gateThreshold || params.gate_threshold || -60
    });
  }

  // Add amp model
  const namAttachment = (v1Preset.attachments || []).find(a => a.type === "nam" || a.type === "audiofx");
  if (namAttachment || v1Preset.audioFxModelId) {
    const resource = namAttachment
      ? { type: "nam", id: namAttachment.id, filePath: namAttachment.filePath }
      : { type: "nam", id: v1Preset.audioFxModelId };
    addNode("amp_nam", "Amp", {
      inputGain: (params.drive || 0.5) * 24 - 12
    }, resource);
  }

  // Add cabinet
  const irAttachment = (v1Preset.attachments || []).find(a => a.type === "ir");
  if (irAttachment || v1Preset.irId) {
    const resource = irAttachment
      ? { type: "ir", id: irAttachment.id, filePath: irAttachment.filePath }
      : { type: "ir", id: v1Preset.irId };
    addNode("cab_ir", "Cab", { mix: 1 }, resource);
  } else if (params.simpleCabEnabled || params.simplecab_enabled) {
    addNode("cab_simple", "Simple Cab", {
      bass: params.simpleCabBass || params.simplecab_bass || 0.5,
      presence: params.simpleCabPresence || params.simplecab_presence || 0.5,
      brightness: params.simpleCabBrightness || params.simplecab_brightness || 0.5
    });
  }

  // Add EQ if enabled
  if (params.eqEnabled || params.eq_enabled) {
    addNode("eq_parametric", "EQ", {
      band0_gain: params.eq_low_gain || 0,
      band0_freq: params.eq_low_freq || 100,
      band1_gain: params.eq_lowmid_gain || 0,
      band1_freq: params.eq_lowmid_freq || 400,
      band1_q: params.eq_lowmid_q || 1,
      band2_gain: params.eq_highmid_gain || 0,
      band2_freq: params.eq_highmid_freq || 2000,
      band2_q: params.eq_highmid_q || 1,
      band3_gain: params.eq_high_gain || 0,
      band3_freq: params.eq_high_freq || 8000
    });
  }

  // Add delay if enabled
  if (params.delayEnabled || params.delay_enabled) {
    addNode("delay_digital", "Delay", {
      time: params.delayTime || params.delay_time || 300,
      feedback: params.delayFeedback || params.delay_feedback || 0.4,
      mix: params.delayMix || params.delay_mix || 0.3
    });
  }

  // Add reverb if enabled
  if (params.reverbEnabled || params.reverb_enabled) {
    addNode("reverb_room", "Reverb", {
      decay: params.reverbDecay || params.reverb_decay || 0.5,
      damping: params.reverbDamping || params.reverb_damping || 0.5,
      mix: params.reverbMix || params.reverb_mix || 0.3
    });
  }

  // Connect to output
  v2.graph.edges.push({
    from: prevNode,
    to: "__output__",
    fromPort: 0,
    toPort: 0,
    gain: 1
  });

  return v2;
}

// Export for use in main.js
window.PresetV2 = {
  EffectTypeRegistry,
  createEmptyPresetV2,
  createSimplePresetV2,
  addNodeToGraph,
  removeNodeFromGraph,
  setNodeParam,
  setNodeBypassed,
  getProcessingOrder,
  validatePresetV2,
  migratePresetV1ToV2,
  BUILTIN_EFFECTS
};
