/**
 * The per-node table in Settings → DSP Performance → Diagnostics: one dense row per
 * node, in signal-chain order, grouped under the chain it belongs to (the global pre
 * chain, each running preset, the global post chain), with the chain's input first.
 *
 * It is a plain table at 11px with tabular figures, so a long chain fits on screen:
 * the scope is a group heading rather than a column, the preset name is that heading
 * rather than a prefix on every row, and Headroom is gone (it is the peak below 0 dBFS).
 * Latency shows milliseconds; the samples are its tooltip.
 *
 * The rendering is a string so views.ts can compare it with the last frame's and skip
 * the DOM write when nothing moved.
 */

import { nodeDspLatencySamples, nodeDspProcessingSharePercent, nodeDspProcessingTimeUs } from "./dspPerformance.js";
import { EffectGuids } from "./effectGuids.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { uiState } from "./state.js";
import type { GraphEdge, GraphNode, SignalGraph, SignalLevelDiagnostics, SignalLevelMetrics, SignalLevelNodeMetrics, SignalPeakHold } from "./types.js";
import { escapeHtml } from "./utils.js";

const DASH = "—";

function formatDb(value: number): string {
  return Number.isFinite(value) ? value.toFixed(1) : DASH;
}

function sortGraphEdges(edges: GraphEdge[]): GraphEdge[] {
  return edges.slice().sort((left, right) => (
    (left.fromPort - right.fromPort)
    || (left.toPort - right.toPort)
    || left.from.localeCompare(right.from)
    || left.to.localeCompare(right.to)
  ));
}

export function getFriendlyGraphNodeName(node: GraphNode | undefined, fallbackNodeType: string, fallbackNodeId: string): string {
  if (node) {
    if (node.id === "__input__" || node.type === "input") return "Input";
    if (node.id === "__output__" || node.type === "output") return "Output";
    if (typeof node.displayName === "string" && node.displayName.trim()) {
      return node.displayName.trim();
    }
    const nodeTypeInfo = node.type ? EffectTypeRegistry.get(node.type) : undefined;
    return nodeTypeInfo?.displayName || node.type || fallbackNodeId || "(Unknown)";
  }

  const typeInfo = fallbackNodeType ? EffectTypeRegistry.get(fallbackNodeType) : undefined;
  return typeInfo?.displayName || fallbackNodeType || fallbackNodeId || "(Unknown)";
}

function getPresetGraph(presetId: string): { graph?: SignalGraph; name: string } {
  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((entry) => entry.id === presetId);
  const mixerPresetName = uiState.mixer?.presets?.[presetId]?.name;
  return { graph: preset?.graph, name: mixerPresetName || preset?.name || presetId };
}

function getNodeGraph(node: SignalLevelNodeMetrics): SignalGraph | undefined {
  if (node.scope === "pre") return uiState.globalSignalChain?.preChainGraph;
  if (node.scope === "post") return uiState.globalSignalChain?.postChainGraph;
  const presetId = node.presetId ?? uiState.activePresetId ?? "";
  return presetId ? getPresetGraph(presetId).graph : undefined;
}

/** " (Split ch 2)" for a node on a splitter's branch, so two branches' identical effects can be told apart. */
function getSplitterBranchSuffix(graph: SignalGraph | undefined, nodeId: string): string {
  if (!graph || nodeId === "__input__" || nodeId === "__output__") {
    return "";
  }

  const nodeById = new Map(graph.nodes.map((node) => [node.id, node]));
  const incomingByNode = new Map<string, GraphEdge[]>();
  const outgoingByNode = new Map<string, GraphEdge[]>();

  for (const edge of graph.edges) {
    const incoming = incomingByNode.get(edge.to);
    if (incoming) incoming.push(edge);
    else incomingByNode.set(edge.to, [edge]);

    const outgoing = outgoingByNode.get(edge.from);
    if (outgoing) outgoing.push(edge);
    else outgoingByNode.set(edge.from, [edge]);
  }

  let currentNodeId = nodeId;
  const visited = new Set<string>();

  while (currentNodeId && !visited.has(currentNodeId)) {
    visited.add(currentNodeId);
    const incomingEdges = sortGraphEdges(incomingByNode.get(currentNodeId) ?? []);
    if (incomingEdges.length !== 1) {
      return "";
    }

    const incomingEdge = incomingEdges[0];
    const upstreamNode = nodeById.get(incomingEdge.from);
    if (!upstreamNode) {
      return "";
    }

    const upstreamOutgoing = sortGraphEdges(outgoingByNode.get(upstreamNode.id) ?? []);
    if (upstreamNode.type === EffectGuids.kSplitter || upstreamOutgoing.length > 1) {
      const splitterName = getFriendlyGraphNodeName(upstreamNode, upstreamNode.type, upstreamNode.id);
      return ` (${splitterName} ch ${incomingEdge.fromPort + 1})`;
    }

    currentNodeId = upstreamNode.id;
  }

  return "";
}

