/**
 * The undo/redo model behind the signal chain, and the A/B pair that owns two
 * of them.
 *
 * Pure data: no DOM, no bridge, no `uiState`. Everything here operates on plain
 * `SignalGraph` values, which is what makes it testable without a WebView — see
 * tests/signalChainHistory.test.ts. The wiring that reads the live graph, talks
 * to the engine and paints the footer lives in history.ts.
 *
 * Two ideas do all the work:
 *
 * - A **snapshot** is a deep-cloned graph plus a signature. The signature is a
 *   key-sorted serialisation, so "did this edit actually change anything?" is a
 *   string compare rather than a structural walk.
 * - A **slot** is one undo stack. There are two, A and B, and only one is live
 *   at a time. Switching slots is not an undo step in either stack: each keeps
 *   its own cursor exactly where the user left it.
 */

import type { GraphEdge, GraphNode, SignalGraph } from "../types.js";
import { isNodeBypassed } from "../graphNodes.js";

/** How many edits one slot remembers before the oldest falls off. */
export const CHAIN_HISTORY_LIMIT = 100;

export type ChainSlotId = "A" | "B";

export interface ChainSnapshot {
  /** Which preset (and scene) this graph belongs to; a mismatch means it is stale. */
  presetId: string;
  sceneId: string | null;
  graph: SignalGraph;
  signature: string;
  /** What the edit that produced this state did, for the undo tooltip. */
  label: string;
}

interface ChainSlot {
  entries: ChainSnapshot[];
  /** Cursor: `entries[index]` is the state the chain is in right now. */
  index: number;
}

// ── Cloning and signatures ─────────────────────────────────────────────────

export function cloneGraph(graph: SignalGraph | null | undefined): SignalGraph {
  return JSON.parse(JSON.stringify(graph ?? { nodes: [], edges: [] })) as SignalGraph;
}

/**
 * JSON with object keys sorted, so two graphs that differ only in key order —
 * which they routinely do after a backend round trip — produce one signature.
 */
function stableStringify(value: unknown): string {
  if (value === null || value === undefined) {
    return "null";
  }
  if (typeof value !== "object") {
    return JSON.stringify(value) ?? "null";
  }
  if (Array.isArray(value)) {
    return `[${value.map(stableStringify).join(",")}]`;
  }
  const record = value as Record<string, unknown>;
  return `{${Object.keys(record)
    .sort()
    .map((key) => `${JSON.stringify(key)}:${stableStringify(record[key])}`)
    .join(",")}}`;
}

function normalizeEdgeForCompare(edge: Partial<GraphEdge>): string {
  const gain = typeof edge.gain === "number" ? edge.gain : 1;
  return `${edge.from ?? ""}>${edge.to ?? ""}:${edge.fromPort ?? 0}/${edge.toPort ?? 0}@${gain}`;
}

/**
 * Config keys the engine owns rather than the user.
 *
 * A hosted plugin's opaque state is captured by the plugin itself, and the
 * backend scrubs the blob before the UI ever sees it, leaving only its length.
 * Neither is an edit: including them would make a plugin saving its own state
 * look like an undo step, and would have a restore push a stale length back at
 * the engine as though it were real config. Snapshots still carry whatever the
 * graph held, so a full restore stays faithful — these are excluded from the
 * comparison only.
 */
const ENGINE_OWNED_CONFIG_KEYS = new Set(["pluginStateBase64", "pluginStateBase64Length"]);

function comparableConfig(config: Record<string, string> | undefined): Record<string, string> {
  const source = config ?? {};
  const result: Record<string, string> = {};
  for (const key of Object.keys(source)) {
    if (!ENGINE_OWNED_CONFIG_KEYS.has(key)) {
      result[key] = source[key];
    }
  }
  return result;
}

/** Every part of a node that a user edit can change. */
function nodeForCompare(node: GraphNode): Record<string, unknown> {
  return {
    id: node.id,
    type: node.type,
    displayName: node.displayName ?? "",
    bypassed: isNodeBypassed(node),
    params: node.params ?? {},
    config: comparableConfig(node.config),
    resources: node.resources ?? [],
  };
}

