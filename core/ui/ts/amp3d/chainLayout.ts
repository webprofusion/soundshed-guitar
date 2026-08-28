/**
 * Graph → chain layout (pure). Walks trunk + splitter parallel lanes and
 * assigns world anchors for the 3D stage.
 */

import { EffectGuids, resolveEffectType } from "../effectGuids.js";
import {
  cabinetCountForCabNode,
  resolveCabinetCount,
} from "./chainCabRules.js";
import type {
  BuildChainLayoutOptions,
  ChainKnobDefInput,
  ChainLaneLayout,
  ChainLayout,
  ChainLayoutEdgeInput,
  ChainLayoutNodeInput,
  ChainUnitAnchor,
  ChainUnitDesc,
  ChainUnitKind,
} from "./chainTypes.js";

const UNIT_SPACING_X = 0.62;
/** Single amp/cab cluster advance along X. */
/**
 * Dual 4x12 cabs span ~1.7m wide; advance far enough that neighbouring
 * 19" racks (~0.52m) never collide with either cab.
 */
const DUAL_CAB_CLUSTER_SPACING_X = 2.35;
const LANE_SPACING_Z = 0.95;
/** Fit distance for 12U flight-case towers (casters + aluminum frame). */
const RACK_FIT = 1.95;
const PEDAL_FIT = 0.95;
const AMP_FIT = 1.55;
const CAB_FIT = 1.45;
const DUAL_CAB_FIT = 2.25;
/**
 * Fixed physical rack height. Real FX fill from the top; remaining U spaces
 * are blank plates. Overflow beyond this starts a new chassis.
 */
export const RACK_TOTAL_U = 12;
/** Visual 1U height used for anchor Y hints (must match rackUnit). */
export const RACK_U_HEIGHT = 0.052;

const DEFAULT_AMP_TYPES = [
  EffectGuids.kAmpNam,
  EffectGuids.kAmpNamOptimized,
  EffectGuids.kAmpNamBlend,
  EffectGuids.kAmpBuiltin,
];
const DEFAULT_CAB_TYPES = [EffectGuids.kCabIr, EffectGuids.kCabSimple];
const DEFAULT_PEDAL_TYPES = [EffectGuids.kFxNam];
/** Routing / utility nodes — kept for graph walking, omitted from 3D stage. */
const DEFAULT_JUNCTION_TYPES = [EffectGuids.kSplitter, EffectGuids.kMixer];
const DEFAULT_UTILITY_TYPES = [
  EffectGuids.kSplitter,
  EffectGuids.kMixer,
  EffectGuids.kInputAnalyzer,
];

interface EdgeRef {
  from: string;
  to: string;
  fromPort: number;
  toPort: number;
  gain: number;
}

function normalizeEdge(edge: ChainLayoutEdgeInput): EdgeRef {
  return {
    from: edge.from,
    to: edge.to,
    fromPort: typeof edge.fromPort === "number" ? edge.fromPort : 0,
    toPort: typeof edge.toPort === "number" ? edge.toPort : 0,
    gain: typeof edge.gain === "number" ? edge.gain : 1,
  };
}

function sortEdgesByPort(edges: EdgeRef[]): EdgeRef[] {
  return [...edges].sort((a, b) => a.fromPort - b.fromPort || a.toPort - b.toPort || a.to.localeCompare(b.to));
}

function buildMaps(graph: BuildChainLayoutOptions["graph"]): {
  nodeById: Map<string, ChainLayoutNodeInput>;
  outgoing: Map<string, EdgeRef[]>;
} {
  const nodeById = new Map<string, ChainLayoutNodeInput>();
  graph.nodes.forEach((node) => nodeById.set(node.id, node));
  const outgoing = new Map<string, EdgeRef[]>();
  graph.edges.forEach((raw) => {
    const edge = normalizeEdge(raw);
    if (!outgoing.has(edge.from)) outgoing.set(edge.from, []);
    outgoing.get(edge.from)!.push(edge);
  });
  outgoing.forEach((list, key) => outgoing.set(key, sortEdgesByPort(list)));
  return { nodeById, outgoing };
}

