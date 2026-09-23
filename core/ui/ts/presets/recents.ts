/**
 * The per-user marks on a preset that the backend owns — favourites, ratings and
 * the recently-played list — applied back into UI state.
 *
 * The engine records the recently-played list itself when a preset loads (a new id goes
 * first, one already listed keeps its place, four at most) and keeps it in the UI settings,
 * where this UI used to; "presetRecents" reports each change.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { presetSearchElement } from "../presets/dom.js";
import { normalizeRecentPresetIds, setFavoriteToggleState } from "../presets/favorites.js";
import { PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { uiState } from "../state.js";
import { adoptEngineUiSettings, updateUiSettings } from "../windowSettings.js";
import { filterPresets } from "./filter.js";
import { renderPresetUI } from "./library.js";

export const PRESET_RECENTS_SETTING = "presets.recents";

export function applyPresetFavoritesFromBackend(favorites: string[]): void {
  uiState.presetFavorites = new Set(favorites);
  setFavoriteToggleState(uiState.activePresetId);
}

export function applyPresetRecentsFromAppSettings(): void {
  const uiRecents = normalizeRecentPresetIds(uiState.uiSettings?.presetRecents);
  const legacyRecents = normalizeRecentPresetIds(uiState.appSettings?.[PRESET_RECENTS_SETTING]);
  const normalized = uiRecents.length ? uiRecents : legacyRecents;
  uiState.uiSettings = {
    ...(uiState.uiSettings ?? { zoom: 1 }),
    presetRecents: normalized,
  };
  if (!uiRecents.length && legacyRecents.length) {
    updateUiSettings({ presetRecents: normalized });
    // The list now lives in UI settings; clear the legacy copy here and in the engine alike.
    updateAppSetting(PRESET_RECENTS_SETTING, null);
  }
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
    return;
  }
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

/**
 * The engine's recently-played list ("presetRecents", or the UI settings shared sync brings).
 * It goes into the UI settings blob too, since uiSettingsChanged sends that blob back whole.
 */
export function applyPresetRecentsFromBackend(presetIds: unknown): void {
  const recents = normalizeRecentPresetIds(presetIds);
  uiState.uiSettings = {
    ...(uiState.uiSettings ?? { zoom: 1 }),
    presetRecents: recents,
  };
  adoptEngineUiSettings({ presetRecents: recents });
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
  }
}

export function applyPresetRatingsFromBackend(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}