export function graphSignature(graph: SignalGraph | null | undefined): string {
  return stableStringify({
    nodes: (graph?.nodes ?? []).map(nodeForCompare),
    edges: (graph?.edges ?? []).map(normalizeEdgeForCompare).sort(),
  });
}

export function createSnapshot(
  graph: SignalGraph | null | undefined,
  presetId: string,
  sceneId: string | null,
  label: string,
): ChainSnapshot {
  const cloned = cloneGraph(graph);
  return { presetId, sceneId, graph: cloned, signature: graphSignature(cloned), label };
}

// ── Change classification ──────────────────────────────────────────────────

/**
 * True when two graphs hold the same nodes, of the same types, in the same
 * order, wired the same way. When they do, one can be turned into the other
 * with per-node parameter messages; when they do not, only a whole-preset
 * reload will do it.
 */
export function haveSameTopology(from: SignalGraph, to: SignalGraph): boolean {
  const fromNodes = from.nodes ?? [];
  const toNodes = to.nodes ?? [];
  if (fromNodes.length !== toNodes.length) {
    return false;
  }
  for (let i = 0; i < fromNodes.length; i += 1) {
    if (fromNodes[i].id !== toNodes[i].id || fromNodes[i].type !== toNodes[i].type) {
      return false;
    }
  }
  const fromEdges = (from.edges ?? []).map(normalizeEdgeForCompare).sort();
  const toEdges = (to.edges ?? []).map(normalizeEdgeForCompare).sort();
  return fromEdges.length === toEdges.length && fromEdges.every((key, i) => key === toEdges[i]);
}

/** One node's worth of state to re-send when replaying a same-topology change. */
export interface NodeStateChange {
  nodeId: string;
  params: Array<{ key: string; value: number }>;
  config: Array<{ key: string; value: string }>;
  bypassed: boolean | null;
}

function sameKeySet(a: Record<string, unknown>, b: Record<string, unknown>): boolean {
  const aKeys = Object.keys(a);
  const bKeys = Object.keys(b);
  return aKeys.length === bKeys.length && aKeys.every((key) => key in b);
}

/**
 * The per-node deltas that turn `from` into `to`, or `null` when the change
 * cannot be expressed that way and the caller must reload the whole preset.
 *
 * Anything the targeted messages cannot say is a `null`: a different topology,
 * a renamed node, a swapped resource (NAM/IR), or a parameter/config key that
 * appeared or disappeared — there is no "unset this key" message.
 */
export function diffNodeStates(from: SignalGraph, to: SignalGraph): NodeStateChange[] | null {
  if (!haveSameTopology(from, to)) {
    return null;
  }

  const fromById = new Map((from.nodes ?? []).map((node) => [node.id, node]));
  const changes: NodeStateChange[] = [];

  for (const target of to.nodes ?? []) {
    const previous = fromById.get(target.id);
    if (!previous) {
      return null;
    }
    if ((previous.displayName ?? "") !== (target.displayName ?? "")) {
      return null;
    }
    if (stableStringify(previous.resources ?? []) !== stableStringify(target.resources ?? [])) {
      return null;
    }

    const previousParams = previous.params ?? {};
    const targetParams = target.params ?? {};
    const previousConfig = comparableConfig(previous.config);
    const targetConfig = comparableConfig(target.config);
    if (!sameKeySet(previousParams, targetParams) || !sameKeySet(previousConfig, targetConfig)) {
      return null;
    }

    const params = Object.keys(targetParams)
      .filter((key) => previousParams[key] !== targetParams[key])
      .map((key) => ({ key, value: targetParams[key] }));
    const config = Object.keys(targetConfig)
      .filter((key) => previousConfig[key] !== targetConfig[key])
      .map((key) => ({ key, value: targetConfig[key] }));
    const wasBypassed = isNodeBypassed(previous);
    const isBypassed = isNodeBypassed(target);

    if (params.length || config.length || wasBypassed !== isBypassed) {
      changes.push({
        nodeId: target.id,
        params,
        config,
        bypassed: wasBypassed === isBypassed ? null : isBypassed,
      });
    }
  }

  return changes;
}

