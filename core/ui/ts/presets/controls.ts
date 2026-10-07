/**
 * The preset toolbar wiring: prev/next, undo/redo, random, and the folder and
 * setlist buttons in the library popover.
 */

import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { exportSelectedPresetCollectionArchive } from "../presets/archive.js";
import { presetFavoriteToggle, presetSearchElement } from "../presets/dom.js";
import { toggleFavoritePreset } from "../presets/favorites.js";
import { PRESET_FOLDER_ALL_ID } from "./sorting.js";
import { isVirtualPresetFolderId } from "../presets/folders.js";
import { uiState } from "../state.js";
import { nextPresetBtn, presetExportFolderButton, presetExtraActionsBtn, presetExtraActionsMenu, presetFolderAddButton, presetFolderDeleteButton, presetFolderNameInput, presetFolderRenameButton, presetLibraryCloseButton, presetLibraryPopover, presetRedoBtn, presetSelector, presetUndoBtn, prevPresetBtn, randomPresetBtn, setlistAddButton, setlistBankInput, setlistCollapsible, setlistNameInput, setlistToggle } from "./dom.js";
import { initializePresetDrag } from "./drag.js";
import { createFolder, deleteFolderById, getCurrentRealPresetFolder, renameFolder, syncPresetFolderToolbarState } from "./folderControls.js";
import { stepPresetHistory } from "./history.js";
import { filterPresets, updatePresetDropdownSelection } from "./filter.js";
import { applyPresetFromLibrary } from "./load.js";
import { closePresetExtraActionsMenu, closePresetLibraryPopover, syncPresetLibraryFeatureVisibility, togglePresetLibraryPopover } from "./popover.js";
import { createSetlist, renderSetlistPanel, setSetlistExpanded } from "./setlists.js";

export function getActivePresetIndex(): number {
  if (!uiState.activePresetId) return -1;
  return uiState.filteredPresets.findIndex((p) => p.id === uiState.activePresetId);
}

