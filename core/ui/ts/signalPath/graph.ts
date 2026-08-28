import type {
  GraphEdge,
  GraphNode,
  Preset,
} from "../types.js";
import {
  type SignalPathEdgeRef,
} from "../fxSelector.js";
export type EdgeRef = SignalPathEdgeRef & { gain: number };

export function normalizeEdge(edge: Partial<GraphEdge>): EdgeRef {
  return {
    from: String(edge.from ?? ""),
    to: String(edge.to ?? ""),
    fromPort: typeof edge.fromPort === "number" ? edge.fromPort : 0,
    toPort: typeof edge.toPort === "number" ? edge.toPort : 0,
    gain: typeof edge.gain === "number" ? edge.gain : 1.0,
  };
}

export function parseEdgeFromDataset(el: HTMLElement): EdgeRef | null {
  const from = el.dataset.edgeFrom;
  const to = el.dataset.edgeTo;
  if (!from || !to) return null;
  const fromPort = Number(el.dataset.edgeFromPort ?? "0");
  const toPort = Number(el.dataset.edgeToPort ?? "0");
  const gain = Number(el.dataset.edgeGain ?? "1");
  return { from, to, fromPort, toPort, gain };
}

export function sortEdgesByPort(edges: EdgeRef[]): EdgeRef[] {
  return edges.slice().sort((a, b) => (a.fromPort - b.fromPort) || (a.toPort - b.toPort) || a.to.localeCompare(b.to));
}

export function buildGraphMaps(graph: NonNullable<Preset["graph"]>): {
  nodeById: Map<string, GraphNode>;
  outgoing: Map<string, EdgeRef[]>;
  incoming: Map<string, EdgeRef[]>;
} {
  const nodeById = new Map<string, GraphNode>(graph.nodes.map((n) => [n.id, n]));
  const outgoing = new Map<string, EdgeRef[]>();
  const incoming = new Map<string, EdgeRef[]>();

  graph.edges.forEach((e) => {
    const edge = normalizeEdge(e);
    if (!edge.from || !edge.to) return;
    if (!outgoing.has(edge.from)) outgoing.set(edge.from, []);
    if (!incoming.has(edge.to)) incoming.set(edge.to, []);
    outgoing.get(edge.from)!.push(edge);
    incoming.get(edge.to)!.push(edge);
  });

  // Normalize ordering for stable render
  outgoing.forEach((list, key) => outgoing.set(key, sortEdgesByPort(list)));
  incoming.forEach((list, key) => incoming.set(key, sortEdgesByPort(list)));

  return { nodeById, outgoing, incoming };
}

export function pickPrimaryOutgoingEdge(outgoing: Map<string, EdgeRef[]>, fromId: string): EdgeRef | null {
  const outs = outgoing.get(fromId) ?? [];
  if (!outs.length) return null;
  // Prefer port 0 if present
  const port0 = outs.find((e) => e.fromPort === 0);
  return port0 ?? outs[0];
}
