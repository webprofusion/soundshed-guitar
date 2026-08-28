import { appendLog } from "./logging.js";
import { clearNotification, showNotification } from "./notifications.js";
import { renderPresetDetails, renderPresetList, renderMixerPanel } from "./views.js";
import { clonePreset, uiState, DEFAULT_GLOBAL_SIGNAL_CHAIN, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty } from "./state.js";
import { buildAttachmentsFromPreset, getDefaultPresets, initializeDataLibraries, REMOTE_BASE_URL } from "./dataLibraries.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl } from "./utils.js";
import { generateResourceId } from "./archiveUtils.js";
import type { AppSettingValue, Attachment, GlobalSignalChainConfig, GraphNode, Preset, PresetFolder, ResourceRef, Setlist } from "./types.js";
import { createEmptyPresetV2, generateUserPresetId } from "./presetV2.js";
import { bindDemoAudioControls } from "./demoAudio.js";
import { postMessage, setAppSetting, getCompositePresetList } from "./bridge.js";
import { renderSignalPathBar } from "./signalPath.js";
import { showConfirm } from "./dialogs.js";
import { isToneSharingSignedIn, openToneSharingPublishPresetModal, openToneSharingSignInModal } from "./toneSharingPanel.js";
import { switchMainPanel } from "./navigation.js";
import { updateUiSettings } from "./windowSettings.js";
import { normalizePresetScenes } from "./presetScenes.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import { STANDARD_TAGS, renderTagChips } from "./presetTags.js";
import { exportCurrentPresetArchive, exportPresetArchiveSession, exportSelectedPresetCollectionArchive, getToneSharingOriginMetadata, importPackWithConfirmation } from "./presets/archive.js";
import { PRESET_FOLDER_ALL_ID, getPresetsForFolderId } from "./presets/folderArchive.js";
import { getPresetArchiveSessionState, sanitizePresetForArchive, stripLegacyGlobals } from "./presets/sanitize.js";
import { requestPresetFromBackend } from "./presets/fetch.js";
import { PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID, sortPresetsAlphabetically } from "./presets/sorting.js";
import { collectPresetIds, ensurePresetFolders, findFolderById, findFolderForPreset, findFolderWithParent, getPresetFolderPath, isDescendantFolder, isVirtualPresetFolderId, persistPresetFolders, removePresetFromFolders, sortPresetFoldersAlphabetically } from "./presets/folders.js";
import { getPresetRating, getRecentPresets, loadFavoritePresetIds, normalizeRecentPresetIds, saveFavoritePresetIds, setFavoriteToggleState, setPresetRating, toggleFavoritePreset, trackRecentPreset } from "./presets/favorites.js";
import { cachePresetInMemory } from "./presets/cache.js";
import { presetFavoriteToggle, presetSearchElement } from "./presets/dom.js";
import { setPresetLibraryRefresher } from "./presets/refresh.js";
export { handlePresetDataMessage, refreshPresetCacheEntryFromBackend, rejectPendingPresetRequest } from "./presets/fetch.js";
export { cachePresetInMemory } from "./presets/cache.js";
export { applyPresetArchiveSessionState, sanitizePresetForArchive } from "./presets/sanitize.js";
export { buildPresetArchiveBlob, buildToneSharingPresetArchiveBlobs, handleDroppedPresetPack, importGeneratedPack, importPackWithConfirmation, importPresetArchive, resolveImportedPresetName, startPresetArchiveSessionFromFile } from "./presets/archive.js";

const presetChooserLabel = document.getElementById("preset-chooser-label") as HTMLButtonElement | null;
const prevPresetBtn = document.getElementById("prev-preset");
const nextPresetBtn = document.getElementById("next-preset");
const presetUndoBtn = document.getElementById("preset-undo") as HTMLButtonElement | null;
const presetRedoBtn = document.getElementById("preset-redo") as HTMLButtonElement | null;
const randomPresetBtn = document.getElementById("preset-random-btn");
const presetExtraActionsBtn = document.getElementById("preset-extra-actions-btn") as HTMLButtonElement | null;
const presetExtraActionsMenu = document.getElementById("preset-extra-actions-menu") as HTMLDivElement | null;
const presetSelector = document.getElementById("preset-selector");
const presetSelectorStatus = document.getElementById("preset-selector-status") as HTMLElement | null;
const presetLibraryPopover = document.getElementById("preset-library-popover");
const presetLibraryCloseButton = document.getElementById("preset-library-close-btn") as HTMLButtonElement | null;
const presetControlBar = presetLibraryPopover?.closest<HTMLElement>(".control-bar") ?? null;
const presetLibraryTabs = document.querySelector(".preset-library-tabs") as HTMLElement | null;
const presetLibraryPresetsPanel = document.getElementById("preset-library-presets-panel") as HTMLElement | null;
const presetLibraryMultiRigTab = document.getElementById("preset-lib-tab-multi-rig") as HTMLButtonElement | null;
const presetLibraryPresetsTab = document.getElementById("preset-lib-tab-presets") as HTMLButtonElement | null;
const presetLibraryMultiRigPanel = document.getElementById("preset-library-multi-rig-panel") as HTMLElement | null;
const presetFolderNameInput = document.getElementById("preset-folder-name") as HTMLInputElement | null;
const presetFolderAddButton = document.getElementById("preset-folder-add") as HTMLButtonElement | null;
const presetFolderRenameButton = document.getElementById("preset-folder-rename") as HTMLButtonElement | null;
const presetFolderDeleteButton = document.getElementById("preset-folder-delete") as HTMLButtonElement | null;
const presetExportFolderButton = document.getElementById("preset-export-folder-btn") as HTMLButtonElement | null;
const setlistNameInput = document.getElementById("setlist-name-input") as HTMLInputElement | null;
const setlistBankInput = document.getElementById("setlist-bank-input") as HTMLInputElement | null;
const setlistAddButton = document.getElementById("setlist-add-btn");
const setlistListElement = document.getElementById("setlist-list");
const setlistSlotsElement = document.getElementById("setlist-slots");
const setlistEditorHeader = document.getElementById("setlist-editor-header");
const setlistCollapsible = document.getElementById("setlist-collapsible");
const setlistToggle = document.getElementById("setlist-toggle");
const setlistPanel = document.getElementById("setlist-panel");
const presetExportSessionButton = document.getElementById("preset-export-session-btn") as HTMLButtonElement | null;
const presetExitSessionButton = document.getElementById("preset-exit-session-btn") as HTMLButtonElement | null;

function syncPresetHeaderPopoverLayer(): void {
  const hasOpenPopover = Boolean(
    presetLibraryPopover?.classList.contains("open") || presetExtraActionsMenu?.classList.contains("open"),
  );
  presetControlBar?.classList.toggle("has-open-popover", hasOpenPopover);
}

const PRESET_RECENTS_SETTING = "presets.recents";

const activeTagFilters = new Set<string>();
let presetChooserOverride: ((presetId: string) => void | Promise<void>) | null = null;
let presetChooserCloseOverride: (() => void) | null = null;


function normalizeSetlistName(name: string): string {
  return name.trim();
}

const PRESET_ALLOWED_KEYS = new Set([
  "id",
  "name",
  "category",
  "description",
  "attachments",
  "fxChain",
  "audioFxModelId",
  "irId",
  "customModelPath",
  "customIrPath",
  "formatVersion",
  "graph",
  "scenes",
  "globalSignalChain",
  "embeddedResources",
  "version",
  "author",
  "tags",
  "createdAt",
  "modifiedAt",
]);

const PRESET_OPTIONAL_STRING_KEYS = [
  "category",
  "description",
  "customModelPath",
  "customIrPath",
  "author",
];

const PRESET_OPTIONAL_ARRAY_KEYS = [
  "attachments",
  "fxChain",
  "embeddedResources",
  "scenes",
  "tags",
];

