/**
 * What the loaded preset allows — overwrite, delete, publish — and the toolbar
 * that shows it: the action buttons, the chooser's status badge and the folder
 * export button.
 *
 * Nearly every preset module refreshes this after it changes something, so it
 * sits below all of them. It used to live in actions.ts, which also imports
 * load, save and the popover to run the actions — that pairing is what tied
 * eight preset modules into one import cycle.
 */

import { Features, isFeatureEnabled } from "../featureFlags.js";
import { getToneSharingOriginMetadata } from "../presets/archive.js";
import { getPresetsForFolderId } from "../presets/folderArchive.js";
import { findFolderById } from "../presets/folders.js";
import { getPresetArchiveSessionState } from "../presets/sanitize.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { uiState } from "../state.js";
import { isToneSharingSignedIn } from "../toneSharingPanel.js";
import { presetExitSessionButton, presetExportFolderButton, presetExportSessionButton, presetSelector, presetSelectorStatus } from "./dom.js";

// Check if preset is a user preset (can be edited/deleted)
export function isUserPreset(presetId: string | null): boolean {
  if (!presetId) return false;
  const preset = uiState.presetCache.get(presetId);
  if (!preset) return false;
  // User presets have numeric IDs (timestamps), start with "user-", or use UUIDs
  return /^\d+$/.test(presetId)
    || presetId.startsWith("user-")
    || /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(presetId);
}

// Check if preset can be modified (user preset OR factory preset editing enabled)
export function canModifyPreset(presetId: string | null): boolean {
  if (getPresetArchiveSessionState()) {
    return Boolean(presetId);
  }
  if (isUserPreset(presetId)) {
    return true;
  }
  // Allow factory preset modification when feature flag is enabled
  return isFeatureEnabled(Features.FactoryPresetArchives);
}

export function isActivePresetNewDraft(): boolean {
  return Boolean(uiState.activePresetIsNew && uiState.activePresetId);
}

