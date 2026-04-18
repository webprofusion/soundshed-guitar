import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage } from "./bridge.js";
import { uiState } from "./state.js";
import type { CustomEffectLibrary, CustomEffectLibraryEntry } from "./types.js";

export interface SaveCurrentCustomEffectRequest {
  id?: string;
  name: string;
  category?: string;
  description?: string;
  origin?: string;
}

export interface ImportGeneratedCustomEffectRequest {
  id?: string;
  name: string;
  category?: string;
  description?: string;
  origin?: string;
  latestRevisionId?: string;
  thumbnailDataUrl?: string;
  tags?: string[];
  defaultParams?: Record<string, number>;
  descriptorSummary?: Record<string, unknown>;
  moduleData: string;
  moduleFileName?: string;
  moduleResourceId?: string;
  moduleSubfolder?: string;
  moduleMetadata?: Record<string, string | number | boolean>;
  descriptorText?: string;
  specText?: string;
  manifest?: Record<string, unknown>;
  sessionId?: string;
}

export function getCustomEffectLibrary(): CustomEffectLibrary {
  return uiState.customEffectLibrary ?? [];
}

export function getCustomEffectEntry(id: string): CustomEffectLibraryEntry | undefined {
  return getCustomEffectLibrary().find((entry) => entry.id === id);
}

export function handleCustomEffectLibrary(entries: CustomEffectLibrary): void {
  uiState.customEffectLibrary = entries ?? [];
  appendLog(`Custom effect library updated: ${uiState.customEffectLibrary.length} entries`);
}

export function saveCustomEffectEntry(entry: CustomEffectLibraryEntry): void {
  if (!entry.id || !entry.name || !entry.baseEffectType || !entry.moduleResourceType || !entry.moduleResourceId) {
    showNotification("Custom Effect entry is missing required fields", "error");
    return;
  }

  const now = new Date().toISOString();
  postMessage({
    type: "saveCustomEffectEntry",
    entry: {
      ...entry,
      createdAt: entry.createdAt ?? now,
      updatedAt: entry.updatedAt ?? now,
    },
  });
}

export function saveCurrentCustomEffect(nodeId: string, entry: SaveCurrentCustomEffectRequest, applyToNode: boolean): void {
  if (!nodeId) {
    showNotification("Custom Effect save failed", "Missing node id");
    return;
  }

  if (!entry.name.trim()) {
    showNotification("Custom Effect save failed", "A name is required");
    return;
  }

  postMessage({
    type: "saveCurrentCustomEffect",
    nodeId,
    applyToNode,
    entry: {
      ...entry,
      name: entry.name.trim(),
      category: entry.category?.trim() ?? "",
      description: entry.description ?? "",
    },
  });
}

export function importGeneratedCustomEffect(nodeId: string, entry: ImportGeneratedCustomEffectRequest, applyToNode: boolean): void {
  if (!nodeId) {
    showNotification("Generated Custom Effect import failed", "Missing node id");
    return;
  }

  if (!entry.name.trim()) {
    showNotification("Generated Custom Effect import failed", "A name is required");
    return;
  }

  if (!entry.moduleData.trim()) {
    showNotification("Generated Custom Effect import failed", "Generated module data is required");
    return;
  }

  postMessage({
    type: "importGeneratedCustomEffect",
    nodeId,
    applyToNode,
    sessionId: entry.sessionId ?? "",
    entry: {
      ...(entry.id ? { id: entry.id } : {}),
      name: entry.name.trim(),
      category: entry.category?.trim() ?? "",
      description: entry.description?.trim() ?? "",
      origin: entry.origin?.trim() ?? "generated",
      latestRevisionId: entry.latestRevisionId ?? "",
      thumbnailDataUrl: entry.thumbnailDataUrl ?? "",
      tags: entry.tags ?? [],
      defaultParams: entry.defaultParams ?? {},
      descriptorSummary: entry.descriptorSummary ?? {},
    },
    module: {
      data: entry.moduleData.trim(),
      fileName: entry.moduleFileName?.trim() ?? "",
      resourceId: entry.moduleResourceId?.trim() ?? "",
      subfolder: entry.moduleSubfolder?.trim() ?? "",
      metadata: entry.moduleMetadata ?? {},
      descriptorText: entry.descriptorText ?? "",
      specText: entry.specText ?? "",
      manifest: entry.manifest ?? {},
    },
  });
}

export function deleteCustomEffectEntry(id: string): void {
  if (!id) {
    return;
  }

  postMessage({
    type: "deleteCustomEffectEntry",
    id,
  });
}

export function requestCustomEffectLibrary(): void {
  postMessage({
    type: "requestCustomEffectLibrary",
  });
}