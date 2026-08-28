import { uiState } from "../state.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import type {
  CustomEffectLibraryEntry,
  GraphNode,
  LibraryResource,
  ResourceRef,
} from "../types.js";
import { postMessage } from "../bridge.js";
import { requestResourceData } from "../archiveUtils.js";
import { escapeHtml, base64ToArrayBuffer } from "../utils.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { buildLayoutMatchText } from "../layoutPreferences.js";
import { getCustomEffectEntry } from "../customEffects.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { getNodeResourceAtIndex, getResourceBaseName } from "./nodeResources.js";
import { getLibraryResourceName, isNeuralModelNode } from "./nodeTypes.js";
import { requestSignalPathRender } from "./render.js";
export const inferredNamArchitectureByResourceId = new Map<string, string>();

export const pendingNamArchitectureResourceIds = new Set<string>();

export const unavailableNamArchitectureResourceIds = new Set<string>();

export function getNodeResourceDisplayName(node: GraphNode, index = 0, overrideResourceType?: string): string {
  const typeInfo = getNodeEffectInfo(node);
  const resourceType = overrideResourceType || typeInfo?.resourceType;
  const resource = getNodeResourceAtIndex(node, index);

  if (resource.filePath) {
    return getResourceBaseName(resource.filePath);
  }

  const libraryName = getLibraryResourceName(resourceType, resource.id);
  return libraryName || resource.id;
}

export function getNodeResourceSummary(node: GraphNode): string {
  const anyNode = node as unknown as { resources?: unknown };
  if (Array.isArray(anyNode.resources)) {
    const names = anyNode.resources
      .map((_, index) => getNodeResourceDisplayName(node, index))
      .filter((name) => Boolean(name));
    if (names.length > 1) return names.join(" + ");
    if (names.length === 1) return names[0];
  }

  return getNodeResourceDisplayName(node, 0);
}

/**
 * Lower-cased make/model text a node is matched against by keyword layout rules:
 * its display name, any loaded resource (amp model / IR / plugin) names, and the
 * effect's own display name.
 */
export function buildNodeLayoutMatchText(node: GraphNode): string {
  const typeInfo = getNodeEffectInfo(node) ?? EffectTypeRegistry.get(node.type);
  return buildLayoutMatchText([
    getNodeDisplayName(node),
    getNodeResourceSummary(node),
    typeInfo?.displayName,
  ]);
}

export function normalizeArchitectureBadge(raw: string): string {
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }
  if (normalized === "2" || normalized === "a2") {
    return "A2";
  }
  if (normalized === "1" || normalized === "a1") {
    return "A1";
  }
  if (normalized === "custom") {
    return "Custom";
  }
  return "";
}

export function mapNamArchitectureTokenToBadge(raw: string): string {
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }

  if (normalized.includes("slimmablecontainer") || normalized.includes("slimmable")) {
    return "A2";
  }
  if (normalized.includes("wavenet")) {
    return "A1";
  }

  return "";
}

export function inferNamArchitectureBadgeFromData(base64Data: string): string {
  let text = "";
  try {
    text = new TextDecoder().decode(base64ToArrayBuffer(base64Data));
  } catch {
    return "";
  }

  const architectureMatch = text.match(/"architecture(?:_version|Version)?"\s*:\s*(?:"([^"]+)"|(\d+(?:\.\d+)?))/i);
  const explicitToken = (architectureMatch?.[1] || architectureMatch?.[2] || "").trim();
  const explicitBadge = normalizeArchitectureBadge(explicitToken) || mapNamArchitectureTokenToBadge(explicitToken);
  if (explicitBadge) {
    return explicitBadge;
  }

  if (/"architecture"\s*:\s*"slimmablecontainer"/i.test(text) || /"submodels"\s*:/i.test(text)) {
    return "A2";
  }
  if (/"architecture"\s*:\s*"wavenet"/i.test(text)) {
    return "A1";
  }

  return "";
}

