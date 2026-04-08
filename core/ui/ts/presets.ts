import { appendLog } from "./logging.js";
import { clearNotification, showNotification } from "./notifications.js";
import { renderPresetDetails, renderPresetList, renderMixerPanel } from "./views.js";
import { clonePreset, uiState, DEFAULT_GLOBAL_SIGNAL_CHAIN, getActivePresetForRender, setActivePresetDraft, setActivePresetSnapshot, setPresetDirty } from "./state.js";
import { buildAttachments, buildAttachmentsFromPreset, getDefaultPresets, initializeDataLibraries, REMOTE_BASE_URL } from "./dataLibraries.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl, sha256HexFromBase64 } from "./utils.js";
import { buildArchiveFileNameWithHash, generateResourceId, requestResourceData, sanitizeFilename } from "./archiveUtils.js";
import type { Preset, Attachment, BlendDefinition, ResourceRef, LibraryResource, PresetFolder, Setlist, GraphNode, SignalGraph, ToneSharingOriginMetadata } from "./types.js";
import { createEmptyPresetV2, migratePresetNodeTypes } from "./presetV2.js";
import { bindDemoAudioControls } from "./demoAudio.js";
import { postMessage, setAppSetting } from "./bridge.js";
import { renderSignalPathBar } from "./signalPath.js";
import { showConfirm } from "./dialogs.js";
import { isToneSharingSignedIn, openToneSharingPublishPresetModal, registerInstalledToneSharingPack, syncToneSharingFavoriteForPreset, syncToneSharingRatingForPreset } from "./toneSharingPanel.js";
import type { InstalledPackMetadata } from "./toneSharingPanel.js";
import { downloadTone3000ResourceByReference, saveTone3000ApiKey } from "./tone3000.js";
import { switchMainPanel } from "./navigation.js";
import { activateLibraryTab } from "./settings.js";
import { updateUiSettings } from "./windowSettings.js";
import { normalizePresetScenes } from "./presetScenes.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";

const presetChooserLabel = document.getElementById("preset-chooser-label") as HTMLButtonElement | null;
const presetFavoriteToggle = document.getElementById("preset-favorite");
const prevPresetBtn = document.getElementById("prev-preset");
const nextPresetBtn = document.getElementById("next-preset");
const randomPresetBtn = document.getElementById("preset-random-btn");
const presetSearchElement = document.getElementById("preset-search") as HTMLInputElement | null;
const presetSelector = document.getElementById("preset-selector");
const presetLibraryPopover = document.getElementById("preset-library-popover");
const presetLibraryTabs = document.querySelector(".preset-library-tabs") as HTMLElement | null;
const presetLibraryPresetsPanel = document.getElementById("preset-library-presets-panel") as HTMLElement | null;
const presetLibraryMultiRigTab = document.getElementById("preset-lib-tab-multi-rig") as HTMLButtonElement | null;
const presetLibraryPresetsTab = document.getElementById("preset-lib-tab-presets") as HTMLButtonElement | null;
const presetLibraryMultiRigPanel = document.getElementById("preset-library-multi-rig-panel") as HTMLElement | null;
const presetFolderNameInput = document.getElementById("preset-folder-name") as HTMLInputElement | null;
const presetFolderAddButton = document.getElementById("preset-folder-add");
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

const PRESET_FOLDER_ALL_ID = "__all__";
const PRESET_FOLDER_FAVORITES_ID = "__favorites__";
const PRESET_FOLDER_RECENTS_ID = "__recents__";
const PRESET_REQUEST_TIMEOUT_MS = 5000;
const PRESET_RECENTS_SETTING = "presets.recents";
const MAX_RECENT_PRESETS = 4;

const activeTagFilters = new Set<string>();
const presetNameCollator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

const pendingPresetRequests = new Map<string, {
  resolve: (preset: Preset) => void;
  reject: (error: Error) => void;
  timeoutId: number;
}>();

function normalizeFolderName(name: string): string {
  return name.trim().toLowerCase();
}

function normalizeSetlistName(name: string): string {
  return name.trim();
}

function comparePresetNames(left: Preset, right: Preset): number {
  const leftName = left.name?.trim() || left.id;
  const rightName = right.name?.trim() || right.id;
  const nameComparison = presetNameCollator.compare(leftName, rightName);
  if (nameComparison !== 0) {
    return nameComparison;
  }
  return presetNameCollator.compare(left.id, right.id);
}

function compareFolderNames(left: PresetFolder, right: PresetFolder): number {
  const nameComparison = presetNameCollator.compare(left.name.trim(), right.name.trim());
  if (nameComparison !== 0) {
    return nameComparison;
  }
  return presetNameCollator.compare(left.id, right.id);
}

function sortPresetsAlphabetically(presets: Preset[]): Preset[] {
  return [...presets].sort(comparePresetNames);
}

function sortPresetFoldersAlphabetically(folders: PresetFolder[]): PresetFolder[] {
  return folders
    .map((folder) => ({
      ...folder,
      children: sortPresetFoldersAlphabetically(folder.children ?? []),
      presetIds: [...(folder.presetIds ?? [])],
    }))
    .sort(compareFolderNames);
}

