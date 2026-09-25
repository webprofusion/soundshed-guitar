/**
 * Everything the host says about presets: loads, saves, the library index,
 * folders, favourites, ratings, recents, unsaved changes, setlists and archive sessions.
 */

import { syncControlsFromState } from "../controls.js";
import { appendLog } from "../logging.js";
import { reconcileActiveCompositePreset } from "../multiPresetMixer.js";
import { showNotification } from "../notifications.js";
import { refreshPerformancePads } from "../performancePads.js";
import { adoptCreatedPreset, applyPresetArchiveSessionState, applyPresetFavoritesFromBackend, applyPresetFoldersFromBackend, applyPresetRatingsFromBackend, applyPresetRecentsFromBackend, applySetlistCursorFromBackend, applySetlistsFromBackend, cachePresetInMemory, handlePresetDataMessage, populatePresetDropdown, refilterPresets, refreshPresetCacheEntryFromBackend, renderActivePreset, setFavoriteToggleState, updatePresetActionButtons, updatePresetDropdownSelection } from "../presets.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { migratePresetNodeTypes } from "../presetV2.js";
import { refreshEffectPresetsFlyout } from "../signalPath.js";
import { applyEnginePresetDirty, clonePreset, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import { setMixerSlots } from "../mixerStore.js";
import { cachePreset, markPresetStored, putLibraryPresetFirst, setActivePresetId, setActivePresetSceneId, setLibraryPresets, setPresetLoadingId, setStoredPresetIds, showAllLibraryPresets } from "../presetLibraryStore.js";
import type { Preset, PresetFolder, Setlist, StoredEffectPreset } from "../types.js";
import { markIgnoreNextStatePreset } from "./echoGuard.js";
import { normalizePresetResources } from "./normalize.js";
import { takePendingSharedPresetHydration } from "./sharedSync.js";
import type { IncomingPayload } from "./types.js";

/**
 * The engine has a (possibly) different active preset: a load, a scene switch or a scene edit
 * (the engine owns those and answers each with this), a new preset (`created`), a setlist
 * step. Its copy replaces the UI's draft.
 */
export function onPresetLoaded(payload: IncomingPayload): void {
  const loaded = payload as { preset?: Preset; sceneId?: string; created?: boolean; activePresetDirty?: boolean };
  const preset = loaded.preset;
  if (preset) {
    // Clear loading state before re-rendering — the re-render below removes
    // all loading classes and overlays baked into the DOM by the render functions.
    setPresetLoadingId(null);
    migratePresetNodeTypes(preset);
    normalizePresetResources(preset);
    const presetChanged = uiState.activePresetId !== preset.id;
    const created = loaded.created === true;
    const preserveNewDraft = created || Boolean(uiState.activePresetIsNew && !presetChanged);
    setActivePresetSceneId(normalizePresetScenes(preset, loaded.sceneId ?? uiState.activePresetSceneId ?? undefined));
    setActivePresetId(preset.id);
    setActivePresetIsNew(preserveNewDraft);
    cachePreset(clonePreset(preset));
    setActivePresetSnapshot(preset);
    setActivePresetDraft(preset);
    applyEnginePresetDirty(loaded.activePresetDirty, presetChanged);
    setFavoriteToggleState(preset.id);
    updatePresetDropdownSelection();
    if (created) {
      adoptCreatedPreset(preset);
    }
  }
  const activePresetIds = (payload as { activePresetIds?: string[] }).activePresetIds;
  if (Array.isArray(activePresetIds)) {
    setMixerSlots(activePresetIds);
    reconcileActiveCompositePreset(); // a setlist step or program change reports the new mixer here, not via "state"
  }
  const parameters = (payload as { parameters?: Record<string, unknown> }).parameters;
  if (parameters) {
    uiState.parameters = {
      values: Array.isArray((parameters as { parameters?: unknown }).parameters)
        ? ((parameters as { parameters: [] }).parameters as [])
        : uiState.parameters.values,
    };
  }
  renderActivePreset();
  refreshPerformancePads();
  syncControlsFromState();
  updatePresetActionButtons();
}

/**
 * The engine's unsaved-changes flag moved. It compares its working copy with the preset as
 * loaded or saved, so it is the word on the matter: an edit put back clears it here too.
 */
export function onPresetDirtyChanged(payload: IncomingPayload): void {
  setPresetDirty((payload as { dirty?: boolean }).dirty === true);
}

/** The engine's recently-played list changed, or was asked for ("getPresetRecents"). */
export function onPresetRecents(payload: IncomingPayload): void {
  applyPresetRecentsFromBackend((payload as { presetIds?: unknown }).presetIds);
}

export function onPresetExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Preset exported", info.path ?? "");
}

export function onPresetExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Preset export failed", info.message ?? "");
}

export function onPresetSaved(payload: IncomingPayload): void {
  const savedPreset = (payload as { preset?: Preset }).preset;
  appendLog(
    `preset saved ← ${savedPreset?.name ?? "unknown"} `
    + `(graphNodes=${savedPreset?.graph?.nodes?.length ?? 0}, scenes=${savedPreset?.scenes?.length ?? 0})`,
  );
  if (savedPreset) {
    normalizePresetResources(savedPreset);
    markPresetStored(savedPreset.id);
    setActivePresetSceneId(normalizePresetScenes(savedPreset, (payload as { sceneId?: string }).sceneId ?? uiState.activePresetSceneId ?? undefined));
    cachePresetInMemory(savedPreset);
    setActivePresetId(savedPreset.id);
    setActivePresetIsNew(false);
    cachePreset(clonePreset(savedPreset));
    setActivePresetSnapshot(savedPreset);
    setActivePresetDraft(savedPreset);
    setPresetDirty(false);
    if (!uiState.presets.some((p) => p.id === savedPreset.id)) {
      putLibraryPresetFirst(clonePreset(savedPreset));
      showAllLibraryPresets();
      populatePresetDropdown();
    }
    renderActivePreset();
    updatePresetDropdownSelection();
    markIgnoreNextStatePreset(savedPreset.id);
  }
  showNotification("Preset saved", (payload as { path?: string }).path ?? savedPreset?.name ?? "");
}

