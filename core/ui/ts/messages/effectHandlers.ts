/**
 * The effect catalog, custom effects, blends and composites.
 *
 * The catalog message is the one that tells the UI what the engine can actually
 * build, so it also decides what the add-node menu offers.
 */

import { handleCompositeEditModeExited, handleCompositeEditStateUpdate, renderCompositeList } from "../compositeEditor.js";
import { handleCompositeDefinitionAdded, handleCompositeDefinitionRemoved, handleCompositeLibrary } from "../compositeEffects.js";
import type { CompositeEffectDefinition } from "../compositeTypes.js";
import { handleCustomEffectLibrary } from "../customEffects.js";
import { EffectGuids } from "../effectGuids.js";
import { applyEffectResponse, applySimpleCabIrMatch } from "../effectResponse.js";
import { applyIrAlignment } from "../irAlignment.js";
import { resolveExposedResourceSlot } from "../exposedResourceSlots.js";
import { refreshFxSelector } from "../fxSelector.js";
import { appendLog } from "../logging.js";
import { handleCompositePresetList, handleCompositePresetLoaded, handleCompositePresetSaved } from "../multiPresetMixer.js";
import { showNotification } from "../notifications.js";
import { parseParamTaper } from "../paramTaper.js";
import { renderActivePreset } from "../presets.js";
import { EffectTypeRegistry } from "../presetV2.js";
import { refreshSelectedNodeParams, renderSignalPathBar } from "../signalPath.js";
import { enterCompositeEditState, exitCompositeEditState, getActivePresetForRender, uiState, updateCompositeEditState } from "../state.js";
import type { CompositePreset, CustomEffectLibrary } from "../types.js";
import type { IncomingPayload } from "./types.js";

export function onCompositeLibrary(payload: IncomingPayload): void {
  const compPayload = payload as { definitions?: CompositeEffectDefinition[] };
  if (compPayload.definitions) {
    handleCompositeLibrary(compPayload.definitions);
    refreshFxSelector();
  }
}

export function onCustomEffectLibrary(payload: IncomingPayload): void {
  const customPayload = payload as { entries?: CustomEffectLibrary };
  if (Array.isArray(customPayload.entries)) {
    handleCustomEffectLibrary(customPayload.entries);
    refreshFxSelector();
  }
}

export function onCustomEffectSaved(payload: IncomingPayload): void {
  const customPayload = payload as { name?: string; applyToNode?: boolean };
  const detail = customPayload.name ?? "Custom Effect";
  appendLog(`custom effect saved ← ${detail}`);
  showNotification(customPayload.applyToNode ? "Custom Effect applied" : "Custom Effect saved", detail);
}

export function onGeneratedCustomEffectBundleExportSaved(payload: IncomingPayload): void {
  const exportPayload = payload as { path?: string };
  showNotification("Custom Effect bundle exported", exportPayload.path ?? "");
}

export function onGeneratedCustomEffectBundleExportFailed(payload: IncomingPayload): void {
  const exportPayload = payload as { message?: string };
  showNotification("Custom Effect bundle export failed", exportPayload.message ?? "");
}

/** An effect's response curve, answering getEffectResponse (see effectResponse.ts). */
export function onEffectResponse(payload: IncomingPayload): void {
  applyEffectResponse(payload);
}

/** The Simple Cabinet settings matched to a library IR, answering matchSimpleCabToIr. */
export function onSimpleCabIrMatch(payload: IncomingPayload): void {
  applySimpleCabIrMatch(payload);
}

/** Where the IR Cabinet's IR B lines up with IR A, answering analyzeIrAlignment. */
export function onIrAlignment(payload: IncomingPayload): void {
  applyIrAlignment(payload);
}