function pickPrimaryOutgoing(outgoing: Map<string, EdgeRef[]>, fromId: string): EdgeRef | null {
  const outs = outgoing.get(fromId) ?? [];
  if (!outs.length) return null;
  return outs.find((e) => e.fromPort === 0) ?? outs[0];
}

function findJoinNodeId(outgoing: Map<string, EdgeRef[]>, splitterId: string, outs: EdgeRef[]): string | null {
  if (outs.length < 2) return null;

  const walkBranch = (startNodeId: string): string[] => {
    const path: string[] = [];
    let currentId = startNodeId;
    const localVisited = new Set<string>();
    let guard = 0;
    while (currentId && !localVisited.has(currentId) && guard++ < 500) {
      localVisited.add(currentId);
      if (currentId === "__output__") break;
      path.push(currentId);
      const edge = pickPrimaryOutgoing(outgoing, currentId);
      if (!edge) break;
      currentId = edge.to;
    }
    return path;
  };

  const branchPaths = outs
    .map((e) => e.to)
    .filter((to) => to && to !== "__output__")
    .map(walkBranch);
  if (branchPaths.length < 2) return null;

  const candidateSet = new Set(branchPaths[0]);
  for (let i = 1; i < branchPaths.length; i += 1) {
    for (const id of Array.from(candidateSet)) {
      if (!branchPaths[i].includes(id)) candidateSet.delete(id);
    }
  }
  candidateSet.delete(splitterId);
  for (const id of branchPaths[0]) {
    if (candidateSet.has(id)) return id;
  }
  return null;
}

function isBypassed(node: ChainLayoutNodeInput): boolean {
  if (typeof node.bypassed === "boolean") return node.bypassed;
  if (typeof node.enabled === "boolean") return !node.enabled;
  return false;
}

function resourceSnaps(node: ChainLayoutNodeInput): ChainUnitDesc["resources"] {
  if (!Array.isArray(node.resources)) return [];
  return node.resources.map((res) => {
    const id = typeof res?.id === "string" && res.id
      ? res.id
      : (typeof res?.resourceId === "string" && res.resourceId
        ? res.resourceId
        : (typeof res?.embeddedId === "string" ? res.embeddedId : ""));
    const filePath = typeof res?.filePath === "string" ? res.filePath : "";
    return { id, filePath };
  });
}

function knobsFor(nodeId: string, knobsByNodeId?: Record<string, ChainKnobDefInput[]>): ChainUnitDesc["knobs"] {
  const defs = knobsByNodeId?.[nodeId] ?? [];
  return defs.map((def) => ({
    key: def.key,
    label: def.label,
    value: def.value,
    defaultValue: def.defaultValue,
    min: def.min,
    max: def.max,
    step: def.step,
    unit: def.unit,
  }));
}

interface TypeSets {
  resolveType: (type: string) => string;
  amp: Set<string>;
  cab: Set<string>;
  pedal: Set<string>;
  junction: Set<string>;
  utility: Set<string>;
}

function buildTypeSets(options: BuildChainLayoutOptions): TypeSets {
  const junction = new Set(options.junctionTypeIds ?? DEFAULT_JUNCTION_TYPES);
  const utility = new Set([
    ...DEFAULT_UTILITY_TYPES,
    ...junction,
    ...(options.utilityTypeIds ?? []),
  ]);
  return {
    resolveType: options.resolveType ?? resolveEffectType,
    amp: new Set(options.ampTypeIds ?? DEFAULT_AMP_TYPES),
    cab: new Set(options.cabTypeIds ?? DEFAULT_CAB_TYPES),
    pedal: new Set(options.pedalTypeIds ?? DEFAULT_PEDAL_TYPES),
    junction,
    utility,
  };
}

function classifyKind(resolvedType: string, types: TypeSets): ChainUnitKind {
  if (resolvedType === "input" || resolvedType === "output") return "io";
  if (types.amp.has(resolvedType)) return "amp";
  if (types.cab.has(resolvedType)) return "cab";
  if (types.pedal.has(resolvedType)) return "pedal";
  if (types.junction.has(resolvedType) || types.utility.has(resolvedType)) return "junction";
  return "rack";
}

/** Utility / routing / I/O — no dedicated 3D product in the chain stage. */
function omitFrom3dStage(kind: ChainUnitKind): boolean {
  return kind === "junction" || kind === "io";
}

