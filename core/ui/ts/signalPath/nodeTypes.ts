import { uiState } from "../state.js";
import type {
  GraphNode,
  LibraryResource,
  ResourceRef,
} from "../types.js";
import { postMessage } from "../bridge.js";
import { arrayBufferToBase64 } from "../utils.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { getFxEffectIcon } from "../iconAssets.js";
import { deduplicateResourcesByHashAndPath, resolveResourceIdAlias } from "../resourceDedup.js";
import { getLibraryResource } from "../resourceLibrary.js";
export function getNodeIcon(nodeType: string): string {
  return getFxEffectIcon(nodeType);
}

export function getCategoryClass(category: string): string {
  const categoryMap: Record<string, string> = {
    "dynamics": "dynamics",
    "amp": "amp",
    "pedal": "amp",
    "preamp": "amp",
    "full-rig": "amp",
    "channel": "amp",
    "cab": "cab",
    "eq": "eq",
    "modulation": "modulation",
    "delay": "delay",
    "reverb": "reverb",
    "utility": "utility",
  };
  return categoryMap[category] || "utility";
}

export function getDeduplicatedLibraryResources(
  resourceType: string | undefined,
  preferredResourceIds: Iterable<string> = [],
): {
  resources: LibraryResource[];
  aliasById: Map<string, string>;
} {
  const resources = resourceType ? (uiState.resourceLibrary[resourceType] || []) : [];
  if (resourceType !== "nam" && resourceType !== "ir") {
    return {
      resources,
      aliasById: new Map(),
    };
  }

  const dedupResult = deduplicateResourcesByHashAndPath(resources, { preferredResourceIds });
  return {
    resources: dedupResult.deduped,
    aliasById: dedupResult.aliasById,
  };
}

export function getCanonicalLibraryResourceId(resourceType: string | undefined, resourceId: string): string {
  if (!resourceId) {
    return resourceId;
  }

  const { aliasById } = getDeduplicatedLibraryResources(resourceType);
  return resolveResourceIdAlias(resourceId, aliasById);
}

export function collectPreferredNodeResourceIds(node: GraphNode, resourceType: string): string[] {
  const resources = (node as unknown as { resources?: ResourceRef[] }).resources ?? [];
  return resources
    .filter((resource) => {
      const refType = resource.resourceType ?? resource.type ?? "";
      // Legacy refs may only carry id/resourceId with no type; still treat those
      // ids as preferred so deduplication never removes the currently selected item.
      return !refType || refType === resourceType;
    })
    .filter((resource) => Boolean(resource.resourceId ?? resource.id))
    .map((resource) => resource.resourceId ?? resource.id ?? "")
    .filter((resourceId) => Boolean(resourceId));
}

export function getLibraryResourceName(resourceType: string | undefined, resourceId: string): string {
  const match = getLibraryResource(resourceType, getCanonicalLibraryResourceId(resourceType, resourceId));
  return match?.name?.trim() ?? "";
}

export function isNeuralModelNode(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  return resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam;
}

export function isNamOrCabIrNode(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  return resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam
    || resolvedType === EffectGuids.kCabIr;
}

export /**
 * Infers the resource type ("nam" | "ir" | null) from a dropped File's extension.
 */
function inferResourceTypeFromFile(file: File): "nam" | "ir" | null {
  const lower = file.name.trim().toLowerCase();
  if (lower.endsWith(".nam")) {
    return "nam";
  }
  if (lower.endsWith(".wav") || lower.endsWith(".ir") || lower.endsWith(".flac")) {
    return "ir";
  }
  return null;
}

export /**
 * Returns whether this node accepts a given resource type via file drop.
 * NAM nodes accept .nam files; cab IR nodes accept IR files.
 */
function nodeAcceptsResourceType(node: GraphNode, resourceType: "nam" | "ir"): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  if (resourceType === "nam") {
    return resolvedType === EffectGuids.kAmpNam
      || resolvedType === EffectGuids.kAmpNamOptimized
      || resolvedType === EffectGuids.kFxNam;
  }
  if (resourceType === "ir") {
    return resolvedType === EffectGuids.kCabIr;
  }
  return false;
}

export /**
 * Saves a dropped NAM or IR file and loads it into the given node.
 * The C++ side deduplicates by hash and calls updateNodeResource automatically
 * when a nodeId is provided.
 */
async function handleNamIrFileDrop(file: File, nodeId: string, resourceIndex?: number): Promise<void> {
  const resourceType = inferResourceTypeFromFile(file);
  if (!resourceType) {
    return;
  }

  const name = file.name.replace(/\.[^.]+$/, "");
  const data = arrayBufferToBase64(await file.arrayBuffer());

  postMessage({
    type: "saveLocalLibraryResource",
    resourceType,
    data,
    fileName: file.name,
    name,
    nodeId,
    ...(resourceIndex !== undefined ? { resourceIndex } : {}),
    category: "Local",
    metadata: { provider: "local" },
  });
}

export function getNodeCategory(node: GraphNode): string {
  const anyNode = node as unknown as { category?: unknown; type?: unknown };
  const explicit = typeof anyNode.category === "string" ? anyNode.category : "";
  if (explicit) return explicit;
  const typeInfo = getNodeEffectInfo(node);
  const category = typeInfo?.category || "utility";
  if (category === "pedal" || category === "preamp" || category === "full-rig") {
    return "amp";
  }
  return category;
}