function getPresetForModal(): Preset | null {
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  return activePreset ? clonePreset(activePreset) : null;
}

function stripGlobalSignalChainForSave(preset: Preset): Preset {
  const cleaned = sanitizePresetForArchive(preset);
  if (Array.isArray(cleaned.scenes) && cleaned.scenes.length > 0) {
    delete (cleaned as Record<string, unknown>).graph;
  }
  return cleaned;
}

function summarizePresetForSaveLog(preset: Preset): string {
  const graphNodes = preset.graph?.nodes?.length ?? 0;
  const graphEdges = preset.graph?.edges?.length ?? 0;
  const scenes = preset.scenes ?? [];
  const sceneSummary = scenes.length
    ? scenes.map((scene) => `${scene.id}:${scene.graph?.nodes?.length ?? 0}`).join(",")
    : "none";
  return `graphNodes=${graphNodes}, graphEdges=${graphEdges}, scenes=${scenes.length}[${sceneSummary}]`;
}

function validatePresetForUi(preset: Preset | null): string[] {
  if (!preset) {
    return ["No preset loaded."];
  }

  const issues: string[] = [];
  const unknownKeys = Object.keys(preset).filter((key) => !PRESET_ALLOWED_KEYS.has(key));
  if (unknownKeys.length) {
    issues.push(`Unknown fields: ${unknownKeys.sort().join(", ")}`);
  }

  if (typeof preset.id !== "string" || !preset.id.trim()) {
    issues.push("Missing or invalid preset id.");
  }

  if (typeof preset.name !== "string" || !preset.name.trim()) {
    issues.push("Missing or invalid preset name.");
  }

  if (preset.graph) {
    if (!Array.isArray(preset.graph.nodes)) {
      issues.push("Preset graph.nodes must be an array.");
    }
    if (!Array.isArray(preset.graph.edges)) {
      issues.push("Preset graph.edges must be an array.");
    }
    preset.graph.nodes?.forEach((node, index) => {
      if (!node || typeof node.id !== "string" || !node.id.trim()) {
        issues.push(`Graph node #${index + 1} is missing a valid id.`);
      }
      if (!node || typeof node.type !== "string" || !node.type.trim()) {
        issues.push(`Graph node #${index + 1} is missing a valid type.`);
      }
    });
    preset.graph.edges?.forEach((edge, index) => {
      if (!edge || typeof edge.from !== "string" || typeof edge.to !== "string") {
        issues.push(`Graph edge #${index + 1} is missing valid endpoints.`);
      }
    });
  }

  if (preset.scenes && !Array.isArray(preset.scenes)) {
    issues.push("Preset scenes must be an array when present.");
  }

  return issues;
}

