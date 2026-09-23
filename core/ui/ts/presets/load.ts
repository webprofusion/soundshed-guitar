/**
 * Loading a preset into the engine: fetching its metadata, resolving the
 * resources it attaches, and the index read at startup.
 */

import { postMessage } from "../bridge.js";
import { REMOTE_BASE_URL, getDefaultPresets } from "../dataLibraries.js";
import { showConfirm } from "../dialogs.js";
import { appendLog } from "../logging.js";
import { clearNotification, showNotification } from "../notifications.js";
import { setFavoriteToggleState } from "../presets/favorites.js";
import { requestPresetFromBackend } from "../presets/fetch.js";
import { stripLegacyGlobals } from "../presets/sanitize.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import { cachePreset, isStoredPreset, resetLibrary, setActivePresetId, setActivePresetSceneId, setPresetLoadingId } from "../presetLibraryStore.js";
import type { Attachment, GlobalSignalChainConfig, Preset } from "../types.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl } from "../utils.js";
import { recordPresetInHistory } from "./history.js";
import { updatePresetDropdownSelection } from "./filter.js";
import { requestPresetUIRender } from "./refresh.js";
import { updatePresetActionButtons } from "./toolbar.js";
import { hasGraphNodes } from "./validate.js";

export function bindLoadButtons(): void {
  const loadModelBtn = document.getElementById("load-model-btn");
  const loadIRBtn = document.getElementById("load-ir-btn");

  if (loadModelBtn) {
    loadModelBtn.addEventListener("click", () => {
      postMessage({ type: "browseModel" });
      appendLog("browseModel → requested");
    });
  }

  if (loadIRBtn) {
    loadIRBtn.addEventListener("click", () => {
      postMessage({ type: "browseIR" });
      appendLog("browseIR → requested");
    });
  }
}

export function loadModelFromPath(filePath: string): void {
  postMessage({
    type: "loadModel",
    filePath,
  });
  appendLog(`loadModel → ${filePath}`);
}

export function loadIRFromPath(filePath: string): void {
  postMessage({
    type: "loadIR",
    filePath,
  });
  appendLog(`loadIR → ${filePath}`);
}

export function requestSignalPathTest(): void {
  clearNotification();
  postMessage({
    type: "runSignalPathTest",
    frequency: 440,
    duration: 1.0,
  });
}

export async function loadPresetMetadata(presetId: string): Promise<Preset> {
  if (uiState.presetCache.has(presetId)) {
    const cached = stripLegacyGlobals(clonePreset(uiState.presetCache.get(presetId) ?? null) as Preset);
    if (hasGraphNodes(cached)) {
      return cached;
    }
    const backendPreset = await requestPresetFromBackend(presetId);
    const resolved = stripLegacyGlobals(backendPreset);
    cachePreset(resolved);
    return clonePreset(resolved) as Preset;
  }

  const localPreset = uiState.presets.find((preset) => preset.id === presetId);
  if (localPreset) {
    const cleaned = stripLegacyGlobals(localPreset);
    cachePreset(cleaned, localPreset.id);
    if (!hasGraphNodes(cleaned)) {
      const backendPreset = await requestPresetFromBackend(presetId);
      const resolved = stripLegacyGlobals(backendPreset);
      cachePreset(resolved);
      return clonePreset(resolved) as Preset;
    }
    return clonePreset(cleaned) as Preset;
  }

  if (!REMOTE_BASE_URL) {
    throw new Error("Remote preset service is not configured.");
  }

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");
  const response = await fetch(`${baseUrl}/presets/${encodeURIComponent(presetId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch preset ${presetId}: ${response.status}`);
  }

  const data = await response.json();
  const preset = Array.isArray(data) ? data[0] : data;
  if (!preset) {
    throw new Error(`Preset ${presetId} not found`);
  }

  const cleaned = stripLegacyGlobals(preset as Preset);
  cachePreset(cleaned);
  return clonePreset(cleaned) as Preset;
}

