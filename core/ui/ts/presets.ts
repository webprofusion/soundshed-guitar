import { appendLog } from "./logging.js";
import { clearNotification, showNotification } from "./notifications.js";
import { renderPresetDetails, renderPresetList, renderMixerPanel } from "./views.js";
import { clonePreset, uiState, DEFAULT_GLOBAL_SIGNAL_CHAIN, getActivePresetForRender, setActivePresetDraft, setActivePresetSnapshot, setPresetDirty } from "./state.js";
import { buildAttachments, buildAttachmentsFromPreset, getDefaultPresets, initializeDataLibraries, REMOTE_BASE_URL } from "./dataLibraries.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl, sha256HexFromBase64 } from "./utils.js";
import { buildArchiveFileName, generateResourceId, requestResourceData, sanitizeFilename } from "./archiveUtils.js";
import type { Preset, Attachment, BlendDefinition, ResourceRef, LibraryResource, PresetFolder, Setlist, GraphNode } from "./types.js";
import { createEmptyPresetV2 } from "./presetV2.js";
import { bindDemoAudioControls } from "./demoAudio.js";
import { postMessage } from "./bridge.js";
import { renderSignalPathBar } from "./signalPath.js";
import { showConfirm } from "./dialogs.js";

const presetChooserLabel = document.getElementById("preset-chooser-label") as HTMLButtonElement | null;
const presetFavoriteToggle = document.getElementById("preset-favorite");
const prevPresetBtn = document.getElementById("prev-preset");
const nextPresetBtn = document.getElementById("next-preset");
const randomPresetBtn = document.getElementById("preset-random-btn");
const presetSearchElement = document.getElementById("preset-search") as HTMLInputElement | null;
const presetSelector = document.getElementById("preset-selector");
const presetLibraryPopover = document.getElementById("preset-library-popover");
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
const PRESET_FOLDER_IMPORTED_NAME = "Imported";
const PRESET_REQUEST_TIMEOUT_MS = 5000;

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
        const cleaned = stripLegacyGlobals(preset);
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
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function applyPresetFavoritesFromBackend(favorites: string[]): void {
  uiState.presetFavorites = new Set(favorites);
  setFavoriteToggleState(uiState.activePresetId);
}

export function applyPresetRatingsFromBackend(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

function openPresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
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

function loadPresetFoldersFromState(): PresetFolder[] {
  return uiState.presetFolders ? [...uiState.presetFolders] : [];
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

function populatePresetFolderSelect(select: HTMLSelectElement | null, selectedId?: string | null): void {
  if (!select) return;

  const folders = uiState.presetFolders ?? [];
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
  let didModify = false;
  const importedFolder = findFolderByName(stored, PRESET_FOLDER_IMPORTED_NAME);
  if (!importedFolder) {
    stored.push({
      id: generateResourceId(PRESET_FOLDER_IMPORTED_NAME),
      name: PRESET_FOLDER_IMPORTED_NAME,
      children: [],
      presetIds: [],
    });
    didModify = true;
  }
  uiState.presetFolders = stored;

  const resolvedActive = uiState.activePresetFolderId && uiState.activePresetFolderId !== PRESET_FOLDER_ALL_ID
    ? findFolderById(stored, uiState.activePresetFolderId)?.id
    : uiState.activePresetFolderId;
  uiState.activePresetFolderId = resolvedActive || PRESET_FOLDER_ALL_ID;

  if (didModify && persistChanges) {
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
  const folders = uiState.presetFolders ?? [];
  removePresetFromFolders(folders, presetId);
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    addPresetToFolder(folderId, presetId);
  }
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

function movePresetFolder(folderId: string, targetParentId: string): void {
  if (!folderId || folderId === PRESET_FOLDER_ALL_ID || folderId === PRESET_FOLDER_FAVORITES_ID) {
    return;
  }
  if (targetParentId === PRESET_FOLDER_FAVORITES_ID) {
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
    return presets;
  }
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], folderId);
    if (!folder) {
      return [];
    }
    const allowedIds = collectPresetIds(folder);
    presets = presets.filter((preset) => allowedIds.has(preset.id));
  }
  return presets;
}

function getPresetFolderExportName(folderId: string): string {
  if (folderId === PRESET_FOLDER_ALL_ID) {
    return "All-Presets";
  }
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    return "Favorite-Presets";
  }
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  return folder?.name || "Preset-Folder";
}

