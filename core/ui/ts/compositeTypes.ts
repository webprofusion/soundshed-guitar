/**
 * Composite Effects Types
 *
 * A composite effect bundles multiple individual effects into a single
 * reusable unit with a simplified control surface. It acts as one node
 * in the parent signal graph while internally running a mini signal graph.
 */

import type { EffectLayout } from "./layoutTypes.js";
import type { GraphNode, GraphEdge, SignalGraph, ResourceRef } from "./types.js";

/**
 * Maps a user-facing parameter to an inner node parameter.
 */
export interface ExposedParameter {
  /** User-facing parameter ID (e.g., "drive") */
  paramId: string;
  /** Label shown in UI */
  displayName: string;
  /** Target node ID within the inner graph */
  nodeId: string;
  /** Parameter key on the target node */
  nodeParamKey: string;
  /** Override min range */
  minValue?: number;
  /** Override max range */
  maxValue?: number;
  /** Override default value */
  defaultValue?: number;
  /** Display unit (e.g., "dB", "Hz", "ms") */
  unit?: string;
  /** Mapping curve: "linear" (default), "log", "exp" */
  curve?: "linear" | "log" | "exp";
}

/**
 * Maps a user-facing resource selector to an inner node resource slot.
 */
export interface ExposedResource {
  /** User-facing resource key (e.g., "ampModel", "cabIr") */
  resourceId: string;
  /** Label shown in UI */
  displayName: string;
  /** Target node ID within the inner graph */
  nodeId: string;
  /** Resource type (e.g., "nam", "ir") */
  resourceType: string;
  /** Target resource slot on the inner node */
  resourceIndex?: number;
  /** Whether browsing custom files is allowed */
  allowBrowseFile?: boolean;
  /** Optional resource parameter key */
  parameterId?: string;
  /** Optional resource parameter value */
  parameterValue?: number;
}

/**
 * Defines a composite effect — a reusable mini signal path that
 * appears as a single node in the parent graph.
 */
export interface CompositeEffectDefinition {
  /** Unique identifier (e.g., "composite-vintage-marshall") */
  id: string;
  /** Display name */
  name: string;
  /** Effect category for UI grouping (e.g., "channel", "amp") */
  category: string;
  /** User-facing description */
  description?: string;
  /** Creator name */
  author?: string;
  /** Searchable tags */
  tags?: string[];
  /** Definition schema version */
  version?: number;
  /** The mini signal graph of inner effects */
  innerGraph: SignalGraph;
  /** Parameters surfaced to the user */
  exposedParams: ExposedParameter[];
  /** Resources surfaced to the user */
  exposedResources?: ExposedResource[];
  /** Custom control layout design */
  layout?: EffectLayout;
  /** ISO 8601 creation timestamp */
  createdAt?: string;
  /** ISO 8601 last modification timestamp */
  modifiedAt?: string;
}

/**
 * Get the effect type ID for a composite definition.
 * This matches the C++ convention: "composite:{id}"
 */
export function compositeEffectTypeId(definitionId: string): string {
  return `composite:${definitionId}`;
}

/**
 * Check if an effect type ID is a composite effect.
 */
export function isCompositeEffectType(effectType: string): boolean {
  return effectType.startsWith("composite:");
}

/**
 * Extract the definition ID from a composite effect type ID.
 */
export function compositeDefinitionId(effectType: string): string | null {
  if (!isCompositeEffectType(effectType)) return null;
  return effectType.substring("composite:".length);
}

/**
 * Create a new empty composite effect definition.
 */
export function createEmptyCompositeDefinition(id?: string): CompositeEffectDefinition {
  const defId = id ?? `composite-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  return {
    id: defId,
    name: "New Composite Effect",
    category: "channel",
    version: 1,
    innerGraph: {
      nodes: [
        { id: "in", type: "input", displayName: "Input", category: "utility", bypassed: false, params: {}, config: {} },
        { id: "out", type: "output", displayName: "Output", category: "utility", bypassed: false, params: {}, config: {} },
      ],
      edges: [
        { from: "in", to: "out", fromPort: 0, toPort: 0, gain: 1.0 },
      ],
    },
    exposedParams: [],
    exposedResources: [],
    createdAt: new Date().toISOString(),
    modifiedAt: new Date().toISOString(),
  };
}

/**
 * Create a composite definition from selected nodes and edges in an existing graph.
 * The nodes are deep-cloned and wrapped with input/output routing.
 */
export function createCompositeFromNodes(
  name: string,
  category: string,
  selectedNodeIds: string[],
  sourceGraph: SignalGraph,
  exposedParams: ExposedParameter[]
): CompositeEffectDefinition {
  const id = `composite-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;

  // Deep-clone selected nodes
  const selectedNodes = sourceGraph.nodes
    .filter((n) => selectedNodeIds.includes(n.id))
    .map((n) => ({ ...n, params: { ...n.params }, config: { ...n.config } }));

  // Get edges that connect selected nodes
  const selectedEdges = sourceGraph.edges.filter(
    (e) => selectedNodeIds.includes(e.from) && selectedNodeIds.includes(e.to)
  );

  // Determine entry/exit nodes (nodes with no incoming/outgoing edges in the selection)
  const nodesWithIncoming = new Set(selectedEdges.map((e) => e.to));
  const nodesWithOutgoing = new Set(selectedEdges.map((e) => e.from));
  const entryNodes = selectedNodes.filter((n) => !nodesWithIncoming.has(n.id));
  const exitNodes = selectedNodes.filter((n) => !nodesWithOutgoing.has(n.id));

  // Build inner graph with input/output wrapper
  const inputNode: GraphNode = {
    id: "__comp_in",
    type: "input",
    displayName: "Input",
    category: "utility",
    bypassed: false,
    params: {},
    config: {},
  };

  const outputNode: GraphNode = {
    id: "__comp_out",
    type: "output",
    displayName: "Output",
    category: "utility",
    bypassed: false,
    params: {},
    config: {},
  };

  const innerNodes = [inputNode, ...selectedNodes, outputNode];
  const innerEdges: GraphEdge[] = [
    ...entryNodes.map((n) => ({
      from: "__comp_in",
      to: n.id,
      fromPort: 0,
      toPort: 0,
      gain: 1.0,
    })),
    ...selectedEdges.map((e) => ({ ...e })),
    ...exitNodes.map((n) => ({
      from: n.id,
      to: "__comp_out",
      fromPort: 0,
      toPort: 0,
      gain: 1.0,
    })),
  ];

  return {
    id,
    name,
    category,
    version: 1,
    innerGraph: { nodes: innerNodes, edges: innerEdges },
    exposedParams,
    createdAt: new Date().toISOString(),
    modifiedAt: new Date().toISOString(),
  };
}
