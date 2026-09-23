/**
 * The save/create preset modal: its two modes, the fields behind them, and the
 * save itself.
 */

import { postMessage } from "../bridge.js";
import { buildAttachmentsFromPreset } from "../dataLibraries.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { cachePresetInMemory } from "../presets/cache.js";
import { PRESET_FOLDER_ALL_ID } from "./sorting.js";
import { findFolderForPreset, isVirtualPresetFolderId } from "../presets/folders.js";
import { stripLegacyGlobals } from "../presets/sanitize.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { STANDARD_TAGS, renderTagChips } from "../presetTags.js";
import { generateUserPresetId } from "../presetV2.js";
import { clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import { cachePreset, putLibraryPresetFirst, replaceLibraryPreset, setActivePresetId, setActivePresetSceneId, showAllLibraryPresets } from "../presetLibraryStore.js";
import type { Preset } from "../types.js";
import { movePresetToFolder, populatePresetFolderSelect } from "./folderControls.js";
import { initPresetModalAdvancedActions, initPresetModalTabs, setPresetModalActiveTab, updatePresetModalJson, updatePresetModalReport } from "./inspectModal.js";
import { populatePresetDropdown } from "./filter.js";
import { requestPresetUIRender } from "./refresh.js";
import { updatePresetActionButtons } from "./toolbar.js";
import { stripGlobalSignalChainForSave, summarizePresetForSaveLog } from "./validate.js";

// Save preset modal helpers
export type SavePresetModalMode = "overwrite" | "save-as" | "save-new";

export function configureSavePresetModalLabels(mode: SavePresetModalMode): void {
  const title = document.getElementById("save-preset-modal-title");
  const confirmBtn = document.getElementById("save-preset-confirm");
  const titleText = mode === "overwrite"
    ? "Overwrite Preset"
    : mode === "save-new"
      ? "Save Preset"
      : "Save Preset As";
  const confirmText = mode === "overwrite"
    ? "Overwrite Preset"
    : mode === "save-new"
      ? "Save Preset"
      : "Save New Preset";

  if (title) {
    title.textContent = titleText;
  }

  if (confirmBtn) {
    confirmBtn.textContent = confirmText;
  }
}

export function populateSavePresetModalFields(preset: Preset | null): void {
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  if (nameInput) {
    nameInput.value = preset?.name ?? "";
  }
  if (categoryInput) {
    categoryInput.value = preset?.category || "User";
  }
  if (descriptionInput) {
    descriptionInput.value = preset?.description || "";
  }
  setPresetTagsPickerValue(preset?.tags ?? []);
}

export function resolvePresetModalFolderId(presetId?: string | null): string {
  if (presetId) {
    const presetFolder = findFolderForPreset(uiState.presetFolders ?? [], presetId);
    if (presetFolder?.id) {
      return presetFolder.id;
    }
  }

  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  return isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
}

export function openCreatePresetModal(mode: Extract<SavePresetModalMode, "save-as" | "save-new">, sourcePreset: Preset | null): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;

  delete modal.dataset.editingPresetId;
  modal.dataset.saveMode = mode;
  if (mode === "save-as" && sourcePreset?.id) {
    modal.dataset.sourcePresetId = sourcePreset.id;
  } else {
    delete modal.dataset.sourcePresetId;
  }
  configureSavePresetModalLabels(mode);

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  populatePresetFolderSelect(folderSelect, resolvePresetModalFolderId(sourcePreset?.id ?? null));
  populateSavePresetModalFields(sourcePreset);

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  updatePresetModalJson(sourcePreset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;
  delete modal.dataset.stagedDesignedPeak;
  updateSavePresetModalPeakInfo(modal);

  modal.style.display = "flex";
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  nameInput?.focus();
  nameInput?.select();
}

export function openSavePresetModal(): void {
  openCreatePresetModal("save-as", getActivePresetForRender());
}

export function closeSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (modal) {
    modal.style.display = "none";
    // Clear editing state
    delete modal.dataset.editingPresetId;
    delete modal.dataset.cleanedPreset;
    delete modal.dataset.saveMode;
    delete modal.dataset.sourcePresetId;
    delete modal.dataset.stagedDesignedPeak;
  }
}

/** The real folder the library had open when New was pressed, for the preset it creates. */
let newPresetFolderId: string | null = null;

function resolveNewPresetFolderId(): string {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  return isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
}

/**
 * New: the engine builds the default preset and loads it unsaved, answering with
 * "presetLoaded" marked `created`, which adoptCreatedPreset files in the library.
 */
export function createDefaultPreset(): void {
  newPresetFolderId = resolveNewPresetFolderId();
  postMessage({ type: "newPreset" });
}

/**
 * The engine's new preset has arrived (presetHandlers.ts has made it the active, clean
 * preset): put it first in the library and in the folder that was open, as unsaved.
 */
export function adoptCreatedPreset(preset: Preset): void {
  const folderId = newPresetFolderId ?? resolveNewPresetFolderId();
  newPresetFolderId = null;

  putLibraryPresetFirst(clonePreset(preset));
  showAllLibraryPresets();
  if (folderId) {
    movePresetToFolder(preset.id, folderId);
  }
  setActivePresetIsNew(true);
  populatePresetDropdown();
  requestPresetUIRender(clonePreset(preset));
  showNotification("Preset created", preset.name);
  updatePresetActionButtons();
}