function getFilteredPresets(query: string): Preset[] {
  const normalized = query.trim().toLowerCase();
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  let basePresets = uiState.presets.slice();

  if (activeFolderId === PRESET_FOLDER_FAVORITES_ID) {
    const favorites = loadFavoritePresetIds();
    basePresets = basePresets.filter((preset) => favorites.has(preset.id));
  }

  if (activeFolderId !== PRESET_FOLDER_ALL_ID) {
    const folder = findFolderById(uiState.presetFolders ?? [], activeFolderId);
    if (folder) {
      const allowedIds = collectPresetIds(folder);
      basePresets = basePresets.filter((preset) => allowedIds.has(preset.id));
    }
  }

  if (!normalized) {
    return basePresets;
  }

  return basePresets.filter((preset) => {
    const tokens = [preset.name, preset.category, preset.description];
    return tokens.some((token) => token && token.toLowerCase().includes(normalized));
  });
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

function addPresetToImportedFolder(presetId: string): void {
  const folders = uiState.presetFolders ?? [];
  let imported = findFolderByName(folders, PRESET_FOLDER_IMPORTED_NAME);
  if (!imported) {
    imported = {
      id: generateResourceId(PRESET_FOLDER_IMPORTED_NAME),
      name: PRESET_FOLDER_IMPORTED_NAME,
      children: [],
      presetIds: [],
    };
    folders.push(imported);
  }
  if (!imported.presetIds.includes(presetId)) {
    imported.presetIds.push(presetId);
  }
  persistPresetFolders();
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
  renderPresetList(uiState.filteredPresets, uiState.activePresetId, async (presetId) => {
    await applyPresetFromLibrary(presetId);
  }, {
    folders: uiState.presetFolders ?? [],
    activeFolderId: uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
    onSelectFolder: setActivePresetFolder,
    onMovePresetToFolder: movePresetToFolder,
    onMoveFolder: movePresetFolder,
    getRating: getPresetRating,
    onRate: setPresetRating,
    getFolderPath: getPresetFolderPath,
    favoritesCount: loadFavoritePresetIds().size,
    favoritesActive: uiState.activePresetFolderId === PRESET_FOLDER_FAVORITES_ID,
    onSelectFavorites: () => setActivePresetFolder(PRESET_FOLDER_FAVORITES_ID),
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
    });
  } catch (error) {
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

export function cachePresetInMemory(preset: Preset): void {
  const cleanedPreset = stripLegacyGlobals(preset);
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
      const parentId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
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
export function openSavePresetModal(): void {
  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;
  const includeGlobalFxInput = document.getElementById("preset-include-global-fx") as HTMLInputElement | null;

  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const defaultFolderId = activeFolderId === PRESET_FOLDER_FAVORITES_ID ? PRESET_FOLDER_ALL_ID : activeFolderId;
  populatePresetFolderSelect(folderSelect, defaultFolderId);

  if (nameInput) nameInput.value = "";
  if (categoryInput) categoryInput.value = "User";
  if (descriptionInput) descriptionInput.value = "";
  if (includeGlobalFxInput) {
    const activePreset = getActivePresetForRender();
    const hasGlobalFx = Boolean((activePreset as Preset & { globalSignalChain?: unknown })?.globalSignalChain);
    includeGlobalFxInput.checked = activePreset ? hasGlobalFx : true;
  }

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  updatePresetModalJson(activePreset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;

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
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const selectedFolderId = activeFolderId === PRESET_FOLDER_FAVORITES_ID ? PRESET_FOLDER_ALL_ID : activeFolderId;

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
  });
}

export function saveCurrentPreset(): void {
  const modal = document.getElementById("save-preset-modal");
  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;
  const includeGlobalFxInput = document.getElementById("preset-include-global-fx") as HTMLInputElement | null;

  const name = nameInput?.value?.trim() || "";
  const category = categoryInput?.value?.trim() || "User";
  const description = descriptionInput?.value?.trim() || "";

  if (!name) {
    showNotification("Error", "Preset name is required");
    return;
  }

  // Check if we're editing an existing preset
  const editingPresetId = modal?.dataset.editingPresetId;

  const selectedFolderId = folderSelect?.value || PRESET_FOLDER_ALL_ID;
  const activePreset = getActivePresetForRender();
  const baseAttachments = buildAttachmentsFromPreset(activePreset ?? {} as Preset);
  const includeGlobalFx = includeGlobalFxInput ? includeGlobalFxInput.checked : true;
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
        attachments: baseAttachments,
      };
      if (includeGlobalFx) {
        updatedPreset.globalSignalChain = uiState.globalSignalChain;
      } else {
        delete (updatedPreset as Record<string, unknown>).globalSignalChain;
      }

      cachePresetInMemory(updatedPreset);
      // Also persist to disk via the C++ backend
      const savePayload: Record<string, unknown> = {
        type: "savePreset",
        presetId: updatedPreset.id,
        name: updatedPreset.name,
        category: updatedPreset.category,
        description: updatedPreset.description,
        includeGlobalSignalChain: includeGlobalFx,
        preset: updatedPreset,
      };
      if (includeGlobalFx) {
        savePayload.globalSignalChain = uiState.globalSignalChain;
      }
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
    attachments: baseAttachments,
  };
  if (includeGlobalFx) {
    newPreset.globalSignalChain = uiState.globalSignalChain;
  } else {
    delete (newPreset as Record<string, unknown>).globalSignalChain;
  }

  cachePresetInMemory(newPreset);
  // Also persist to disk via the C++ backend
  const savePayload: Record<string, unknown> = {
    type: "savePreset",
    presetId: newPreset.id,
    name: newPreset.name,
    category: newPreset.category,
    description: newPreset.description,
    includeGlobalSignalChain: includeGlobalFx,
    preset: newPreset,
  };
  if (includeGlobalFx) {
    savePayload.globalSignalChain = uiState.globalSignalChain;
  }
  postMessage(savePayload);
  uiState.presets.unshift(newPreset);
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

export function initializeSavePresetModal(): void {
  const closeBtn = document.getElementById("save-preset-modal-close");
  const cancelBtn = document.getElementById("save-preset-cancel");
  const confirmBtn = document.getElementById("save-preset-confirm");
  const modal = document.getElementById("save-preset-modal");

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
};

type PresetArchive = {
  formatVersion: number;
  preset: Preset;
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
};

type PresetCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Preset[];
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
};

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