function cleanupPresetForUi(
  preset: Preset,
): { cleaned: Preset; removedKeys: string[]; normalizedAliases: number; removedGlobalEq: boolean } {
  const cleaned: Preset = clonePreset(preset);
  const removedKeys: string[] = [];
  let normalizedAliases = 0;
  let removedGlobalEq = false;

  const normalizeRef = (ref: ResourceRef): ResourceRef => {
    const normalized: ResourceRef = { ...ref };
    const legacyId = typeof normalized.id === "string" ? normalized.id : "";
    const legacyType = typeof normalized.type === "string" ? normalized.type : "";
    const resourceId = typeof normalized.resourceId === "string" ? normalized.resourceId : "";
    const resourceType = typeof normalized.resourceType === "string" ? normalized.resourceType : "";

    const finalId = resourceId || legacyId;
    const finalType = resourceType || legacyType;

    if (finalId) {
      normalized.resourceId = finalId;
    }
    if (finalType) {
      normalized.resourceType = finalType;
    }

    if (legacyId && legacyId === normalized.resourceId) {
      delete normalized.id;
      normalizedAliases += 1;
    }
    if (legacyType && legacyType === normalized.resourceType) {
      delete normalized.type;
      normalizedAliases += 1;
    }

    return normalized;
  };

  Object.keys(cleaned).forEach((key) => {
    if (!PRESET_ALLOWED_KEYS.has(key)) {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  if ("globals" in cleaned) {
    delete (cleaned as Record<string, unknown>).globals;
    removedKeys.push("globals");
  }
  if ("global" in cleaned) {
    delete (cleaned as Record<string, unknown>).global;
    removedKeys.push("global");
  }
  if ("globalSignalChain" in cleaned) {
    delete (cleaned as Record<string, unknown>).globalSignalChain;
    removedKeys.push("globalSignalChain");
  }

  PRESET_OPTIONAL_STRING_KEYS.forEach((key) => {
    const value = cleaned[key] as string | null | undefined;
    if (typeof value === "string" && value.trim() === "") {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  PRESET_OPTIONAL_ARRAY_KEYS.forEach((key) => {
    const value = cleaned[key] as unknown;
    if (Array.isArray(value) && value.length === 0) {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  if (cleaned.graph?.nodes) {
    cleaned.graph.nodes = cleaned.graph.nodes.map((node) => {
      const nextNode = { ...node };
      const normalizedResources = Array.isArray(nextNode.resources)
        ? nextNode.resources.map((ref) => normalizeRef(ref))
        : [];
      if (normalizedResources.length || Array.isArray(nextNode.resources)) {
        nextNode.resources = normalizedResources;
      }
      return nextNode;
    });
  }

  if (cleaned.graph?.nodes?.length && cleaned.graph?.edges?.length) {
    const eqDefaults = (() => {
      const fallback = {
        lowGain: 0.0,
        lowFreq: 100.0,
        lowMidGain: 0.0,
        lowMidFreq: 400.0,
        lowMidQ: 1.0,
        highMidGain: 0.0,
        highMidFreq: 2000.0,
        highMidQ: 1.0,
        highGain: 0.0,
        highFreq: 8000.0,
      };
      const eqNode = DEFAULT_GLOBAL_SIGNAL_CHAIN.postChainGraph.nodes.find((node) => node.id === "global_eq" || node.type === "eq_parametric");
      if (!eqNode || !eqNode.params) {
        return fallback;
      }
      return {
        lowGain: eqNode.params.lowGain ?? fallback.lowGain,
        lowFreq: eqNode.params.lowFreq ?? fallback.lowFreq,
        lowMidGain: eqNode.params.lowMidGain ?? fallback.lowMidGain,
        lowMidFreq: eqNode.params.lowMidFreq ?? fallback.lowMidFreq,
        lowMidQ: eqNode.params.lowMidQ ?? fallback.lowMidQ,
        highMidGain: eqNode.params.highMidGain ?? fallback.highMidGain,
        highMidFreq: eqNode.params.highMidFreq ?? fallback.highMidFreq,
        highMidQ: eqNode.params.highMidQ ?? fallback.highMidQ,
        highGain: eqNode.params.highGain ?? fallback.highGain,
        highFreq: eqNode.params.highFreq ?? fallback.highFreq,
      };
    })();
    const isDefaultGlobalEqNode = (node: GraphNode): boolean => {
      const anyNode = node as unknown as {
        id?: unknown;
        type?: unknown;
        label?: unknown;
        enabled?: unknown;
        bypassed?: unknown;
        params?: Record<string, number>;
      };
      const nodeId = typeof anyNode.id === "string" ? anyNode.id : "";
      const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";
      const nodeLabel = typeof anyNode.label === "string" ? anyNode.label : "";
      if (nodeId !== "global_eq" && nodeLabel !== "Global EQ") {
        return false;
      }
      if (nodeType && nodeType !== "eq_parametric") {
        return false;
      }
      const enabled = typeof anyNode.enabled === "boolean"
        ? anyNode.enabled
        : (typeof anyNode.bypassed === "boolean" ? !anyNode.bypassed : true);
      if (enabled) {
        return false;
      }
      const params = anyNode.params ?? {};
      return (
        params.lowGain === eqDefaults.lowGain
        && params.lowFreq === eqDefaults.lowFreq
        && params.lowMidGain === eqDefaults.lowMidGain
        && params.lowMidFreq === eqDefaults.lowMidFreq
        && params.lowMidQ === eqDefaults.lowMidQ
        && params.highMidGain === eqDefaults.highMidGain
        && params.highMidFreq === eqDefaults.highMidFreq
        && params.highMidQ === eqDefaults.highMidQ
        && params.highGain === eqDefaults.highGain
        && params.highFreq === eqDefaults.highFreq
      );
    };

    const eqNode = cleaned.graph.nodes.find((node) => isDefaultGlobalEqNode(node));
    if (eqNode) {
      const eqId = eqNode.id;
      const hasInputToEq = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === eqId);
      const hasEqToDoubler = cleaned.graph.edges.some((edge) => edge.from === eqId && edge.to === "global_doubler");
      const hasEqToOutput = cleaned.graph.edges.some((edge) => edge.from === eqId && edge.to === "__output__");
      const hasDoubler = cleaned.graph.nodes.some((node) => node.id === "global_doubler");
      const hasOutput = cleaned.graph.nodes.some((node) => node.id === "__output__");

      cleaned.graph.nodes = cleaned.graph.nodes.filter((node) => node.id !== eqId);
      cleaned.graph.edges = cleaned.graph.edges.filter((edge) => edge.from !== eqId && edge.to !== eqId);

      if (hasInputToEq && hasEqToDoubler && hasDoubler) {
        const alreadyLinked = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === "global_doubler");
        if (!alreadyLinked) {
          cleaned.graph.edges.push({ from: "__input__", to: "global_doubler", fromPort: 0, toPort: 0, gain: 1 });
        }
      } else if (hasInputToEq && hasEqToOutput && hasOutput) {
        const alreadyLinked = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === "__output__");
        if (!alreadyLinked) {
          cleaned.graph.edges.push({ from: "__input__", to: "__output__", fromPort: 0, toPort: 0, gain: 1 });
        }
      }

      removedGlobalEq = true;
    }
  }

  if (cleaned.audioFxModelId == null || cleaned.audioFxModelId === "") {
    delete cleaned.audioFxModelId;
    removedKeys.push("audioFxModelId");
  }

  if (cleaned.irId == null || cleaned.irId === "") {
    delete cleaned.irId;
    removedKeys.push("irId");
  }

  if (cleaned.customModelPath == null || cleaned.customModelPath === "") {
    delete cleaned.customModelPath;
    removedKeys.push("customModelPath");
  }

  if (cleaned.customIrPath == null || cleaned.customIrPath === "") {
    delete cleaned.customIrPath;
    removedKeys.push("customIrPath");
  }

  return {
    cleaned,
    removedKeys: removedKeys.filter((value, index) => removedKeys.indexOf(value) === index),
    normalizedAliases,
    removedGlobalEq,
  };
}

function setPresetModalActiveTab(modal: HTMLElement, tabId: string): void {
  const tabButtons = Array.from(modal.querySelectorAll<HTMLElement>(".preset-modal-tab-btn"));
  const tabPanels = Array.from(modal.querySelectorAll<HTMLElement>(".preset-modal-tab-panel"));
  tabButtons.forEach((button) => {
    const active = button.dataset.presetModalTab === tabId;
    button.classList.toggle("active", active);
    button.setAttribute("aria-selected", active ? "true" : "false");
  });
  tabPanels.forEach((panel) => {
    const active = panel.dataset.presetModalTabPanel === tabId;
    panel.classList.toggle("active", active);
  });
}

function initPresetModalTabs(modal: HTMLElement): void {
  if (modal.dataset.tabsBound === "true") {
    return;
  }
  modal.dataset.tabsBound = "true";
  modal.addEventListener("click", (event) => {
    const target = (event.target as HTMLElement | null)?.closest(".preset-modal-tab-btn") as HTMLElement | null;
    if (!target) {
      return;
    }
    const tabId = target.dataset.presetModalTab;
    if (tabId) {
      setPresetModalActiveTab(modal, tabId);
    }
  });
}

function updatePresetModalJson(preset: Preset | null): void {
  const pre = document.getElementById("preset-json-view") as HTMLPreElement | null;
  if (!pre) {
    return;
  }
  const withGlobalChain = preset
    ? (() => {
        const cleaned = stripGlobalSignalChainForSave(stripLegacyGlobals(preset));
        const chain = (preset as Preset & { globalSignalChain?: unknown }).globalSignalChain;
        return chain ? { ...cleaned, globalSignalChain: chain } : cleaned;
      })()
    : null;
  pre.textContent = withGlobalChain ? JSON.stringify(withGlobalChain, null, 2) : "";
}

function updatePresetModalReport(lines: string[]): void {
  const report = document.getElementById("preset-json-report") as HTMLPreElement | null;
  if (!report) {
    return;
  }
  report.textContent = lines.length ? lines.join("\n") : "No issues found.";
}

function initPresetModalAdvancedActions(modal: HTMLElement): void {
  if (modal.dataset.advancedBound === "true") {
    return;
  }
  modal.dataset.advancedBound = "true";

  const validateBtn = document.getElementById("preset-json-validate") as HTMLButtonElement | null;
  const cleanupBtn = document.getElementById("preset-json-cleanup") as HTMLButtonElement | null;

  validateBtn?.addEventListener("click", () => {
    const preset = getPresetForModal();
    const issues = validatePresetForUi(preset);
    updatePresetModalReport(issues);
  });

  cleanupBtn?.addEventListener("click", () => {
    const preset = getPresetForModal();
    if (!preset) {
      updatePresetModalReport(["No preset loaded."]);
      return;
    }
    const result = cleanupPresetForUi(preset);
    modal.dataset.cleanedPreset = JSON.stringify(result.cleaned);
    updatePresetModalJson(result.cleaned);
    const reportLines: string[] = [];
    if (result.removedKeys.length) {
      reportLines.push(`Removed fields: ${result.removedKeys.sort().join(", ")}`);
    }
    if (result.normalizedAliases > 0) {
      reportLines.push(`Normalized resource ref aliases: ${result.normalizedAliases}`);
    }
    if (result.removedGlobalEq) {
      reportLines.push("Removed default global EQ node.");
    }
    updatePresetModalReport(reportLines.length ? reportLines : ["No unused fields removed."]);
  });
}

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
    uiState.appSettings[PRESET_RECENTS_SETTING] = normalized as unknown as AppSettingValue;
    setAppSetting(PRESET_RECENTS_SETTING, null);
  }
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
    return;
  }
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function recordRecentPreset(presetId: string | null | undefined): void {
  trackRecentPreset(presetId);
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
  }
}

export function applyPresetRatingsFromBackend(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

function openPresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  closePresetExtraActionsMenu();
  syncPresetLibraryFeatureVisibility();
  // Refresh the Multi-Rig list on every open (not just once at app boot,
  // when app settings — and therefore the MultiRig feature flag — may not
  // have finished loading yet) so the tab reliably shows up for anyone with
  // saved Multi-Rig presets, and stays in sync with other sessions/windows.
  if (isFeatureEnabled(Features.MultiRig)) {
    getCompositePresetList();
  }
  const controlBar = presetLibraryPopover.closest<HTMLElement>(".control-bar");
  controlBar?.classList.remove("is-collapsed");
  const collapseButton = document.getElementById("control-bar-collapse-btn");
  collapseButton?.setAttribute("aria-expanded", "true");
  collapseButton?.setAttribute("aria-label", "Collapse controls");
  if (collapseButton instanceof HTMLElement) {
    collapseButton.title = "Collapse controls";
  }
  presetLibraryPopover.classList.add("open");
  presetLibraryPopover.setAttribute("aria-hidden", "false");
  presetChooserLabel?.setAttribute("aria-expanded", "true");
  syncPresetHeaderPopoverLayer();
}

function closePresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  const onClose = presetChooserCloseOverride;
  presetLibraryPopover.classList.remove("open");
  presetLibraryPopover.setAttribute("aria-hidden", "true");
  presetChooserLabel?.setAttribute("aria-expanded", "false");
  presetChooserOverride = null;
  presetChooserCloseOverride = null;
  onClose?.();
  syncPresetHeaderPopoverLayer();
}

export function openPresetChooserForSelection(
  onSelect: (presetId: string) => void | Promise<void>,
  onClose?: () => void,
): void {
  presetChooserOverride = onSelect;
  presetChooserCloseOverride = onClose ?? null;
  openPresetLibraryPopover();
  presetSearchElement?.focus({ preventScroll: true });
}

function togglePresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  if (presetLibraryPopover.classList.contains("open")) {
    closePresetLibraryPopover();
  } else {
    openPresetLibraryPopover();
  }
}

function openPresetExtraActionsMenu(): void {
  if (!presetExtraActionsBtn || !presetExtraActionsMenu) {
    return;
  }
  closePresetLibraryPopover();
  presetExtraActionsMenu.classList.add("open");
  presetExtraActionsMenu.setAttribute("aria-hidden", "false");
  presetExtraActionsBtn.setAttribute("aria-expanded", "true");
  syncPresetHeaderPopoverLayer();
}

function closePresetExtraActionsMenu(): void {
  if (!presetExtraActionsBtn || !presetExtraActionsMenu) {
    return;
  }
  presetExtraActionsMenu.classList.remove("open");
  presetExtraActionsMenu.setAttribute("aria-hidden", "true");
  presetExtraActionsBtn.setAttribute("aria-expanded", "false");
  syncPresetHeaderPopoverLayer();
}

function togglePresetExtraActionsMenu(): void {
  if (!presetExtraActionsMenu) {
    return;
  }
  if (presetExtraActionsMenu.classList.contains("open")) {
    closePresetExtraActionsMenu();
  } else {
    openPresetExtraActionsMenu();
  }
}

/** Setlists only make sense against the regular preset library, not the Multi-Rig list. */
export function setSetlistPanelVisible(visible: boolean): void {
  if (setlistCollapsible) {
    setlistCollapsible.hidden = !visible;
  }
}

export function syncPresetLibraryFeatureVisibility(): void {
  const multiRigEnabled = isFeatureEnabled(Features.MultiRig);
  // The Multi-Rig tab only earns its place in the header once there's
  // something to show there — otherwise it's a permanently-empty tab for
  // users who've never saved a Multi-Rig preset.
  const hasCompositePresets = (uiState.compositePresets?.length ?? 0) > 0;
  const showMultiRigTab = multiRigEnabled && hasCompositePresets;

  presetLibraryPopover?.classList.toggle("preset-library-popover-simple", !showMultiRigTab);

  if (presetLibraryTabs) {
    presetLibraryTabs.hidden = !showMultiRigTab;
    presetLibraryTabs.setAttribute("aria-hidden", String(!showMultiRigTab));
  }

  if (presetLibraryMultiRigTab) {
    presetLibraryMultiRigTab.hidden = !showMultiRigTab;
    presetLibraryMultiRigTab.setAttribute("aria-hidden", String(!showMultiRigTab));
    presetLibraryMultiRigTab.tabIndex = showMultiRigTab ? 0 : -1;
  }

  // Whenever the tab itself isn't shown, force the view back to the normal
  // Presets panel — this is what stops a since-removed/never-fetched
  // Multi-Rig tab from leaving the preset list stuck hidden behind it.
  if (!showMultiRigTab) {
    presetLibraryPresetsTab?.classList.add("active");
    presetLibraryPresetsTab?.setAttribute("aria-selected", "true");
    presetLibraryMultiRigTab?.classList.remove("active");
    presetLibraryMultiRigTab?.setAttribute("aria-selected", "false");
    if (presetLibraryPresetsPanel) {
      presetLibraryPresetsPanel.hidden = false;
    }
    if (presetLibraryMultiRigPanel) {
      presetLibraryMultiRigPanel.hidden = true;
    }
    setSetlistPanelVisible(true);
  }
}

document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => {
  syncPresetLibraryFeatureVisibility();
});

