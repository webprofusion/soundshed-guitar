/**
 * The preset library as the UI holds it — the list, the full-preset cache, the
 * filtered view, which preset is active and which one is loading — and the
 * commands that change it.
 *
 * These five fields used to be assigned from thirteen modules: message
 * handlers, the save modal, the mixer tabs, the settings panel, Tone Sharing's
 * pack removal. Each open-coded its own version of "put this preset in the
 * list and the cache", and they did not agree — some replaced the list entry,
 * some appended, some also reset the filter. The commands below name those
 * operations, and `scripts/check-state-writes.js` keeps the writes here.
 *
 * Reads still go straight to `uiState`. This module imports nothing but the
 * state object, so any module can use it without an import cycle.
 */

import { uiState } from "./state.js";
import type { Preset } from "./types.js";

/**
 * The presets the engine's own list names ("presetList") or has just saved: user, factory
 * and factory-archive presets, which `loadPreset {presetId}` loads without a body. Anything
 * else in the library (a new unsaved preset, a shared or tone-sharing preset that arrived
 * as a body) still has to be loaded with its body. See "What the engine can load by id".
 */
const storedPresetIds = new Set<string>();

// ── The list ─────────────────────────────────────────────────────────────────

/** Replaces the library list, and shows all of it until the next filter. */
export function setLibraryPresets(presets: Preset[]): void {
  uiState.presets = presets;
  uiState.filteredPresets = presets.slice();
}

/** Starts the library over from an index: the list, the unfiltered view, and a cache entry for each. */
export function resetLibrary(presets: readonly Preset[]): void {
  setLibraryPresets([...presets]);
  for (const preset of uiState.presets) {
    uiState.presetCache.set(preset.id, preset);
  }
}

/** Puts a preset at the top of the list, replacing any entry with its id. The filtered view is left alone. */
export function putLibraryPresetFirst(preset: Preset): void {
  uiState.presets = [preset, ...uiState.presets.filter((entry) => entry.id !== preset.id)];
}

/** Replaces the list entry with id `id`, if there is one. Returns whether it did. */
export function replaceLibraryPreset(preset: Preset, id: string = preset.id): boolean {
  const index = uiState.presets.findIndex((entry) => entry.id === id);
  if (index < 0) {
    return false;
  }
  uiState.presets[index] = preset;
  return true;
}

/** Replaces the list entry with the preset's id, or appends the preset when there is none. */
export function upsertLibraryPreset(preset: Preset): void {
  if (!replaceLibraryPreset(preset)) {
    uiState.presets.push(preset);
  }
}

/** Appends the preset unless the list already has an entry with its id, which is kept as it is. */
export function addLibraryPresetIfMissing(preset: Preset): void {
  if (!uiState.presets.some((entry) => entry.id === preset.id)) {
    uiState.presets.push(preset);
  }
}

/** Removes presets from the list, the filtered view and the cache. */
export function removeLibraryPresets(ids: Iterable<string>): void {
  const removed = new Set(ids);
  if (removed.size === 0) {
    return;
  }
  uiState.presets = uiState.presets.filter((preset) => !removed.has(preset.id));
  uiState.filteredPresets = uiState.filteredPresets.filter((preset) => !removed.has(preset.id));
  for (const id of removed) {
    uiState.presetCache.delete(id);
    storedPresetIds.delete(id);
  }
}

// ── What the engine can load by id ───────────────────────────────────────────

/** Replaces the set with the ids of the engine's preset list. */
export function setStoredPresetIds(ids: Iterable<string>): void {
  storedPresetIds.clear();
  for (const id of ids) {
    storedPresetIds.add(id);
  }
}

/** The engine has saved this preset, so it can load it by id from now on. */
export function markPresetStored(presetId: string): void {
  storedPresetIds.add(presetId);
}

export function isStoredPreset(presetId: string): boolean {
  return storedPresetIds.has(presetId);
}

// ── The filtered view ────────────────────────────────────────────────────────

/** Sets what the library list shows — normally the result of presets/filter.ts's getFilteredPresets. */
export function setFilteredPresets(presets: Preset[]): void {
  uiState.filteredPresets = presets;
}

/** Shows the whole list again, unfiltered. */
export function showAllLibraryPresets(): void {
  uiState.filteredPresets = uiState.presets.slice();
}

// ── The cache ────────────────────────────────────────────────────────────────

/**
 * Stores the full preset under `id` (its own id unless given). The list holds
 * summaries as often as full presets; this is where the full one lives.
 */
export function cachePreset(preset: Preset, id: string = preset.id): void {
  uiState.presetCache.set(id, preset);
}

// ── Active and loading ───────────────────────────────────────────────────────

/**
 * Which preset the library treats as current. state.ts's setFocusedMixerPresetId
 * also sets it, because focusing a mixer tab makes that preset current.
 */
export function setActivePresetId(presetId: string | null): void {
  uiState.activePresetId = presetId;
}

/**
 * Which scene of the active preset is selected. state.ts's setActivePresetDraft
 * also sets it, when it normalises a new draft's scenes.
 */
export function setActivePresetSceneId(sceneId: string | null): void {
  uiState.activePresetSceneId = sceneId;
}

/**
 * The preset the engine is loading, or null. Set it before rendering so the
 * list, details and signal path bar all draw the loading state; the engine's
 * "presetLoaded" (or a load error) clears it.
 */
export function setPresetLoadingId(presetId: string | null): void {
  uiState.presetLoadingId = presetId;
}