export function requestNamArchitectureInference(resource: LibraryResource): void {
  const resourceId = resource.id;
  if (!resourceId
      || inferredNamArchitectureByResourceId.has(resourceId)
      || pendingNamArchitectureResourceIds.has(resourceId)
      || unavailableNamArchitectureResourceIds.has(resourceId)) {
    return;
  }

  pendingNamArchitectureResourceIds.add(resourceId);

  void (async () => {
    try {
      const data = await requestResourceData("nam", resourceId);
      if (!data) {
        unavailableNamArchitectureResourceIds.add(resourceId);
        return;
      }

      const badge = inferNamArchitectureBadgeFromData(data);
      if (!badge) {
        unavailableNamArchitectureResourceIds.add(resourceId);
        return;
      }

      inferredNamArchitectureByResourceId.set(resourceId, badge);

      const existingBadge = normalizeArchitectureBadge(
        resource.metadata?.architectureVersion
        || resource.metadata?.architecture_version
        || resource.metadata?.architecture
        || "",
      );

      if (!existingBadge) {
        const metadata = {
          ...(resource.metadata ?? {}),
          architectureVersion: badge,
        };
        resource.metadata = metadata;

        postMessage({
          type: "updateLibraryResource",
          resourceType: "nam",
          resourceId,
          name: resource.name,
          category: resource.category,
          description: resource.description,
          metadata,
        });
      }

      requestSignalPathRender();
    } catch {
      unavailableNamArchitectureResourceIds.add(resourceId);
    } finally {
      pendingNamArchitectureResourceIds.delete(resourceId);
    }
  })();
}

export function getNodeArchitectureBadge(node: GraphNode): string {
  if (!isNeuralModelNode(node)) {
    return "";
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];

  for (let index = 0; index < Math.max(1, resources.length); index += 1) {
    const current = getNodeResourceAtIndex(node, index);
    if (!current.id) {
      continue;
    }

    const resource = getLibraryResource("nam", current.id);
    const metadata = resource?.metadata ?? {};
    const label = normalizeArchitectureBadge(
      metadata.architectureVersion
      || metadata.architecture_version
      || metadata.architecture
      || "",
    );
    if (label) {
      return label;
    }

    if (resource?.id && inferredNamArchitectureByResourceId.has(resource.id)) {
      const inferred = inferredNamArchitectureByResourceId.get(resource.id);
      if (inferred) {
        return inferred;
      }
    }

    if (resource) {
      requestNamArchitectureInference(resource);
    }
  }

  return "";
}

export function hasNamCalibrationMetadataValue(value: string | undefined): boolean {
  if (typeof value !== "string") {
    return false;
  }
  const trimmed = value.trim();
  if (!trimmed.length) {
    return false;
  }
  const parsed = Number(trimmed);
  return Number.isFinite(parsed);
}

export function isNodeFullyCalibratedFromNamMetadata(node: GraphNode): boolean {
  if (!isNeuralModelNode(node)) {
    return false;
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];
  const resourceCount = Math.max(1, resources.length);

  let modelsWithCalibration = 0;
  let consideredModels = 0;
  for (let index = 0; index < resourceCount; index += 1) {
    const current = getNodeResourceAtIndex(node, index);
    if (!current.id) {
      continue;
    }
    consideredModels += 1;
    const resource = getLibraryResource("nam", current.id);
    const metadata = resource?.metadata ?? {};
    const hasInputCalibration = hasNamCalibrationMetadataValue(metadata.inputLevelDbu);
    const hasOutputCalibration = hasNamCalibrationMetadataValue(metadata.outputLevelDbu);
    if (hasInputCalibration && hasOutputCalibration) {
      modelsWithCalibration += 1;
    }
  }

  return consideredModels > 0 && modelsWithCalibration === consideredModels;
}

export function getNodeNamCalibrationMetadataChip(node: GraphNode): string {
  if (!isNodeFullyCalibratedFromNamMetadata(node)) {
    return "";
  }

  return `<span class="default-effect-shell-chip default-effect-shell-chip-calibration default-effect-shell-chip-calibration-complete" title="Loaded model includes both input and output calibration metadata (inputLevelDbu/outputLevelDbu).">Calibrated</span>`;
}

export function getMissingResourceEntries(node: GraphNode): Array<{ resourceType?: string; resourceId?: string; filePath?: string }> {
  const typeInfo = getNodeEffectInfo(node);
  const backendMissing = (uiState.missingNodeResources ?? []).filter((entry) => entry.nodeId === node.id);

  const refs: Array<{ resourceType?: string; resourceId?: string; filePath?: string }> = [];
  const addRef = (ref?: ResourceRef): void => {
    if (!ref) return;
    const resourceType = ref.type || typeInfo?.resourceType;
    const resourceId = ref.id;
    const filePath = ref.filePath;
    refs.push({ resourceType, resourceId, filePath });
  };

  if (Array.isArray(node.resources)) {
    node.resources.forEach((ref) => addRef(ref));
  }

  const isBackendMissing = (entry: { resourceType?: string; resourceId?: string; filePath?: string }): boolean => {
    return backendMissing.some((missing) => {
      if (entry.filePath && missing.filePath) {
        return entry.filePath === missing.filePath;
      }
      if (entry.resourceType && entry.resourceId && missing.resourceType && missing.resourceId) {
        return entry.resourceType === missing.resourceType && entry.resourceId === missing.resourceId;
      }
      return false;
    });
  };

  return refs.filter((entry) => {
    if (entry.resourceType && entry.resourceId) {
      const resource = getLibraryResource(entry.resourceType, entry.resourceId);
      if (!resource) {
        return true;
      }
      if (resource.fileMissing === true) {
        return true;
      }
      return false;
    }

    if (entry.filePath) {
      return isBackendMissing(entry);
    }

    return true;
  });
}