document.addEventListener("mixerPresetTabSelected", (event) => {
  const customEvent = event as CustomEvent<{ presetId?: string }>;
  const presetId = customEvent.detail?.presetId ?? "";
  if (!presetId) {
    return;
  }

  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  if (!preset) {
    return;
  }

  uiState.activePresetId = presetId;
  setFavoriteToggleState(presetId);
  updatePresetDropdownSelection();
  renderPresetUI(clonePreset(preset));
  updatePresetActionButtons();
});

function populatePresetFolderSelect(select: HTMLSelectElement | null, selectedId?: string | null): void {
  if (!select) return;

  const folders = sortPresetFoldersAlphabetically(uiState.presetFolders ?? []);
  const options: Array<{ id: string; label: string }> = [
    { id: PRESET_FOLDER_ALL_ID, label: "All Presets" },
  ];

  const buildOptions = (nodes: PresetFolder[], depth: number): void => {
    nodes.forEach((folder) => {
      const indent = "\u00A0".repeat(depth * 2);
      options.push({ id: folder.id, label: `${indent}${folder.name}` });
      if (folder.children?.length) {
        buildOptions(folder.children, depth + 1);
      }
    });
  };

  buildOptions(folders, 0);

  select.innerHTML = options
    .map((option) => `<option value="${option.id}">${option.label}</option>`)
    .join("");

  const resolved = selectedId ?? uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  select.value = resolved || PRESET_FOLDER_ALL_ID;
}

export function applyPresetFoldersFromBackend(folders: PresetFolder[], activeFolderId?: string | null): void {
  uiState.presetFolders = Array.isArray(folders) ? folders : [];
  uiState.activePresetFolderId = activeFolderId ?? PRESET_FOLDER_ALL_ID;
  ensurePresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

function ensureSetlists(): void {
  const stored = uiState.setlists ?? [];
  uiState.activeSetlistId = uiState.activeSetlistId || (stored[0]?.id ?? null);
}

function persistSetlists(): void {
  postMessage({
    type: "setSetlists",
    setlists: uiState.setlists ?? [],
    activeSetlistId: uiState.activeSetlistId ?? "",
    cursorIndex: uiState.setlistCursorIndex ?? 0,
  });
}

function setActiveSetlist(id: string | null): void {
  uiState.activeSetlistId = id;
  persistSetlists();
  renderSetlistPanel();
}

export function applySetlistsFromBackend(setlists: Setlist[], activeSetlistId?: string | null): void {
  uiState.setlists = Array.isArray(setlists) ? setlists : [];
  uiState.activeSetlistId = activeSetlistId ?? (uiState.setlists[0]?.id ?? null);
  renderSetlistPanel();
}

export function applySetlistCursorFromBackend(cursorIndex: number, _presetId?: string, activeSetlistId?: string): void {
  if (typeof activeSetlistId === "string" && activeSetlistId && activeSetlistId !== uiState.activeSetlistId) {
    uiState.activeSetlistId = activeSetlistId;
  }
  uiState.setlistCursorIndex = cursorIndex;
  renderSetlistPanel();
  // The backend applies the slot's preset itself and reports it via "presetLoaded";
  // loading it again from here would race that swap and leave both presets in the mixer.
}

async function selectSetlistSlot(index: number): Promise<void> {
  const activeSetlist = findSetlistById(uiState.activeSetlistId);
  if (!activeSetlist || index < 0 || index >= activeSetlist.slots.length) return;
  const presetId = activeSetlist.slots[index].presetId;
  if (!presetId) return;
  if (uiState.presetDirty && uiState.activePresetId && uiState.activePresetId !== presetId) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) return;
    setPresetDirty(false);
  }
  uiState.setlistCursorIndex = index;
  // "setSetlistCursor" is the single switch verb — the backend moves the cursor *and* swaps
  // the preset in, matching what a footswitch or MIDI program change does.
  postMessage({ type: "setSetlistCursor", cursorIndex: index });
  uiState.presetLoadingId = presetId;
  renderSetlistPanel();
  renderActivePreset();
}

