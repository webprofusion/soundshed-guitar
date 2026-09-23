/**
 * The filtered preset set — folder, tags and the search box — and the small
 * redraws that follow from it: the chooser label and the active preset.
 *
 * These lived in library.ts with the renderer. The modules that act on presets
 * (load, save, the popover, folders, setlists) refilter and redraw after they
 * change something, and library.ts imports several of them to bind its rows, so
 * reaching for these through library.ts tied them all into one import cycle.
 * The full redraw is requested through presets/refresh.ts instead.
 */

import { getRecentPresets, loadFavoritePresetIds } from "../presets/favorites.js";
import { collectPresetIds, findFolderById, isVirtualPresetFolderId } from "../presets/folders.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID, sortPresetsAlphabetically } from "../presets/sorting.js";
import { getActivePresetForRender, uiState } from "../state.js";
import { setFilteredPresets } from "../presetLibraryStore.js";
import type { Preset } from "../types.js";
import { renderMixerPanel } from "../views.js";
import { presetChooserLabel, presetSearchElement } from "./dom.js";
import { requestPresetUIRender } from "./refresh.js";

export const activeTagFilters = new Set<string>();

export function getFilteredPresets(query: string): Preset[] {
  const normalized = query.trim().toLowerCase();
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const preserveOrder = activeFolderId === PRESET_FOLDER_RECENTS_ID;
  let basePresets = uiState.presets.slice();

  if (activeFolderId === PRESET_FOLDER_FAVORITES_ID) {
    const favorites = loadFavoritePresetIds();
    basePresets = basePresets.filter((preset) => favorites.has(preset.id));
  }

  if (activeFolderId === PRESET_FOLDER_RECENTS_ID) {
    basePresets = getRecentPresets();
  }

  if (!isVirtualPresetFolderId(activeFolderId)) {
    const folder = findFolderById(uiState.presetFolders ?? [], activeFolderId);
    if (folder) {
      const allowedIds = collectPresetIds(folder);
      basePresets = basePresets.filter((preset) => allowedIds.has(preset.id));
    }
  }

  if (activeTagFilters.size > 0) {
    basePresets = basePresets.filter((preset) => {
      const presetTags = preset.tags ?? [];
      return Array.from(activeTagFilters).every((tag) => presetTags.includes(tag));
    });
  }

  if (!normalized) {
    return preserveOrder ? basePresets : sortPresetsAlphabetically(basePresets);
  }

  const filteredPresets = basePresets.filter((preset) => {
    const tokens = [preset.name, preset.category, preset.description];
    return tokens.some((token) => token && token.toLowerCase().includes(normalized));
  });

  return preserveOrder ? filteredPresets : sortPresetsAlphabetically(filteredPresets);
}

/** Re-applies the folder, tag and search filter to the list, without redrawing. */
export function refilterPresets(): void {
  setFilteredPresets(getFilteredPresets(presetSearchElement?.value ?? ""));
}

export function renderActivePreset(): void {
  const active = getActivePresetForRender();
  requestPresetUIRender(active);
  renderMixerPanel();
}

export function filterPresets(query: string): void {
  setFilteredPresets(getFilteredPresets(query));
  requestPresetUIRender(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function populatePresetDropdown(): void {
  updatePresetDropdownSelection();
}

export function updatePresetDropdownSelection(): void {
  if (!presetChooserLabel) return;
  const preset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  presetChooserLabel.textContent = preset?.name ?? "Select Preset";
}