export async function enrichAttachment(attachment: Attachment): Promise<Attachment> {
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment, REMOTE_BASE_URL);
  if (!url || !isRemoteUrl(url)) {
    return attachment;
  }

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch attachment from ${url}`);
  }

  const buffer = await response.arrayBuffer();
  return { ...attachment, data: arrayBufferToBase64(buffer) };
}

/**
 * Loads a preset the engine stores (user, factory or factory archive) by id alone: the engine
 * reads it and answers with "presetLoaded", which carries the preset for the cache, the draft
 * and the render. With a full copy cached it is drawn at once, loading, as a body load drew it;
 * with only the library's summary, the preset playing stays on screen, the new one shown
 * loading, until the engine's copy arrives.
 */
function loadStoredPreset(presetId: string): void {
  clearNotification();
  const cached = uiState.presetCache.get(presetId) ?? null;
  if (cached && hasGraphNodes(cached)) {
    const draft = stripLegacyGlobals(cached);
    setActivePresetSceneId(normalizePresetScenes(draft, uiState.activePresetSceneId ?? undefined));
    setActivePresetId(presetId);
    setActivePresetIsNew(false);
    setActivePresetSnapshot(draft);
    setActivePresetDraft(draft);
    setFavoriteToggleState(presetId);
    updatePresetDropdownSelection();
  }
  // Set loading state BEFORE rendering so all render functions (list, details,
  // signal path bar) see it and bake the loading class/overlay into their output.
  setPresetLoadingId(presetId);
  requestPresetUIRender(clonePreset(getActivePresetForRender()));
  updatePresetActionButtons();
  // The scene playing now is kept when the preset has one of the same id, as a body load
  // does; the engine falls back to the preset's first scene.
  const sceneId = uiState.activePresetSceneId;
  postMessage({ type: "loadPreset", presetId, ...(sceneId ? { sceneId } : {}) });
  recordPresetInHistory(presetId);
}

/**
 * Loads a preset chosen in the library. One the engine stores goes by id; one it does not (a
 * new unsaved preset, or a shared one that arrived as a body) is sent with its body.
 */
export async function applyPresetFromLibrary(presetId: string): Promise<void> {
  // Loading the preset being edited reloads its saved copy, so it discards the edits just as
  // surely as switching away does.
  if (uiState.presetDirty && uiState.activePresetId) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) {
      return;
    }
    setPresetDirty(false);
  }
  if (isStoredPreset(presetId)) {
    loadStoredPreset(presetId);
    return;
  }
  try {
    clearNotification();
    const preset = await loadPresetMetadata(presetId);
    const attachments = await Promise.all((preset.attachments ?? []).map(enrichAttachment));
    const presetWithGlobals = preset as Preset & { globalSignalChain?: GlobalSignalChainConfig };
    const hasGlobalChain = Boolean(presetWithGlobals.globalSignalChain);
    const resolvedChain = hasGlobalChain
      ? JSON.parse(JSON.stringify(presetWithGlobals.globalSignalChain)) as GlobalSignalChainConfig
      : null;
    const presetPayload: Preset = {
      ...stripLegacyGlobals(preset),
      attachments,
      ...(hasGlobalChain && resolvedChain ? { globalSignalChain: resolvedChain } : {}),
    };
    const sceneId = normalizePresetScenes(presetPayload, uiState.activePresetSceneId ?? undefined);
    setActivePresetSceneId(sceneId);

    if (hasGlobalChain && resolvedChain) {
      uiState.globalSignalChain = resolvedChain;
    }
    cachePreset(clonePreset(presetPayload));
    setActivePresetId(presetPayload.id);
    setActivePresetIsNew(false);
    setActivePresetSnapshot(presetPayload);
    setActivePresetDraft(presetPayload);
    setPresetDirty(false);
    setFavoriteToggleState(presetPayload.id);
    updatePresetDropdownSelection();
    // Set loading state BEFORE rendering so all render functions (list, details,
    // signal path bar) see it and bake the loading class/overlay into their output.
    setPresetLoadingId(presetPayload.id);
    requestPresetUIRender(clonePreset(presetPayload));
    updatePresetActionButtons();
    postMessage({
      type: "loadPreset",
      preset: presetPayload,
      ...(sceneId ? { sceneId } : {}),
    });
    recordPresetInHistory(presetPayload.id);
  } catch (error) {
    setPresetLoadingId(null);
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

export async function loadPresetIndex(): Promise<void> {
  try {
    if (!REMOTE_BASE_URL) {
      throw new Error("Remote preset service disabled");
    }

    const response = await fetch(`${REMOTE_BASE_URL.replace(/\/$/, "")}/presets`);
    if (!response.ok) {
      throw new Error(`Failed to fetch presets index: ${response.status}`);
    }

    const data = await response.json();
    const presets = Array.isArray(data) ? data : data.presets ?? [];
    const basePresets = presets.length ? presets : getDefaultPresets();
    resetLibrary(basePresets);
    requestPresetUIRender(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  } catch (error) {
    console.error("Failed to load preset index", error);
    const basePresets = getDefaultPresets();
    resetLibrary(basePresets);
    requestPresetUIRender(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }
}