function findSetlistById(id: string | null | undefined): Setlist | undefined {
  if (!id) {
    return undefined;
  }
  return (uiState.setlists ?? []).find((setlist) => setlist.id === id);
}

function isBankAvailable(bank: number, excludeId?: string): boolean {
  return !(uiState.setlists ?? []).some((setlist) => setlist.bank === bank && setlist.id !== excludeId);
}

export function createSetlist(name: string, bank?: number | null): Setlist | null {
  const trimmed = normalizeSetlistName(name);
  if (!trimmed) {
    showNotification("Setlist name required", "Enter a setlist name.");
    return null;
  }
  if (typeof bank === "number" && !isBankAvailable(bank)) {
    showNotification("Bank already used", "Only one setlist can use a bank number.");
    return null;
  }

  const newSetlist: Setlist = {
    id: generateResourceId(trimmed),
    name: trimmed,
    bank: typeof bank === "number" ? bank : null,
    slots: [],
  };
  uiState.setlists = uiState.setlists ?? [];
  uiState.setlists.push(newSetlist);
  persistSetlists();
  setActiveSetlist(newSetlist.id);
  return newSetlist;
}

function addPresetToSetlist(presetId: string): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.push({ presetId });
  persistSetlists();
  renderSetlistPanel();
}

export function assignPresetToActiveSetlistSlot(slotIndex: number, presetId: string): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist || slotIndex < 0 || !presetId.trim()) {
    return false;
  }

  while (setlist.slots.length <= slotIndex) {
    setlist.slots.push({ presetId: "" });
  }

  setlist.slots[slotIndex] = { presetId };
  persistSetlists();
  renderSetlistPanel();
  return true;
}

export function clearActiveSetlistSlot(slotIndex: number): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist || slotIndex < 0 || slotIndex >= setlist.slots.length) {
    return false;
  }
  setlist.slots[slotIndex] = { presetId: "" };
  persistSetlists();
  renderSetlistPanel();
  return true;
}

export function updateActiveSetlistDetails(name: string, bank?: number | null): boolean {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    showNotification("No setlist selected", "Select a setlist before editing it.");
    return false;
  }

  const trimmed = normalizeSetlistName(name);
  if (!trimmed) {
    showNotification("Setlist name required", "Enter a setlist name.");
    return false;
  }
  if (typeof bank === "number" && (!Number.isFinite(bank) || bank < 0)) {
    showNotification("Invalid bank", "Bank must be a non-negative number.");
    return false;
  }
  if (typeof bank === "number" && !isBankAvailable(bank, setlist.id)) {
    showNotification("Bank already used", "Only one setlist can use a bank number.");
    return false;
  }

  setlist.name = trimmed;
  setlist.bank = typeof bank === "number" ? bank : null;
  persistSetlists();
  renderSetlistPanel();
  return true;
}

export function deleteActiveSetlist(): boolean {
  const setlists = uiState.setlists ?? [];
  const activeIndex = setlists.findIndex((setlist) => setlist.id === uiState.activeSetlistId);
  if (activeIndex < 0) {
    showNotification("No setlist selected", "Select a setlist before deleting it.");
    return false;
  }

  setlists.splice(activeIndex, 1);
  const nextActive = setlists[activeIndex] ?? setlists[activeIndex - 1] ?? null;
  uiState.activeSetlistId = nextActive?.id ?? null;
  uiState.setlistCursorIndex = 0;
  persistSetlists();
  renderSetlistPanel();
  return true;
}

function moveSetlistSlot(fromIndex: number, toIndex: number): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  if (fromIndex < 0 || toIndex < 0 || fromIndex >= setlist.slots.length || toIndex >= setlist.slots.length) {
    return;
  }
  if (fromIndex === toIndex) {
    return;
  }
  const [slot] = setlist.slots.splice(fromIndex, 1);
  setlist.slots.splice(toIndex, 0, slot);
  persistSetlists();
  renderSetlistPanel();
}

function removeSetlistSlot(index: number): void {
  const setlist = findSetlistById(uiState.activeSetlistId);
  if (!setlist) {
    return;
  }
  setlist.slots.splice(index, 1);
  persistSetlists();
  renderSetlistPanel();
}

function renderSetlistPanel(): void {
  if (!setlistListElement || !setlistSlotsElement || !setlistEditorHeader) {
    return;
  }

  const setlists = uiState.setlists ?? [];
  setlistListElement.innerHTML = setlists.length
    ? setlists
        .map((setlist) => {
          const active = setlist.id === uiState.activeSetlistId ? "active" : "";
          const bankLabel = typeof setlist.bank === "number" ? `Bank ${setlist.bank}` : "No Bank";
          return `
            <div class="setlist-item ${active}" data-setlist-id="${setlist.id}">
              <span>${setlist.name}</span>
              <span class="bank-pill">${bankLabel}</span>
            </div>
          `;
        })
        .join("")
    : '<div class="preset-library-empty">No setlists yet.</div>';

  setlistListElement.querySelectorAll<HTMLElement>(".setlist-item").forEach((item) => {
    item.addEventListener("click", () => {
      const id = item.dataset.setlistId ?? null;
      setActiveSetlist(id);
    });
  });

  const activeSetlist = findSetlistById(uiState.activeSetlistId);
  if (!activeSetlist) {
    setlistEditorHeader.textContent = "Select a setlist";
    setlistSlotsElement.innerHTML = "";
    return;
  }

  setlistEditorHeader.textContent = `${activeSetlist.name}${typeof activeSetlist.bank === "number" ? ` (Bank ${activeSetlist.bank})` : ""}`;
  if (!activeSetlist.slots.length) {
    setlistSlotsElement.innerHTML = '<div class="preset-library-empty">Drop presets to add slots.</div>';
  } else {
    const cursorIdx = uiState.setlistCursorIndex ?? 0;
    setlistSlotsElement.innerHTML = activeSetlist.slots
      .map((slot, index) => {
        const presetName = uiState.presetCache.get(slot.presetId)?.name ?? slot.presetId;
        const isActive = index === cursorIdx ? " active" : "";
        return `
          <div class="setlist-slot${isActive}" data-slot-index="${index}" draggable="true">
            <span class="setlist-slot-title">${presetName}</span>
            <button class="setlist-slot-remove" data-slot-index="${index}" type="button">×</button>
          </div>
        `;
      })
      .join("");
  }

  setlistSlotsElement.querySelectorAll<HTMLButtonElement>(".setlist-slot-remove").forEach((button) => {
    button.addEventListener("click", (e) => {
      e.stopPropagation();
      const index = Number(button.dataset.slotIndex ?? -1);
      if (index >= 0) {
        removeSetlistSlot(index);
      }
    });
  });

  setlistSlotsElement.querySelectorAll<HTMLElement>(".setlist-slot").forEach((slotEl) => {
    slotEl.addEventListener("click", () => {
      const index = Number(slotEl.dataset.slotIndex ?? -1);
      if (index >= 0) {
        void selectSetlistSlot(index);
      }
    });

    slotEl.addEventListener("dragstart", (event) => {
      const index = slotEl.dataset.slotIndex ?? "";
      event.dataTransfer?.setData("application/x-setlist-slot", index);
      event.dataTransfer?.setDragImage(slotEl, 20, 20);
    });

    slotEl.addEventListener("dragover", (event) => {
      event.preventDefault();
    });

    slotEl.addEventListener("drop", (event) => {
      event.preventDefault();
      const fromIndex = Number(event.dataTransfer?.getData("application/x-setlist-slot") ?? -1);
      const toIndex = Number(slotEl.dataset.slotIndex ?? -1);
      if (fromIndex >= 0 && toIndex >= 0) {
        moveSetlistSlot(fromIndex, toIndex);
      }
    });
  });
}

