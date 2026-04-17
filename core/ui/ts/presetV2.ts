/**
 * Preset V2 TypeScript Utilities
 * 
 * This module provides utilities for working with the new V2 preset format
 * that supports signal graphs with arbitrary effect types.
 */

import type { Preset, GraphNode, GraphEdge, ResourceRef } from "./types.js";
import { EffectGuids, resolveEffectType } from "./effectGuids.js";

export interface ParameterDef {
  key: string;
  name: string;
  default: number;
  min: number;
  max: number;
  unit: string;
  step?: number;
  labels?: string[];
  group?: string;
  advanced?: boolean;
}

export interface EffectTypeInfo {
  type: string;
  displayName: string;
  category: string;
  description?: string;
  catalogHidden?: boolean;
  requiresResource: boolean;
  resourceType?: string;
  resourceFilterHint?: string[];
  /** Legacy string IDs that map to this effect type. */
  aliases?: string[];
  parameters: ParameterDef[];
  exposedResources?: Array<{
    resourceId: string;
    displayName: string;
    nodeId: string;
    resourceType: string;
    resourceIndex?: number;
    allowBrowseFile?: boolean;
    parameterId?: string;
    parameterValue?: number;
  }>;
}

export interface WasmDescriptorEntry {
  key: string;
  value: string;
}

export const WASM_GUEST_DESCRIPTOR_CONFIG_KEY = "wasmGuestDescriptor";

/**
 * Effect type registry - mirrors the C++ EffectRegistry
 */
class EffectRegistry {
  private types = new Map<string, EffectTypeInfo>();
  private aliasMap = new Map<string, string>();

  register(type: string, info: EffectTypeInfo): void {
    this.types.set(type, info);
    // Register any legacy aliases so Resolve() can find them
    if (info.aliases) {
      for (const alias of info.aliases) {
        this.aliasMap.set(alias, type);
      }
    }
  }

  /** Resolve a legacy alias string to its canonical UUID (or return unchanged). */
  resolve(type: string): string {
    return this.aliasMap.get(type) ?? type;
  }

  get(type: string): EffectTypeInfo | undefined {
    return this.types.get(this.resolve(type));
  }

  getByCategory(category: string): EffectTypeInfo[] {
    return Array.from(this.types.values()).filter(t => t.category === category);
  }

  getAll(): EffectTypeInfo[] {
    return Array.from(this.types.values());
  }
}

export const EffectTypeRegistry = new EffectRegistry();