/**
 * How a node is named in the description.
 *
 * The raw `displayName` is often a GUID — for a NAM amp or a blend it is the
 * resource's id, not anything a user would recognise. Resolving that needs the
 * effect registry and the resource library, which this module deliberately does
 * not reach for, so callers hand in `getNodeDisplayName` instead.
 */
export type NodeLabelResolver = (node: GraphNode) => string;

const fallbackNodeLabel: NodeLabelResolver = (node) => node.displayName?.trim() || node.type || "effect";

function labelFor(node: GraphNode | undefined, resolve: NodeLabelResolver): string {
  return node ? resolve(node) : "effect";
}

/**
 * A short description of what one edit did, shown on the undo/redo buttons.
 * Best effort: it reads the difference, it is not told what happened.
 */
export function describeGraphChange(
  from: SignalGraph | null | undefined,
  to: SignalGraph,
  resolveLabel: NodeLabelResolver = fallbackNodeLabel,
): string {
  if (!from) {
    return "Chain edit";
  }

  const fromNodes = from.nodes ?? [];
  const toNodes = to.nodes ?? [];
  const fromIds = new Set(fromNodes.map((node) => node.id));
  const toIds = new Set(toNodes.map((node) => node.id));

  const added = toNodes.filter((node) => !fromIds.has(node.id));
  if (added.length) {
    return `Add ${labelFor(added[0], resolveLabel)}`;
  }
  const removed = fromNodes.filter((node) => !toIds.has(node.id));
  if (removed.length) {
    return `Remove ${labelFor(removed[0], resolveLabel)}`;
  }

  const fromById = new Map(fromNodes.map((node) => [node.id, node]));
  const replaced = toNodes.find((node) => fromById.get(node.id)?.type !== node.type);
  if (replaced) {
    return `Replace ${labelFor(replaced, resolveLabel)}`;
  }
  if (fromNodes.some((node, index) => toNodes[index]?.id !== node.id)) {
    return "Reorder chain";
  }

  const changes = diffNodeStates(from, to);
  if (changes === null) {
    return "Chain edit";
  }
  if (!changes.length) {
    return "No change";
  }
  if (changes.length > 1) {
    return `Edit ${changes.length} effects`;
  }

  const first = changes[0];
  const label = labelFor(toNodes.find((node) => node.id === first.nodeId), resolveLabel);
  if (first.bypassed !== null) {
    return `${label} ${first.bypassed ? "off" : "on"}`;
  }
  if (first.params.length === 1) {
    return `${label} ${first.params[0].key}`;
  }
  return `${label} settings`;
}

// ── The A/B pair of stacks ─────────────────────────────────────────────────

function emptySlot(): ChainSlot {
  return { entries: [], index: -1 };
}

/**
 * Two independent undo stacks, one live at a time.
 *
 * Every method that changes the chain returns the snapshot the caller should
 * apply to the engine, or `null` when there is nothing to do — the model never
 * reaches out and applies anything itself.
 */
export class ChainHistory {
  private readonly slots: Record<ChainSlotId, ChainSlot> = { A: emptySlot(), B: emptySlot() };
  private active: ChainSlotId = "A";

  constructor(private readonly limit: number = CHAIN_HISTORY_LIMIT) {}

  /**
   * Start over from `seed`. Both slots begin holding it, so switching to B
   * before editing anything is silent, and A is made live again.
   */
  reset(seed: ChainSnapshot): void {
    this.slots.A = { entries: [{ ...seed, label: "Loaded" }], index: 0 };
    this.slots.B = { entries: [{ ...seed, graph: cloneGraph(seed.graph), label: "Loaded" }], index: 0 };
    this.active = "A";
  }

  isSeeded(): boolean {
    return this.slots[this.active].index >= 0;
  }

