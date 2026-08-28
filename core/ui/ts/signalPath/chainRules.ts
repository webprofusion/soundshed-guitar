import type {
  GraphNode,
  Preset,
} from "../types.js";
import { EffectTypeRegistry } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { buildGraphMaps } from "./graph.js";
export function hasCabIrInSameSignalPath(nodeId: string, preset: Preset): boolean {
  const graph = preset.graph;
  if (!graph) {
    return false;
  }

  const { nodeById, outgoing, incoming } = buildGraphMaps(graph);
  if (!nodeById.has(nodeId)) {
    return false;
  }

  const isCabNode = (candidateId: string): boolean => {
    const candidate = nodeById.get(candidateId);
    if (!candidate) {
      return false;
    }
    return EffectTypeRegistry.resolve(candidate.type) === EffectGuids.kCabIr;
  };

  const search = (direction: "downstream" | "upstream"): boolean => {
    const visited = new Set<string>([nodeId]);
    const queue: string[] = [nodeId];

    while (queue.length > 0) {
      const current = queue.shift();
      if (!current) {
        continue;
      }

      const edges = direction === "downstream"
        ? (outgoing.get(current) ?? [])
        : (incoming.get(current) ?? []);
      for (const edge of edges) {
        const nextId = direction === "downstream" ? edge.to : edge.from;
        if (!nextId || visited.has(nextId)) {
          continue;
        }
        if (isCabNode(nextId)) {
          return true;
        }
        visited.add(nextId);
        queue.push(nextId);
      }
    }

    return false;
  };

  return search("downstream") || search("upstream");
}

export function shouldShowFullRigCabModelNote(node: GraphNode, preset: Preset): boolean {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kCabIr) {
    return false;
  }
  return hasFullRigNamInSameSignalPath(node.id, preset);
}

export function hasFullRigNamInSameSignalPath(nodeId: string, preset: Preset): boolean {
  const graph = preset.graph;
  if (!graph) {
    return false;
  }

  const { nodeById, outgoing, incoming } = buildGraphMaps(graph);
  if (!nodeById.has(nodeId)) {
    return false;
  }

  const isFullRigNamNode = (candidateId: string): boolean => {
    const candidate = nodeById.get(candidateId);
    if (!candidate) {
      return false;
    }
    return nodeUsesFullRigNamCategory(candidate);
  };

  const visited = new Set<string>([nodeId]);
  const queue: string[] = [nodeId];
  while (queue.length > 0) {
    const current = queue.shift();
    if (!current) {
      continue;
    }

    const neighbors = [
      ...(outgoing.get(current) ?? []).map((edge) => edge.to),
      ...(incoming.get(current) ?? []).map((edge) => edge.from),
    ];
    for (const nextId of neighbors) {
      if (!nextId || visited.has(nextId)) {
        continue;
      }
      if (isFullRigNamNode(nextId)) {
        return true;
      }
      visited.add(nextId);
      queue.push(nextId);
    }
  }

  return false;
}

export function nodeUsesFullRigNamCategory(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  const isNamEffect = resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam
    || resolvedType === EffectGuids.kAmpNamBlend;
  if (!isNamEffect) {
    return false;
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];
  const resourceCount = Math.max(1, resources.length);
  for (let index = 0; index < resourceCount; index += 1) {
    const resource = getNodeResourceAtIndex(node, index);
    if (!resource.id) {
      continue;
    }

    const libraryResource = getLibraryResource("nam", resource.id);
    const category = normalizeNamGearCategory(
      String(libraryResource?.category ?? libraryResource?.metadata?.gear ?? ""),
    );
    if (category === "full-rig") {
      return true;
    }

    // Fall back to NAM gear_type metadata (e.g. "amp_cab" / "amp+cab") from the
    // model file header. If gear_type contains "cab", the capture includes a
    // cabinet model and should be treated as a full rig.
    const gearType = String(libraryResource?.metadata?.gear_type ?? "").toLowerCase();
    if (gearType && gearType.includes("cab")) {
      return true;
    }
  }

  return false;
}

export function normalizeNamGearCategory(raw: string): "pedal" | "preamp" | "amp" | "full-rig" | "cab" | "" {
  const value = raw.trim().toLowerCase();
  if (!value) {
    return "";
  }
  if (value === "outboard") {
    return "preamp";
  }
  if (value === "pedal" || value === "preamp" || value === "amp" || value === "full-rig" || value === "cab") {
    return value;
  }
  return "";
}