export function buildMissingResourceTooltip(entries: Array<{ resourceType?: string; resourceId?: string; filePath?: string }>): string {
  if (!entries.length) {
    return "";
  }
  const details = entries.map((entry) => {
    if (entry.filePath) {
      return entry.filePath;
    }
    if (entry.resourceType && entry.resourceId) {
      return `${entry.resourceType}:${entry.resourceId}`;
    }
    return "Missing resource";
  });
  return `Missing resource file: ${details.join(", ")}`;
}

export function getNodeDisplayName(node: GraphNode): string {
  // Support backend presets that use label/enabled instead of displayName/bypassed.
  const anyNode = node as unknown as { id?: unknown; type?: unknown; displayName?: unknown; label?: unknown };
  const nodeId = typeof anyNode.id === "string" ? anyNode.id : "";
  const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";

  if (nodeId === "__input__" || nodeType === "input") return "Input";
  if (nodeId === "__output__" || nodeType === "output") return "Output";

  const explicit = typeof anyNode.displayName === "string" && anyNode.displayName.trim()
    ? anyNode.displayName.trim()
    : (typeof anyNode.label === "string" && anyNode.label.trim() ? anyNode.label.trim() : "");
  const typeInfo = getNodeEffectInfo(node);
  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (blendId) {
    const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
    if (blend?.name) {
      return blend.name;
    }
  }

  const customEffectId = (node as unknown as { config?: Record<string, string> }).config?.customEffectId;
  if (customEffectId) {
    const customEffect = uiState.customEffectLibrary?.find((entry) => entry.id === customEffectId);
    if (customEffect?.name) {
      return customEffect.name;
    }
  }

  const resourceTitle = typeInfo?.requiresResource ? getNodeResourceSummary(node) : "";
  if (resourceTitle) return resourceTitle;

  if (explicit && explicit !== (typeInfo?.displayName || "")) {
    return explicit;
  }

  if (explicit) return explicit;
  return typeInfo?.displayName || nodeType || "(Unknown)";
}

export function getLinkedCustomEffectEntry(node: GraphNode): CustomEffectLibraryEntry | undefined {
  const customEffectId = (node as unknown as { config?: Record<string, string> }).config?.customEffectId ?? "";
  return customEffectId ? getCustomEffectEntry(customEffectId) : undefined;
}

export function hasCustomEffectModuleSelection(node: GraphNode): boolean {
  const resource = getNodeResourceAtIndex(node, 0);
  return Boolean(resource.id || resource.filePath);
}

export function buildCustomEffectActionStatus(node: GraphNode): string {
  const linkedEntry = getLinkedCustomEffectEntry(node);
  if (linkedEntry?.name) {
    return `Linked to ${linkedEntry.name} in My Custom Effects. You can also prompt a new revision for this node.`;
  }

  if (hasCustomEffectModuleSelection(node)) {
    return `Current module: ${getNodeResourceDisplayName(node, 0, "wasm") || "WASM module selected"}. Prompt a new module or save this one to My Custom Effects.`;
  }

  return "Describe a Custom Effect to generate a new module for this node, then save or apply it here.";
}

export function buildCustomEffectActions(node: GraphNode): string {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kWasmHost) {
    return "";
  }

  if (!isFeatureEnabled(Features.CustomEffects)) {
    return "";
  }

  const hasModule = hasCustomEffectModuleSelection(node);
  const linkedEntry = getLinkedCustomEffectEntry(node);
  const saveLabel = linkedEntry ? "Update My Custom Effect" : "Save To My Custom Effects";

  return `
    <div class="node-resource-selector node-custom-effect-actions" data-node-id="${node.id}">
      <label>Custom Effect Designer</label>
      <div class="resource-controls">
        <button type="button" class="primary-btn custom-effect-design-btn" data-node-id="${node.id}">Design With AI</button>
        <button type="button" class="primary-btn custom-effect-save-btn" data-node-id="${node.id}" ${hasModule ? "" : "disabled"}>${saveLabel}</button>
        <button type="button" class="secondary-btn custom-effect-use-btn" data-node-id="${node.id}" ${hasModule ? "" : "disabled"}>Use This Effect</button>
      </div>
      <div class="resource-path-info">${escapeHtml(buildCustomEffectActionStatus(node))}</div>
    </div>
  `;
}