export function onPresetArchiveSessionStarted(payload: IncomingPayload): void {
  const sessionPayload = payload as { active?: boolean; archiveName?: string; archiveKey?: string; presetCount?: number };
  applyPresetArchiveSessionState({
    active: Boolean(sessionPayload.active),
    archiveName: sessionPayload.archiveName,
    archiveKey: sessionPayload.archiveKey,
    presetCount: sessionPayload.presetCount,
  });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session started", sessionPayload.archiveName ?? "");
}

export function onPresetArchiveSessionEnded(): void {
  applyPresetArchiveSessionState({ active: false });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session ended");
}

export function onPresetArchiveSessionFailed(payload: IncomingPayload): void {
  showNotification("Preset archive session failed", (payload as { message?: string }).message ?? "");
}

export function onPresetList(payload: IncomingPayload): void {
  const presetListPayload = payload as { presets?: Array<{ id: string; name: string; category?: string; source?: string }> };
  if (Array.isArray(presetListPayload.presets)) {
    appendLog(`preset list received ← ${presetListPayload.presets.length} presets`);
    const cachedBeforeUpdate = new Set(uiState.presetCache.keys());
    const nextPresets: Preset[] = [];
    for (const p of presetListPayload.presets) {
      const incomingCategory = p.category ?? "Factory";
      const existingCached = uiState.presetCache.get(p.id);
      const nextPreset: Preset = existingCached
        ? {
            ...existingCached,
            name: p.name,
            category: incomingCategory,
          }
        : { id: p.id, name: p.name, category: incomingCategory } as Preset;
      cachePreset(nextPreset, p.id);
      nextPresets.push(nextPreset);
    }
    setLibraryPresets(nextPresets);
    setStoredPresetIds(nextPresets.map((entry) => entry.id));
    // A new preset stays this UI's own, and listed, until it is saved.
    const unsaved = uiState.activePresetIsNew && uiState.activePresetId ? uiState.presetCache.get(uiState.activePresetId) : undefined;
    if (unsaved && !nextPresets.some((entry) => entry.id === unsaved.id)) {
      putLibraryPresetFirst(unsaved);
    }
    // The list also arrives unasked (after a delete, or another instance's change), so keep
    // the folder, tags and search the library is showing rather than listing everything.
    refilterPresets();
    populatePresetDropdown();
    renderActivePreset();

    if (takePendingSharedPresetHydration()) {
      presetListPayload.presets.forEach((presetSummary) => {
        if (cachedBeforeUpdate.has(presetSummary.id)) {
          refreshPresetCacheEntryFromBackend(presetSummary.id);
        }
      });
    }
  }
}

export function onPresetData(payload: IncomingPayload): void {
  const presetPayload = payload as { preset?: Preset };
  if (presetPayload.preset) {
    migratePresetNodeTypes(presetPayload.preset);
    normalizePresetResources(presetPayload.preset);
    normalizePresetScenes(presetPayload.preset);
    handlePresetDataMessage(presetPayload.preset, (payload as { requestId?: string }).requestId);
  }
}

export function onPresetFolders(payload: IncomingPayload): void {
  const foldersPayload = payload as { folders?: PresetFolder[]; activeFolderId?: string | null };
  applyPresetFoldersFromBackend(foldersPayload.folders ?? [], foldersPayload.activeFolderId ?? null);
}

export function onPresetFavorites(payload: IncomingPayload): void {
  const favoritesPayload = payload as { favorites?: string[] };
  applyPresetFavoritesFromBackend(Array.isArray(favoritesPayload.favorites) ? favoritesPayload.favorites : []);
}

export function onPresetRatings(payload: IncomingPayload): void {
  const ratingsPayload = payload as { ratings?: Record<string, number> };
  applyPresetRatingsFromBackend(ratingsPayload.ratings ?? {});
}

export function onSetlists(payload: IncomingPayload): void {
  const setlistsPayload = payload as { setlists?: Setlist[]; activeSetlistId?: string | null };
  applySetlistsFromBackend(setlistsPayload.setlists ?? [], setlistsPayload.activeSetlistId ?? null);
  refreshPerformancePads();
}

export function onEffectPresets(payload: IncomingPayload): void {
  const effectPresetsPayload = payload as { byEffectType?: Record<string, StoredEffectPreset[]> };
  uiState.effectPresets = effectPresetsPayload.byEffectType ?? {};
  // The backend re-broadcasts after each save/delete. Only the presets flyout shows
  // the list; re-rendering the params panel too would rebuild the button it hangs from.
  refreshEffectPresetsFlyout();
}

export function onSetlistCursorChanged(payload: IncomingPayload): void {
  const cursorPayload = payload as { cursorIndex?: number; presetId?: string; activeSetlistId?: string };
  if (typeof cursorPayload.cursorIndex === "number") {
    applySetlistCursorFromBackend(cursorPayload.cursorIndex, cursorPayload.presetId, cursorPayload.activeSetlistId);
    refreshPerformancePads();
  }
}