export function onEffectCatalog(payload: IncomingPayload): void {
  const catalogPayload = payload as { catalog?: Array<Record<string, unknown>> };
  if (Array.isArray(catalogPayload.catalog)) {
    for (const entry of catalogPayload.catalog) {
      if (!entry || typeof entry !== "object") {
        continue;
      }
      const effect = entry as {
        type?: unknown;
        name?: unknown;
        category?: unknown;
        parameters?: unknown;
        presets?: unknown;
        requiresResource?: unknown;
        resourceType?: unknown;
        exposedResources?: unknown;
      };
      const type = typeof effect.type === "string" ? effect.type : "";
      if (!type) {
        continue;
      }
      const existing = EffectTypeRegistry.get(type);
      const rawName = typeof effect.name === "string" ? effect.name.trim() : "";
      const rawCategory = typeof effect.category === "string" ? effect.category.trim() : "";
      const displayName = rawName || existing?.displayName || type;
      const category = rawCategory || existing?.category || "utility";
      const requiresResource =
        typeof effect.requiresResource === "boolean" ? effect.requiresResource : existing?.requiresResource ?? false;
      const resourceType = typeof effect.resourceType === "string" ? effect.resourceType : existing?.resourceType;
      const existingParamsByKey = new Map(
        (existing?.parameters ?? []).map((param) => [param.key, param]),
      );
      const parameters = Array.isArray(effect.parameters)
        ? effect.parameters
            .filter((param) => param && typeof param === "object")
            .map((param) => {
              const p = param as {
                key?: unknown;
                name?: unknown;
                default?: unknown;
                min?: unknown;
                max?: unknown;
                unit?: unknown;
                step?: unknown;
                labels?: unknown;
                group?: unknown;
                advanced?: unknown;
                taper?: unknown;
              };
              const key = typeof p.key === "string" ? p.key : "";
              const existingParam = key ? existingParamsByKey.get(key) : undefined;
              const labels = Array.isArray(p.labels) ? p.labels.filter((label) => typeof label === "string") : null;
              return {
                key,
                name: typeof p.name === "string" ? p.name : existingParam?.name ?? "",
                default: typeof p.default === "number" ? p.default : existingParam?.default ?? 0,
                min: typeof p.min === "number" ? p.min : existingParam?.min ?? 0,
                max: typeof p.max === "number" ? p.max : existingParam?.max ?? 1,
                unit: typeof p.unit === "string" ? p.unit : existingParam?.unit ?? "",
                step: typeof p.step === "number" ? p.step : existingParam?.step,
                labels: labels ?? existingParam?.labels,
                group: typeof p.group === "string" ? p.group : existingParam?.group,
                advanced: typeof p.advanced === "boolean" ? p.advanced : existingParam?.advanced,
                // Absent is linear. Not inherited from the entry being replaced: an engine that
                // leaves the field out has declared linear, unlike a missing name or range.
                taper: parseParamTaper(p.taper),
              };
            })
            .filter((param) => param.key !== "")
        : existing?.parameters ?? [];

      const exposedResources = Array.isArray(effect.exposedResources)
        ? effect.exposedResources
            .map((resource, declarationIndex) => {
              const r = (resource && typeof resource === "object" ? resource : {}) as {
                resourceId?: unknown;
                displayName?: unknown;
                nodeId?: unknown;
                resourceType?: unknown;
                resourceIndex?: unknown;
                allowBrowseFile?: unknown;
                parameterId?: unknown;
                parameterValue?: unknown;
              };
              return {
                resourceId: typeof r.resourceId === "string" ? r.resourceId : "",
                displayName: typeof r.displayName === "string" ? r.displayName : "",
                nodeId: typeof r.nodeId === "string" ? r.nodeId : "",
                resourceType: typeof r.resourceType === "string" ? r.resourceType : "",
                // For a composite the index sent here is the inner node's slot,
                // not the slot on the node the picker writes.
                resourceIndex: resolveExposedResourceSlot(
                  type,
                  typeof r.resourceIndex === "number" ? r.resourceIndex : undefined,
                  declarationIndex,
                ),
                allowBrowseFile: typeof r.allowBrowseFile === "boolean" ? r.allowBrowseFile : true,
                parameterId: typeof r.parameterId === "string" ? r.parameterId : undefined,
                parameterValue: typeof r.parameterValue === "number" ? r.parameterValue : undefined,
              };
            })
            .filter((resource) => resource.resourceId && resource.resourceType)
        : existing?.exposedResources;
      const presets = Array.isArray(effect.presets)
        ? effect.presets
            .filter((preset) => preset && typeof preset === "object")
            .flatMap((preset) => {
              const p = preset as {
                id?: unknown;
                name?: unknown;
                source?: unknown;
                parameters?: unknown;
                parameterOrder?: unknown;
              };
              if (typeof p.id !== "string" || !p.id || typeof p.name !== "string") {
                return [];
              }
              const parameters = p.parameters && typeof p.parameters === "object" && !Array.isArray(p.parameters)
                ? Object.fromEntries(
                    Object.entries(p.parameters)
                      .filter(([, value]) => typeof value === "number" && Number.isFinite(value)),
                  )
                : {};
              return [{
                id: p.id,
                name: p.name,
                source: p.source === "custom" ? "custom" as const : "factory" as const,
                parameters,
                parameterOrder: Array.isArray(p.parameterOrder)
                  ? p.parameterOrder.filter((key): key is string => typeof key === "string" && key in parameters)
                  : undefined,
              }];
            })
        : existing?.presets;

      EffectTypeRegistry.register(type, {
        type,
        displayName,
        category,
        catalogHidden: existing?.catalogHidden ?? type === EffectGuids.kAmpNamBlend,
        requiresResource,
        resourceType,
        parameters,
        presets,
        exposedResources,
      });
    }
    refreshFxSelector();
    if (getActivePresetForRender()) {
      renderActivePreset();
      refreshSelectedNodeParams();
    }
  }
}