function parseNumericDescriptorValue(value: string | undefined, fallback: number): number {
  if (typeof value !== "string") {
    return fallback;
  }
  const parsed = Number.parseFloat(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function parseBooleanDescriptorValue(value: string | undefined, fallback: boolean): boolean {
  if (typeof value !== "string") {
    return fallback;
  }
  const normalized = value.trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(normalized)) {
    return true;
  }
  if (["0", "false", "no", "off"].includes(normalized)) {
    return false;
  }
  return fallback;
}

function parseIndexedDescriptorKey(key: string, prefix: string): { index: number; field: string } | null {
  const stem = `${prefix}.`;
  if (!key.startsWith(stem)) {
    return null;
  }
  const fieldSeparator = key.indexOf(".", stem.length);
  if (fieldSeparator < 0) {
    return null;
  }
  const index = Number.parseInt(key.slice(stem.length, fieldSeparator), 10);
  if (!Number.isInteger(index) || index < 0) {
    return null;
  }
  return { index, field: key.slice(fieldSeparator + 1) };
}

function parseWasmDescriptorEntries(configValue: unknown): WasmDescriptorEntry[] {
  if (typeof configValue !== "string" || !configValue.trim()) {
    return [];
  }

  try {
    const parsed = JSON.parse(configValue) as unknown;
    if (!Array.isArray(parsed)) {
      return [];
    }
    return parsed
      .filter((entry): entry is { key?: unknown; value?: unknown } => Boolean(entry) && typeof entry === "object")
      .map((entry) => ({
        key: typeof entry.key === "string" ? entry.key : "",
        value: typeof entry.value === "string" ? entry.value : "",
      }))
      .filter((entry) => entry.key.length > 0);
  } catch {
    return [];
  }
}

function buildWasmNodeEffectInfo(base: EffectTypeInfo, node: Pick<GraphNode, "config">): EffectTypeInfo {
  const entries = parseWasmDescriptorEntries(node.config?.[WASM_GUEST_DESCRIPTOR_CONFIG_KEY]);
  if (entries.length === 0) {
    return base;
  }

  const paramGroups = new Map<number, Record<string, string>>();
  const resourceGroups = new Map<number, Record<string, string>>();
  let displayName = "";
  let category = "";
  let description = "";

  for (const entry of entries) {
    if (entry.key === "effect.name" || entry.key === "effect.title") {
      displayName = entry.value;
      continue;
    }
    if (entry.key === "effect.category") {
      category = entry.value;
      continue;
    }
    if (entry.key === "effect.description") {
      description = entry.value;
      continue;
    }

    const paramKey = parseIndexedDescriptorKey(entry.key, "param");
    if (paramKey) {
      const group = paramGroups.get(paramKey.index) ?? {};
      group[paramKey.field] = entry.value;
      paramGroups.set(paramKey.index, group);
      continue;
    }

    const resourceKey = parseIndexedDescriptorKey(entry.key, "resource");
    if (resourceKey) {
      const group = resourceGroups.get(resourceKey.index) ?? {};
      group[resourceKey.field] = entry.value;
      resourceGroups.set(resourceKey.index, group);
    }
  }

  const parameters = [...paramGroups.entries()]
    .sort((left, right) => left[0] - right[0])
    .map(([index, fields]): ParameterDef | null => {
      if (!fields.id) {
        return null;
      }
      const labels = typeof fields.labels === "string"
        ? fields.labels.split("|").map((label) => label.trim()).filter(Boolean)
        : undefined;
      const unit = fields.unit || (labels && labels.length > 0 ? "enum" : "");
      const min = parseNumericDescriptorValue(fields.min, 0);
      const max = parseNumericDescriptorValue(fields.max, 1);
      return {
        key: fields.id,
        name: fields.title || fields.name || fields.id,
        default: parseNumericDescriptorValue(fields.default, 0),
        min,
        max: max >= min ? max : min,
        unit,
        step: parseNumericDescriptorValue(fields.step, labels && labels.length > 0 ? 1 : 0) || undefined,
        labels,
        group: fields.group,
        advanced: parseBooleanDescriptorValue(fields.advanced, false),
      };
    })
    .filter((param): param is ParameterDef => param !== null);

  const exposedResources: NonNullable<EffectTypeInfo["exposedResources"]> = [];
  for (const [index, fields] of [...resourceGroups.entries()].sort((left, right) => left[0] - right[0])) {
    if (!fields.id) {
      continue;
    }
    const resourceIndex = Number.parseInt(fields.slot ?? `${index + 1}`, 10);
    if (!Number.isInteger(resourceIndex) || resourceIndex <= 0) {
      continue;
    }
    exposedResources.push({
      resourceId: fields.id,
      displayName: fields.title || fields.name || fields.id,
      nodeId: "",
      resourceType: fields.type || "blob",
      resourceIndex,
      allowBrowseFile: parseBooleanDescriptorValue(fields.allowBrowseFile, true),
      parameterId: fields.parameterId,
      parameterValue: typeof fields.parameterValue === "string"
        ? parseNumericDescriptorValue(fields.parameterValue, 0)
        : undefined,
    });
  }

  return {
    ...base,
    displayName: displayName || base.displayName,
    category: category || base.category,
    description: description || base.description,
    parameters: [...parameters, ...(base.parameters ?? [])],
    exposedResources: [...(base.exposedResources ?? []), ...exposedResources],
  };
}

export function getNodeEffectInfo(node: Pick<GraphNode, "type" | "config">): EffectTypeInfo | undefined {
  const base = EffectTypeRegistry.get(node.type);
  if (!base) {
    return undefined;
  }
  if (node.type !== EffectGuids.kWasmHost) {
    return base;
  }
  return buildWasmNodeEffectInfo(base, node);
}

// ─── Effect stub registrations ─────────────────────────────────────────────────
// These seed the EffectTypeRegistry with aliases and UI-only flags (catalogHidden).
// All other metadata (displayName, category, requiresResource, parameters) is
// provided authoritatively by the backend via the "effectCatalog" message.
//
// Exception: "input" / "output" routing nodes are NOT registered effects in the
// backend and retain their full definitions here.
// ───────────────────────────────────────────────────────────────────────────────

interface EffectStub {
  type: string;
  aliases?: string[];
  catalogHidden?: boolean;
}

const EFFECT_STUBS: EffectStub[] = [
  // Dynamics
  { type: EffectGuids.kDynamicsGate,     aliases: ["dynamics_gate", "gate_noise"] },
  { type: EffectGuids.kCompressorVca,    aliases: ["compressor_vca"] },
  { type: EffectGuids.kCompressorOpto,   aliases: ["compressor_opto"] },
  { type: EffectGuids.kLimiterBrickwall, aliases: ["limiter_brickwall"] },
  // Drive
  { type: EffectGuids.kOverdrive,        aliases: ["overdrive"] },
  { type: EffectGuids.kDistortion,       aliases: ["distortion"] },
  { type: EffectGuids.kFuzz,             aliases: ["fuzz"] },
  // Amp
  { type: EffectGuids.kAmpBuiltin,       aliases: ["amp_builtin"] },
  { type: EffectGuids.kAmpNam,           aliases: ["amp_nam"],          catalogHidden: true },
  { type: EffectGuids.kAmpNamOptimized,  aliases: ["amp_nam_optimized"] },
  { type: EffectGuids.kFxNam,            aliases: ["fx_nam"] },
  { type: EffectGuids.kAmpNamBlend,      aliases: ["amp_nam_blend"],    catalogHidden: true },
  // Cabinet
  { type: EffectGuids.kCabIr,            aliases: ["cab_ir", "ir_cab"] },
  { type: EffectGuids.kCabSimple,        aliases: ["cab_simple"] },
  // EQ
  { type: EffectGuids.kEqParametric,     aliases: ["eq_parametric"] },
  // Utility
  { type: EffectGuids.kGain,             aliases: ["gain"] },
  { type: EffectGuids.kWasmHost,         aliases: ["wasm_host"] },
  { type: EffectGuids.kSplitter,         aliases: ["splitter"] },
  { type: EffectGuids.kMixer,            aliases: ["mixer"] },
  // Delay
  { type: EffectGuids.kDelayDigital,     aliases: ["delay_digital"] },
  { type: EffectGuids.kDelayDoubler,     aliases: ["delay_doubler"] },
  // Reverb
  { type: EffectGuids.kReverbRoom,       aliases: ["reverb_room"] },
  { type: EffectGuids.kReverbChamber,    aliases: ["reverb_chamber"] },
  { type: EffectGuids.kReverbSpring,     aliases: ["reverb_spring"] },
  { type: EffectGuids.kReverbAdvanced,   aliases: ["reverb_advanced"] },
  { type: EffectGuids.kReverbIr,         aliases: ["reverb_ir"] },
  { type: EffectGuids.kReverbAmbient,    aliases: ["reverb_ambient"] },
  // Modulation
  { type: EffectGuids.kChorus,           aliases: ["chorus"] },
  { type: EffectGuids.kFlanger,          aliases: ["flanger"] },
  { type: EffectGuids.kPhaser,           aliases: ["phaser"] },
  { type: EffectGuids.kTremolo,          aliases: ["tremolo"] },
  { type: EffectGuids.kAutoWah,          aliases: ["auto_wah"] },
  // Pitch
  { type: EffectGuids.kPitchShift,       aliases: ["pitch_shift"] },
  { type: EffectGuids.kTranspose,        aliases: ["transpose"] },
  { type: EffectGuids.kTransposeStft,    aliases: ["transpose_stft"] },
  { type: EffectGuids.kOctave,           aliases: ["octave"] },
  // Synth
  { type: EffectGuids.kSynthSaw,         aliases: ["synth_saw"] },
];

// Routing nodes — not registered in the backend, need full definitions here.
const ROUTING_NODE_EFFECTS: EffectTypeInfo[] = [
  {
    type: "input",
    displayName: "Input",
    category: "utility",
    catalogHidden: true,
    requiresResource: false,
    parameters: [
      { key: "gainDb", name: "Gain", default: 0, min: -24, max: 24, unit: "dB" }
    ]
  },
  {
    type: "output",
    displayName: "Output",
    category: "utility",
    catalogHidden: true,
    requiresResource: false,
    parameters: [
      { key: "gainDb", name: "Gain", default: 0, min: -24, max: 24, unit: "dB" }
    ]
  },
];

// Seed registry: stubs for backend effects, full defs for routing nodes.
// The backend "effectCatalog" message will hydrate displayName, category,
// requiresResource and parameters for all EFFECT_STUBS entries.
EFFECT_STUBS.forEach(stub =>
  EffectTypeRegistry.register(stub.type, {
    type: stub.type,
    aliases: stub.aliases,
    catalogHidden: stub.catalogHidden,
    displayName: "",
    category: "",
    requiresResource: false,
    parameters: [],
  })
);
ROUTING_NODE_EFFECTS.forEach(effect => EffectTypeRegistry.register(effect.type, effect));

// Keep a reference to routing nodes for callers that need them.
export const BUILTIN_EFFECTS: EffectTypeInfo[] = ROUTING_NODE_EFFECTS;

export function generateUserPresetId(): string {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return `user-${crypto.randomUUID()}`;
  }
  const rand = Math.random().toString(36).slice(2, 8);
  return `user-${Date.now()}-${rand}`;
}

