/**
 * What can be done to the preset that is currently loaded — overwrite, edit,
 * delete, publish — and the toolbar state that says which of those are allowed.
 *
 * A factory preset cannot be modified in place, so most of these first check
 * whether the active preset is the user's own.
 */

import { postMessage } from "../bridge.js";
import { buildAttachmentsFromPreset } from "../dataLibraries.js";
import { showConfirm } from "../dialogs.js";
import { switchMainPanel } from "../navigation.js";
import { showNotification } from "../notifications.js";
import { exportCurrentPresetArchive, exportPresetArchiveSession, getToneSharingOriginMetadata, importPackWithConfirmation } from "../presets/archive.js";
import { cachePresetInMemory } from "../presets/cache.js";
import { presetSearchElement } from "../presets/dom.js";
import { isPresetFavorite, setPresetFavorite } from "../presets/favorites.js";
import { persistPresetFolders, removePresetFromFolders } from "../presets/folders.js";
import { requestPresetUIRender, setPresetLibraryRefresher } from "../presets/refresh.js";
import { clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import { cachePreset, removeLibraryPresets, replaceLibraryPreset, setActivePresetId, setFilteredPresets } from "../presetLibraryStore.js";
import { isToneSharingSignedIn, openToneSharingPublishPresetModal, openToneSharingSignInModal } from "../toneSharingPanel.js";
import type { Preset } from "../types.js";
import { populatePresetFolderSelect } from "./folderControls.js";
import { initPresetModalAdvancedActions, initPresetModalTabs, setPresetModalActiveTab, updatePresetModalJson, updatePresetModalReport } from "./inspectModal.js";
import { getFilteredPresets, populatePresetDropdown, renderActivePreset, updatePresetDropdownSelection } from "./filter.js";
import { applyPresetFromLibrary } from "./load.js";
import { closePresetExtraActionsMenu, togglePresetExtraActionsMenu } from "./popover.js";
import { configureSavePresetModalLabels, createDefaultPreset, populateSavePresetModalFields, resolvePresetModalFolderId, updateSavePresetModalPeakInfo } from "./saveModal.js";
import { canModifyPreset, isActivePresetNewDraft, updatePresetActionButtons } from "./toolbar.js";
import { stripGlobalSignalChainForSave } from "./validate.js";

export function openPublishPresetFlow(): void {
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  if (!activePreset) {
    showNotification("No preset", "Select a preset to publish.");
    return;
  }

  const toneSharingOrigin = getToneSharingOriginMetadata(activePreset);
  if (toneSharingOrigin?.republishBlocked) {
    showNotification("Save As first", "Imported Tone Sharing presets need a local copy before they can be published again.");
    return;
  }

  if (!isToneSharingSignedIn()) {
    switchMainPanel("sharing");
    openToneSharingSignInModal();
    return;
  }

  openToneSharingPublishPresetModal(activePreset.name ?? "", activePreset.description ?? "");
}

// Delete preset via backend storage
export function deletePresetFromBackend(presetId: string): boolean {
  if (!presetId) return false;
  postMessage({ type: "deletePreset", presetId });
  return true;
}

// Delete current preset
export async function deleteCurrentPreset(): Promise<void> {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot delete factory presets");
    return;
  }

  const preset = uiState.presetCache.get(activePresetId);
  const presetName = preset?.name ?? "Unknown";

  const confirmed = await showConfirm(`Are you sure you want to delete "${presetName}"?`, "Delete preset");
  if (!confirmed) {
    return;
  }

  if (deletePresetFromBackend(activePresetId)) {
    // Remove from UI state
    removeLibraryPresets([activePresetId]);
    removePresetFromFolders(uiState.presetFolders ?? [], activePresetId);
    persistPresetFolders();
    if (isPresetFavorite(activePresetId)) {
      setPresetFavorite(activePresetId, false);
    }
    setFilteredPresets(getFilteredPresets(presetSearchElement?.value ?? ""));

    // The deleted preset's unsaved changes went with it; loading the next one has nothing to confirm.
    setPresetDirty(false);

    // Select first preset if available
    if (uiState.presets.length > 0) {
      const nextPresetId = uiState.presets[0].id;
      setActivePresetId(nextPresetId);
      void applyPresetFromLibrary(nextPresetId);
    } else {
      setActivePresetId(null);
    }

    populatePresetDropdown();
    renderActivePreset();
    showNotification("Preset deleted", presetName);
  } else {
    showNotification("Error", "Failed to delete preset");
  }
}