function isVirtualPresetFolderId(folderId: string | null | undefined): boolean {
  return folderId === PRESET_FOLDER_ALL_ID
    || folderId === PRESET_FOLDER_FAVORITES_ID
    || folderId === PRESET_FOLDER_RECENTS_ID;
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

function stripLegacyGlobals(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globals;
  delete (cleaned as Record<string, unknown>).global;
  return cleaned;
}

function stripGlobalSignalChainForSave(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globalSignalChain;
  if (Array.isArray(cleaned.scenes) && cleaned.scenes.length > 0) {
    delete (cleaned as Record<string, unknown>).graph;
  }
  return cleaned;
}

function cloneDefaultGlobalSignalChain(): import("./types.js").GlobalSignalChainConfig {
  return JSON.parse(JSON.stringify(DEFAULT_GLOBAL_SIGNAL_CHAIN)) as import("./types.js").GlobalSignalChainConfig;
}

function resolveGlobalSignalChain(preset: Preset): import("./types.js").GlobalSignalChainConfig {
  const chain = (preset as Preset & { globalSignalChain?: import("./types.js").GlobalSignalChainConfig }).globalSignalChain;
  if (chain) {
    return JSON.parse(JSON.stringify(chain)) as import("./types.js").GlobalSignalChainConfig;
  }
  if (uiState.globalSignalChain) {
    return JSON.parse(JSON.stringify(uiState.globalSignalChain)) as import("./types.js").GlobalSignalChainConfig;
  }
  return cloneDefaultGlobalSignalChain();
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

function loadFavoritePresetIds(): Set<string> {
  return uiState.presetFavorites ? new Set(uiState.presetFavorites) : new Set();
}

function normalizeRecentPresetIds(value: unknown): string[] {
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

function loadRecentPresetIds(): string[] {
  return normalizeRecentPresetIds(uiState.uiSettings?.presetRecents);
}

function saveRecentPresetIds(ids: string[]): void {
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

function getRecentPresets(): Preset[] {
  return loadRecentPresetIds()
    .map((presetId) => uiState.presetCache.get(presetId) ?? uiState.presets.find((preset) => preset.id === presetId) ?? null)
    .filter((preset): preset is Preset => Boolean(preset))
    .map((preset) => clonePreset(preset));
}

function trackRecentPreset(presetId: string | null | undefined): void {
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

function saveFavoritePresetIds(ids: Set<string>): void {
  uiState.presetFavorites = new Set(ids);
  postMessage({ type: "setPresetFavorites", favorites: Array.from(ids) });
}

function isPresetFavorite(presetId: string): boolean {
  return loadFavoritePresetIds().has(presetId);
}

function setFavoriteToggleState(presetId: string | null): void {
  if (!presetFavoriteToggle) {
    return;
  }
  const active = presetId ? isPresetFavorite(presetId) : false;
  presetFavoriteToggle.textContent = active ? "♥" : "♡";
  presetFavoriteToggle.classList.toggle("active", active);
}

function toggleFavoritePreset(presetId: string): void {
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
    filterPresets(presetSearchElement?.value ?? "");
  }
}

function loadPresetRatings(): Record<string, number> {
  return uiState.presetRatings ? { ...uiState.presetRatings } : {};
}

function savePresetRatings(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  postMessage({ type: "setPresetRatings", ratings });
}

function getPresetRating(presetId: string): number | null {
  const ratings = loadPresetRatings();
  const rating = ratings[presetId];
  return typeof rating === "number" && rating >= 1 && rating <= 5 ? rating : null;
}

function setPresetRating(presetId: string, rating: number | null): void {
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
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
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
    uiState.appSettings[PRESET_RECENTS_SETTING] = normalized as unknown as import("./types.js").AppSettingValue;
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
  syncPresetLibraryFeatureVisibility();
  presetLibraryPopover.classList.add("open");
  presetLibraryPopover.setAttribute("aria-hidden", "false");
  presetChooserLabel?.setAttribute("aria-expanded", "true");
}

function closePresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  presetLibraryPopover.classList.remove("open");
  presetLibraryPopover.setAttribute("aria-hidden", "true");
  presetChooserLabel?.setAttribute("aria-expanded", "false");
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

export function syncPresetLibraryFeatureVisibility(): void {
  const multiRigEnabled = isFeatureEnabled(Features.MultiRig);

  presetLibraryPopover?.classList.toggle("preset-library-popover-simple", !multiRigEnabled);

  if (presetLibraryTabs) {
    presetLibraryTabs.hidden = !multiRigEnabled;
    presetLibraryTabs.setAttribute("aria-hidden", String(!multiRigEnabled));
  }

  if (presetLibraryMultiRigTab) {
    presetLibraryMultiRigTab.hidden = !multiRigEnabled;
    presetLibraryMultiRigTab.setAttribute("aria-hidden", String(!multiRigEnabled));
    presetLibraryMultiRigTab.tabIndex = multiRigEnabled ? 0 : -1;
  }

  if (!multiRigEnabled) {
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

function loadPresetFoldersFromState(): PresetFolder[] {
  return uiState.presetFolders ? sortPresetFoldersAlphabetically(uiState.presetFolders) : [];
}

function savePresetFoldersToBackend(folders: PresetFolder[], activeFolderId?: string | null): void {
  postMessage({
    type: "setPresetFolders",
    folders,
    activeFolderId: activeFolderId ?? uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
  });
}

function findFolderById(folders: PresetFolder[], folderId: string): PresetFolder | undefined {
  for (const folder of folders) {
    if (folder.id === folderId) {
      return folder;
    }
    const childMatch = findFolderById(folder.children ?? [], folderId);
    if (childMatch) {
      return childMatch;
    }
  }
  return undefined;
}

function findFolderByName(folders: PresetFolder[], name: string): PresetFolder | undefined {
  const normalized = normalizeFolderName(name);
  for (const folder of folders) {
    if (normalizeFolderName(folder.name) === normalized) {
      return folder;
    }
  }
  return undefined;
}

function findFolderWithParent(
  folders: PresetFolder[],
  folderId: string,
  parent: PresetFolder | null = null,
): { folder: PresetFolder; parent: PresetFolder | null } | null {
  for (const folder of folders) {
    if (folder.id === folderId) {
      return { folder, parent };
    }
    const childMatch = findFolderWithParent(folder.children ?? [], folderId, folder);
    if (childMatch) {
      return childMatch;
    }
  }
  return null;
}

function isDescendantFolder(folder: PresetFolder, targetId: string): boolean {
  if (!folder.children?.length) {
    return false;
  }
  return folder.children.some((child) => child.id === targetId || isDescendantFolder(child, targetId));
}

function findFolderForPreset(folders: PresetFolder[], presetId: string): PresetFolder | undefined {
  for (const folder of folders) {
    if ((folder.presetIds ?? []).includes(presetId)) {
      return folder;
    }
    const childMatch = findFolderForPreset(folder.children ?? [], presetId);
    if (childMatch) {
      return childMatch;
    }
  }
  return undefined;
}

function findFolderPath(folders: PresetFolder[], targetId: string, trail: string[] = []): string[] | null {
  for (const folder of folders) {
    const nextTrail = [...trail, folder.name];
    if (folder.id === targetId) {
      return nextTrail;
    }
    const childTrail = findFolderPath(folder.children ?? [], targetId, nextTrail);
    if (childTrail) {
      return childTrail;
    }
  }
  return null;
}

function getPresetFolderPath(presetId: string): string | null {
  const folders = uiState.presetFolders ?? [];
  const folder = findFolderForPreset(folders, presetId);
  if (!folder) {
    return null;
  }
  const path = findFolderPath(folders, folder.id);
  return path ? path.join(" > ") : folder.name;
}

function buildArchivePresetFolder(folder: PresetFolder, allowedPresetIds: Set<string>): PresetArchiveFolder | null {
  const presetIds = (folder.presetIds ?? []).filter((presetId) => allowedPresetIds.has(presetId));
  const children = (folder.children ?? [])
    .map((child) => buildArchivePresetFolder(child, allowedPresetIds))
    .filter((child): child is PresetArchiveFolder => Boolean(child));

  if (presetIds.length === 0 && children.length === 0) {
    return null;
  }

  return {
    name: folder.name,
    ...(presetIds.length > 0 ? { presetIds } : {}),
    ...(children.length > 0 ? { children } : {}),
  };
}

function buildArchivePresetFoldersForExport(folderId: string, presets: Preset[]): PresetArchiveFolder[] {
  if (isVirtualPresetFolderId(folderId) && folderId !== PRESET_FOLDER_ALL_ID) {
    return [];
  }

  const allowedPresetIds = new Set(presets.map((preset) => preset.id));
  if (allowedPresetIds.size === 0) {
    return [];
  }

  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], folderId);
    const serializedFolder = folder ? buildArchivePresetFolder(folder, allowedPresetIds) : null;
    return serializedFolder ? [serializedFolder] : [];
  }

  return (uiState.presetFolders ?? [])
    .map((folder) => buildArchivePresetFolder(folder, allowedPresetIds))
    .filter((folder): folder is PresetArchiveFolder => Boolean(folder));
}

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

function ensurePresetFolders(persistChanges: boolean = true): void {
  const stored = loadPresetFoldersFromState();
  uiState.presetFolders = stored;
  const requestedActive = uiState.activePresetFolderId;

  const resolvedActive = requestedActive && !isVirtualPresetFolderId(requestedActive)
    ? findFolderById(stored, requestedActive)?.id
    : requestedActive;
  uiState.activePresetFolderId = resolvedActive || PRESET_FOLDER_ALL_ID;

  if (persistChanges && (requestedActive ?? PRESET_FOLDER_ALL_ID) !== uiState.activePresetFolderId) {
    savePresetFoldersToBackend(uiState.presetFolders ?? [], uiState.activePresetFolderId);
  }
}

function persistPresetFolders(): void {
  savePresetFoldersToBackend(uiState.presetFolders ?? [], uiState.activePresetFolderId);
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

function findSetlistById(id: string | null | undefined): Setlist | undefined {
  if (!id) {
    return undefined;
  }
  return (uiState.setlists ?? []).find((setlist) => setlist.id === id);
}

function isBankAvailable(bank: number, excludeId?: string): boolean {
  return !(uiState.setlists ?? []).some((setlist) => setlist.bank === bank && setlist.id !== excludeId);
}

function createSetlist(name: string, bank?: number | null): void {
  const trimmed = normalizeSetlistName(name);
  if (!trimmed) {
    showNotification("Setlist name required", "Enter a setlist name.");
    return;
  }
  if (typeof bank === "number" && !isBankAvailable(bank)) {
    showNotification("Bank already used", "Only one setlist can use a bank number.");
    return;
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
    setlistSlotsElement.innerHTML = activeSetlist.slots
      .map((slot, index) => {
        const presetName = uiState.presetCache.get(slot.presetId)?.name ?? slot.presetId;
        return `
          <div class="setlist-slot" data-slot-index="${index}" draggable="true">
            <span class="setlist-slot-title">${presetName}</span>
            <button class="setlist-slot-remove" data-slot-index="${index}" type="button">×</button>
          </div>
        `;
      })
      .join("");
  }

  setlistSlotsElement.querySelectorAll<HTMLButtonElement>(".setlist-slot-remove").forEach((button) => {
    button.addEventListener("click", () => {
      const index = Number(button.dataset.slotIndex ?? -1);
      if (index >= 0) {
        removeSetlistSlot(index);
      }
    });
  });

  setlistSlotsElement.querySelectorAll<HTMLElement>(".setlist-slot").forEach((slotEl) => {
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
}

function removePresetFromFolders(folders: PresetFolder[], presetId: string): void {
  folders.forEach((folder) => {
    folder.presetIds = (folder.presetIds ?? []).filter((id) => id !== presetId);
    if (folder.children?.length) {
      removePresetFromFolders(folder.children, presetId);
    }
  });
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

function collectPresetIds(folder: PresetFolder): Set<string> {
  const ids = new Set<string>(folder.presetIds ?? []);
  (folder.children ?? []).forEach((child) => {
    collectPresetIds(child).forEach((id) => ids.add(id));
  });
  return ids;
}

function getPresetsForFolderId(folderId: string): Preset[] {
  let presets = uiState.presets.slice();
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    const favorites = loadFavoritePresetIds();
    presets = presets.filter((preset) => favorites.has(preset.id));
    return sortPresetsAlphabetically(presets);
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return getRecentPresets();
  }
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], folderId);
    if (!folder) {
      return [];
    }
    const allowedIds = collectPresetIds(folder);
    presets = presets.filter((preset) => allowedIds.has(preset.id));
  }
  return sortPresetsAlphabetically(presets);
}

function getPresetFolderExportName(folderId: string): string {
  if (folderId === PRESET_FOLDER_ALL_ID) {
    return "All-Presets";
  }
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    return "Favorite-Presets";
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return "Recent-Presets";
  }
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  return folder?.name || "Preset-Folder";
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

function findFolderByNameInList(folders: PresetFolder[], name: string): PresetFolder | undefined {
  const normalized = normalizeFolderName(name);
  return folders.find((folder) => normalizeFolderName(folder.name) === normalized);
}

function applyImportedPresetFolders(
  archiveFolders: PresetArchiveFolder[],
  presetIdMap: Map<string, string>,
  importedPresetIds: string[],
): void {
  const folders = uiState.presetFolders ?? [];
  const assignedPresetIds = new Set<string>();
  const ensureChildFolder = (siblings: PresetFolder[], name: string): PresetFolder => {
    const existing = findFolderByNameInList(siblings, name);
    if (existing) {
      existing.children = existing.children ?? [];
      existing.presetIds = existing.presetIds ?? [];
      return existing;
    }

    const created: PresetFolder = {
      id: generateResourceId(name),
      name,
      children: [],
      presetIds: [],
    };
    siblings.push(created);
    return created;
  };

  const applyFolderNodes = (siblings: PresetFolder[], nodes: PresetArchiveFolder[]): void => {
    nodes.forEach((node) => {
      const folder = ensureChildFolder(siblings, node.name);
      (node.presetIds ?? []).forEach((sourcePresetId) => {
        const importedPresetId = presetIdMap.get(sourcePresetId);
        if (!importedPresetId) {
          return;
        }
        if (!folder.presetIds.includes(importedPresetId)) {
          folder.presetIds.push(importedPresetId);
        }
        assignedPresetIds.add(importedPresetId);
      });
      applyFolderNodes(folder.children, node.children ?? []);
    });
  };

  applyFolderNodes(folders, archiveFolders);

  if (assignedPresetIds.size > 0 || importedPresetIds.length > 0) {
    persistPresetFolders();
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
  const visiblePresets = uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID
    ? uiState.filteredPresets
    : sortPresetsAlphabetically(uiState.filteredPresets);

  renderPresetList(visiblePresets, uiState.activePresetId, async (presetId) => {
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

function requestPresetFromBackend(presetId: string): Promise<Preset> {
  const existing = pendingPresetRequests.get(presetId);
  if (existing) {
    return new Promise((resolve, reject) => {
      const chainedResolve = (preset: Preset) => {
        existing.resolve(preset);
        resolve(preset);
      };
      const chainedReject = (error: Error) => {
        existing.reject(error);
        reject(error);
      };
      pendingPresetRequests.set(presetId, {
        resolve: chainedResolve,
        reject: chainedReject,
        timeoutId: existing.timeoutId,
      });
    });
  }

  postMessage({ type: "getPresetById", presetId });

  return new Promise((resolve, reject) => {
    const timeoutId = window.setTimeout(() => {
      pendingPresetRequests.delete(presetId);
      reject(new Error(`Preset ${presetId} request timed out`));
    }, PRESET_REQUEST_TIMEOUT_MS);

    pendingPresetRequests.set(presetId, { resolve, reject, timeoutId });
  });
}

export function handlePresetDataMessage(preset: Preset): void {
  if (!preset?.id) return;
  const pending = pendingPresetRequests.get(preset.id);
  if (pending) {
    window.clearTimeout(pending.timeoutId);
    pendingPresetRequests.delete(preset.id);
    pending.resolve(preset);
  }
  uiState.presetCache.set(preset.id, clonePreset(preset));
  const index = uiState.presets.findIndex((p) => p.id === preset.id);
  if (index >= 0) {
    uiState.presets[index] = clonePreset(preset);
  } else {
    uiState.presets.push(clonePreset(preset));
  }
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
    const presetWithGlobals = preset as Preset & { globalSignalChain?: import("./types.js").GlobalSignalChainConfig };
    const hasGlobalChain = Boolean(presetWithGlobals.globalSignalChain);
    const resolvedChain = hasGlobalChain
      ? JSON.parse(JSON.stringify(presetWithGlobals.globalSignalChain)) as import("./types.js").GlobalSignalChainConfig
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
    setActivePresetSnapshot(presetPayload);
    setActivePresetDraft(presetPayload);
    setPresetDirty(false);
    setFavoriteToggleState(presetPayload.id);
    updatePresetDropdownSelection();
    renderPresetUI(clonePreset(presetPayload));
    updatePresetActionButtons();
    postMessage({
      type: "loadPreset",
      preset: presetPayload,
      ...(sceneId ? { sceneId } : {}),
    });
  } catch (error) {
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

export function cachePresetInMemory(preset: Preset): void {
  const cleanedPreset = stripLegacyGlobals(preset);
  normalizePresetScenes(cleanedPreset);
  uiState.presetCache.set(cleanedPreset.id, cleanedPreset);
  if (!uiState.presets.some((p) => p.id === cleanedPreset.id)) {
    uiState.presets.push(cleanedPreset);
  }
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

const exportChooserId = "preset-export-chooser";

function getOrCreateExportChooser(): HTMLDivElement {
  let chooser = document.getElementById(exportChooserId) as HTMLDivElement | null;
  if (chooser) {
    return chooser;
  }

  chooser = document.createElement("div");
  chooser.id = exportChooserId;
  chooser.className = "preset-export-chooser";
  chooser.setAttribute("role", "menu");
  chooser.setAttribute("aria-hidden", "true");

  const exportItem = document.createElement("button");
  exportItem.type = "button";
  exportItem.className = "preset-export-chooser-item";
  exportItem.dataset.action = "export";
  exportItem.textContent = "Export Preset File…";
  chooser.appendChild(exportItem);

  const publishItem = document.createElement("button");
  publishItem.type = "button";
  publishItem.className = "preset-export-chooser-item";
  publishItem.dataset.action = "publish";
  publishItem.textContent = "Publish Preset…";
  chooser.appendChild(publishItem);

  chooser.addEventListener("click", (event) => {
    const button = (event.target as HTMLElement | null)?.closest<HTMLButtonElement>(".preset-export-chooser-item");
    if (!button || button.disabled) {
      return;
    }

    const action = button.dataset.action;
    closeExportChooser();

    if (action === "export") {
      void exportCurrentPresetArchive();
      return;
    }

    if (action === "publish") {
      const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
      openToneSharingPublishPresetModal(activePreset?.name ?? "", activePreset?.description ?? "");
    }
  });

  document.body.appendChild(chooser);
  return chooser;
}

function closeExportChooser(): void {
  const chooser = document.getElementById(exportChooserId) as HTMLDivElement | null;
  if (!chooser) {
    return;
  }
  chooser.classList.remove("open");
  chooser.setAttribute("aria-hidden", "true");
}

function openExportChooser(anchor: HTMLElement): void {
  const chooser = getOrCreateExportChooser();
  const publishItem = chooser.querySelector<HTMLButtonElement>('[data-action="publish"]');
  const signedIn = isToneSharingSignedIn();
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const toneSharingOrigin = getToneSharingOriginMetadata(activePreset);
  if (publishItem) {
    publishItem.disabled = !signedIn || Boolean(toneSharingOrigin?.republishBlocked);
    publishItem.title = !signedIn
      ? "Sign in to Tone Sharing to publish"
      : toneSharingOrigin?.republishBlocked
        ? "Use Save As before republishing a preset imported from Tone Sharing"
        : "Publish preset";
  }

  const anchorRect = anchor.getBoundingClientRect();
  chooser.style.left = `${Math.max(8, anchorRect.left + window.scrollX)}px`;
  chooser.style.top = `${anchorRect.bottom + window.scrollY + 6}px`;
  chooser.classList.add("open");
  chooser.setAttribute("aria-hidden", "false");
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

  document.addEventListener("click", () => {
    closePresetLibraryPopover();
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
      let name = presetFolderNameInput?.value ?? "";
      if (!name.trim()) {
        name = window.prompt("Folder name", "") ?? "";
      }
      const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
      const parentId = isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
      const created = createFolder(name, parentId);
      if (created && presetFolderNameInput) {
        presetFolderNameInput.value = "";
      }
    });
  }

  if (presetExportFolderButton) {
    presetExportFolderButton.addEventListener("click", () => void exportSelectedPresetCollectionArchive());
  }
}

// Save preset modal helpers
function configureSavePresetModalLabels(isOverwrite: boolean): void {
  const title = document.getElementById("save-preset-modal-title");
  const confirmBtn = document.getElementById("save-preset-confirm");

  if (title) {
    title.textContent = isOverwrite ? "Overwrite Preset" : "Save Preset As";
  }

  if (confirmBtn) {
    confirmBtn.textContent = isOverwrite ? "Overwrite Preset" : "Save New Preset";
  }
}

export function openSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;

  // Save As must always create a new preset, never reuse edit state.
  delete modal.dataset.editingPresetId;
  configureSavePresetModalLabels(false);

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const defaultFolderId = isVirtualPresetFolderId(activeFolderId) ? PRESET_FOLDER_ALL_ID : activeFolderId;
  populatePresetFolderSelect(folderSelect, defaultFolderId);

  if (nameInput) nameInput.value = "";
  if (categoryInput) categoryInput.value = "User";
  if (descriptionInput) descriptionInput.value = "";
  setPresetTagsPickerValue([]);

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  updatePresetModalJson(activePreset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;
  delete modal.dataset.stagedDesignedPeak;
  updateSavePresetModalPeakInfo(modal);

  modal.style.display = "flex";
}

export function closeSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (modal) {
    modal.style.display = "none";
    // Clear editing state
    delete modal.dataset.editingPresetId;
    delete modal.dataset.cleanedPreset;
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
      const basePreset = stripLegacyGlobals(cleanedPreset ?? existingPreset);
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
        presetId: updatedPreset.id,
        name: updatedPreset.name,
        category: updatedPreset.category,
        description: updatedPreset.description,
        ...(sceneId ? { sceneId } : {}),
        includeGlobalSignalChain: includeGlobalFx,
        preset: stripGlobalSignalChainForSave(updatedPreset),
      };
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
      setActivePresetSnapshot(updatedPreset);
      setActivePresetDraft(updatedPreset);
      setPresetDirty(false);
      updatePresetActionButtons();
      return;
    }
  }

  // Creating new preset
  const basePreset = stripLegacyGlobals(cleanedPreset ?? clonePreset(activePreset ?? ({} as Preset)));
  const fallbackId = `user-${Date.now()}`;
  const newPresetId = fallbackId;
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
    presetId: newPreset.id,
    name: newPreset.name,
    category: newPreset.category,
    description: newPreset.description,
    ...(sceneId ? { sceneId } : {}),
    includeGlobalSignalChain: includeGlobalFx,
    preset: stripGlobalSignalChainForSave(newPreset),
  };
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

  // Wire tag chip toggle clicks
  document.getElementById("preset-tags-picker")?.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
    btn.addEventListener("click", () => btn.classList.toggle("active"));
  });

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

type PresetArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
  metadata?: Record<string, string>;
};

/**
 * Reference to a tone3000-sourced resource that must be re-downloaded by the
 * recipient using their own API key, as redistribution is prohibited by tone3000 terms.
 */
type Tone3000ResourceRef = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  modelUrl?: string;
  toneId?: string;
  modelId?: string;
  creatorId?: string;
  creatorName?: string;
};

type PresetArchiveFolder = {
  name: string;
  presetIds?: string[];
  children?: PresetArchiveFolder[];
};

type PresetArchive = {
  formatVersion: number;
  preset: Preset;
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  /** tone3000-sourced resources excluded from the archive per their redistribution terms. */
  tone3000Resources?: Tone3000ResourceRef[];
};

type PresetCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Preset[];
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  presetFolders?: PresetArchiveFolder[];
  /** tone3000-sourced resources excluded from the archive per their redistribution terms. */
  tone3000Resources?: Tone3000ResourceRef[];
};

type ImportPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

type ImportPackContext = {
  source: ImportPackSource;
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

type ImportPackSummary = {
  format: "generatedPack" | "presetArchive";
  title: string;
  presetCount: number;
  resourceCount: number;
  blendCount: number;
  tone3000ResourceCount: number;
  packId?: string;
};

function buildImportSummaryMessage(summary: ImportPackSummary): string {
  const lines = [
    `Import pack \"${summary.title}\"?`,
    `Format: ${summary.format === "generatedPack" ? "Generated Pack" : "Preset Archive"}`,
    `Presets: ${summary.presetCount}`,
    `Resources: ${summary.resourceCount}`,
    `Blends: ${summary.blendCount}`,
  ];
  if (summary.tone3000ResourceCount > 0) {
    lines.push(`Tone3000 resources: ${summary.tone3000ResourceCount} (requires your Tone3000 API key)`);
  }
  if (summary.packId) {
    lines.push(`Pack ID: ${summary.packId}`);
  }
  lines.push("This will import resources and presets into your local library.");
  return lines.join("\n");
}

async function inspectImportPack(file: File, context: ImportPackContext): Promise<ImportPackSummary> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const manifestEntry = zip.file("pack-manifest.json");
  if (manifestEntry) {
    const indexEntry = zip.file("resources/indexes/resources-index.json");

    let manifest: GeneratorPackManifest = {};
    try {
      manifest = JSON.parse(await manifestEntry.async("text")) as GeneratorPackManifest;
    } catch {
      manifest = {};
    }

    let generatedResourceCount = 0;
    if (indexEntry) {
      try {
        const parsed = JSON.parse(await indexEntry.async("text")) as GeneratorResourceIndex;
        generatedResourceCount = Array.isArray(parsed.items) ? parsed.items.length : 0;
      } catch {
        generatedResourceCount = 0;
      }
    }

    const presetCount = Object.keys(zip.files).filter(
      (name) => name.startsWith("presets/") && name.endsWith(".json") && !zip.files[name].dir,
    ).length;

    return {
      format: "generatedPack",
      title: context.titleHint ?? manifest.packId ?? file.name.replace(/\.zip$/i, ""),
      presetCount,
      resourceCount: generatedResourceCount,
      blendCount: 0,
      tone3000ResourceCount: 0,
      packId: context.packId ?? manifest.packId,
    };
  }

  const presetEntry = zip.file("preset.json");
  const presetsEntry = zip.file("presets.json");
  if (!presetEntry && !presetsEntry) {
    throw new Error("Archive is missing preset.json, presets.json, or pack-manifest.json");
  }

  let presetCount = 0;
  let resourceCount = 0;
  let blendCount = 0;
  let tone3000ResourceCount = 0;

  if (presetEntry) {
    const archive = JSON.parse(await presetEntry.async("text")) as PresetArchive;
    presetCount = archive.preset ? 1 : 0;
    resourceCount = Array.isArray(archive.resources) ? archive.resources.length : 0;
    blendCount = Array.isArray(archive.blends) ? archive.blends.length : 0;
    tone3000ResourceCount = Array.isArray(archive.tone3000Resources) ? archive.tone3000Resources.length : 0;
  } else if (presetsEntry) {
    const archive = JSON.parse(await presetsEntry.async("text")) as PresetCollectionArchive;
    presetCount = Array.isArray(archive.presets) ? archive.presets.length : 0;
    resourceCount = Array.isArray(archive.resources) ? archive.resources.length : 0;
    blendCount = Array.isArray(archive.blends) ? archive.blends.length : 0;
    tone3000ResourceCount = Array.isArray(archive.tone3000Resources) ? archive.tone3000Resources.length : 0;
  }

  return {
    format: "presetArchive",
    title: context.titleHint ?? file.name.replace(/\.zip$/i, ""),
    presetCount,
    resourceCount,
    blendCount,
    tone3000ResourceCount,
    packId: context.packId,
  };
}

