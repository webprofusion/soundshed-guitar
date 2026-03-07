/**
 * Composite Effects Management
 *
 * Handles composite effect library state, messaging with the engine,
 * and integration with the signal path UI.
 */

import { uiState } from "./state.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { EffectTypeRegistry } from "./presetV2.js";
import type {
  CompositeEffectDefinition,
  ExposedParameter,
} from "./compositeTypes.js";
import {
  compositeEffectTypeId,
  isCompositeEffectType,
  compositeDefinitionId,
  createEmptyCompositeDefinition,
  createCompositeFromNodes,
} from "./compositeTypes.js";
import type { SignalGraph } from "./types.js";

// ─────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────

/**
 * Get the full composite library from UI state.
 */
export function getCompositeLibrary(): CompositeEffectDefinition[] {
  return uiState.compositeLibrary ?? [];
}

/**
 * Find a composite definition by ID.
 */
export function getCompositeDefinition(
  id: string
): CompositeEffectDefinition | undefined {
  return getCompositeLibrary().find((d) => d.id === id);
}

/**
 * Find a composite definition by its effect type ID.
 */
export function getCompositeByEffectType(
  effectType: string
): CompositeEffectDefinition | undefined {
  const defId = compositeDefinitionId(effectType);
  if (!defId) return undefined;
  return getCompositeDefinition(defId);
}

// ─────────────────────────────────────────────────────────────
// Message Handlers (Engine → UI)
// ─────────────────────────────────────────────────────────────

/**
 * Handle the full composite library sync from the engine.
 */
export function handleCompositeLibrary(
  definitions: CompositeEffectDefinition[]
): void {
  uiState.compositeLibrary = definitions ?? [];
  // Register each composite as an effect type so signal path can resolve them
  registerCompositeEffectTypes(uiState.compositeLibrary);
  appendLog(
    `Composite library updated: ${uiState.compositeLibrary.length} definitions`
  );
}

/**
 * Handle a single composite definition added/updated.
 */
export function handleCompositeDefinitionAdded(
  definition: CompositeEffectDefinition
): void {
  if (!definition?.id) return;

  const lib = uiState.compositeLibrary ?? [];
  const idx = lib.findIndex((d) => d.id === definition.id);
  if (idx >= 0) {
    lib[idx] = definition;
  } else {
    lib.push(definition);
  }
  uiState.compositeLibrary = lib;

  // Re-register to keep EffectTypeRegistry in sync
  registerCompositeEffectType(definition);

  appendLog(`Composite definition added/updated: ${definition.name}`);
}

/**
 * Handle a composite definition removal.
 */
export function handleCompositeDefinitionRemoved(id: string): void {
  if (!id) return;

  const lib = uiState.compositeLibrary ?? [];
  uiState.compositeLibrary = lib.filter((d) => d.id !== id);
  appendLog(`Composite definition removed: ${id}`);
}

// ─────────────────────────────────────────────────────────────
// Commands (UI → Engine)
// ─────────────────────────────────────────────────────────────

/**
 * Save or update a composite effect definition.
 */
export function saveCompositeDefinition(
  definition: CompositeEffectDefinition
): void {
  if (!definition.id || !definition.name) {
    showNotification("Composite definition requires an ID and name", "error");
    return;
  }
  if (
    !definition.innerGraph?.nodes ||
    definition.innerGraph.nodes.length === 0
  ) {
    showNotification("Composite definition requires an inner graph", "error");
    return;
  }

  definition.modifiedAt = new Date().toISOString();
  if (!definition.createdAt) {
    definition.createdAt = definition.modifiedAt;
  }

  window.NAMBridge.postMessage({
    type: "saveCompositeDefinition",
    definition,
  });
}

/**
 * Delete a composite effect definition.
 */
export function deleteCompositeDefinition(id: string): void {
  window.NAMBridge.postMessage({
    type: "deleteCompositeDefinition",
    id,
  });
}

/**
 * Add a composite effect to the current preset's signal path.
 */
export function addCompositeToSignalPath(
  definitionId: string,
  afterNodeId?: string
): void {
  const def = getCompositeDefinition(definitionId);
  if (!def) {
    showNotification("Composite definition not found", "error");
    return;
  }

  const typeId = compositeEffectTypeId(definitionId);

  window.NAMBridge.postMessage({
    type: "addSignalPathNode",
    node: {
      id: `composite-${Date.now()}`,
      type: typeId,
      displayName: def.name,
      category: def.category,
      bypassed: false,
      params: Object.fromEntries(
        def.exposedParams.map((ep) => [ep.paramId, ep.defaultValue ?? 0])
      ),
      config: {},
    },
    afterNodeId: afterNodeId ?? null,
  });
}

/**
 * Request the engine to send the full composite library.
 */
export function requestCompositeLibrary(): void {
  window.NAMBridge.postMessage({
    type: "requestCompositeLibrary",
  });
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

/**
 * Build a list of available composite effect entries for the FX selector.
 * Returns entries compatible with the effect browser format.
 */
export function getCompositeEffectEntries(): Array<{
  type: string;
  displayName: string;
  category: string;
  description: string;
  isComposite: true;
}> {
  return getCompositeLibrary().map((def) => ({
    type: compositeEffectTypeId(def.id),
    displayName: def.name,
    category: def.category,
    description: def.description ?? "",
    isComposite: true as const,
  }));
}

/**
 * Register a single composite definition with the EffectTypeRegistry
 * so the signal path renderer can resolve its display name/category.
 */
function registerCompositeEffectType(def: CompositeEffectDefinition): void {
  const typeId = compositeEffectTypeId(def.id);
  EffectTypeRegistry.register(typeId, {
    type: typeId,
    displayName: def.name,
    category: def.category,
    requiresResource: false,
    catalogHidden: true,
    parameters: def.exposedParams.map((ep) => ({
      key: ep.paramId,
      name: ep.displayName,
      default: ep.defaultValue ?? 0,
      min: ep.minValue ?? 0,
      max: ep.maxValue ?? 1,
      unit: ep.unit ?? "",
    })),
    exposedResources: (def.exposedResources ?? []).map((er) => ({
      resourceId: er.resourceId,
      displayName: er.displayName,
      nodeId: er.nodeId,
      resourceType: er.resourceType,
      resourceIndex: er.resourceIndex ?? 0,
      allowBrowseFile: er.allowBrowseFile ?? true,
      parameterId: er.parameterId,
      parameterValue: er.parameterValue,
    })),
  });
}

/**
 * Register all composite definitions with the EffectTypeRegistry.
 */
function registerCompositeEffectTypes(
  defs: CompositeEffectDefinition[]
): void {
  for (const def of defs) {
    registerCompositeEffectType(def);
  }
}

// Re-export type utilities
export {
  compositeEffectTypeId,
  isCompositeEffectType,
  compositeDefinitionId,
  createEmptyCompositeDefinition,
  createCompositeFromNodes,
};
export type { CompositeEffectDefinition, ExposedParameter };