export async function selectPreviousPreset(): Promise<void> {
  if (!uiState.filteredPresets.length) return;

  let index = getActivePresetIndex();
  if (index <= 0) {
    index = uiState.filteredPresets.length - 1;
  } else {
    index--;
  }

  const preset = uiState.filteredPresets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

export async function selectNextPreset(): Promise<void> {
  if (!uiState.filteredPresets.length) return;

  let index = getActivePresetIndex();
  if (index < 0 || index >= uiState.filteredPresets.length - 1) {
    index = 0;
  } else {
    index++;
  }

  const preset = uiState.filteredPresets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

export function initializePresetControls(): void {
  syncPresetLibraryFeatureVisibility();

  if (presetSelector) {
    presetSelector.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePresetLibraryPopover();
    });
  }

  if (presetFavoriteToggle) {
    presetFavoriteToggle.addEventListener("click", (event) => {
      event.stopPropagation();
      const presetId = uiState.activePresetId;
      if (!presetId) {
        showNotification("No preset", "Select a preset to favourite");
        return;
      }
      toggleFavoritePreset(presetId);
    });
  }

  if (presetLibraryPopover) {
    presetLibraryPopover.addEventListener("click", (event) => {
      event.stopPropagation();
    });
  }

  if (presetLibraryCloseButton) {
    presetLibraryCloseButton.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetLibraryPopover();
    });
  }

  document.addEventListener("click", (event) => {
    const targetNode = event.target as Node | null;
    const targetElement = event.target instanceof Element ? event.target : null;
    if (!targetNode) {
      return;
    }

    const insidePresetSelector = Boolean(presetSelector?.contains(targetNode));
    const insidePresetPopover = Boolean(presetLibraryPopover?.contains(targetNode));
    const insideExtraActions = Boolean(
      presetExtraActionsBtn?.contains(targetNode) || presetExtraActionsMenu?.contains(targetNode),
    );
    const insideDialog = Boolean(targetElement?.closest("#dialog-modal"));

    if (insidePresetSelector || insidePresetPopover || insideExtraActions || insideDialog) {
      return;
    }

    closePresetLibraryPopover();
    closePresetExtraActionsMenu();
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closePresetLibraryPopover();
    }
  });

  if (prevPresetBtn) {
    prevPresetBtn.addEventListener("click", async () => {
      await selectPreviousPreset();
    });
  }

  if (nextPresetBtn) {
    nextPresetBtn.addEventListener("click", async () => {
      await selectNextPreset();
    });
  }

  presetUndoBtn?.addEventListener("click", () => {
    void stepPresetHistory(-1, applyPresetFromLibrary);
  });

  presetRedoBtn?.addEventListener("click", () => {
    void stepPresetHistory(1, applyPresetFromLibrary);
  });

  if (randomPresetBtn) {
    randomPresetBtn.addEventListener("click", async () => {
      if (!uiState.presets.length) {
        showNotification("No presets", "Preset library is empty");
        return;
      }
      const list = uiState.filteredPresets.length ? uiState.filteredPresets : uiState.presets;
      let candidates = list;
      if (uiState.activePresetId && list.length > 1) {
        candidates = list.filter((preset) => preset.id !== uiState.activePresetId);
      }
      const randomIndex = Math.floor(Math.random() * candidates.length);
      const preset = candidates[randomIndex];
      if (preset) {
        await applyPresetFromLibrary(preset.id);
      }
    });
  }

  if (presetSearchElement) {
    presetSearchElement.addEventListener("input", (event) => {
      filterPresets((event.target as HTMLInputElement).value ?? "");
    });
  }

  if (setlistToggle) {
    setlistToggle.addEventListener("click", () => {
      const expanded = setlistCollapsible?.classList.contains("open") ?? false;
      setSetlistExpanded(!expanded);
    });
  }

  if (setlistAddButton) {
    setlistAddButton.addEventListener("click", () => {
      const name = setlistNameInput?.value ?? "";
      const bankValue = setlistBankInput?.value ?? "";
      const bank = bankValue === "" ? null : Number(bankValue);
      if (bankValue !== "" && (!Number.isFinite(bank) || bank! < 0)) {
        showNotification("Invalid bank", "Bank must be a non-negative number.");
        return;
      }
      createSetlist(name, bank);
      if (setlistNameInput) {
        setlistNameInput.value = "";
      }
      if (setlistBankInput) {
        setlistBankInput.value = "";
      }
      renderSetlistPanel();
    });
  }

  // Presets dragged onto folders, the setlist panel and setlist pads.
  initializePresetDrag();

  if (presetFolderAddButton) {
    presetFolderAddButton.addEventListener("click", () => {
      const name = presetFolderNameInput?.value ?? "";
      if (!name.trim()) {
        showNotification("Folder name required", "Enter a folder name to create.");
        return;
      }
      const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
      const parentId = isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
      const created = createFolder(name, parentId);
      if (created && presetFolderNameInput) {
        presetFolderNameInput.value = "";
      }
    });
  }

  if (presetFolderRenameButton) {
    presetFolderRenameButton.addEventListener("click", async () => {
      const activeFolder = getCurrentRealPresetFolder();
      if (!activeFolder) {
        showNotification("Select a folder", "Choose a real folder to rename.");
        return;
      }

      const trimmed = (presetFolderNameInput?.value ?? "").trim();
      if (!trimmed) {
        showNotification("Folder name required", "Enter a folder name to rename.");
        return;
      }

      const confirmed = await showConfirm(`Rename folder "${activeFolder.name}" to "${trimmed}"?`, "Rename folder");
      if (!confirmed) {
        return;
      }

      const renamed = renameFolder(activeFolder.id, trimmed);
      if (renamed && presetFolderNameInput) {
        presetFolderNameInput.value = "";
      }
    });
  }

  if (presetFolderDeleteButton) {
    presetFolderDeleteButton.addEventListener("click", async () => {
      const activeFolder = getCurrentRealPresetFolder();
      if (!activeFolder) {
        showNotification("Select a folder", "Choose a real folder to delete.");
        return;
      }

      const confirmed = await showConfirm(`Delete folder "${activeFolder.name}" and all subfolders?`, "Delete folder");
      if (!confirmed) {
        return;
      }

      deleteFolderById(activeFolder.id);
    });
  }

  if (presetExportFolderButton) {
    presetExportFolderButton.addEventListener("click", () => void exportSelectedPresetCollectionArchive());
  }

  syncPresetFolderToolbarState();
}
