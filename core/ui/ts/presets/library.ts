/**
 * The preset list as the user sees it: the library rows, the details pane and
 * the tag filter bar. The filtered set and the chooser label are in filter.ts.
 */

import { createDeferredView } from "../deferredView.js";
import { bindDemoAudioControls } from "../demoAudio.js";
import { switchMainPanel } from "../navigation.js";
import { STANDARD_TAGS, renderTagChips } from "../presetTags.js";
import { getPresetRating, getRecentPresets, loadFavoritePresetIds, setPresetRating } from "../presets/favorites.js";
import { getPresetFolderPath, sortPresetFoldersAlphabetically } from "../presets/folders.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID, sortPresetsAlphabetically } from "../presets/sorting.js";
import { renderSignalPathBar } from "../signalPath.js";
import { uiState } from "../state.js";
import type { Preset } from "../types.js";
import { renderPresetDetails, renderPresetList } from "../views.js";
import { activeTagFilters, filterPresets, refilterPresets } from "./filter.js";
import { movePresetFolder, setActivePresetFolder, syncPresetFolderToolbarState } from "./folderControls.js";
import { presetLibraryPopover } from "./dom.js";
import { syncPresetFolderPickerLabel } from "./folderPicker.js";
import { applyPresetFromLibrary, bindLoadButtons, requestSignalPathTest } from "./load.js";
import { closePresetLibraryPopover, takePresetChooserOverride } from "./popover.js";
import { setPresetUIRenderer } from "./refresh.js";
import { updatePresetFolderExportButtons } from "./toolbar.js";

/**
 * The library rows and folder tree live in the preset popover, so they are built only
 * while it is open, and when it opens (popover.ts calls flushShownViews).
 */
const presetRowsView = createDeferredView(
  () => Boolean(presetLibraryPopover?.classList.contains("open")),
  renderPresetRows,
);

export function renderPresetUI(preset: Preset | null): void {
  // Settings can arrive after the preset list, and saves can reset the filtered view.
  refilterPresets();
  syncPresetFolderToolbarState();
  presetRowsView.request();

  renderPresetDetails(preset, {
    onPresetSelected: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onApplyPreset: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onRequestSignalTest: requestSignalPathTest,
    onBindLoadButtons: bindLoadButtons,
  });
  bindDemoAudioControls();
  renderSignalPathBar();
  updatePresetFolderExportButtons();
}

function renderPresetRows(): void {
  const visiblePresets = uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID
    ? uiState.filteredPresets
    : sortPresetsAlphabetically(uiState.filteredPresets);

  renderPresetList(visiblePresets, uiState.activePresetId, async (presetId) => {
    const override = takePresetChooserOverride();
    if (override) {
      await override(presetId);
      closePresetLibraryPopover();
      return;
    }
    await applyPresetFromLibrary(presetId);
  }, {
    folders: sortPresetFoldersAlphabetically(uiState.presetFolders ?? []),
    activeFolderId: uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
    onSelectFolder: setActivePresetFolder,
    onMoveFolder: movePresetFolder,
    getRating: getPresetRating,
    onRate: setPresetRating,
    getFolderPath: getPresetFolderPath,
    recentsCount: getRecentPresets().length,
    recentsActive: uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID,
    onSelectRecents: () => setActivePresetFolder(PRESET_FOLDER_RECENTS_ID),
    favoritesCount: loadFavoritePresetIds().size,
    favoritesActive: uiState.activePresetFolderId === PRESET_FOLDER_FAVORITES_ID,
    onSelectFavorites: () => setActivePresetFolder(PRESET_FOLDER_FAVORITES_ID),
    hasAnyPresets: uiState.presets.length > 0,
    onOpenToneSharing: () => {
      closePresetLibraryPopover();
      switchMainPanel("sharing");
    },
  });
  syncPresetFolderPickerLabel();
}

export function initializePresetTagFilterBar(): void {
  const bar = document.getElementById("preset-tag-filter-bar");
  if (!bar) return;
  renderTagChips(bar, STANDARD_TAGS, "preset-tag-filter-chip");
  bar.querySelectorAll<HTMLButtonElement>(".preset-tag-filter-chip").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tag = btn.dataset.tag ?? "";
      if (!tag) return;
      if (activeTagFilters.has(tag)) {
        activeTagFilters.delete(tag);
        btn.classList.remove("active");
      } else {
        activeTagFilters.add(tag);
        btn.classList.add("active");
      }
      const searchInput = document.getElementById("preset-search") as HTMLInputElement | null;
      filterPresets(searchInput?.value ?? "");
    });
  });
}

// Supply the real redraw to the modules that can only request one — see
// presets/refresh.ts for why the indirection exists.
setPresetUIRenderer(renderPresetUI);