// Update preset action button states
export function updatePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("preset-save-btn") as HTMLButtonElement | null;
  const saveAsBtn = document.getElementById("preset-save-as-btn") as HTMLButtonElement | null;
  const saveAsToolbarBtn = document.getElementById("preset-save-as-toolbar-btn") as HTMLButtonElement | null;
  const deleteBtn = document.getElementById("preset-delete-btn") as HTMLButtonElement | null;
  const exportBtn = document.getElementById("preset-export-btn") as HTMLButtonElement | null;
  const importBtn = document.getElementById("preset-import-btn") as HTMLButtonElement | null;
  const publishBtn = document.getElementById("preset-publish-btn") as HTMLButtonElement | null;

  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const canModify = canModifyPreset(uiState.activePresetId);
  const toneSharingOrigin = getToneSharingOriginMetadata(activePreset);
  const hasActivePreset = Boolean(activePreset);
  const isNewPresetDraft = Boolean(hasActivePreset && isActivePresetNewDraft());
  const archiveSession = getPresetArchiveSessionState();

  if (editBtn) {
    editBtn.hidden = isNewPresetDraft;
    editBtn.disabled = !canModify;
    editBtn.title = canModify
      ? (isNewPresetDraft ? "Set preset details before saving" : "Edit preset details")
      : "Save As to edit factory presets";
  }
  if (saveBtn) {
    saveBtn.disabled = !canModify;
    saveBtn.title = canModify
      ? (isNewPresetDraft ? "Save preset details first" : "Save changes to this preset")
      : "Factory presets cannot be overwritten. Use Save As to keep your own copy.";
    saveBtn.classList.toggle("preset-action-btn-unsaved", Boolean(uiState.presetDirty));
  }
  for (const button of [saveAsBtn, saveAsToolbarBtn]) {
    if (button) {
      const showSaveAs = hasActivePreset && !isNewPresetDraft;
      button.hidden = !showSaveAs;
      button.disabled = !showSaveAs;
      button.title = "Save this preset as a new copy";
    }
  }
  if (deleteBtn) {
    deleteBtn.disabled = !canModify;
    deleteBtn.title = canModify ? "Delete Preset" : "Cannot delete factory presets";
  }
  if (exportBtn) {
    exportBtn.disabled = !hasActivePreset;
    exportBtn.title = hasActivePreset ? "Export preset file" : "No preset to export";
  }
  if (importBtn) {
    importBtn.disabled = false;
    importBtn.title = "Import a preset file";
  }
  if (presetExportSessionButton) {
    const exportTitle = archiveSession
      ? `Export archive session (${uiState.presets.length} presets)`
      : "No preset archive session active";
    presetExportSessionButton.hidden = !archiveSession;
    presetExportSessionButton.disabled = !archiveSession || uiState.presets.length === 0;
    presetExportSessionButton.title = exportTitle;
  }
  if (presetExitSessionButton) {
    presetExitSessionButton.hidden = !archiveSession;
    presetExitSessionButton.disabled = !archiveSession;
    presetExitSessionButton.title = archiveSession
      ? "Exit archive session and restore the normal preset library"
      : "No preset archive session active";
  }
  if (publishBtn) {
    publishBtn.disabled = !hasActivePreset;
    publishBtn.title = !hasActivePreset
      ? "No preset to publish"
      : toneSharingOrigin?.republishBlocked
        ? "Save As before publishing this imported preset"
        : !isToneSharingSignedIn()
          ? "Sign in to Tone Sharing to publish"
          : "Publish to Tone Sharing";
  }

  if (presetSelector) {
    presetSelector.classList.toggle("has-unsaved-changes", Boolean(activePreset && uiState.presetDirty));
  }
  if (presetSelectorStatus) {
    let status = "No preset";
    let statusTitle = "No preset selected";
    presetSelectorStatus.classList.remove("is-warning", "is-dirty");

    if (activePreset) {
      if (archiveSession) {
        status = "Archive";
        statusTitle = `Session-only preset archive: ${archiveSession.archiveName ?? "Preset archive"}. Restart or exit the session to restore the normal preset library.`;
      } else if (toneSharingOrigin?.republishBlocked) {
        status = "Imported";
        statusTitle = "Imported from Tone Sharing. Use Save As to publish.";
        presetSelectorStatus.classList.add("is-warning");
      } else if (isNewPresetDraft) {
        status = "New";
        statusTitle = "New preset draft. Save to set the title and store it.";
      } else if (canModify) {
        status = "User";
        statusTitle = "User preset";
      } else {
        status = "Factory";
        statusTitle = "Factory preset. Use Save As to edit.";
      }

      if (uiState.presetDirty) {
        status += " • Unsaved";
        statusTitle += " Unsaved changes.";
        presetSelectorStatus.classList.add("is-dirty");
      }
    }

    presetSelectorStatus.textContent = status;
    presetSelectorStatus.title = statusTitle;
  }
}

export function updatePresetFolderExportButtons(): void {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const folderPresets = getPresetsForFolderId(activeFolderId);
  const folderName = activeFolderId === PRESET_FOLDER_ALL_ID
    ? "All Presets"
    : activeFolderId === PRESET_FOLDER_FAVORITES_ID
      ? "Favourites"
      : activeFolderId === PRESET_FOLDER_RECENTS_ID
        ? "Recents"
      : (findFolderById(uiState.presetFolders ?? [], activeFolderId)?.name ?? "Folder");

  if (presetExportFolderButton) {
    const isAll = activeFolderId === PRESET_FOLDER_ALL_ID;
    const count = isAll ? uiState.presets.length : folderPresets.length;
    const archiveSession = getPresetArchiveSessionState();
    const title = count
      ? (archiveSession && isAll
        ? `Export archive session (${count})`
        : isAll
          ? `Export all presets (${count})`
          : `Export ${folderName} (${count})`)
      : (isAll ? "No presets to export" : `No presets in ${folderName}`);
    presetExportFolderButton.toggleAttribute("disabled", count === 0);
    presetExportFolderButton.title = title;
    presetExportFolderButton.setAttribute("aria-label", title);
  }
}