function toInstalledPackSource(source: ImportPackSource, format: ImportPackSummary["format"]): InstalledPackMetadata["source"] {
  if (source === "toneSharingApi") {
    return "toneSharingApi";
  }
  if (format === "generatedPack" || source === "generatedPack") {
    return "generatedPack";
  }
  return "zipImport";
}

export async function importPackWithConfirmation(file: File, context: ImportPackContext = { source: "zipImport" }): Promise<void> {
  try {
    const summary = await inspectImportPack(file, context);
    const confirmed = await showConfirm(buildImportSummaryMessage(summary), "Import Pack");
    if (!confirmed) {
      return;
    }

    const installedSource = toInstalledPackSource(context.source, summary.format);
    if (summary.format === "generatedPack") {
      await importGeneratedPack(file, {
        source: installedSource,
        packId: context.packId,
        titleHint: summary.title,
      });
      return;
    }

    await importPresetArchive(file, {
      source: installedSource,
      packId: context.packId,
      titleHint: summary.title,
    });
  } catch (error) {
    showNotification("Import failed", error instanceof Error ? error.message : String(error));
  }
}

function getLibraryResource(resourceType: string, resourceId: string): LibraryResource | undefined {
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return resources.find((res) => res.id === resourceId);
}

function getLibraryResourceByHash(resourceType: string, hash?: string): LibraryResource | undefined {
  if (!hash) {
    return undefined;
  }
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return resources.find((res) => res.hash?.toLowerCase() === hash.toLowerCase());
}

function getPresetGraphs(preset: Preset | null | undefined): SignalGraph[] {
  const graphs: SignalGraph[] = [];
  if (preset?.graph) {
    graphs.push(preset.graph);
  }
  if (Array.isArray(preset?.scenes)) {
    preset.scenes.forEach((scene) => {
      if (scene?.graph) {
        graphs.push(scene.graph);
      }
    });
  }
  return graphs;
}

function hasAnyGraphNodes(preset: Preset | null | undefined): boolean {
  return getPresetGraphs(preset).some((graph) => Array.isArray(graph.nodes) && graph.nodes.length > 0);
}

