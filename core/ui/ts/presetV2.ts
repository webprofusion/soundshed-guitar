/**
 * Preset V2 TypeScript Utilities
 * 
 * This module provides utilities for working with the new V2 preset format
 * that supports signal graphs with arbitrary effect types.
 */

import type { Preset, GraphNode } from "./types.js";
import type { ParamTaper } from "./paramTaper.js";
import { EffectGuids, migrateLegacyEffectType, resolveEffectType } from "./effectGuids.js";

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
  /** How a knob travels across min..max; absent means linear. See paramTaper.ts. */
  taper?: ParamTaper;
}

export interface EffectPresetDefinition {
  id: string;
  name: string;
  source: "factory" | "custom";
  parameters: Record<string, number>;
  parameterOrder?: string[];
}

export interface EffectTypeInfo {
  type: string;
  displayName: string;
  category: string;
  description?: string;
  thumbnailDataUrl?: string;
  catalogHidden?: boolean;
  requiresResource: boolean;
  resourceType?: string;
  resourceFilterHint?: string[];
  parameters: ParameterDef[];
  presets?: EffectPresetDefinition[];
  exposedResources?: Array<{
    resourceId: string;
    displayName: string;
    nodeId: string;
    resourceType: string;
    /**
     * The slot on the graph node itself that this picker reads and writes —
     * always, whatever the effect. A WASM module numbers its blobs from 1
     * because slot 0 is the module; a composite's slots run in declaration
     * order. A composite definition's own `resourceIndex` means the slot on the
     * inner node it forwards to and must be translated, not copied, into this.
     */
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

  register(type: string, info: EffectTypeInfo): void {
    this.types.set(type, info);
  }

  /** Resolve a legacy alias or retired type to the UUID it runs as (or return unchanged). */
  resolve(type: string): string {
    return resolveEffectType(type);
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
  let thumbnailDataUrl = "";
  let thumbnailBase64 = "";
  let thumbnailMimeType = "image/png";

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
    if (entry.key === "effect.thumbnailDataUrl") {
      thumbnailDataUrl = entry.value;
      continue;
    }
    if (entry.key === "effect.thumbnailBase64") {
      thumbnailBase64 = entry.value;
      continue;
    }
    if (entry.key === "effect.thumbnailMimeType") {
      thumbnailMimeType = entry.value || "image/png";
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
    .map(([_index, fields]): ParameterDef | null => {
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

  if (!thumbnailDataUrl && thumbnailBase64) {
    thumbnailDataUrl = `data:${thumbnailMimeType};base64,${thumbnailBase64}`;
  }

  return {
    ...base,
    displayName: displayName || base.displayName,
    category: category || base.category,
    description: description || base.description,
    thumbnailDataUrl: thumbnailDataUrl || base.thumbnailDataUrl,
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
// These seed the EffectTypeRegistry with UI-only flags (catalogHidden).
// All other metadata (displayName, category, requiresResource, parameters) is
// provided authoritatively by the backend via the "effectCatalog" message. Legacy
// IDs resolve through resolveEffectType, which mirrors the engine's aliases.
//
// Exception: "input" / "output" routing nodes are NOT registered effects in the
// backend and retain their full definitions here.
// ───────────────────────────────────────────────────────────────────────────────

interface EffectStub {
  type: string;
  catalogHidden?: boolean;
}

const EFFECT_STUBS: EffectStub[] = [
  // Dynamics
  { type: EffectGuids.kDynamicsGate },
  { type: EffectGuids.kCompressorVca },
  { type: EffectGuids.kCompressorOpto },
  { type: EffectGuids.kLimiterBrickwall },
  // Drive
  { type: EffectGuids.kOverdrive },
  { type: EffectGuids.kDistortion },
  { type: EffectGuids.kFuzz },
  // Amp
  { type: EffectGuids.kAmpBuiltin },
  { type: EffectGuids.kAmpNamOptimized },
  { type: EffectGuids.kFxNam },
  { type: EffectGuids.kAmpNamBlend, catalogHidden: true },
  // Cabinet
  { type: EffectGuids.kCabIr },
  { type: EffectGuids.kCabSimple },
  // EQ
  { type: EffectGuids.kEqParametric },
  { type: EffectGuids.kEqGraphic },
  // Utility
  { type: EffectGuids.kGain },
  { type: EffectGuids.kWasmHost },
  { type: EffectGuids.kSplitter },
  { type: EffectGuids.kMixer },
  // Delay
  { type: EffectGuids.kDelayDigital },
  { type: EffectGuids.kDelayTape },
  { type: EffectGuids.kDelayAnalog },
  { type: EffectGuids.kDelayDoubler },
  // Reverb
  { type: EffectGuids.kReverbRoom },
  { type: EffectGuids.kReverbChamber },
  { type: EffectGuids.kReverbSpring },
  { type: EffectGuids.kReverbAdvanced },
  { type: EffectGuids.kReverbIr },
  { type: EffectGuids.kReverbAmbient },
  // Modulation
  { type: EffectGuids.kChorus },
  { type: EffectGuids.kFlanger },
  { type: EffectGuids.kPhaser },
  { type: EffectGuids.kTremolo },
  { type: EffectGuids.kRingMod },
  { type: EffectGuids.kAutoWah },
  // Pitch
  { type: EffectGuids.kPitchShift },
  { type: EffectGuids.kTranspose },
  { type: EffectGuids.kTransposeStft },
  { type: EffectGuids.kOctave },
  // Synth
  { type: EffectGuids.kSynthSaw },
];

// Routing nodes — not registered in the backend, need full definitions here. The engine declares the gain range a second time, as kBoundaryGainMinDb/kBoundaryGainMaxDb in presets/PresetTypes.h, which is what automation maps a MIDI or DAW value onto; move one and move the other.
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
 * Migrate a preset's graph node types from legacy string IDs to canonical UUIDs.
 * Safe to call on presets that are already migrated — UUIDs pass through unchanged,
 * retired ones included (see RETIRED_EFFECT_TYPES).
 */
export function migratePresetNodeTypes(preset: Preset): Preset {
  if (!preset.graph?.nodes) return preset;
  for (const node of preset.graph.nodes) {
    node.type = migrateLegacyEffectType(node.type);
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
