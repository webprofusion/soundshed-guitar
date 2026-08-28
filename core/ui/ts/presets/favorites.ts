import { clonePreset, uiState } from "../state.js";
import type { Preset } from "../types.js";
import { postMessage } from "../bridge.js";
import { syncToneSharingFavoriteForPreset, syncToneSharingRatingForPreset } from "../toneSharingPanel.js";
import { updateUiSettings } from "../windowSettings.js";
import { PRESET_FOLDER_FAVORITES_ID } from "./sorting.js";
import { presetFavoriteToggle } from "./dom.js";
import { requestPresetLibraryRefresh } from "./refresh.js";
/** How many recently-loaded presets the Recents folder keeps. */
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

export function saveRecentPresetIds(ids: string[]): void {
  const normalized = normalizeRecentPresetIds(ids);
  const current = loadRecentPresetIds();
  const unchanged = normalized.length === current.length && normalized.every((id, index) => current[index] === id);
  if (unchanged) {
    return;
  }
  uiState.uiSettings = {
    ...(uiState.uiSettings ?? { zoom: 1 }),
    presetRecents: normalized,
  };
  updateUiSettings({ presetRecents: normalized });
}

export function getRecentPresets(): Preset[] {
  return loadRecentPresetIds()
    .map((presetId) => uiState.presetCache.get(presetId) ?? uiState.presets.find((preset) => preset.id === presetId) ?? null)
    .filter((preset): preset is Preset => Boolean(preset))
    .map((preset) => clonePreset(preset));
}

export function trackRecentPreset(presetId: string | null | undefined): void {
  const id = typeof presetId === "string" ? presetId.trim() : "";
  if (!id) {
    return;
  }
  const current = loadRecentPresetIds();
  if (current.includes(id)) {
    return;
  }
  saveRecentPresetIds([id, ...current]);
}

export function saveFavoritePresetIds(ids: Set<string>): void {
  uiState.presetFavorites = new Set(ids);
  postMessage({ type: "setPresetFavorites", favorites: Array.from(ids) });
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

export function toggleFavoritePreset(presetId: string): void {
  const favorites = loadFavoritePresetIds();
  if (favorites.has(presetId)) {
    favorites.delete(presetId);
  } else {
    favorites.add(presetId);
  }
  saveFavoritePresetIds(favorites);
  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  void syncToneSharingFavoriteForPreset(preset, favorites.has(presetId)).catch((error) => {
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

export function savePresetRatings(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  postMessage({ type: "setPresetRatings", ratings });
}

export function getPresetRating(presetId: string): number | null {
  const ratings = loadPresetRatings();
  const rating = ratings[presetId];
  return typeof rating === "number" && rating >= 1 && rating <= 5 ? rating : null;
}

export function setPresetRating(presetId: string, rating: number | null): void {
  const ratings = loadPresetRatings();
  if (rating === null) {
    delete ratings[presetId];
  } else {
    ratings[presetId] = rating;
  }
  savePresetRatings(ratings);
  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  void syncToneSharingRatingForPreset(preset, rating).catch((error) => {
    console.warn("Tone Sharing rating sync failed", error);
  });
  requestPresetLibraryRefresh(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}