function collectPresetBlendIds(preset: Preset): string[] {
  if (!preset.graph?.nodes) {
    return [];
  }

  const ids = new Set<string>();
  preset.graph.nodes.forEach((node) => {
    const blendId = node.config?.blendId ?? "";
    if (blendId) {
      ids.add(blendId);
    }
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

  if (preset.graph?.nodes) {
    preset.graph.nodes.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => addRef(res.type, res.id, res.filePath));
      }
    });
  }

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
  });

  return refs;
}

async function exportPresetCollectionArchive(presets: Preset[], archiveName: string): Promise<void> {
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

  const blendIds = new Set<string>();
  presets.forEach((preset) => {
    collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
  });
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.has(blend.id));
  const refMap = new Map<string, ResourceRef>();
  presets.forEach((preset) => {
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
    const fileName = buildArchiveFileName(resource, resourceType);
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      missingCount += 1;
      continue;
    }
    const hash = await sha256HexFromBase64(data);
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

  const exportPresets = presets.map((preset) => clonePreset(uiState.presetCache.get(preset.id) ?? preset));
  const archive: PresetCollectionArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    presets: exportPresets,
    resources: exportResources,
    blends: blendDefs,
  };

  zip.file("presets.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);

  if (missingCount > 0) {
    showNotification("Export warning", `${missingCount} resources could not be read`);
  }

  postMessage({
    type: "savePresetArchive",
    fileName: `${sanitizeFilename(archiveName)}.soundshed.zip`,
    data,
  });
}

async function exportActivePresetFolderArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const presets = getPresetsForFolderId(activeFolderId);
  const archiveName = getPresetFolderExportName(activeFolderId);
  await exportPresetCollectionArchive(presets, archiveName);
}

async function exportAllPresetsArchive(): Promise<void> {
  const presets = uiState.presets.slice();
  await exportPresetCollectionArchive(presets, "All-Presets");
}

async function exportSelectedPresetCollectionArchive(): Promise<void> {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  if (activeFolderId === PRESET_FOLDER_ALL_ID) {
    await exportAllPresetsArchive();
    return;
  }
  await exportActivePresetFolderArchive();
}

async function exportCurrentPresetArchive(): Promise<void> {
  const presetId = uiState.activePresetId ?? "";
  const preset = uiState.presetCache.get(presetId) ?? null;
  if (!preset) {
    showNotification("Export failed", "No preset selected");
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

  const blendIds = collectPresetBlendIds(preset);
  const blendDefs = (uiState.blendLibrary ?? []).filter((blend) => blendIds.includes(blend.id));
  const resourceRefs = collectPresetResourceRefs(preset, blendDefs);
  const exportResources: PresetArchiveResource[] = [];

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
    const fileName = buildArchiveFileName(resource, resourceType);
    const data = await requestResourceData(resourceType, resourceId);
    if (!data) {
      continue;
    }
    const hash = await sha256HexFromBase64(data);
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
  };

  zip.file("preset.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  postMessage({
    type: "savePresetArchive",
    fileName: `${sanitizeFilename(preset.name || preset.id || "preset")}.soundshed.zip`,
    data,
  });
}

