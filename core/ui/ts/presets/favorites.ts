import { clonePreset, uiState } from "../state.js";
import type { Preset } from "../types.js";
import { sendPresetFavorite, sendPresetRating } from "../bridge.js";
import { syncToneSharingFavoriteForPreset, syncToneSharingRatingForPreset } from "../toneSharingPanel.js";
import { PRESET_FOLDER_FAVORITES_ID } from "./sorting.js";
import { presetFavoriteToggle } from "./dom.js";
import { requestPresetLibraryRefresh } from "./refresh.js";
/** How many recently-loaded presets the Recents folder keeps (the engine keeps the list). */
const MAX_RECENT_PRESETS = 4;

export function loadFavoritePresetIds(): Set<string> {
  return uiState.presetFavorites ? new Set(uiState.presetFavorites) : new Set();
}

export function normalizeRecentPresetIds(value: unknown): string[] {
  if (!Array.isArray(value)) {
    return [];
  }

  const ids: string[] = [];
  value.forEach((entry) => {
    if (typeof entry !== "string") {
      return;
    }
    const id = entry.trim();
    if (!id || ids.includes(id)) {
      return;
    }
    ids.push(id);
  });

  return ids.slice(0, MAX_RECENT_PRESETS);
}

export function loadRecentPresetIds(): string[] {
  return normalizeRecentPresetIds(uiState.uiSettings?.presetRecents);
}

export function getRecentPresets(): Preset[] {
  return loadRecentPresetIds()
    .map((presetId) => uiState.presetCache.get(presetId) ?? uiState.presets.find((preset) => preset.id === presetId) ?? null)
    .filter((preset): preset is Preset => Boolean(preset))
    .map((preset) => clonePreset(preset));
}

export function isPresetFavorite(presetId: string): boolean {
  return loadFavoritePresetIds().has(presetId);
}

export function setFavoriteToggleState(presetId: string | null): void {
  if (!presetFavoriteToggle) {
    return;
  }
  const active = presetId ? isPresetFavorite(presetId) : false;
  presetFavoriteToggle.classList.toggle("active", active);
  presetFavoriteToggle.setAttribute("aria-pressed", active ? "true" : "false");
}

/**
 * Marks or unmarks one preset as a favourite. The engine changes the one entry and answers
 * with the whole list ("presetFavorites"); the local copy changes now so the UI redraws at once.
 */
export function setPresetFavorite(presetId: string, favorite: boolean): void {
  const favorites = loadFavoritePresetIds();
  if (favorite) {
    favorites.add(presetId);
  } else {
    favorites.delete(presetId);
  }
  uiState.presetFavorites = favorites;
  sendPresetFavorite(presetId, favorite);
}

export function toggleFavoritePreset(presetId: string): void {
  const favorite = !isPresetFavorite(presetId);
  setPresetFavorite(presetId, favorite);
  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  void syncToneSharingFavoriteForPreset(preset, favorite).catch((error) => {
    console.warn("Tone Sharing favorite sync failed", error);
  });
  setFavoriteToggleState(presetId);
  if (uiState.activePresetFolderId === PRESET_FOLDER_FAVORITES_ID) {
    requestPresetLibraryRefresh();
  }
}

export function loadPresetRatings(): Record<string, number> {
  return uiState.presetRatings ? { ...uiState.presetRatings } : {};
}

export function getPresetRating(presetId: string): number | null {
  const ratings = loadPresetRatings();
  const rating = ratings[presetId];
  return typeof rating === "number" && rating >= 1 && rating <= 5 ? rating : null;
}

/**
 * Rates one preset, or clears its rating with null. The engine changes the one entry and
 * answers with the whole map ("presetRatings"); the local copy changes now.
 */
export function setPresetRating(presetId: string, rating: number | null): void {
  const ratings = loadPresetRatings();
  if (rating === null) {
    delete ratings[presetId];
  } else {
    ratings[presetId] = rating;
  }
  uiState.presetRatings = ratings;
  sendPresetRating(presetId, rating ?? 0);
  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  void syncToneSharingRatingForPreset(preset, rating).catch((error) => {
    console.warn("Tone Sharing rating sync failed", error);
  });
  requestPresetLibraryRefresh(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}