// Save (overwrite) current preset
export function saveOverwriteCurrentPreset(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (isActivePresetNewDraft()) {
    openEditPresetModal();
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot overwrite factory presets. Use 'Save As' instead.");
    return;
  }

  const existingPreset = getActivePresetForRender();
  if (!existingPreset) {
    showNotification("Error", "Preset not found");
    return;
  }

  // Build updated preset with current parameters from graph nodes
  const baseAttachments = buildAttachmentsFromPreset(existingPreset);
  const includeGlobalFx = false;

  const updatedPreset: Preset = {
    ...existingPreset,
    attachments: baseAttachments,
  };
  delete (updatedPreset as Record<string, unknown>).globalSignalChain;

  cachePresetInMemory(updatedPreset);
  // Persist to disk via the C++ backend
  const savePayload: Record<string, unknown> = {
    type: "savePreset",
    saveMode: "overwrite",
    presetId: updatedPreset.id,
    name: updatedPreset.name,
    category: updatedPreset.category,
    description: updatedPreset.description,
    includeGlobalSignalChain: includeGlobalFx,
    preset: stripGlobalSignalChainForSave(updatedPreset),
  };
  postMessage(savePayload);

  // Update cache
  cachePreset(updatedPreset, activePresetId);
  replaceLibraryPreset(updatedPreset, activePresetId);

  setActivePresetIsNew(false);
  setActivePresetSnapshot(updatedPreset);
  setActivePresetDraft(updatedPreset);
  setPresetDirty(false);
  showNotification("Preset saved", existingPreset.name);
}

// Open edit preset modal (reuses save modal with pre-filled data)
export function openEditPresetModal(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot edit factory presets. Use 'Save As' instead.");
    return;
  }

  const preset = getActivePresetForRender();
  if (!preset) {
    showNotification("Error", "Preset not found");
    return;
  }

  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;
  const isNewPresetDraft = isActivePresetNewDraft();
  configureSavePresetModalLabels(isNewPresetDraft ? "save-new" : "overwrite");
  modal.dataset.saveMode = isNewPresetDraft ? "save-new" : "overwrite";
  delete modal.dataset.sourcePresetId;

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  populatePresetFolderSelect(folderSelect, resolvePresetModalFolderId(activePresetId));
  populateSavePresetModalFields(preset);

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  updatePresetModalJson(preset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;
  delete modal.dataset.stagedDesignedPeak;
  updateSavePresetModalPeakInfo(modal);

  // Store that we're editing, not creating
  modal.dataset.editingPresetId = activePresetId;

  modal.style.display = "flex";
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  nameInput?.focus();
  nameInput?.select();
}

// Supply the real redraw to the modules that can only request one — see
// presets/refresh.ts for why the indirection exists.
setPresetLibraryRefresher((activePreset) => {
  setFilteredPresets(getFilteredPresets(presetSearchElement?.value ?? ""));
  populatePresetDropdown();
  if (activePreset) {
    requestPresetUIRender(clonePreset(activePreset));
  }
  updatePresetDropdownSelection();
  updatePresetActionButtons();
});

document.addEventListener("presetDirtyChanged", () => {
  updatePresetActionButtons();
});

// Initialize preset action buttons
export function initializePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn");
  const newBtn = document.getElementById("preset-new-btn");
  const saveBtn = document.getElementById("preset-save-btn");
  const saveAsBtn = document.getElementById("preset-save-as-btn");
  const deleteBtn = document.getElementById("preset-delete-btn");
  const publishBtn = document.getElementById("preset-publish-btn");
  const extraActionsBtn = document.getElementById("preset-extra-actions-btn") as HTMLButtonElement | null;
  const extraActionsMenu = document.getElementById("preset-extra-actions-menu");
  const exportBtn = document.getElementById("preset-export-btn");
  const exportSessionBtn = document.getElementById("preset-export-session-btn");
  const exitSessionBtn = document.getElementById("preset-exit-session-btn");
  const importBtn = document.getElementById("preset-import-btn");
  const importInput = document.getElementById("preset-import-input") as HTMLInputElement | null;

  if (editBtn) {
    editBtn.addEventListener("click", openEditPresetModal);
  }

  if (newBtn) {
    newBtn.addEventListener("click", createDefaultPreset);
  }

  if (saveBtn) {
    saveBtn.addEventListener("click", saveOverwriteCurrentPreset);
  }

  if (saveAsBtn) {
    saveAsBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
    });
  }

  if (deleteBtn) {
    deleteBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      void deleteCurrentPreset();
    });
  }

  if (publishBtn) {
    publishBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      if ((publishBtn as HTMLButtonElement).disabled) {
        return;
      }
      openPublishPresetFlow();
    });
  }

  if (extraActionsBtn) {
    extraActionsBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePresetExtraActionsMenu();
    });
  }

  if (extraActionsMenu) {
    extraActionsMenu.addEventListener("click", (event) => {
      event.stopPropagation();
    });
  }

  if (exportBtn) {
    exportBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      if ((exportBtn as HTMLButtonElement).disabled) {
        return;
      }
      closePresetExtraActionsMenu();
      void exportCurrentPresetArchive();
    });
  }

  if (importBtn) {
    importBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      importInput?.click();
    });
  }

  if (exportSessionBtn) {
    exportSessionBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      void exportPresetArchiveSession();
    });
  }

  if (exitSessionBtn) {
    exitSessionBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      postMessage({ type: "endPresetArchiveSession" });
    });
  }

  if (importInput) {
    importInput.addEventListener("change", () => {
      const file = importInput.files?.[0];
      importInput.value = "";
      if (file) {
        void importPackWithConfirmation(file, { source: "zipImport" });
      }
    });
  }

  document.addEventListener("click", () => {
    closePresetExtraActionsMenu();
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closePresetExtraActionsMenu();
    }
  });

  updatePresetActionButtons();
}