function collectPresetBlendIds(preset: Preset): string[] {
  const ids = new Set<string>();
  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      const blendId = node.config?.blendId ?? "";
      if (blendId) {
        ids.add(blendId);
      }
    });
  });

  return Array.from(ids);
}

function collectPresetResourceRefs(preset: Preset, blendDefs: BlendDefinition[]): ResourceRef[] {
  const refs: ResourceRef[] = [];
  const seen = new Set<string>();

  const addRef = (type: string | undefined, id: string | undefined, filePath?: string): void => {
    if (!type || !id) {
      return;
    }
    const key = `${type}:${id}`;
    if (seen.has(key)) {
      return;
    }
    seen.add(key);
    refs.push({ type, id, filePath });
  };

  getPresetGraphs(preset).forEach((graph) => {
    graph.nodes?.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => addRef(res.resourceType ?? res.type, res.resourceId ?? res.id, res.filePath));
      }
    });
  });

  if (preset.audioFxModelId) {
    addRef("nam", preset.audioFxModelId);
  }
  if (preset.irId) {
    addRef("ir", preset.irId);
  }

  preset.attachments?.forEach((attachment) => {
    if (!attachment.id) {
      return;
    }
    const type = attachment.type === "audiofx" ? "nam" : attachment.type === "ir" ? "ir" : attachment.type;
    addRef(type, attachment.id, attachment.filePath);
  });

  blendDefs.forEach((blend) => {
    (blend.models ?? []).forEach((modelId) => addRef("nam", modelId));
    (blend.modelMappings ?? []).forEach((mapping) => addRef("nam", mapping.id));
  });

  return refs;
}

async function exportPresetCollectionArchive(presets: Preset[], archiveName: string, sourceFolderId: string): Promise<void> {
  if (!presets.length) {
    showNotification("Export failed", "No presets to export");
    return;
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Export failed", "Archive library not available");
    return;
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    showNotification("Export failed", "Unable to create archive");
    return;
  }

  const exportSourcePresets: Preset[] = [];
  let unresolvedPresetCount = 0;
  for (const listedPreset of presets) {
    const cachedPreset = uiState.presetCache.get(listedPreset.id) ?? listedPreset;
    if (hasAnyGraphNodes(cachedPreset)) {
      exportSourcePresets.push(clonePreset(cachedPreset));
      continue;
    }

    try {
      const fetchedPreset = await requestPresetFromBackend(listedPreset.id);
      exportSourcePresets.push(clonePreset(fetchedPreset));
    } catch {
      // Fall back to whatever is available so metadata/name still exports.
      exportSourcePresets.push(clonePreset(cachedPreset));
      unresolvedPresetCount += 1;
    }
  }

  const blendIds = new Set<string>();
  exportSourcePresets.forEach((preset) => {
    collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
  });
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.has(blend.id));
  const refMap = new Map<string, ResourceRef>();
  exportSourcePresets.forEach((preset) => {
    collectPresetResourceRefs(preset, blendDefs).forEach((ref) => {
      const resourceType = ref.resourceType ?? ref.type ?? "";
      const resourceId = ref.resourceId ?? ref.id ?? "";
      if (!resourceType || !resourceId) {
        return;
      }
      refMap.set(`${resourceType}:${resourceId}`, ref);
    });
  });

  const exportResources: PresetArchiveResource[] = [];
  let missingCount = 0;

  for (const ref of refMap.values()) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) {
      continue;
    }
    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) {
      missingCount += 1;
      continue;
    }
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      missingCount += 1;
      continue;
    }
    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: resource.id,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
      ...(resource.metadata && Object.keys(resource.metadata).length > 0 ? { metadata: { ...resource.metadata } } : {}),
    });
  }

  const exportPresets = exportSourcePresets.map((preset) => clonePreset(preset));
  const presetFolders = buildArchivePresetFoldersForExport(sourceFolderId, exportPresets);
  const archive: PresetCollectionArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    presets: exportPresets,
    resources: exportResources,
    blends: blendDefs,
    ...(presetFolders.length > 0 ? { presetFolders } : {}),
  };

  zip.file("presets.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);

  if (missingCount > 0) {
    showNotification("Export warning", `${missingCount} resources could not be read`);
  }
  if (unresolvedPresetCount > 0) {
    showNotification("Export warning", `${unresolvedPresetCount} presets could not be fully resolved before export`);
  }

  const normalizeArchiveExportBaseName = (raw: string, fallback: string): string => {
    let normalized = sanitizeFilename(raw, fallback);
    const suffixPattern = /\.(?:soundshed\.(?:preset|presets)|zip)$/i;
    while (suffixPattern.test(normalized)) {
      normalized = normalized.replace(suffixPattern, "");
    }
    normalized = normalized.replace(/\.+$/, "");
    return normalized || fallback;
  };

  postMessage({
    type: "savePresetArchive",
    fileName: `${normalizeArchiveExportBaseName(archiveName, "presets")}.soundshed.presets`,
    data,
  });
}

async function exportActivePresetFolderArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const presets = getPresetsForFolderId(activeFolderId);
  const archiveName = getPresetFolderExportName(activeFolderId);
  await exportPresetCollectionArchive(presets, archiveName, activeFolderId);
}

async function exportAllPresetsArchive(): Promise<void> {
  const presets = uiState.presets.slice();
  await exportPresetCollectionArchive(presets, "All-Presets", PRESET_FOLDER_ALL_ID);
}

async function exportSelectedPresetCollectionArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  if (activeFolderId === PRESET_FOLDER_ALL_ID) {
    await exportAllPresetsArchive();
    return;
  }
  await exportActivePresetFolderArchive();
}

export async function buildPresetArchiveBlob(preset: Preset): Promise<Blob> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    throw new Error("Unable to create archive");
  }

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];

  for (const ref of resourceRefs) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) continue;
    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) continue;
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) continue;
    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: resource.id,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
      ...(resource.metadata && Object.keys(resource.metadata).length > 0 ? { metadata: { ...resource.metadata } } : {}),
    });
  }

  const archive: PresetArchive = {
    formatVersion: 1,
    preset: clonePreset(preset),
    resources: exportResources,
    blends: blendDefs,
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  return zip.generateAsync({ type: "blob" });
}

export async function buildToneSharingPresetArchiveBlobs(preset: Preset): Promise<{ publicBlob: Blob; privateBlob: Blob }> {
  const privateBlob = await buildPresetArchiveBlob(preset);
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    throw new Error("Unable to create archive");
  }

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];
  const exportTone3000Resources: Tone3000ResourceRef[] = [];

  for (const ref of resourceRefs) {
    const resourceType = ref.resourceType ?? ref.type ?? "";
    const resourceId = ref.resourceId ?? ref.id ?? "";
    if (!resourceType || !resourceId) {
      continue;
    }

    const resource = getLibraryResource(resourceType, resourceId);
    if (!resource) {
      continue;
    }

    if (resource.metadata?.provider === "tone3000") {
      const toneId = resource.metadata.toneId?.trim() ?? "";
      const modelId = resource.metadata.modelId?.trim() ?? "";
      if (!toneId || !modelId) {
        throw new Error(`Tone3000 resource \"${resource.name || resource.id}\" is missing toneId/modelId metadata and cannot be shared.`);
      }

      exportTone3000Resources.push({
        id: resource.id,
        name: resource.name,
        category: resource.category,
        type: resourceType,
        toneId,
        modelId,
        creatorId: resource.metadata.creatorId?.trim() || undefined,
        creatorName: resource.metadata.creatorName?.trim() || resource.metadata.authorUsername?.trim() || undefined,
      });
      continue;
    }

    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      continue;
    }

    const hash = await sha256HexFromBase64(data);
    const fileName = buildArchiveFileNameWithHash(resource, resourceType, hash);
    resourcesFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: resource.id,
      name: resource.name,
      category: resource.category,
      type: resourceType,
      fileName,
      hash,
    });
  }

  const archive: PresetArchive = {
    formatVersion: 1,
    preset: clonePreset(preset),
    resources: exportResources,
    blends: blendDefs,
    ...(exportTone3000Resources.length > 0 ? { tone3000Resources: exportTone3000Resources } : {}),
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  return {
    publicBlob: await zip.generateAsync({ type: "blob" }),
    privateBlob,
  };
}

let tone3000RequirementResolve: ((value: boolean) => void) | null = null;

function closeTone3000RequirementModal(result: boolean): void {
  const modal = document.getElementById("tone3000-required-modal") as HTMLElement | null;
  const status = document.getElementById("tone3000-required-status") as HTMLElement | null;
  if (modal) {
    modal.style.display = "none";
  }
  if (status) {
    status.textContent = "";
  }
  if (tone3000RequirementResolve) {
    tone3000RequirementResolve(result);
    tone3000RequirementResolve = null;
  }
}