function fitForKind(kind: ChainUnitKind, cabinetCount = 0, _stackCount = 1): number {
  switch (kind) {
    case "amp":
    case "amp_cab_cluster":
      return cabinetCount >= 2 ? DUAL_CAB_FIT : AMP_FIT;
    case "cab":
      return cabinetCount >= 2 ? DUAL_CAB_FIT : CAB_FIT;
    case "pedal":
      return PEDAL_FIT;
    case "rack_stack":
    case "rack":
      // Always a full 12U tower.
      return RACK_FIT;
    default:
      return RACK_FIT;
  }
}

interface PlacedSlot {
  nodeIds: string[];
  primaryNodeId: string;
  kind: ChainUnitKind;
  showHead: boolean;
  cabinetCount: number;
  pairedNodeId?: string;
}

function clusterSlots(
  nodeIds: string[],
  nodeById: Map<string, ChainLayoutNodeInput>,
  types: TypeSets,
  fullRigByNodeId: Record<string, boolean>,
): PlacedSlot[] {
  const slots: PlacedSlot[] = [];
  let index = 0;
  while (index < nodeIds.length) {
    const nodeId = nodeIds[index];
    const node = nodeById.get(nodeId);
    if (!node) {
      index += 1;
      continue;
    }
    const resolved = types.resolveType(node.type);
    const kind = classifyKind(resolved, types);

        // Splitter/mixer/analyzer/I/O: walk past them — no 3D stand-in.
        if (omitFrom3dStage(kind)) {
          index += 1;
          continue;
        }

        if (kind === "amp") {
      const nextId = nodeIds[index + 1];
      const nextNode = nextId ? nodeById.get(nextId) : undefined;
      const nextResolved = nextNode ? types.resolveType(nextNode.type) : "";
      const nextIsCab = nextNode ? types.cab.has(nextResolved) : false;
      const fullRig = Boolean(fullRigByNodeId[nodeId]);

      if (nextIsCab && nextNode) {
        slots.push({
          nodeIds: [nodeId, nextId],
          primaryNodeId: nodeId,
          kind: "amp_cab_cluster",
          showHead: true,
          cabinetCount: resolveCabinetCount({
            hasAmp: true,
            ampIsFullRig: fullRig,
            cabNode: nextNode,
          }),
          pairedNodeId: nextId,
        });
        index += 2;
        continue;
      }

      const cabinetCount = resolveCabinetCount({
        hasAmp: true,
        ampIsFullRig: fullRig,
        cabNode: null,
      });
      slots.push({
        nodeIds: [nodeId],
        primaryNodeId: nodeId,
        kind: cabinetCount > 0 ? "amp_cab_cluster" : "amp",
        showHead: true,
        cabinetCount,
      });
      index += 1;
      continue;
    }

    if (kind === "cab") {
      const irCount = cabinetCountForCabNode(node);
      slots.push({
        nodeIds: [nodeId],
        primaryNodeId: nodeId,
        kind: "cab",
        showHead: false,
        // Empty cab still shows one shell so the node is visible in 3D.
        cabinetCount: irCount > 0 ? Math.min(2, irCount) : 1,
      });
      index += 1;
      continue;
    }

        // Consecutive standard effects share one 12U rack chassis (top→bottom).
        if (kind === "rack") {
          const stackIds = [nodeId];
          let look = index + 1;
          while (look < nodeIds.length && stackIds.length < RACK_TOTAL_U) {
            const nextId = nodeIds[look];
            const nextNode = nodeById.get(nextId);
            if (!nextNode) {
              look += 1;
              continue;
            }
            const nextKind = classifyKind(types.resolveType(nextNode.type), types);
            if (omitFrom3dStage(nextKind)) {
              look += 1;
              continue;
            }
            if (nextKind !== "rack") break;
            stackIds.push(nextId);
            look += 1;
          }
          slots.push({
            nodeIds: stackIds,
            primaryNodeId: stackIds[0],
            kind: stackIds.length > 1 ? "rack_stack" : "rack",
            showHead: false,
            cabinetCount: 0,
          });
          index = look;
          continue;
        }

        slots.push({
          nodeIds: [nodeId],
          primaryNodeId: nodeId,
          kind,
          showHead: false,
          cabinetCount: 0,
        });
        index += 1;
      }
      return slots;
    }