async function importPresetArchive(file: File): Promise<void> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Import failed", "Archive library not available");
    return;
  }

  const buffer = await file.arrayBuffer();
  const zip = await zipLib.loadAsync(buffer);
  const presetEntry = zip.file("preset.json");
  if (!presetEntry) {
    showNotification("Import failed", "Archive is missing preset.json");
    return;
  }

  const presetText = await presetEntry.async("text");
  const archive = JSON.parse(presetText) as PresetArchive;
  if (!archive.preset) {
    showNotification("Import failed", "Archive has no preset data");
    return;
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
  const resourcesToImport = archive.resources ?? [];
  for (const resource of resourcesToImport) {
    const fileName = resource.fileName ?? "";
    const existing = getLibraryResourceByHash(resource.type, resource.hash);
    if (existing) {
      idMap.set(resource.id, existing.id);
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
        provider: "presetArchive",
        sourceFile: fileName,
      },
      data,
    });
  }

  const blendIdMap = new Map<string, string>();
  const blends = archive.blends ?? [];
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

  const importedPreset = clonePreset(archive.preset);
  importedPreset.id = generateResourceId(importedPreset.id || importedPreset.name || "preset");
  importedPreset.name = importedPreset.name?.endsWith(" (Imported)")
    ? importedPreset.name
    : `${importedPreset.name || "Imported Preset"} (Imported)`;

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

  cachePresetInMemory(importedPreset);
  uiState.presets.unshift(importedPreset);
  addPresetToImportedFolder(importedPreset.id);
  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  uiState.presetCache.set(importedPreset.id, importedPreset);
  uiState.activePresetId = importedPreset.id;
  populatePresetDropdown();
  renderPresetUI(clonePreset(importedPreset));
  updatePresetDropdownSelection();
  showNotification("Preset imported", importedPreset.name ?? "");
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
  const includeGlobalFx = Boolean((existingPreset as Preset & { globalSignalChain?: unknown }).globalSignalChain);

  const updatedPreset: Preset = {
    ...existingPreset,
    attachments: baseAttachments,
  };
  if (includeGlobalFx) {
    updatedPreset.globalSignalChain = uiState.globalSignalChain;
  } else {
    delete (updatedPreset as Record<string, unknown>).globalSignalChain;
  }

  cachePresetInMemory(updatedPreset);
  // Persist to disk via the C++ backend
  const savePayload: Record<string, unknown> = {
    type: "savePreset",
    presetId: updatedPreset.id,
    name: updatedPreset.name,
    category: updatedPreset.category,
    description: updatedPreset.description,
    includeGlobalSignalChain: includeGlobalFx,
    preset: updatedPreset,
  };
  if (includeGlobalFx) {
    savePayload.globalSignalChain = uiState.globalSignalChain;
  }
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

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  const presetFolder = findFolderForPreset(uiState.presetFolders ?? [], activePresetId);
  const selectedFolderId = presetFolder?.id ?? PRESET_FOLDER_ALL_ID;
  populatePresetFolderSelect(folderSelect, selectedFolderId);

  // Pre-fill with existing preset data
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  const categoryInput = document.getElementById("preset-category-input") as HTMLInputElement | null;
  const descriptionInput = document.getElementById("preset-description-input") as HTMLTextAreaElement | null;
  const includeGlobalFxInput = document.getElementById("preset-include-global-fx") as HTMLInputElement | null;

  if (nameInput) nameInput.value = preset.name;
  if (categoryInput) categoryInput.value = preset.category || "User";
  if (descriptionInput) descriptionInput.value = preset.description || "";
  if (includeGlobalFxInput) {
    includeGlobalFxInput.checked = Boolean((preset as Preset & { globalSignalChain?: unknown }).globalSignalChain);
  }

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  updatePresetModalJson(preset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;

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
    exportBtn.title = uiState.activePresetId ? "Export Preset" : "No preset to export";
  }
}

function updatePresetFolderExportButtons(): void {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const folderPresets = getPresetsForFolderId(activeFolderId);
  const folderName = activeFolderId === PRESET_FOLDER_ALL_ID
    ? "All Presets"
    : activeFolderId === PRESET_FOLDER_FAVORITES_ID
      ? "Favourites"
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
    exportBtn.addEventListener("click", () => void exportCurrentPresetArchive());
  }

  if (importBtn) {
    importBtn.addEventListener("click", () => importInput?.click());
  }

  if (importInput) {
    importInput.addEventListener("change", () => {
      const file = importInput.files?.[0];
      importInput.value = "";
      if (file) {
        void importPresetArchive(file);
      }
    });
  }

  // Initial state
  updatePresetActionButtons();
}