async function promptForTone3000ApiKey(resourceCount: number): Promise<boolean> {
  const modal = document.getElementById("tone3000-required-modal") as HTMLElement | null;
  const message = document.getElementById("tone3000-required-message") as HTMLElement | null;
  const status = document.getElementById("tone3000-required-status") as HTMLElement | null;
  const input = document.getElementById("tone3000-required-api-key") as HTMLInputElement | null;
  const saveButton = document.getElementById("tone3000-required-save") as HTMLButtonElement | null;
  const settingsButton = document.getElementById("tone3000-required-open-settings") as HTMLButtonElement | null;
  const cancelButton = document.getElementById("tone3000-required-cancel") as HTMLButtonElement | null;
  const closeButton = document.getElementById("tone3000-required-close") as HTMLButtonElement | null;

  if (!modal || !message || !status || !input || !saveButton || !settingsButton || !cancelButton || !closeButton) {
    throw new Error("Tone3000 access modal is not available");
  }

  if (tone3000RequirementResolve) {
    tone3000RequirementResolve(false);
    tone3000RequirementResolve = null;
  }

  message.textContent = `${resourceCount} Tone3000 resource(s) in this shared preset require your own Tone3000 API key before import can continue.`;
  status.textContent = "";
  input.value = "";
  modal.style.display = "flex";

  return new Promise<boolean>((resolve) => {
    tone3000RequirementResolve = resolve;

    saveButton.onclick = async () => {
      const apiKey = input.value.trim();
      if (!apiKey) {
        status.textContent = "Enter your Tone3000 API key to continue.";
        return;
      }
      status.textContent = "Starting Tone3000 session...";
      saveButton.disabled = true;
      try {
        const saved = await saveTone3000ApiKey(apiKey);
        if (!saved) {
          status.textContent = "Tone3000 authentication failed. Check the API key and try again.";
          return;
        }
        closeTone3000RequirementModal(true);
      } finally {
        saveButton.disabled = false;
      }
    };

    settingsButton.onclick = () => {
      switchMainPanel("settings");
      activateLibraryTab("tone3000");
      status.textContent = "Opened Settings › Library › Tone3000.";
    };

    cancelButton.onclick = () => closeTone3000RequirementModal(false);
    closeButton.onclick = () => closeTone3000RequirementModal(false);
    modal.onmousedown = (event) => {
      if (event.target === modal) {
        closeTone3000RequirementModal(false);
      }
    };
  });
}

async function exportCurrentPresetArchive(): Promise<void> {
  const presetId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(presetId) ?? null;
  if (!preset) {
    showNotification("Export failed", "No preset selected");
    return;
  }

  let blob: Blob;
  try {
    blob = await buildPresetArchiveBlob(preset);
  } catch (error) {
    showNotification("Export failed", (error as Error).message);
    return;
  }

  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const normalizeArchiveExportBaseName = (raw: string, fallback: string): string => {
    let normalized = sanitizeFilename(raw, fallback);
    const suffixPattern = /\.(?:soundshed\.(?:preset|presets)|zip)$/i;
    while (suffixPattern.test(normalized)) {
      normalized = normalized.replace(suffixPattern, "");
    }
    normalized = normalized.replace(/\.+$/, "");
    return normalized || fallback;
  };

  postMessage({
    type: "savePresetArchive",
    fileName: `${normalizeArchiveExportBaseName(preset.name || preset.id || "preset", "preset")}.soundshed.preset`,
    data,
  });
}