function walkLinearBranch(
  startEdge: EdgeRef,
  joinId: string | null,
  outgoing: Map<string, EdgeRef[]>,
  nodeById: Map<string, ChainLayoutNodeInput>,
  visited: Set<string>,
): string[] {
  const ids: string[] = [];
  let edge: EdgeRef | null = startEdge;
  let guard = 0;
  while (edge && guard++ < 200) {
    if (joinId && edge.to === joinId) break;
    if (edge.to === "__output__" || edge.to === "__input__") break;
    const node = nodeById.get(edge.to);
    if (!node) break;
    if (visited.has(node.id)) break;
    ids.push(node.id);
    visited.add(node.id);
    const resolved = resolveEffectType(node.type);
    if (resolved === EffectGuids.kSplitter || (outgoing.get(node.id)?.length ?? 0) >= 2) break;
    edge = pickPrimaryOutgoing(outgoing, node.id);
  }
  return ids;
}

interface LaneBuild {
  laneIndex: number;
  nodeIds: string[];
}

/** Build ordered lanes of node ids from the graph (excluding pure I/O). */
export function collectChainLanes(graph: BuildChainLayoutOptions["graph"]): LaneBuild[] {
  const { nodeById, outgoing } = buildMaps(graph);
  const visited = new Set<string>();
  const lanes: LaneBuild[] = [];
  let trunkNodes: string[] = [];
  let currentId = "__input__";
  let guard = 0;

  const flushTrunk = () => {
    if (trunkNodes.length) {
      lanes.push({ laneIndex: 0, nodeIds: trunkNodes });
      trunkNodes = [];
    }
  };

  // Prefer explicit input id, else a node typed input, else first edge from unknown.
  if (!outgoing.has("__input__")) {
    const inputNode = graph.nodes.find((n) => n.type === "input" || n.id === "in");
    if (inputNode) currentId = inputNode.id;
  }

  while (guard++ < 500) {
    if (currentId !== "__input__" && currentId !== "__output__") {
      if (visited.has(currentId)) break;
      const node = nodeById.get(currentId);
      if (node) {
        const outs = outgoing.get(currentId) ?? [];
        const isSplit = outs.length >= 2 || resolveEffectType(node.type) === EffectGuids.kSplitter;
        if (isSplit) {
          visited.add(currentId);
          trunkNodes.push(currentId);
          const joinId = findJoinNodeId(outgoing, currentId, outs);
          if (joinId && outs.length >= 2) {
            flushTrunk();
            sortEdgesByPort(outs).forEach((edge, branchIndex) => {
              const branchVisited = new Set<string>(visited);
              const branchIds = walkLinearBranch(edge, joinId, outgoing, nodeById, branchVisited);
              branchIds.forEach((id) => visited.add(id));
              lanes.push({ laneIndex: branchIndex, nodeIds: branchIds });
            });
            currentId = joinId;
            continue;
          }
        } else {
          visited.add(currentId);
          trunkNodes.push(currentId);
        }
      }
    }

    const edge = pickPrimaryOutgoing(outgoing, currentId);
    if (!edge) break;
    if (edge.to === "__output__") break;
    currentId = edge.to;
  }

  flushTrunk();

  if (!lanes.length) {
    const ids = graph.nodes
      .filter((n) => n.id !== "__input__" && n.id !== "__output__" && n.type !== "input" && n.type !== "output")
      .map((n) => n.id);
    if (ids.length) lanes.push({ laneIndex: 0, nodeIds: ids });
  }

  return lanes;
}