export function saveCurrentPreset(): void {
  const modal = document.getElementById("save-preset-modal");
  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  const name = nameInput?.value?.trim() || "";
  const category = categoryInput?.value?.trim() || "User";
  const description = descriptionInput?.value?.trim() || "";
  const tags = getPresetTagsPickerValue();

  if (!name) {
    showNotification("Error", "Preset name is required");
    return;
  }

  // Check if we're editing an existing preset
  const editingPresetId = modal?.dataset.editingPresetId;
  const saveMode = (modal?.dataset.saveMode as SavePresetModalMode | undefined) ?? (editingPresetId ? "overwrite" : "save-as");
  const sourcePresetId = modal?.dataset.sourcePresetId?.trim() || "";

  const selectedFolderId = folderSelect?.value || PRESET_FOLDER_ALL_ID;
  const activePreset = getActivePresetForRender();
  const baseAttachments = buildAttachmentsFromPreset(activePreset ?? {} as Preset);
  const includeGlobalFx = false;
  const stagedPeakStr = modal?.dataset.stagedDesignedPeak;
  const stagedDesignedPeak = stagedPeakStr !== undefined ? parseFloat(stagedPeakStr) : undefined;
  let cleanedPreset: Preset | null = null;
  if (modal?.dataset.cleanedPreset) {
    try {
      cleanedPreset = JSON.parse(modal.dataset.cleanedPreset) as Preset;
    } catch {
      cleanedPreset = null;
    }
  }

  if (editingPresetId) {
    // Editing existing preset
    const existingPreset = uiState.presetCache.get(editingPresetId);
    if (existingPreset) {
      // Prefer the live edited preset (draft) as the graph source. For new-preset
      // drafts the cache entry holds the empty initial graph because the "state"
      // broadcast handler only refreshes the cache the first time a preset id is
      // seen — later signal-graph edits live only on activePresetDraft. Using the
      // stale cache here would persist an empty effect graph on the first save.
      const liveSource = activePreset && activePreset.id === editingPresetId ? activePreset : existingPreset;
      const basePreset = stripLegacyGlobals(cleanedPreset ?? liveSource);
      const updatedPreset: Preset = {
        ...basePreset,
        name,
        category,
        description,
        tags: tags.length > 0 ? tags : undefined,
        attachments: baseAttachments,
      };
      if (stagedDesignedPeak !== undefined && isFinite(stagedDesignedPeak)) {
        updatedPreset.designedPeakInputDbfs = Math.round(stagedDesignedPeak * 10) / 10;
      }
      const sceneId = normalizePresetScenes(updatedPreset, uiState.activePresetSceneId ?? undefined);
      setActivePresetSceneId(sceneId);
      delete (updatedPreset as Record<string, unknown>).globalSignalChain;

      cachePresetInMemory(updatedPreset);
      // Also persist to disk via the C++ backend
      const savePayload: Record<string, unknown> = {
        type: "savePreset",
        saveMode,
        presetId: updatedPreset.id,
        name: updatedPreset.name,
        category: updatedPreset.category,
        description: updatedPreset.description,
        ...(sceneId ? { sceneId } : {}),
        includeGlobalSignalChain: includeGlobalFx,
        preset: stripGlobalSignalChainForSave(updatedPreset),
      };
      appendLog(`save preset → ${updatedPreset.name} (${summarizePresetForSaveLog(updatedPreset)})`);
      postMessage(savePayload);
      cachePreset(updatedPreset, editingPresetId);
      replaceLibraryPreset(updatedPreset, editingPresetId);
      showAllLibraryPresets();
      populatePresetDropdown();
      requestPresetUIRender(clonePreset(updatedPreset));
      movePresetToFolder(editingPresetId, selectedFolderId);
      closeSavePresetModal();
      showNotification("Preset updated", name);
      setActivePresetIsNew(false);
      setActivePresetSnapshot(updatedPreset);
      setActivePresetDraft(updatedPreset);
      setPresetDirty(false);
      updatePresetActionButtons();
      return;
    }
  }

  // Creating new preset
  const basePreset = stripLegacyGlobals(cleanedPreset ?? clonePreset(activePreset ?? ({} as Preset)));
  const newPresetId = generateUserPresetId();
  const newPreset: Preset = {
    ...basePreset,
    id: newPresetId,
    name,
    category,
    description,
    tags: tags.length > 0 ? tags : undefined,
    attachments: baseAttachments,
  };
  delete (newPreset as Record<string, unknown>).toneSharingOrigin;
  if (stagedDesignedPeak !== undefined && isFinite(stagedDesignedPeak)) {
    newPreset.designedPeakInputDbfs = Math.round(stagedDesignedPeak * 10) / 10;
  }
  const sceneId = normalizePresetScenes(newPreset, uiState.activePresetSceneId ?? undefined);
  setActivePresetSceneId(sceneId);
  delete (newPreset as Record<string, unknown>).globalSignalChain;

  cachePresetInMemory(newPreset);
  // Also persist to disk via the C++ backend
  const savePayload: Record<string, unknown> = {
    type: "savePreset",
    saveMode,
    presetId: newPreset.id,
    name: newPreset.name,
    category: newPreset.category,
    description: newPreset.description,
    ...(saveMode === "save-as" ? { sourcePresetId, requireNewPresetId: true } : {}),
    ...(sceneId ? { sceneId } : {}),
    includeGlobalSignalChain: includeGlobalFx,
    preset: stripGlobalSignalChainForSave(newPreset),
  };
  appendLog(`save preset → ${newPreset.name} (${summarizePresetForSaveLog(newPreset)})`);
  postMessage(savePayload);
  showAllLibraryPresets();
  cachePreset(newPreset);
  if (selectedFolderId) {
    movePresetToFolder(newPreset.id, selectedFolderId);
  }
  setActivePresetId(newPreset.id);
  populatePresetDropdown();
  requestPresetUIRender(clonePreset(newPreset));
  closeSavePresetModal();
  showNotification("Preset saved", newPreset.name);
  setActivePresetIsNew(false);
  setActivePresetSnapshot(newPreset);
  setActivePresetDraft(newPreset);
  setPresetDirty(false);
  updatePresetActionButtons();
}