/** nodeId → position in signal order (Kahn's algorithm). */
export function getGraphTopologicalOrder(graph: SignalGraph | undefined): Map<string, number> {
  const order = new Map<string, number>();
  if (!graph || !graph.nodes.length) return order;

  const indegree = new Map<string, number>();
  const outgoingByNode = new Map<string, string[]>();

  for (const node of graph.nodes) {
    indegree.set(node.id, 0);
  }
  for (const edge of graph.edges) {
    indegree.set(edge.to, (indegree.get(edge.to) ?? 0) + 1);
    const out = outgoingByNode.get(edge.from);
    if (out) out.push(edge.to);
    else outgoingByNode.set(edge.from, [edge.to]);
  }

  const queue = graph.nodes.map((n) => n.id).filter((id) => (indegree.get(id) ?? 0) === 0);
  let idx = 0;
  while (queue.length) {
    const nodeId = queue.shift()!;
    order.set(nodeId, idx++);
    for (const to of outgoingByNode.get(nodeId) ?? []) {
      const remaining = (indegree.get(to) ?? 0) - 1;
      indegree.set(to, remaining);
      if (remaining === 0) queue.push(to);
    }
  }
  return order;
}

interface TableRow {
  label: string;
  channelCount: number | null;
  timeUs: number | null;
  latencySamples: number | null;
  peakDbfs: number;
  clipped: boolean;
  clipCount: number;
}

function renderRow(row: TableRow, odd: boolean): string {
  const channels = row.channelCount == null || row.channelCount <= 0 ? null : Math.round(row.channelCount);
  const channelClass = channels == null ? "stereo-unknown" : (channels > 1 ? "stereo-on" : "stereo-off");
  const share = nodeDspProcessingSharePercent(row.timeUs);
  const time = row.timeUs != null && Number.isFinite(row.timeUs) ? row.timeUs.toFixed(1) : DASH;
  const sampleRate = uiState.dspPerformance?.sampleRate ?? 0;
  const latency = row.latencySamples != null && Number.isFinite(row.latencySamples) && row.latencySamples > 0
    ? row.latencySamples
    : null;
  const latencyText = latency == null ? DASH : (sampleRate > 0 ? `${((latency / sampleRate) * 1000).toFixed(2)} ms` : `${latency} smp`);
  const latencyTitle = latency == null ? "" : ` title="${latency} samples"`;
  const clip = row.clipped
    ? `<span class="clip-indicator"></span>${row.clipCount}`
    : "<span class=\"clip-indicator\"></span>";

  return `<tr class="dsp-node-row${odd ? " odd" : ""}${row.clipped ? " clip-on" : ""}">`
    + `<td class="name" title="${escapeHtml(row.label)}">${escapeHtml(row.label)}</td>`
    + `<td class="${channelClass}">${channels ?? DASH}</td>`
    + `<td>${time}</td>`
    + `<td>${share == null || !Number.isFinite(share) ? DASH : share.toFixed(1)}</td>`
    + `<td${latencyTitle}>${latencyText}</td>`
    + `<td>${formatDb(row.peakDbfs)}</td>`
    + `<td class="clip">${clip}</td>`
    + "</tr>";
}