function makeUnitDesc(
  slot: PlacedSlot,
  nodeById: Map<string, ChainLayoutNodeInput>,
  types: TypeSets,
  options: BuildChainLayoutOptions,
  laneIndex: number,
  orderInLane: number,
): ChainUnitDesc {
  const primary = nodeById.get(slot.primaryNodeId)!;
  const resolved = types.resolveType(primary.type);
    const friendlyNodeLabel = (node: ChainLayoutNodeInput, fallbackType: string): string => {
      const raw = (node.displayName && node.displayName.trim()) || "";
      const looksLikeId = !raw
        || raw === node.id
        || /^[0-9a-f]{8}-[0-9a-f]{4}-/i.test(raw);
      if (!looksLikeId) return raw;
          // Fall back to a readable type/category title (never the node GUID).
          const category = (node.category && node.category.trim()) || "";
          if (category && !/^[0-9a-f]{8}-/i.test(category)) {
            return category.replace(/[_-]+/g, " ");
          }
          return fallbackType.replace(/[_-]+/g, " ") || "FX";
        };
    const label = friendlyNodeLabel(primary, resolved);
    const displayText = options.displayTextByNodeId?.[slot.primaryNodeId] || label;

    const isRack = slot.kind === "rack" || slot.kind === "rack_stack";
    const stack = isRack
      ? slot.nodeIds.map((id) => {
        const node = nodeById.get(id)!;
        const effectType = types.resolveType(node.type);
        const nodeLabel = friendlyNodeLabel(node, effectType);
        const rawDisplay = options.displayTextByNodeId?.[id] || "";
        const display = rawDisplay && rawDisplay !== id && !/^[0-9a-f]{8}-[0-9a-f]{4}-/i.test(rawDisplay)
          ? rawDisplay
          : nodeLabel;
        return {
          nodeId: id,
          effectType,
          label: nodeLabel,
          category: node.category || "",
          bypassed: isBypassed(node),
          knobs: knobsFor(id, options.knobsByNodeId),
          displayText: display,
          resources: resourceSnaps(node),
        };
      })
      : undefined;

  return {
    nodeId: slot.primaryNodeId,
    effectType: resolved,
    label,
    category: primary.category || "",
    kind: slot.kind,
    bypassed: isBypassed(primary),
    knobs: knobsFor(slot.primaryNodeId, options.knobsByNodeId),
    displayText,
    resources: resourceSnaps(primary),
    laneIndex,
    orderInLane,
    pairedNodeId: slot.pairedNodeId,
    cabinetCount: slot.cabinetCount,
    showHead: slot.showHead,
    stack,
      rackUnitCount: isRack ? RACK_TOTAL_U : undefined,
    };
  }