function setSetlistExpanded(expanded: boolean): void {
  if (!setlistCollapsible || !setlistToggle || !setlistPanel) {
    return;
  }
  setlistCollapsible.classList.toggle("open", expanded);
  setlistToggle.setAttribute("aria-expanded", expanded ? "true" : "false");
  setlistPanel.setAttribute("aria-hidden", expanded ? "false" : "true");
}

function setActivePresetFolder(folderId: string): void {
  uiState.activePresetFolderId = folderId;
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
  syncPresetFolderToolbarState();
}

function addPresetToFolder(folderId: string, presetId: string): void {
  const folders = uiState.presetFolders ?? [];
  const folder = findFolderById(folders, folderId);
  if (!folder) {
    return;
  }
  if (!folder.presetIds.includes(presetId)) {
    folder.presetIds.push(presetId);
  }
}

function movePresetToFolder(presetId: string, folderId: string): void {
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    toggleFavoritePreset(presetId);
    return;
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return;
  }
  const folders = uiState.presetFolders ?? [];
  removePresetFromFolders(folders, presetId);
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    addPresetToFolder(folderId, presetId);
  }
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

function movePresetFolder(folderId: string, targetParentId: string): void {
  if (!folderId || isVirtualPresetFolderId(folderId)) {
    return;
  }
  if (targetParentId === PRESET_FOLDER_FAVORITES_ID || targetParentId === PRESET_FOLDER_RECENTS_ID) {
    return;
  }

  const folders = uiState.presetFolders ?? [];
  const result = findFolderWithParent(folders, folderId);
  if (!result) {
    return;
  }

  if (targetParentId && targetParentId !== PRESET_FOLDER_ALL_ID) {
    if (folderId === targetParentId) {
      return;
    }
    if (isDescendantFolder(result.folder, targetParentId)) {
      return;
    }
  }

  if (result.parent) {
    result.parent.children = (result.parent.children ?? []).filter((child) => child.id !== folderId);
  } else {
    uiState.presetFolders = (uiState.presetFolders ?? []).filter((folder) => folder.id !== folderId);
  }

  if (targetParentId && targetParentId !== PRESET_FOLDER_ALL_ID) {
    const targetParent = findFolderById(folders, targetParentId);
    if (!targetParent) {
      return;
    }
    targetParent.children = targetParent.children ?? [];
    targetParent.children.push(result.folder);
  } else {
    uiState.presetFolders = uiState.presetFolders ?? [];
    uiState.presetFolders.push(result.folder);
  }

  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

function getFilteredPresets(query: string): Preset[] {
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

function createFolder(name: string, parentId?: string): boolean {
  const trimmed = name.trim();
  if (!trimmed) {
    showNotification("Folder name required", "Enter a folder name to create.");
    return false;
  }

  const newFolder: PresetFolder = {
    id: generateResourceId(trimmed),
    name: trimmed,
    children: [],
    presetIds: [],
  };

  if (parentId && parentId !== PRESET_FOLDER_ALL_ID) {
    const parent = findFolderById(uiState.presetFolders ?? [], parentId);
    if (parent) {
      parent.children = parent.children ?? [];
      parent.children.push(newFolder);
    } else {
      uiState.presetFolders?.push(newFolder);
    }
  } else {
    uiState.presetFolders?.push(newFolder);
  }

  persistPresetFolders();
  setActivePresetFolder(newFolder.id);
  showNotification("Folder created", trimmed);
  return true;
}

function renameFolder(folderId: string, nextName: string): boolean {
  const trimmed = nextName.trim();
  if (!trimmed) {
    showNotification("Folder name required", "Enter a folder name to rename.");
    return false;
  }

  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    showNotification("Folder not found", "Select a valid folder to rename.");
    return false;
  }

  folder.name = trimmed;
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
  showNotification("Folder renamed", trimmed);
  return true;
}

function deleteFolderById(folderId: string): boolean {
  const folders = uiState.presetFolders ?? [];
  const result = findFolderWithParent(folders, folderId);
  if (!result) {
    showNotification("Folder not found", "Select a valid folder to delete.");
    return false;
  }

  if (result.parent) {
    result.parent.children = (result.parent.children ?? []).filter((child) => child.id !== folderId);
  } else {
    uiState.presetFolders = (uiState.presetFolders ?? []).filter((folder) => folder.id !== folderId);
  }

  setActivePresetFolder(PRESET_FOLDER_ALL_ID);
  showNotification("Folder deleted", result.folder.name);
  return true;
}

function getCurrentRealPresetFolder(): PresetFolder | null {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  if (isVirtualPresetFolderId(activeFolderId)) {
    return null;
  }
  return findFolderById(uiState.presetFolders ?? [], activeFolderId) ?? null;
}

function syncPresetFolderToolbarState(): void {
  const activeFolder = getCurrentRealPresetFolder();
  const disabled = !activeFolder;

  if (presetFolderRenameButton) {
    presetFolderRenameButton.disabled = disabled;
  }

  if (presetFolderDeleteButton) {
    presetFolderDeleteButton.disabled = disabled;
  }
}

export function bindLoadButtons(): void {
  const loadModelBtn = document.getElementById("load-model-btn");
  const loadIRBtn = document.getElementById("load-ir-btn");

  if (loadModelBtn) {
    loadModelBtn.addEventListener("click", () => {
      postMessage({ type: "browseModel" });
      appendLog("browseModel → requested");
    });
  }

  if (loadIRBtn) {
    loadIRBtn.addEventListener("click", () => {
      postMessage({ type: "browseIR" });
      appendLog("browseIR → requested");
    });
  }
}

export function loadModelFromPath(filePath: string): void {
  postMessage({
    type: "loadModel",
    filePath,
  });
  appendLog(`loadModel → ${filePath}`);
}

export function loadIRFromPath(filePath: string): void {
  postMessage({
    type: "loadIR",
    filePath,
  });
  appendLog(`loadIR → ${filePath}`);
}

export function requestSignalPathTest(): void {
  clearNotification();
  postMessage({
    type: "runSignalPathTest",
    frequency: 440,
    duration: 1.0,
  });
}

function renderPresetUI(preset: Preset | null): void {
  syncPresetFolderToolbarState();

  const visiblePresets = uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID
    ? uiState.filteredPresets
    : sortPresetsAlphabetically(uiState.filteredPresets);

  renderPresetList(visiblePresets, uiState.activePresetId, async (presetId) => {
    const override = presetChooserOverride;
    if (override) {
      presetChooserOverride = null;
      await override(presetId);
      closePresetLibraryPopover();
      return;
    }
    await applyPresetFromLibrary(presetId);
  }, {
    folders: sortPresetFoldersAlphabetically(uiState.presetFolders ?? []),
    activeFolderId: uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
    onSelectFolder: setActivePresetFolder,
    onMovePresetToFolder: movePresetToFolder,
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

export function renderActivePreset(): void {
  const active = getActivePresetForRender();
  renderPresetUI(active);
  renderMixerPanel();
}

export function filterPresets(query: string): void {
  uiState.filteredPresets = getFilteredPresets(query);
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function initializePresetTagFilterBar(): void {
  const bar = document.getElementById("preset-tag-filter-bar");
  if (!bar) return;
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

function hasGraphNodes(preset: Preset | null | undefined): boolean {
  return Boolean(preset?.graph && Array.isArray(preset.graph.nodes) && preset.graph.nodes.length > 0);
}

async function loadPresetMetadata(presetId: string): Promise<Preset> {
  if (uiState.presetCache.has(presetId)) {
    const cached = stripLegacyGlobals(clonePreset(uiState.presetCache.get(presetId) ?? null) as Preset);
    if (hasGraphNodes(cached)) {
      return cached;
    }
    const backendPreset = await requestPresetFromBackend(presetId);
    const resolved = stripLegacyGlobals(backendPreset);
    uiState.presetCache.set(resolved.id, resolved);
    return clonePreset(resolved) as Preset;
  }

  const localPreset = uiState.presets.find((preset) => preset.id === presetId);
  if (localPreset) {
    const cleaned = stripLegacyGlobals(localPreset);
    uiState.presetCache.set(localPreset.id, cleaned);
    if (!hasGraphNodes(cleaned)) {
      const backendPreset = await requestPresetFromBackend(presetId);
      const resolved = stripLegacyGlobals(backendPreset);
      uiState.presetCache.set(resolved.id, resolved);
      return clonePreset(resolved) as Preset;
    }
    return clonePreset(cleaned) as Preset;
  }

  if (!REMOTE_BASE_URL) {
    throw new Error("Remote preset service is not configured.");
  }

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");
  const response = await fetch(`${baseUrl}/presets/${encodeURIComponent(presetId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch preset ${presetId}: ${response.status}`);
  }

  const data = await response.json();
  const preset = Array.isArray(data) ? data[0] : data;
  if (!preset) {
    throw new Error(`Preset ${presetId} not found`);
  }

  const cleaned = stripLegacyGlobals(preset as Preset);
  uiState.presetCache.set(cleaned.id, cleaned);
  return clonePreset(cleaned) as Preset;
}

async function enrichAttachment(attachment: Attachment): Promise<Attachment> {
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment, REMOTE_BASE_URL);
  if (!url || !isRemoteUrl(url)) {
    return attachment;
  }

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch attachment from ${url}`);
  }

  const buffer = await response.arrayBuffer();
  return { ...attachment, data: arrayBufferToBase64(buffer) };
}

export async function applyPresetFromLibrary(presetId: string): Promise<void> {
  if (uiState.presetDirty && uiState.activePresetId && uiState.activePresetId !== presetId) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) {
      return;
    }
    setPresetDirty(false);
  }
  try {
    clearNotification();
    const preset = await loadPresetMetadata(presetId);
    const attachments = await Promise.all((preset.attachments ?? []).map(enrichAttachment));
    const presetWithGlobals = preset as Preset & { globalSignalChain?: GlobalSignalChainConfig };
    const hasGlobalChain = Boolean(presetWithGlobals.globalSignalChain);
    const resolvedChain = hasGlobalChain
      ? JSON.parse(JSON.stringify(presetWithGlobals.globalSignalChain)) as GlobalSignalChainConfig
      : null;
    const presetPayload: Preset = {
      ...stripLegacyGlobals(preset),
      attachments,
      ...(hasGlobalChain && resolvedChain ? { globalSignalChain: resolvedChain } : {}),
    };
    const sceneId = normalizePresetScenes(presetPayload, uiState.activePresetSceneId ?? undefined);
    uiState.activePresetSceneId = sceneId;

    if (hasGlobalChain && resolvedChain) {
      uiState.globalSignalChain = resolvedChain;
    }
    uiState.presetCache.set(presetPayload.id, clonePreset(presetPayload));
    uiState.activePresetId = presetPayload.id;
    setActivePresetIsNew(false);
    setActivePresetSnapshot(presetPayload);
    setActivePresetDraft(presetPayload);
    setPresetDirty(false);
    setFavoriteToggleState(presetPayload.id);
    updatePresetDropdownSelection();
    // Set loading state BEFORE rendering so all render functions (list, details,
    // signal path bar) see it and bake the loading class/overlay into their output.
    uiState.presetLoadingId = presetPayload.id;
    renderPresetUI(clonePreset(presetPayload));
    updatePresetActionButtons();
    postMessage({
      type: "loadPreset",
      preset: presetPayload,
      ...(sceneId ? { sceneId } : {}),
    });
    recordPresetInHistory(presetPayload.id);
  } catch (error) {
    uiState.presetLoadingId = null;
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

// ── Preset navigation history ────────────────────────────────────────────────
// Tracks which presets were loaded, so the user can step back to the one they
// were just on after auditioning something else. Only preset IDs are held, so a
// deep history costs nothing; the cap keeps it to a useful working window rather
// than a full session log. This is navigation history, not an edit undo stack —
// it does not restore unsaved parameter tweaks.

const presetHistory: string[] = [];
let presetHistoryIndex = -1;
let replayingPresetHistory = false;
const MAX_PRESET_HISTORY = 10;

function recordPresetInHistory(presetId: string): void {
  // Undo/redo re-apply presets through the same path; those must move the cursor,
  // not rewrite the history they are walking.
  if (replayingPresetHistory) {
    return;
  }
  if (presetHistory[presetHistoryIndex] === presetId) {
    return;
  }

  // Branching from a past entry drops the forward steps that are no longer reachable.
  presetHistory.length = presetHistoryIndex + 1;
  presetHistory.push(presetId);
  if (presetHistory.length > MAX_PRESET_HISTORY) {
    presetHistory.shift(); // oldest falls off; cursor still points at the newest
  } else {
    presetHistoryIndex++;
  }
  updatePresetHistoryButtons();
}

function updatePresetHistoryButtons(): void {
  if (presetUndoBtn) {
    presetUndoBtn.disabled = presetHistoryIndex <= 0;
  }
  if (presetRedoBtn) {
    presetRedoBtn.disabled = presetHistoryIndex >= presetHistory.length - 1;
  }
}

async function stepPresetHistory(offset: -1 | 1): Promise<void> {
  const targetIndex = presetHistoryIndex + offset;
  if (targetIndex < 0 || targetIndex >= presetHistory.length) {
    return;
  }
  const targetId = presetHistory[targetIndex];
  if (!targetId) {
    return;
  }

  replayingPresetHistory = true;
  try {
    await applyPresetFromLibrary(targetId);
  } finally {
    replayingPresetHistory = false;
  }

  // Only advance the cursor once the load actually succeeded — applyPresetFromLibrary
  // swallows failures and can also be cancelled by the unsaved-changes prompt.
  if (uiState.activePresetId === targetId) {
    presetHistoryIndex = targetIndex;
  }
  updatePresetHistoryButtons();
  updatePresetDropdownSelection();
}

export async function loadPresetIndex(): Promise<void> {
  try {
    if (!REMOTE_BASE_URL) {
      throw new Error("Remote preset service disabled");
    }

    const response = await fetch(`${REMOTE_BASE_URL.replace(/\/$/, "")}/presets`);
    if (!response.ok) {
      throw new Error(`Failed to fetch presets index: ${response.status}`);
    }

    const data = await response.json();
    const presets = Array.isArray(data) ? data : data.presets ?? [];
    const basePresets = presets.length ? presets : getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  } catch (error) {
    console.error("Failed to load preset index", error);
    const basePresets = getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }
}

export async function initializePresets(): Promise<void> {
  await initializeDataLibraries();

  if (REMOTE_BASE_URL) {
    await loadPresetIndex();
  } else {
    const basePresets = getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => uiState.presetCache.set(preset.id, preset));
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }

  // Backend-backed user data
  postMessage({ type: "getPresetList" });
  postMessage({ type: "getPresetFolders" });
  postMessage({ type: "getPresetFavorites" });
  postMessage({ type: "getPresetRatings" });
  postMessage({ type: "getSetlists" });
  postMessage({ type: "getEffectPresets" });

  ensurePresetFolders(false);  // Don't persist — backend response will arrive with saved data
  ensureSetlists();
  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  renderSetlistPanel();
  updatePresetDropdownSelection();
  setFavoriteToggleState(uiState.activePresetId);

  populatePresetDropdown();
  postMessage({ type: "requestState" });
}

export function populatePresetDropdown(): void {
  updatePresetDropdownSelection();
}

export function updatePresetDropdownSelection(): void {
  if (!presetChooserLabel) return;
  const preset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  presetChooserLabel.textContent = preset?.name ?? "Select Preset";
}

function getActivePresetIndex(): number {
  if (!uiState.activePresetId) return -1;
  return uiState.presets.findIndex((p) => p.id === uiState.activePresetId);
}

function openPublishPresetFlow(): void {
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

export async function selectPreviousPreset(): Promise<void> {
  if (!uiState.presets.length) return;

  let index = getActivePresetIndex();
  if (index <= 0) {
    index = uiState.presets.length - 1;
  } else {
    index--;
  }

  const preset = uiState.presets[index];
  if (preset) {
    await applyPresetFromLibrary(preset.id);
    updatePresetDropdownSelection();
  }
}

export async function selectNextPreset(): Promise<void> {
  if (!uiState.presets.length) return;

  let index = getActivePresetIndex();
  if (index < 0 || index >= uiState.presets.length - 1) {
    index = 0;
  } else {
    index++;
  }

  const preset = uiState.presets[index];
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
        showNotification("No preset", "Select a preset to favorite");
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
    void stepPresetHistory(-1);
  });

  presetRedoBtn?.addEventListener("click", () => {
    void stepPresetHistory(1);
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

  if (setlistSlotsElement) {
    setlistSlotsElement.addEventListener("dragover", (event) => {
      event.preventDefault();
      setlistSlotsElement.classList.add("drag-over");
    });
    setlistSlotsElement.addEventListener("dragleave", () => {
      setlistSlotsElement.classList.remove("drag-over");
    });
    setlistSlotsElement.addEventListener("drop", (event) => {
      event.preventDefault();
      setlistSlotsElement.classList.remove("drag-over");
      const presetId = event.dataTransfer?.getData("text/plain") ?? "";
      if (presetId) {
        addPresetToSetlist(presetId);
        setSetlistExpanded(true);
      }
    });
  }

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

// Save preset modal helpers
type SavePresetModalMode = "overwrite" | "save-as" | "save-new";

function configureSavePresetModalLabels(mode: SavePresetModalMode): void {
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

function isActivePresetNewDraft(): boolean {
  return Boolean(uiState.activePresetIsNew && uiState.activePresetId);
}

function populateSavePresetModalFields(preset: Preset | null): void {
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

function resolvePresetModalFolderId(presetId?: string | null): string {
  if (presetId) {
    const presetFolder = findFolderForPreset(uiState.presetFolders ?? [], presetId);
    if (presetFolder?.id) {
      return presetFolder.id;
    }
  }

  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  return isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
}

function openCreatePresetModal(mode: Extract<SavePresetModalMode, "save-as" | "save-new">, sourcePreset: Preset | null): void {
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

export function createDefaultPreset(): void {
  const newPreset = createEmptyPresetV2();
  uiState.activePresetSceneId = normalizePresetScenes(newPreset);
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const selectedFolderId = isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;

  uiState.presets.unshift(newPreset);
  uiState.filteredPresets = uiState.presets.slice();
  uiState.presetCache.set(newPreset.id, newPreset);
  if (selectedFolderId) {
    movePresetToFolder(newPreset.id, selectedFolderId);
  }
  uiState.activePresetId = newPreset.id;
  setActivePresetIsNew(true);
  setActivePresetSnapshot(newPreset);
  setActivePresetDraft(newPreset);
  setPresetDirty(false);
  populatePresetDropdown();
  renderPresetUI(clonePreset(newPreset));
  showNotification("Preset created", newPreset.name);
  updatePresetActionButtons();
  postMessage({
    type: "loadPreset",
    preset: newPreset,
    ...(uiState.activePresetSceneId ? { sceneId: uiState.activePresetSceneId } : {}),
  });
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
      uiState.activePresetSceneId = sceneId;
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
      uiState.presetCache.set(editingPresetId, updatedPreset);
      const index = uiState.presets.findIndex((p) => p.id === editingPresetId);
      if (index >= 0) {
        uiState.presets[index] = updatedPreset;
      }
      uiState.filteredPresets = uiState.presets.slice();
      populatePresetDropdown();
      renderPresetUI(clonePreset(updatedPreset));
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
  uiState.activePresetSceneId = sceneId;
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
  uiState.filteredPresets = uiState.presets.slice();
  uiState.presetCache.set(newPreset.id, newPreset);
  if (selectedFolderId) {
    movePresetToFolder(newPreset.id, selectedFolderId);
  }
  uiState.activePresetId = newPreset.id;
  populatePresetDropdown();
  renderPresetUI(clonePreset(newPreset));
  closeSavePresetModal();
  showNotification("Preset saved", newPreset.name);
  setActivePresetIsNew(false);
  setActivePresetSnapshot(newPreset);
  setActivePresetDraft(newPreset);
  setPresetDirty(false);
  updatePresetActionButtons();
}

function getPresetTagsPickerValue(): string[] {
  const picker = document.getElementById("preset-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

function setPresetTagsPickerValue(tags: string[]): void {
  const picker = document.getElementById("preset-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

function formatPeakDb(value: number | null | undefined): string {
  if (value == null || !isFinite(value)) return "\u2014";
  return `${value.toFixed(1)} dBFS`;
}

export function refreshSavePresetModalPeakInfoIfOpen(): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal || modal.style.display === "none" || modal.style.display === "") return;
  updateSavePresetModalPeakInfo(modal);
}

function updateSavePresetModalPeakInfo(modal: HTMLElement): void {
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

// Delete preset via backend storage
export function deletePresetFromBackend(presetId: string): boolean {
  if (!presetId) return false;
  postMessage({ type: "deletePreset", presetId });
  return true;
}

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
function canModifyPreset(presetId: string | null): boolean {
  if (getPresetArchiveSessionState()) {
    return Boolean(presetId);
  }
  if (isUserPreset(presetId)) {
    return true;
  }
  // Allow factory preset modification when feature flag is enabled
  return isFeatureEnabled(Features.FactoryPresetArchives);
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
    const index = uiState.presets.findIndex((p) => p.id === activePresetId);
    if (index >= 0) {
      uiState.presets.splice(index, 1);
    }
    removePresetFromFolders(uiState.presetFolders ?? [], activePresetId);
    persistPresetFolders();
    const favorites = loadFavoritePresetIds();
    if (favorites.delete(activePresetId)) {
      saveFavoritePresetIds(favorites);
    }
    uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
    uiState.presetCache.delete(activePresetId);

    // Select first preset if available
    if (uiState.presets.length > 0) {
      uiState.activePresetId = uiState.presets[0].id;
      void applyPresetFromLibrary(uiState.activePresetId);
    } else {
      uiState.activePresetId = null;
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
  uiState.presetCache.set(activePresetId, updatedPreset);
  const index = uiState.presets.findIndex((p) => p.id === activePresetId);
  if (index >= 0) {
    uiState.presets[index] = updatedPreset;
  }

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

// Update preset action button states
export function updatePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("preset-save-btn") as HTMLButtonElement | null;
  const saveAsBtn = document.getElementById("preset-save-as-btn") as HTMLButtonElement | null;
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
      ? (isNewPresetDraft ? "Save preset details first" : "Save Preset")
      : "Cannot overwrite factory presets";
    saveBtn.classList.toggle("preset-action-btn-unsaved", Boolean(uiState.presetDirty));
  }
  if (saveAsBtn) {
    const showSaveAs = hasActivePreset && !isNewPresetDraft;
    saveAsBtn.hidden = !showSaveAs;
    saveAsBtn.disabled = !showSaveAs;
    saveAsBtn.title = "Save this preset as a new copy";
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

// Supply the real redraw to the modules that can only request one — see
// presets/refresh.ts for why the indirection exists.
setPresetLibraryRefresher((activePreset) => {
  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  populatePresetDropdown();
  if (activePreset) {
    renderPresetUI(clonePreset(activePreset));
  }
  updatePresetDropdownSelection();
  updatePresetActionButtons();
});

function updatePresetFolderExportButtons(): void {
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