/**
 * Create a new empty preset V2 structure
 */
export function createEmptyPresetV2(): Preset {
  return {
    id: generateUserPresetId(),
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
      nodes: [
        {
          id: "__input__",
          type: "input",
          displayName: "Input",
          category: "utility",
          bypassed: false,
          params: {},
          config: {}
        },
        {
          id: "__output__",
          type: "output",
          displayName: "Output",
          category: "utility",
          bypassed: false,
          params: {},
          config: {}
        }
      ],
      edges: [
        {
          from: "__input__",
          to: "__output__",
          fromPort: 0,
          toPort: 0,
          gain: 1
        }
      ]
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
    type: EffectGuids.kDynamicsGate,
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
      type: EffectGuids.kAmpNam,
      displayName: "Amp",
      category: "amp",
      bypassed: false,
      params: { inputGain: 0, outputGain: 0 },
      config: {},
      resources: [ampResource]
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
      type: EffectGuids.kCabIr,
      displayName: "Cab",
      category: "cab",
      bypassed: false,
      params: { mix: 1 },
      config: {},
      resources: [cabResource]
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
 * Migrate a preset's graph node types from legacy string IDs to canonical UUIDs.
 * Safe to call on presets that are already migrated — UUIDs pass through unchanged.
 */
export function migratePresetNodeTypes(preset: Preset): Preset {
  if (!preset.graph?.nodes) return preset;
  for (const node of preset.graph.nodes) {
    node.type = resolveEffectType(node.type);
  }
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