export function getPresetTagsPickerValue(): string[] {
  const picker = document.getElementById("preset-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

export function setPresetTagsPickerValue(tags: string[]): void {
  const picker = document.getElementById("preset-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

export function formatPeakDb(value: number | null | undefined): string {
  if (value == null || !isFinite(value)) return "\u2014";
  return `${value.toFixed(1)} dBFS`;
}

export function refreshSavePresetModalPeakInfoIfOpen(): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal || modal.style.display === "none" || modal.style.display === "") return;
  updateSavePresetModalPeakInfo(modal);
}

export function updateSavePresetModalPeakInfo(modal: HTMLElement): void {
  const rawPeakEl = document.getElementById("preset-modal-raw-peak");
  const designedPeakEl = document.getElementById("preset-modal-designed-peak");

  // Live 10s raw input peak
  const rawPeak = uiState.signalPeakHold?.rawInput.peakDbfs;
  if (rawPeakEl) rawPeakEl.textContent = formatPeakDb(rawPeak ?? null);

  // Staged designed peak takes priority over stored value
  const stagedStr = modal.dataset.stagedDesignedPeak;
  if (stagedStr !== undefined) {
    const staged = parseFloat(stagedStr);
    if (designedPeakEl) designedPeakEl.textContent = formatPeakDb(staged);
    return;
  }

  // Stored designed peak from active preset
  const activePreset = uiState.activePresetId
    ? (uiState.presetCache.get(uiState.activePresetId) ?? uiState.presets.find((p) => p.id === uiState.activePresetId))
    : null;
  const editingId = modal.dataset.editingPresetId;
  const sourcePreset = editingId
    ? (uiState.presetCache.get(editingId) ?? uiState.presets.find((p) => p.id === editingId))
    : activePreset;
  if (designedPeakEl) designedPeakEl.textContent = formatPeakDb(sourcePreset?.designedPeakInputDbfs ?? null);
}

export function initializeSavePresetModal(): void {
  const closeBtn = document.getElementById("save-preset-modal-close");
  const cancelBtn = document.getElementById("save-preset-cancel");
  const confirmBtn = document.getElementById("save-preset-confirm");
  const modal = document.getElementById("save-preset-modal");

  // Populate from the standard tag list, then wire tag chip toggle clicks
  const tagsPicker = document.getElementById("preset-tags-picker");
  if (tagsPicker) {
    renderTagChips(tagsPicker, STANDARD_TAGS, "preset-tag-chip");
    tagsPicker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
      btn.addEventListener("click", () => btn.classList.toggle("active"));
    });
  }

  if (closeBtn) {
    closeBtn.addEventListener("click", closeSavePresetModal);
  }

  if (cancelBtn) {
    cancelBtn.addEventListener("click", closeSavePresetModal);
  }

  if (confirmBtn) {
    confirmBtn.addEventListener("click", () => {
      saveCurrentPreset();
    });
  }

  if (modal) {
    modal.addEventListener("mousedown", (event) => {
      if (event.target === modal) {
        closeSavePresetModal();
      }
    });
  }

  const applyPeakBtn = document.getElementById("preset-modal-apply-peak");
  if (applyPeakBtn && modal) {
    applyPeakBtn.addEventListener("click", () => {
      const rawPeak = uiState.signalPeakHold?.rawInput.peakDbfs;
      if (rawPeak == null || !isFinite(rawPeak)) {
        showNotification("No peak data available yet");
        return;
      }
      modal.dataset.stagedDesignedPeak = String(rawPeak);
      updateSavePresetModalPeakInfo(modal);
    });
  }
}

export function initializeSaveAsButton(): void {
  const saveAsBtn = document.getElementById("preset-save-as-btn");
  if (saveAsBtn) {
    saveAsBtn.addEventListener("click", openSavePresetModal);
  }
}