const HEADER = "<thead><tr>"
  + "<th class=\"name\">Node</th>"
  + "<th title=\"Channels the node ran with in the last block\">Ch</th>"
  + "<th title=\"Processing time in the last block, in microseconds\" class=\"unit\">µs</th>"
  + "<th title=\"Share of the chain's processing time\">%</th>"
  + "<th title=\"Latency the node adds\">Latency</th>"
  + "<th title=\"Peak level, held for 10 seconds, in dBFS\">Peak</th>"
  + "<th title=\"Clipping\">Clip</th>"
  + "</tr></thead>";

/** The table for one diagnostics frame: the chain input, then each chain's nodes in signal order. */
export function renderDspNodeTableHtml(
  diagnostics: SignalLevelDiagnostics,
  input: SignalLevelMetrics,
  hold: SignalPeakHold | null | undefined,
): string {
  const preOrder = getGraphTopologicalOrder(uiState.globalSignalChain?.preChainGraph);
  const postOrder = getGraphTopologicalOrder(uiState.globalSignalChain?.postChainGraph);
  const presetOrders = new Map<string, Map<string, number>>();
  const presetOrder = (presetId: string): Map<string, number> => {
    let order = presetOrders.get(presetId);
    if (!order) {
      order = getGraphTopologicalOrder(getPresetGraph(presetId).graph);
      presetOrders.set(presetId, order);
    }
    return order;
  };

  // Group by chain: pre, each preset (the active one first), post.
  const activePresetId = uiState.activePresetId ?? "";
  const groupKey = (node: SignalLevelNodeMetrics): string => node.scope === "pre" || node.scope === "post"
    ? node.scope
    : `preset:${node.presetId ?? activePresetId}`;
  const groupRank = (key: string): string => key === "pre"
    ? "0"
    : key === "post" ? "3" : (key === `preset:${activePresetId}` ? "1" : `2${key}`);

  const groups = new Map<string, SignalLevelNodeMetrics[]>();
  for (const node of diagnostics.nodes ?? []) {
    const key = groupKey(node);
    const list = groups.get(key);
    if (list) list.push(node);
    else groups.set(key, [node]);
  }

  const parts: string[] = [renderRow({
    label: "Chain input",
    channelCount: null,
    timeUs: null,
    latencySamples: null,
    peakDbfs: hold?.input.peakDbfs ?? input.peakDbfs,
    clipped: input.clipped,
    clipCount: input.clipCount,
  }, false)];

  const orderedKeys = Array.from(groups.keys()).sort((a, b) => groupRank(a).localeCompare(groupRank(b)));
  for (const key of orderedKeys) {
    const nodes = groups.get(key)!;
    const presetId = key.startsWith("preset:") ? key.slice("preset:".length) : "";
    const order = key === "pre" ? preOrder : key === "post" ? postOrder : presetOrder(presetId);
    nodes.sort((a, b) => (order.get(a.nodeId) ?? 9999) - (order.get(b.nodeId) ?? 9999));

    const title = key === "pre" ? "Global pre chain" : key === "post" ? "Global post chain" : getPresetGraph(presetId).name;
    parts.push(`<tr class="dsp-node-group"><th colspan="7">${escapeHtml(title)}</th></tr>`);

    nodes.forEach((node, index) => {
      const graph = getNodeGraph(node);
      const graphNode = graph?.nodes.find((entry) => entry.id === node.nodeId);
      const levels = node.levels ?? { peakDbfs: Number.NaN, clipped: false, clipCount: 0 };
      parts.push(renderRow({
        label: getFriendlyGraphNodeName(graphNode, node.nodeType, node.nodeId) + getSplitterBranchSuffix(graph, node.nodeId),
        channelCount: node.channelCount ?? null,
        timeUs: nodeDspProcessingTimeUs(node),
        latencySamples: nodeDspLatencySamples(node),
        peakDbfs: hold?.nodes[node.nodeId]?.peakDbfs ?? levels.peakDbfs,
        clipped: levels.clipped,
        clipCount: levels.clipCount,
      }, index % 2 === 1));
    });
  }

  return `<table class="dsp-node-table">${HEADER}<tbody>${parts.join("")}</tbody></table>`;
}