export function buildChainLayout(options: BuildChainLayoutOptions): ChainLayout {
  const types = buildTypeSets(options);
  const { nodeById } = buildMaps(options.graph);
  const fullRigByNodeId = options.fullRigByNodeId ?? {};
  const rawLanes = collectChainLanes(options.graph);

  const units: ChainUnitDesc[] = [];
  const anchors: ChainUnitAnchor[] = [];
  const layoutLanes: ChainLaneLayout[] = [];

  let cursorX = 0;
  let index = 0;
  while (index < rawLanes.length) {
    const head = rawLanes[index];
    const run: LaneBuild[] = [head];
    if (head.laneIndex === 0) {
      if (rawLanes[index + 1] && rawLanes[index + 1].laneIndex === 1) {
        let j = index + 1;
        while (j < rawLanes.length && rawLanes[j].laneIndex > 0) {
          run.push(rawLanes[j]);
          j += 1;
        }
        index = j;
      } else {
        index += 1;
      }
    } else {
      let j = index + 1;
      while (j < rawLanes.length && rawLanes[j].laneIndex > 0) {
        run.push(rawLanes[j]);
        j += 1;
      }
      index = j;
    }

    const runLaneCount = run.length;
        const zSpread = runLaneCount <= 1 ? 0 : ((runLaneCount - 1) * LANE_SPACING_Z) / 2;

        const slotSpan = (slot: { kind: ChainUnitKind; cabinetCount: number }): number => {
                  if (slot.cabinetCount >= 2) return DUAL_CAB_CLUSTER_SPACING_X;
                  if (slot.kind === "amp_cab_cluster" || slot.kind === "amp") {
                    return slot.cabinetCount >= 1 ? 0.9 : 0.78;
                  }
                  if (slot.kind === "cab") return slot.cabinetCount >= 2 ? DUAL_CAB_CLUSTER_SPACING_X : 0.9;
                  if (slot.kind === "rack" || slot.kind === "rack_stack") return 0.88;
                  return UNIT_SPACING_X;
                };

                run.forEach((lane) => {
                  const slots = clusterSlots(lane.nodeIds, nodeById, types, fullRigByNodeId);
                  const z = runLaneCount <= 1 ? 0 : lane.laneIndex * LANE_SPACING_Z - zSpread;
                  const unitNodeIds: string[] = [];
                  let laneCursor = cursorX;

                  slots.forEach((slot, order) => {
                    const desc = makeUnitDesc(slot, nodeById, types, options, lane.laneIndex, order);
                    units.push(desc);
                    const stackIds = (desc.stack ?? [])
                      .filter((s) => !s.blank)
                      .map((s) => s.nodeId);
                    const hostedIds = stackIds.length ? stackIds : [desc.nodeId];
                    hostedIds.forEach((id) => {
                      if (!unitNodeIds.includes(id)) unitNodeIds.push(id);
                    });
                    if (desc.pairedNodeId && !unitNodeIds.includes(desc.pairedNodeId)) {
                      unitNodeIds.push(desc.pairedNodeId);
                    }

                    const stackCount = hostedIds.length;
                    const fit = fitForKind(desc.kind, desc.cabinetCount, stackCount);
                    const rackU = desc.rackUnitCount ?? RACK_TOTAL_U;
                    // Signal order fills top → bottom inside the fixed 12U chassis.
                    hostedIds.forEach((id, stackIndex) => {
                      const uFromTop = stackIndex;
                      const uFromBottom = Math.max(0, rackU - 1 - uFromTop);
                      anchors.push({
                        nodeId: id,
                        x: laneCursor,
                        y: uFromBottom * RACK_U_HEIGHT,
                        z,
                        fitDistance: fit,
                        kind: desc.kind === "rack_stack" ? "rack" : desc.kind,
                      });
                    });
                    if (desc.pairedNodeId && !hostedIds.includes(desc.pairedNodeId)) {
                      anchors.push({
                        nodeId: desc.pairedNodeId,
                        x: laneCursor,
                        y: 0,
                        z,
                        fitDistance: fitForKind("cab", desc.cabinetCount),
                        kind: "cab",
                      });
                    }
                    laneCursor += slotSpan(desc);
                  });

                  layoutLanes.push({ laneIndex: lane.laneIndex, z, unitNodeIds });
                });

        // Advance trunk cursor by the widest lane in this run.
        const runWidth = Math.max(
          UNIT_SPACING_X,
          ...run.map((lane) => {
            const slots = clusterSlots(lane.nodeIds, nodeById, types, fullRigByNodeId);
            return slots.reduce((sum, slot) => sum + slotSpan(slot), 0);
          }),
        );
        cursorX += runWidth + UNIT_SPACING_X * 0.4;
      }

  let minX = 0;
  let maxX = 0;
  let minZ = 0;
  let maxZ = 0;
  if (anchors.length) {
    minX = Math.min(...anchors.map((a) => a.x));
    maxX = Math.max(...anchors.map((a) => a.x));
    minZ = Math.min(...anchors.map((a) => a.z));
    maxZ = Math.max(...anchors.map((a) => a.z));
  }

  const structureSignature = units.map((unit) =>
    [
      unit.nodeId,
      unit.effectType,
      unit.kind,
      unit.cabinetCount,
      unit.showHead ? "h" : "",
      unit.pairedNodeId ?? "",
        (unit.stack ?? []).map((s) => s.nodeId).join("+"),
        unit.knobs.map((k) => k.key).join(","),
        (unit.stack ?? []).map((s) => `${s.nodeId}:${s.knobs.map((k) => k.key).join(",")}`).join(";"),
        unit.bypassed ? "b" : "a",
        unit.resources.map((r) => r.id || r.filePath).join("+"),
      ].join(":"),
    ).join("|");

  return {
    units,
    anchors,
    lanes: layoutLanes,
    bounds: { minX, maxX, minZ, maxZ },
    structureSignature,
  };
}

export function findAnchorForNode(layout: ChainLayout, nodeId: string): ChainUnitAnchor | undefined {
  return layout.anchors.find((anchor) => anchor.nodeId === nodeId);
}

export function findUnitForNode(layout: ChainLayout, nodeId: string): ChainUnitDesc | undefined {
  return layout.units.find(
    (unit) =>
      unit.nodeId === nodeId
      || unit.pairedNodeId === nodeId
      || Boolean(unit.stack?.some((slot) => slot.nodeId === nodeId)),
  );
}