  getActiveSlotId(): ChainSlotId {
    return this.active;
  }

  getOtherSlotId(): ChainSlotId {
    return this.active === "A" ? "B" : "A";
  }

  /** The state the chain is in right now, as far as the model knows. */
  getCurrent(): ChainSnapshot | null {
    const slot = this.slots[this.active];
    return slot.entries[slot.index] ?? null;
  }

  /**
   * Record a new state. Recording from a past cursor position drops the redo
   * entries that are no longer reachable — the standard branching rule.
   */
  push(snapshot: ChainSnapshot): void {
    const slot = this.slots[this.active];
    if (slot.entries[slot.index]?.signature === snapshot.signature) {
      return;
    }
    slot.entries.length = slot.index + 1;
    slot.entries.push(snapshot);
    if (slot.entries.length > this.limit) {
      slot.entries.shift();
    }
    slot.index = slot.entries.length - 1;
  }

  /**
   * Rewrite the state under the cursor without creating an undo step. Absorbs
   * the small differences the engine introduces when it normalises a graph we
   * just sent it — those are not user edits.
   */
  amendCurrent(snapshot: ChainSnapshot): void {
    const slot = this.slots[this.active];
    if (slot.index < 0) {
      this.push(snapshot);
      return;
    }
    slot.entries[slot.index] = { ...snapshot, label: slot.entries[slot.index].label };
  }

  canUndo(): boolean {
    return this.slots[this.active].index > 0;
  }

  canRedo(): boolean {
    const slot = this.slots[this.active];
    return slot.index >= 0 && slot.index < slot.entries.length - 1;
  }

  undo(): ChainSnapshot | null {
    if (!this.canUndo()) {
      return null;
    }
    const slot = this.slots[this.active];
    slot.index -= 1;
    return slot.entries[slot.index];
  }

  redo(): ChainSnapshot | null {
    if (!this.canRedo()) {
      return null;
    }
    const slot = this.slots[this.active];
    slot.index += 1;
    return slot.entries[slot.index];
  }

  /** The edit undo would reverse, and the one redo would replay. */
  peekUndoLabel(): string | null {
    const slot = this.slots[this.active];
    return this.canUndo() ? slot.entries[slot.index].label : null;
  }

  peekRedoLabel(): string | null {
    const slot = this.slots[this.active];
    return this.canRedo() ? slot.entries[slot.index + 1].label : null;
  }

  /**
   * Make `id` the live slot and return the snapshot to apply, or `null` if it
   * was already live or holds nothing.
   */
  setActiveSlot(id: ChainSlotId): ChainSnapshot | null {
    if (id === this.active) {
      return null;
    }
    this.active = id;
    return this.getCurrent();
  }

  /**
   * Copy the live state over the other slot, as a new undo step there — so the
   * user can fork a variant to compare against without losing what it held.
   */
  copyActiveToOther(): ChainSlotId | null {
    const current = this.getCurrent();
    if (!current) {
      return null;
    }
    const otherId = this.getOtherSlotId();
    const other = this.slots[otherId];
    const copy: ChainSnapshot = {
      ...current,
      graph: cloneGraph(current.graph),
      label: `Copied from ${this.active}`,
    };
    if (other.entries[other.index]?.signature === copy.signature) {
      return otherId;
    }
    other.entries.length = other.index + 1;
    other.entries.push(copy);
    if (other.entries.length > this.limit) {
      other.entries.shift();
    }
    other.index = other.entries.length - 1;
    return otherId;
  }

  /** Whether A and B currently hold the same chain. */
  slotsMatch(): boolean {
    const a = this.slots.A.entries[this.slots.A.index];
    const b = this.slots.B.entries[this.slots.B.index];
    return Boolean(a && b && a.signature === b.signature);
  }

  /** Edit count behind a slot, for the footer's tooltip. */
  describeSlot(id: ChainSlotId): string {
    const slot = this.slots[id];
    if (slot.index < 0) {
      return "empty";
    }
    return `${slot.index} edit${slot.index === 1 ? "" : "s"}`;
  }
}