export function onCompositeDefinitionAdded(payload: IncomingPayload): void {
  const compAddPayload = payload as { definition?: CompositeEffectDefinition };
  if (compAddPayload.definition) {
    handleCompositeDefinitionAdded(compAddPayload.definition);
    refreshFxSelector();
    renderCompositeList();
  }
}

export function onCompositeDefinitionRemoved(payload: IncomingPayload): void {
  const compRemovePayload = payload as { id?: string };
  if (compRemovePayload.id) {
    handleCompositeDefinitionRemoved(compRemovePayload.id);
    refreshFxSelector();
    renderCompositeList();
  }
}

export function onCompositeEditState(payload: IncomingPayload): void {
  // C++ broadcasts the composite's current inner graph after each mutation
  const editPayload = payload as { definition?: CompositeEffectDefinition };
  if (editPayload.definition) {
    const isAlreadyEditing = uiState.compositeEditMode;
    if (isAlreadyEditing) {
      updateCompositeEditState(editPayload.definition);
    } else {
      enterCompositeEditState(editPayload.definition);
    }
    renderSignalPathBar();
    handleCompositeEditStateUpdate();
  }
}

export function onCompositeEditModeExited(): void {
  // C++ confirms we've left composite edit mode
  exitCompositeEditState();
  handleCompositeEditModeExited();
  renderSignalPathBar();
}

export function onCompositePresetList(payload: IncomingPayload): void {
  const list = (payload as { compositePresets?: CompositePreset[] }).compositePresets;
  if (Array.isArray(list)) {
    handleCompositePresetList(list);
  }
}

export function onCompositePresetSaved(payload: IncomingPayload): void {
  const saved = payload as { id?: string; name?: string };
  handleCompositePresetSaved(saved.id ?? "", saved.name ?? "");
}

export function onCompositePresetLoaded(payload: IncomingPayload): void {
  const loaded = payload as { id?: string; name?: string };
  handleCompositePresetLoaded(loaded.id ?? "", loaded.name ?? "");
}