type ArchiveImportContext = {
  source: InstalledPackMetadata["source"];
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

type ArchiveImportOptions = {
  previewOnly?: boolean;
  suppressNotifications?: boolean;
};

function normalizeToneSharingOrigin(value: unknown): ToneSharingOriginMetadata | undefined {
  if (!value || typeof value !== "object") {
    return undefined;
  }
  const origin = value as Record<string, unknown>;
  const source = origin.source === "toneSharingApi" ? "toneSharingApi" : null;
  const itemId = typeof origin.itemId === "string" ? origin.itemId.trim() : "";
  if (!source || !itemId) {
    return undefined;
  }
  return {
    source,
    itemId,
    originalPresetId: typeof origin.originalPresetId === "string" ? origin.originalPresetId : undefined,
    importedAt: typeof origin.importedAt === "string" ? origin.importedAt : undefined,
    importedFromPackId: typeof origin.importedFromPackId === "string" ? origin.importedFromPackId : undefined,
    creatorId: typeof origin.creatorId === "string" ? origin.creatorId : undefined,
    creatorHandle: typeof origin.creatorHandle === "string" ? origin.creatorHandle : undefined,
    republishBlocked: origin.republishBlocked !== false,
  };
}

function createToneSharingOrigin(
  itemId: string,
  sourcePresetId: string,
  options?: { packId?: string; creatorId?: string; creatorHandle?: string },
): ToneSharingOriginMetadata {
  return {
    source: "toneSharingApi",
    itemId,
    originalPresetId: sourcePresetId,
    importedAt: new Date().toISOString(),
    importedFromPackId: options?.packId,
    creatorId: options?.creatorId,
    creatorHandle: options?.creatorHandle,
    republishBlocked: true,
  };
}

function getToneSharingOriginMetadata(preset: Preset | null | undefined): ToneSharingOriginMetadata | undefined {
  return normalizeToneSharingOrigin(preset?.toneSharingOrigin);
}

function buildInstalledPackEntryId(file: File, context: ArchiveImportContext): string {
  if (context.source === "toneSharingApi" && context.itemId) {
    return `tone-sharing-api:item:${context.itemId}`;
  }
  if (context.source === "toneSharingApi" && context.packId) {
    return `tone-sharing-api:${context.packId}`;
  }
  if (context.source === "generatedPack" && context.packId) {
    return `generated:${context.packId}`;
  }
  const base = (context.titleHint || file.name).trim().toLowerCase();
  return `${context.source}:${base}:${file.size}`;
}

/**
 * Download tone3000 resources referenced in a preset archive using the user's
 * own authenticated session. Redistribution of tone3000 files is prohibited by
 * their terms, so archives carry only a model URL rather than file bytes.
 */
async function importTone3000ArchiveResources(
  refs: Tone3000ResourceRef[],
  idMap: Map<string, string>,
  importedResources: Array<{ type: string; id: string }>,
): Promise<void> {
  if (!uiState.tone3000Session?.accessToken) {
    const storedApiKey = typeof uiState.appSettings["tone3000.apiKey"] === "string"
      ? (uiState.appSettings["tone3000.apiKey"] as string).trim()
      : "";

    if (storedApiKey) {
      appendLog("tone3000 session missing; attempting auto-start from saved API key");
      await saveTone3000ApiKey(storedApiKey);
    }

    if (!uiState.tone3000Session?.accessToken) {
      const granted = await promptForTone3000ApiKey(refs.length);
      if (!granted) {
        throw new Error("Tone3000 API key is required to import this shared preset");
      }
    }
  }

  let succeeded = 0;
  let failed = 0;

  for (const ref of refs) {
    // If the resource was previously imported, reuse it.
    const existing = getLibraryResource(ref.type, ref.id);
    if (existing && !existing.fileMissing) {
      idMap.set(ref.id, existing.id);
      importedResources.push({ type: ref.type, id: existing.id });
      succeeded += 1;
      continue;
    }

    try {
      const buffer = await downloadTone3000ResourceByReference({
        toneId: ref.toneId,
        modelId: ref.modelId,
        modelUrl: ref.modelUrl,
      });
      const data = arrayBufferToBase64(buffer);
      const extension = ref.type === "ir" ? ".wav" : ".nam";
      const fileName = `${sanitizeFilename(ref.name ?? ref.id, "resource")}${extension}`;

      postMessage({
        type: "importRemoteResource",
        provider: "tone3000",
        resourceType: ref.type,
        resourceId: ref.id,
        name: ref.name ?? ref.id,
        description: "",
        category: ref.category ?? "",
        subfolder: "preset-imports",
        fileName,
        metadata: {
          provider: "tone3000",
          toneId: ref.toneId ?? "",
          creatorId: ref.creatorId ?? "",
          creatorName: ref.creatorName ?? "",
          authorUsername: ref.creatorName ?? "",
          modelId: ref.modelId ?? "",
          ...(ref.modelUrl ? { modelUrl: ref.modelUrl } : {}),
        },
        data,
      });

      idMap.set(ref.id, ref.id);
      importedResources.push({ type: ref.type, id: ref.id });
      succeeded += 1;
      appendLog(`tone3000 archive resource imported: ${ref.name ?? ref.id}`);
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      appendLog(`tone3000 archive resource failed (${ref.name ?? ref.id}): ${msg}`);
      failed += 1;
    }
  }

  if (failed > 0) {
    throw new Error(`Tone3000 resource download incomplete: ${succeeded} succeeded, ${failed} failed.`);
  }
}

export async function importPresetArchive(
  file: File,
  context: ArchiveImportContext = { source: "zipImport" },
  options: ArchiveImportOptions = {},
): Promise<Preset[]> {
  const previewOnly = Boolean(options.previewOnly);
  const suppressNotifications = Boolean(options.suppressNotifications);
  const notifyImportError = (title: string, message: string): void => {
    if (!suppressNotifications) {
      showNotification(title, message);
    }
  };

  const zipLib = window.JSZip;
  if (!zipLib) {
    notifyImportError("Import failed", "Archive library not available");
    return [];
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const presetEntry = zip.file("preset.json");
  const presetsEntry = zip.file("presets.json");
  if (!presetEntry && !presetsEntry) {
    notifyImportError("Import failed", "Archive is missing preset.json or presets.json");
    return [];
  }

  let resourcesToImport: PresetArchiveResource[] = [];
  let tone3000ResourcesToImport: Tone3000ResourceRef[] = [];
  let blends: BlendDefinition[] = [];
  let presetsToImport: Preset[] = [];
  let presetFoldersToImport: PresetArchiveFolder[] = [];

  if (presetEntry) {
    const presetText = await presetEntry.async("text");
    const archive = JSON.parse(presetText) as PresetArchive;
    if (!archive.preset) {
      notifyImportError("Import failed", "Archive has no preset data");
      return [];
    }
    resourcesToImport = archive.resources ?? [];
    tone3000ResourcesToImport = archive.tone3000Resources ?? [];
    blends = archive.blends ?? [];
    presetsToImport = [archive.preset];
  } else if (presetsEntry) {
    const presetsText = await presetsEntry.async("text");
    const archive = JSON.parse(presetsText) as PresetCollectionArchive;
    if (!Array.isArray(archive.presets) || archive.presets.length === 0) {
      notifyImportError("Import failed", "Archive has no presets data");
      return [];
    }
    resourcesToImport = archive.resources ?? [];
    tone3000ResourcesToImport = archive.tone3000Resources ?? [];
    blends = archive.blends ?? [];
    presetsToImport = archive.presets;
    presetFoldersToImport = archive.presetFolders ?? [];
  }

  const zipFiles = Object.values(zip.files) as JSZipObject[];
  const fileMap = new Map<string, JSZipObject>();
  zipFiles.forEach((entry) => {
    if (!entry.dir) {
      const name = entry.name.replace(/^resources\//, "");
      fileMap.set(name, entry);
    }
  });

  const idMap = new Map<string, string>();
  const importedResources: Array<{ type: string; id: string }> = [];
  for (const resource of resourcesToImport) {
    const fileName = resource.fileName ?? "";
    const existing = getLibraryResourceByHash(resource.type, resource.hash);
    if (existing) {
      idMap.set(resource.id, existing.id);
      importedResources.push({ type: resource.type, id: existing.id });
      continue;
    }
    const entry = fileMap.get(fileName);
    if (!entry) {
      continue;
    }
    const dataBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(dataBuffer);
    const newId = generateResourceId(fileName);
    idMap.set(resource.id, newId);
    importedResources.push({ type: resource.type, id: newId });

    postMessage({
      type: "importRemoteResource",
      provider: "presetArchive",
      resourceType: resource.type,
      resourceId: newId,
      name: resource.name ?? fileName,
      description: "",
      category: resource.category ?? "",
      subfolder: "preset-imports",
      fileName,
      hash: resource.hash ?? "",
      metadata: {
        ...(resource.metadata ?? {}),
        archiveProvider: "presetArchive",
        sourceFile: fileName,
      },
      data,
    });
  }

  // Download tone3000-sourced resources using the user's own authenticated session.
  if (tone3000ResourcesToImport.length > 0) {
    await importTone3000ArchiveResources(tone3000ResourcesToImport, idMap, importedResources);
  }

  const blendIdMap = new Map<string, string>();
  const presetIdMap = new Map<string, string>();
  blends.forEach((blend) => {
    const newBlendId = generateResourceId(blend.id || blend.name || "blend");
    blendIdMap.set(blend.id, newBlendId);

    const remapModel = (id: string) => idMap.get(id) ?? id;
    const models = (blend.models ?? []).map(remapModel);
    const modelMappings = (blend.modelMappings ?? []).map((mapping) => ({
      ...mapping,
      id: remapModel(mapping.id),
    }));

    postMessage({
      type: "saveBlendDefinition",
      blend: {
        ...blend,
        id: newBlendId,
        models,
        modelMappings,
      },
    });
  });

  const importedPresets: Preset[] = [];
  for (const sourcePreset of presetsToImport) {
    const importedPreset = clonePreset(sourcePreset);
    migratePresetNodeTypes(importedPreset);
    const sourcePresetId = importedPreset.id || importedPreset.name || "preset";
    const archiveOrigin = getToneSharingOriginMetadata(importedPreset);
    importedPreset.id = generateResourceId(sourcePresetId);
    presetIdMap.set(sourcePresetId, importedPreset.id);
    importedPreset.name = importedPreset.name || "Imported Preset";
    const contextOrigin = context.source === "toneSharingApi" && context.itemId
      ? createToneSharingOrigin(context.itemId, sourcePresetId, {
          packId: context.packId,
          creatorId: context.creatorId,
          creatorHandle: context.creatorHandle,
        })
      : undefined;
    const resolvedOrigin = archiveOrigin ?? contextOrigin;
    if (resolvedOrigin) {
      importedPreset.toneSharingOrigin = {
        ...resolvedOrigin,
        originalPresetId: resolvedOrigin.originalPresetId ?? sourcePresetId,
        importedAt: resolvedOrigin.importedAt ?? new Date().toISOString(),
        importedFromPackId: resolvedOrigin.importedFromPackId ?? context.packId,
        creatorId: resolvedOrigin.creatorId ?? context.creatorId,
        creatorHandle: resolvedOrigin.creatorHandle ?? context.creatorHandle,
        republishBlocked: resolvedOrigin.republishBlocked !== false,
      };
    }

    if (importedPreset.graph?.nodes) {
      importedPreset.graph.nodes.forEach((node) => {
        if (Array.isArray(node.resources)) {
          node.resources.forEach((res) => {
            const resourceId = res.resourceId ?? res.id;
            if (resourceId) {
              const mapped = idMap.get(resourceId) ?? resourceId;
              res.resourceId = mapped;
              res.id = mapped;
            }
          });
        }
        if (node.config?.blendId) {
          node.config.blendId = blendIdMap.get(node.config.blendId) ?? node.config.blendId;
        }
      });
    }

    if (importedPreset.audioFxModelId) {
      importedPreset.audioFxModelId = idMap.get(importedPreset.audioFxModelId) ?? importedPreset.audioFxModelId;
    }
    if (importedPreset.irId) {
      importedPreset.irId = idMap.get(importedPreset.irId) ?? importedPreset.irId;
    }

    if (Array.isArray(importedPreset.attachments)) {
      importedPreset.attachments = importedPreset.attachments.map((attachment) => ({
        ...attachment,
        id: attachment.id ? (idMap.get(attachment.id) ?? attachment.id) : attachment.id,
      }));
    }

    if (!previewOnly) {
      cachePresetInMemory(importedPreset);
      uiState.presets = [importedPreset, ...uiState.presets.filter((preset) => preset.id !== importedPreset.id)];
      uiState.presetCache.set(importedPreset.id, importedPreset);
    }
    importedPresets.push(importedPreset);
  }

  if (importedPresets.length === 0) {
    notifyImportError("Import failed", "No presets were imported");
    return [];
  }

  if (previewOnly) {
    return importedPresets;
  }

  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  const latestPreset = importedPresets[importedPresets.length - 1];
  uiState.activePresetId = latestPreset.id;
  const importedPresetIds = importedPresets.map((preset) => preset.id);
  if (presetFoldersToImport.length > 0) {
    applyImportedPresetFolders(presetFoldersToImport, presetIdMap, importedPresetIds);
  }
  populatePresetDropdown();
  renderPresetUI(clonePreset(latestPreset));
  updatePresetDropdownSelection();
  const uniqueResources = Array.from(new Map(importedResources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values());
  registerInstalledToneSharingPack({
    id: buildInstalledPackEntryId(file, context),
    title: context.titleHint ?? file.name.replace(/\.zip$/i, ""),
    source: context.source,
    importedAt: new Date().toISOString(),
    packId: context.packId,
    archiveFileName: file.name,
    presetIds: importedPresetIds,
    resources: uniqueResources,
  });
  showNotification(importedPresets.length === 1 ? "Preset imported" : "Presets imported", importedPresets.length === 1
    ? (latestPreset.name ?? "")
    : `${importedPresets.length} presets imported`);
  updatePresetActionButtons();

  return importedPresets;
}

interface GeneratorPackManifest {
  packId?: string;
  packVersion?: string;
  formatVersion?: number;
}

interface GeneratorResourceIndexItem {
  resourceId: string;
  resourceType: "nam" | "ir";
  provider: string;
  contentHash: string;
  fileExt: string;
  filePath: string;
  displayName: string;
  originalFileName: string;
}

interface GeneratorResourceIndex {
  items: GeneratorResourceIndexItem[];
}

interface GeneratorPackNode {
  id: string;
  type: string;
  params?: Record<string, number>;
  resource?: { resourceType: string; resourceId: string };
}

interface GeneratorPresetV2 {
  id: string;
  name: string;
  category?: string;
  description?: string;
  tags?: string[];
  global?: { inputTrim?: number; outputTrim?: number };
  graph: {
    nodes: GeneratorPackNode[];
    edges: Array<{ from: string; to: string }>;
  };
}

export async function importGeneratedPack(file: File, context: ArchiveImportContext = { source: "generatedPack" }): Promise<void> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Import failed", "Archive library not available");
    return;
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);

  // Detect generator pack by presence of pack-manifest.json; fall back to preset archive.
  const manifestEntry = zip.file("pack-manifest.json");
  if (!manifestEntry) {
    await importPresetArchive(file, context);
    return;
  }

  let manifest: GeneratorPackManifest;
  try {
    manifest = JSON.parse(await manifestEntry.async("text")) as GeneratorPackManifest;
  } catch {
    showNotification("Import failed", "Invalid pack manifest");
    return;
  }

  const indexEntry = zip.file("resources/indexes/resources-index.json");
  if (!indexEntry) {
    showNotification("Import failed", "Pack is missing resource index");
    return;
  }

  let index: GeneratorResourceIndex;
  try {
    index = JSON.parse(await indexEntry.async("text")) as GeneratorResourceIndex;
  } catch {
    showNotification("Import failed", "Invalid resource index");
    return;
  }

  // Import resource blobs first so they are on disk before presets are saved.
  let resourcesSkipped = 0;
  const importedResources: Array<{ type: string; id: string }> = [];
  for (const item of index.items) {
    const blobEntry = zip.file(item.filePath);
    if (!blobEntry) {
      resourcesSkipped++;
      continue;
    }
    const data = arrayBufferToBase64(await blobEntry.async("arraybuffer"));
    postMessage({
      type: "importRemoteResource",
      provider: item.provider || "generated",
      resourceType: item.resourceType,
      resourceId: item.resourceId,
      name: item.displayName,
      description: "",
      category: "",
      subfolder: `generated/${item.resourceType}`,
      fileName: item.originalFileName,
      hash: item.contentHash,
      data,
    });
    importedResources.push({ type: item.resourceType, id: item.resourceId });
  }

  // Import each preset JSON from the presets/ directory.
  const presetEntryNames = Object.keys(zip.files).filter(
    (name) => name.startsWith("presets/") && name.endsWith(".json") && !zip.files[name].dir
  );

  const importedPresets: Preset[] = [];
  for (const entryName of presetEntryNames) {
    const entry = zip.file(entryName);
    if (!entry) continue;

    let genPreset: GeneratorPresetV2;
    try {
      genPreset = JSON.parse(await entry.async("text")) as GeneratorPresetV2;
    } catch {
      continue;
    }

    const importedId = generateResourceId(genPreset.id || genPreset.name || "preset");
    const importedName = genPreset.name ?? "Generated Preset";

    const appPreset: Preset = {
      id: importedId,
      name: importedName,
      category: genPreset.category ?? "Generated",
      description: genPreset.description ?? "",
      tags: genPreset.tags,
      formatVersion: 2,
      globals: {
        inputTrim: genPreset.global?.inputTrim ?? 0,
        outputTrim: genPreset.global?.outputTrim ?? 0,
        masterVolume: 1,
        autoLevelInput: false,
        autoLevelOutput: false,
      },
      graph: {
        nodes: genPreset.graph.nodes.map((node) => ({
          id: node.id,
          type: node.type,
          displayName: node.type,
          category: "",
          bypassed: false,
          params: (node.params ?? {}) as Record<string, number>,
          config: {},
          resources: node.resource
            ? [{ resourceType: node.resource.resourceType, resourceId: node.resource.resourceId }]
            : undefined,
        })),
        edges: genPreset.graph.edges.map((edge) => ({
          from: edge.from,
          to: edge.to,
          fromPort: 0,
          toPort: 0,
          gain: 1,
        })),
      },
    };

    postMessage({
      type: "savePreset",
      presetId: importedId,
      name: importedName,
      category: appPreset.category,
      description: appPreset.description,
      includeGlobalSignalChain: false,
      preset: appPreset,
    });

    cachePresetInMemory(appPreset);
    uiState.presets = [appPreset, ...uiState.presets.filter((preset) => preset.id !== appPreset.id)];
    uiState.presetCache.set(importedId, appPreset);
    importedPresets.push(appPreset);
  }

  if (!importedPresets.length) {
    showNotification("Import failed", "No presets found in pack");
    return;
  }

  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  const latestPreset = importedPresets[0];
  uiState.activePresetId = latestPreset.id;
  populatePresetDropdown();
  renderPresetUI(clonePreset(latestPreset));
  updatePresetDropdownSelection();
  registerInstalledToneSharingPack({
    id: buildInstalledPackEntryId(file, {
      ...context,
      packId: context.packId ?? manifest.packId,
    }),
    title: context.titleHint ?? manifest.packId ?? file.name.replace(/\.zip$/i, ""),
    source: context.source,
    importedAt: new Date().toISOString(),
    packId: context.packId ?? manifest.packId,
    archiveFileName: file.name,
    presetIds: importedPresets.map((preset) => preset.id),
    resources: Array.from(new Map(importedResources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values()),
  });
  const packLabel = manifest.packId ?? file.name;
  const suffix = resourcesSkipped > 0 ? ` (${resourcesSkipped} resource file${resourcesSkipped !== 1 ? "s" : ""} missing in pack)` : "";
  showNotification(
    importedPresets.length === 1 ? "Preset imported" : `${importedPresets.length} presets imported`,
    `${packLabel}${suffix}`
  );
  updatePresetActionButtons();
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

// Delete current preset
export async function deleteCurrentPreset(): Promise<void> {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!isUserPreset(activePresetId)) {
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
      applyPresetFromLibrary(uiState.activePresetId);
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

  if (!isUserPreset(activePresetId)) {
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

  if (!isUserPreset(activePresetId)) {
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
  configureSavePresetModalLabels(true);

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const presetFolder = findFolderForPreset(uiState.presetFolders ?? [], activePresetId);
  const selectedFolderId = presetFolder?.id ?? PRESET_FOLDER_ALL_ID;
  populatePresetFolderSelect(folderSelect, selectedFolderId);

  // Pre-fill with existing preset data
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;

  if (nameInput) nameInput.value = preset.name;
  if (categoryInput) categoryInput.value = preset.category || "User";
  if (descriptionInput) descriptionInput.value = preset.description || "";
  setPresetTagsPickerValue(preset.tags ?? []);

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
}

// Update preset action button states
export function updatePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("preset-save-btn") as HTMLButtonElement | null;
  const deleteBtn = document.getElementById("preset-delete-btn") as HTMLButtonElement | null;
  const exportBtn = document.getElementById("preset-export-btn") as HTMLButtonElement | null;

  const canModify = isUserPreset(uiState.activePresetId);

  if (editBtn) {
    editBtn.disabled = !canModify;
    editBtn.title = canModify ? "Edit Preset" : "Cannot edit factory presets";
  }
  if (saveBtn) {
    saveBtn.disabled = !canModify;
    saveBtn.title = canModify ? "Save Preset" : "Cannot overwrite factory presets";
    saveBtn.classList.toggle("preset-action-btn-unsaved", Boolean(uiState.presetDirty));
  }
  if (deleteBtn) {
    deleteBtn.disabled = !canModify;
    deleteBtn.title = canModify ? "Delete Preset" : "Cannot delete factory presets";
  }
  if (exportBtn) {
    exportBtn.disabled = !uiState.activePresetId;
    exportBtn.title = uiState.activePresetId ? "Export or Publish Preset" : "No preset to export";
  }
}

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
    const title = count
      ? (isAll ? `Export all presets (${count})` : `Export ${folderName} (${count})`)
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
  const deleteBtn = document.getElementById("preset-delete-btn");
  const exportBtn = document.getElementById("preset-export-btn");
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

  if (deleteBtn) {
    deleteBtn.addEventListener("click", () => void deleteCurrentPreset());
  }

  if (exportBtn) {
    exportBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      if ((exportBtn as HTMLButtonElement).disabled) {
        return;
      }

      const chooser = document.getElementById(exportChooserId) as HTMLDivElement | null;
      if (chooser?.classList.contains("open")) {
        closeExportChooser();
        return;
      }

      openExportChooser(exportBtn as HTMLElement);
    });
  }

  if (importBtn) {
    importBtn.addEventListener("click", () => importInput?.click());
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

  // Initial state
  document.addEventListener("click", (event) => {
    const target = event.target as HTMLElement | null;
    if (!target) {
      closeExportChooser();
      return;
    }
    if (target.closest(`#${exportChooserId}`) || target.closest("#preset-export-btn")) {
      return;
    }
    closeExportChooser();
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closeExportChooser();
    }
  });

  updatePresetActionButtons();
}
