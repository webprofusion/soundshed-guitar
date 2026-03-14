import { uiState, clonePreset, isExperimentalFeaturesEnabled } from "./state.js";
import { setAppSetting, postMessage } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { handleAppSettingUpdate } from "./tone3000.js";
import { updateSignalDiagnosticsView, updateDSPPerformancePlot } from "./views.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import { getAudioFxLibrary, getIrLibrary } from "./dataLibraries.js";
import { buildArchiveFileName, requestResourceData, sanitizeFilename, arrayBufferToBase64 } from "./archiveUtils.js";
import { sha256HexFromBase64 } from "./utils.js";
import type { AppSettingValue, Preset, BlendDefinition, ResourceRef, LibraryResource } from "./types.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { themeSwitcher, type ThemeName } from "./theme-switcher.js";
import { renderIcon } from "./iconAssets.js";
import { showConfirm } from "./dialogs.js";
import { initCompositeEditor, renderCompositeList } from "./compositeEditor.js";
import { initLayoutManager, renderLayoutList } from "./layoutManager.js";
import { initBlendManager, renderBlendList } from "./blendManager.js";
import { updateSelectedNodePeakMeter } from "./signalPath.js";

const API_KEY_SETTING = "tone3000.apiKey";
const DIAGNOSTICS_SETTING = "diagnostics.signalLevelsEnabled";
const INTERFACE_CALIBRATION_ENABLED_SETTING = "audio.interfaceCalibration.enabled";
const INTERFACE_CALIBRATION_REFERENCE_SETTING = "audio.interfaceCalibration.referenceDbu";
const ADVANCED_OPTIONS_SETTING = "ui.advancedOptionsEnabled";
const EXPERIMENTAL_FEATURES_SETTING = "ui.experimentalFeaturesEnabled";

const apiKeyInput = document.getElementById("tone3000-api-key-input") as HTMLInputElement | null;
const saveButton = document.getElementById("tone3000-api-key-save");
const clearButton = document.getElementById("tone3000-api-key-clear");
const sessionStatus = document.getElementById("tone3000-session-status");
const openAudioPreferencesButton = document.getElementById("open-audio-preferences");
const diagnosticsToggle = document.getElementById("signal-diagnostics-toggle") as HTMLInputElement | null;
const interfaceCalibrationToggle = document.getElementById("interface-calibration-toggle") as HTMLInputElement | null;
const interfaceCalibrationReferenceInput = document.getElementById("interface-calibration-reference") as HTMLInputElement | null;
const equipmentTabButtons = Array.from(document.querySelectorAll(".equipment-tab-btn"));
const equipmentTabPanels = Array.from(document.querySelectorAll(".equipment-tab-panel"));
const themeSelect = document.getElementById("theme-select") as HTMLSelectElement | null;
const librarySearchInput = document.getElementById("equipment-library-search") as HTMLInputElement | null;
const libraryTypeSelect = document.getElementById("equipment-library-type") as HTMLSelectElement | null;
const librarySourceSelect = document.getElementById("equipment-library-source") as HTMLSelectElement | null;
const libraryViewSelect = document.getElementById("equipment-library-view") as HTMLSelectElement | null;
const libraryCategorySelect = document.getElementById("equipment-library-category") as HTMLSelectElement | null;
const libraryCleanupSelect = document.getElementById("equipment-library-cleanup-scope") as HTMLSelectElement | null;
const libraryCleanupButton = document.getElementById("equipment-library-cleanup-btn") as HTMLButtonElement | null;
const libraryResults = document.getElementById("equipment-library-results");
const librarySummary = document.getElementById("equipment-library-summary");
const libraryTabButtons = Array.from(document.querySelectorAll(".library-tab-btn"));
const libraryTabPanels = Array.from(document.querySelectorAll(".library-tab-panel"));
const libraryExportButton = document.getElementById("library-export-btn");
const libraryExportResourcesSelect = document.getElementById("library-export-resources") as HTMLSelectElement | null;
const advancedOptionsToggle = document.getElementById("advanced-options-toggle") as HTMLInputElement | null;
const updateCheckToggle = document.getElementById("update-check-toggle") as HTMLInputElement | null;
const experimentalFeaturesToggle = document.getElementById("experimental-features-toggle") as HTMLInputElement | null;
const advancedTabButton = document.querySelector('.library-tab-btn[data-library-tab="advanced"]') as HTMLElement | null;
let settingsInitialized = false;
let libraryFiltersInitialized = false;
let equipmentTabsInitialized = false;
let themeSelectInitialized = false;
let libraryTabsInitialized = false;
let libraryStateRequestedAt = 0;
let suppressViewStateUpdates = false;

export function initSettingsPanel(): void {
  if (settingsInitialized) {
    return;
  }
  settingsInitialized = true;
  saveButton?.addEventListener("click", () => void saveApiKey());
  clearButton?.addEventListener("click", () => void clearApiKey());
  openAudioPreferencesButton?.addEventListener("click", () => {
    postMessage({ type: "openAudioPreferences" });
    appendLog("openAudioPreferences → requested");
  });
  initDiagnosticsToggle();
  initInterfaceCalibrationControls();
  initAdvancedOptionsToggle();
  initUpdateCheckToggle();
  initExperimentalFeaturesToggle();
  initEquipmentTabs();
  initLibraryFilters();
  initLibraryCleanup();
  initThemeSelect();
  initLibraryTabs();
  initLibraryExport();

  refreshSettingsView();
  initTone3000Browser();
}

function updateSettingsViewState(update: { equipmentTab?: string; libraryTab?: string; advancedTab?: string }): void {
  const current = uiState.uiViewState ?? {};
  const next = {
    ...current,
    settings: {
      ...(current.settings ?? {}),
      ...update,
    },
  };

  if (JSON.stringify(current) === JSON.stringify(next)) {
    return;
  }

  uiState.uiViewState = next;
  if (suppressViewStateUpdates) {
    return;
  }
  postMessage({ type: "uiViewStateChanged", viewState: next });
}

export function setSettingsViewStateSuppressed(suppressed: boolean): void {
  suppressViewStateUpdates = suppressed;
}

function initLibraryExport(): void {
  if (!libraryExportButton) {
    return;
  }

  if ((libraryExportButton as HTMLButtonElement).dataset.bound === "true") {
    return;
  }

  (libraryExportButton as HTMLButtonElement).dataset.bound = "true";
  libraryExportButton.addEventListener("click", () => void exportLibraryArchive());
}

function initLibraryCleanup(): void {
  if (!libraryCleanupButton) {
    return;
  }

  if (libraryCleanupButton.dataset.bound === "true") {
    return;
  }

  libraryCleanupButton.dataset.bound = "true";
  libraryCleanupButton.addEventListener("click", () => void cleanupUnusedResources());
}

function initEquipmentTabs(): void {
  if (equipmentTabsInitialized) {
    return;
  }
  equipmentTabsInitialized = true;
  equipmentTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.equipmentTab ?? "settings";
      activateEquipmentTab(tabId);
    });
  });
  activateEquipmentTab("settings");
}

export function initThemeSelect(): void {
  if (!themeSelect || themeSelectInitialized) {
    return;
  }
  themeSelectInitialized = true;

  const themes: Array<{ value: ThemeName; label: string }> = [
    { value: "light", label: "Light" },
    { value: "dark", label: "Dark" },
    { value: "classic", label: "Vintage" },
  ];

  themeSelect.innerHTML = themes
    .map((theme) => `<option value="${theme.value}">${theme.label}</option>`)
    .join("");

  themeSelect.value = themeSwitcher.getCurrentTheme();

  themeSelect.addEventListener("change", () => {
    const value = themeSelect.value as ThemeName;
    themeSwitcher.setTheme(value);
  });

  window.addEventListener("themeChanged", ((event: CustomEvent) => {
    themeSelect.value = event.detail.theme as ThemeName;
  }) as EventListener);
}

export function activateEquipmentTab(tabId: string): void {
  equipmentTabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.equipmentTab === tabId;
    button.classList.toggle("active", isActive);
  });

  equipmentTabPanels.forEach((panel) => {
    const isMatch = (panel as HTMLElement).id === `equipment-tab-${tabId}`;
    panel.classList.toggle("active", isMatch);
  });

  if (tabId === "performance") {
    updateDSPPerformancePlot();
  }

  updateSettingsViewState({ equipmentTab: tabId });
}

export function initLibraryFilters(): void {
  if (libraryFiltersInitialized) {
    return;
  }
  libraryFiltersInitialized = true;

  librarySearchInput?.addEventListener("input", () => renderLibraryView());
  libraryTypeSelect?.addEventListener("change", () => renderLibraryView());
  librarySourceSelect?.addEventListener("change", () => renderLibraryView());
  libraryViewSelect?.addEventListener("change", () => renderLibraryView());
  libraryCategorySelect?.addEventListener("change", () => renderLibraryView());
  bindLibraryActions();
}

export function initLibraryTabs(): void {
  if (libraryTabsInitialized) {
    return;
  }
  libraryTabsInitialized = true;

  libraryTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.libraryTab ?? "tone3000";
      activateLibraryTab(tabId);
    });
  });

  activateLibraryTab("tone3000");
}

export function activateLibraryTab(tabId: string): void {
  libraryTabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.libraryTab === tabId;
    button.classList.toggle("active", isActive);
  });

  libraryTabPanels.forEach((panel) => {
    const isMatch = (panel as HTMLElement).id === `library-tab-${tabId}`;
    panel.classList.toggle("active", isMatch);
  });

  if (tabId === "resources" && !suppressViewStateUpdates) {
    postMessage({ type: "requestState" });
  }

  if (tabId === "riffs") {
    postMessage({ type: "getRiffLibrary" });
  }

  if (tabId === "advanced") {
    initAdvancedSubTabs();
  }

  updateSettingsViewState({ libraryTab: tabId });
}

let advancedSubTabsInitialized = false;

function initAdvancedSubTabs(): void {
  if (advancedSubTabsInitialized) return;
  advancedSubTabsInitialized = true;

  initCompositeEditor();
  initBlendManager();
  initLayoutManager();

  const subTabButtons = Array.from(document.querySelectorAll<HTMLElement>(".advanced-sub-tab-btn"));
  const subTabPanels = Array.from(document.querySelectorAll<HTMLElement>(".advanced-sub-panel"));

  subTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.advancedTab ?? "composites";
      applyAdvancedSubTab(tabId, subTabButtons, subTabPanels);
    });
  });
}

export function activateAdvancedSubTab(tabId: string): void {
  initAdvancedSubTabs();
  const subTabButtons = Array.from(document.querySelectorAll<HTMLElement>(".advanced-sub-tab-btn"));
  const subTabPanels = Array.from(document.querySelectorAll<HTMLElement>(".advanced-sub-panel"));
  applyAdvancedSubTab(tabId, subTabButtons, subTabPanels);
}

function applyAdvancedSubTab(tabId: string, subTabButtons: HTMLElement[], subTabPanels: HTMLElement[]): void {
  subTabButtons.forEach((b) => {
    b.classList.toggle("active", (b as HTMLElement).dataset.advancedTab === tabId);
  });
  subTabPanels.forEach((p) => {
    p.classList.toggle("active", (p as HTMLElement).id === `advanced-tab-${tabId}`);
  });

  if (tabId === "composites") {
    renderCompositeList();
  } else if (tabId === "blends") {
    renderBlendList();
  } else if (tabId === "layouts") {
    renderLayoutList();
  }

  updateSettingsViewState({ advancedTab: tabId });
}

const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";

function initAdvancedOptionsToggle(): void {
  if (!advancedOptionsToggle || advancedOptionsToggle.dataset.bound === "true") return;
  advancedOptionsToggle.dataset.bound = "true";
  advancedOptionsToggle.addEventListener("change", () => {
    const enabled = Boolean(advancedOptionsToggle.checked);
    uiState.appSettings[ADVANCED_OPTIONS_SETTING] = enabled;
    setAppSetting(ADVANCED_OPTIONS_SETTING, enabled);
    updateAdvancedTabVisibility();
  });
}

function initUpdateCheckToggle(): void {
  if (!updateCheckToggle || updateCheckToggle.dataset.bound === "true") return;
  updateCheckToggle.dataset.bound = "true";
  updateCheckToggle.addEventListener("change", () => {
    const enabled = Boolean(updateCheckToggle.checked);
    uiState.appSettings[UPDATE_CHECK_ENABLED_SETTING] = enabled;
    setAppSetting(UPDATE_CHECK_ENABLED_SETTING, enabled);
  });
}

function initExperimentalFeaturesToggle(): void {
  if (!experimentalFeaturesToggle || experimentalFeaturesToggle.dataset.bound === "true") return;
  experimentalFeaturesToggle.dataset.bound = "true";
  experimentalFeaturesToggle.addEventListener("change", () => {
    const enabled = Boolean(experimentalFeaturesToggle.checked);
    uiState.appSettings[EXPERIMENTAL_FEATURES_SETTING] = enabled;
    setAppSetting(EXPERIMENTAL_FEATURES_SETTING, enabled);
    updateExperimentalFeaturesVisibility();
  });
}

function updateExperimentalFeaturesVisibility(): void {
  const enabled = Boolean(getSettingValue(EXPERIMENTAL_FEATURES_SETTING));
  if (advancedTabButton) {
    advancedTabButton.style.display = enabled ? "" : "none";
  }
  // If advanced tab was active and now hidden, switch to first library tab
  if (!enabled && advancedTabButton?.classList.contains("active")) {
    activateLibraryTab("tone3000");
  }
}

function updateAdvancedTabVisibility(): void {
  const enabled = Boolean(getSettingValue(ADVANCED_OPTIONS_SETTING));
  if (advancedTabButton) {
    advancedTabButton.style.display = enabled ? "" : "none";
  }
  // If advanced tab was active but now hidden, switch to first tab
  if (!enabled && advancedTabButton?.classList.contains("active")) {
    activateLibraryTab("tone3000");
  }
}

export function initDiagnosticsToggle(): void {
  if (!diagnosticsToggle) {
    return;
  }

  if (diagnosticsToggle.dataset.bound === "true") {
    return;
  }

  diagnosticsToggle.dataset.bound = "true";
  diagnosticsToggle.addEventListener("change", () => void updateDiagnosticsSetting());

  const applyBtn = document.getElementById("apply-designed-peak-btn") as HTMLButtonElement | null;
  if (applyBtn && applyBtn.dataset.bound !== "true") {
    applyBtn.dataset.bound = "true";
    applyBtn.addEventListener("click", () => {
      const peakDbfs = (applyBtn as HTMLButtonElement & { _peakDbfs?: number })._peakDbfs;
      if (peakDbfs == null || !isFinite(peakDbfs)) {
        showNotification("No peak value available — enable diagnostics and play first");
        return;
      }
      const activeId = uiState.activePresetId;
      if (!activeId) {
        showNotification("No active preset");
        return;
      }
      const preset = clonePreset(
        uiState.presetCache.get(activeId) ??
        uiState.presets.find((p) => p.id === activeId) ??
        ({} as import("./types.js").Preset)
      );
      preset.designedPeakInputDbfs = Math.round(peakDbfs * 10) / 10;
      delete (preset as Record<string, unknown>).globalSignalChain;
      uiState.presetCache.set(activeId, preset);
      postMessage({
        type: "savePreset",
        presetId: preset.id,
        name: preset.name ?? "",
        category: preset.category ?? "",
        description: preset.description ?? "",
        includeGlobalSignalChain: false,
        preset,
      });
      showNotification(`Designed peak input set to ${preset.designedPeakInputDbfs.toFixed(1)} dBFS`);
    });
  }
}

export function refreshSettingsView(): void {
  const stored = getSettingValue(API_KEY_SETTING);
  if (apiKeyInput) {
    apiKeyInput.value = "";
    apiKeyInput.placeholder = stored ? "API key stored" : "Enter your Tone3000 API key";
  }
  if (diagnosticsToggle) {
    diagnosticsToggle.checked = Boolean(getSettingValue(DIAGNOSTICS_SETTING));
  }
  const interfaceEnabledSetting = getSettingValue(INTERFACE_CALIBRATION_ENABLED_SETTING);
  const interfaceEnabled = interfaceEnabledSetting === null ? true : Boolean(interfaceEnabledSetting);
  if (interfaceCalibrationToggle) {
    interfaceCalibrationToggle.checked = interfaceEnabled;
  }
  if (interfaceCalibrationReferenceInput) {
    const referenceValue = Number(getSettingValue(INTERFACE_CALIBRATION_REFERENCE_SETTING));
    const resolvedValue = Number.isFinite(referenceValue) ? referenceValue : 12.0;
    interfaceCalibrationReferenceInput.value = resolvedValue.toFixed(1);
    interfaceCalibrationReferenceInput.disabled = !interfaceEnabled;
  }
  if (themeSelect) {
    themeSelect.value = themeSwitcher.getCurrentTheme();
  }
  if (advancedOptionsToggle) {
    advancedOptionsToggle.checked = Boolean(getSettingValue(ADVANCED_OPTIONS_SETTING));
  }
  if (updateCheckToggle) {
    const updateCheckEnabled = getSettingValue(UPDATE_CHECK_ENABLED_SETTING);
    updateCheckToggle.checked = updateCheckEnabled === null ? true : Boolean(updateCheckEnabled);
  }
  if (experimentalFeaturesToggle) {
    experimentalFeaturesToggle.checked = Boolean(getSettingValue(EXPERIMENTAL_FEATURES_SETTING));
  }
  updateAdvancedTabVisibility();
  updateExperimentalFeaturesVisibility();
  updateSessionStatus();
  updateSignalDiagnosticsView();
  renderLibraryView();
  refreshSettingsUpdateBanner();
}

async function saveApiKey(): Promise<void> {
  const apiKey = apiKeyInput?.value.trim() ?? "";
  if (!apiKey) {
    showNotification("Tone3000 API key required");
    return;
  }

  uiState.appSettings[API_KEY_SETTING] = apiKey;
  setAppSetting(API_KEY_SETTING, apiKey);
  appendLog("tone3000 api key saved");

  await handleAppSettingUpdate(API_KEY_SETTING, apiKey);
  updateSessionStatus();
}

async function clearApiKey(): Promise<void> {
  uiState.appSettings[API_KEY_SETTING] = null;
  setAppSetting(API_KEY_SETTING, null);
  if (apiKeyInput) {
    apiKeyInput.value = "";
  }

  await handleAppSettingUpdate(API_KEY_SETTING, null);
  updateSessionStatus();
}

function updateDiagnosticsSetting(): void {
  const enabled = Boolean(diagnosticsToggle?.checked);
  uiState.appSettings[DIAGNOSTICS_SETTING] = enabled;
  setAppSetting(DIAGNOSTICS_SETTING, enabled);
  if (!enabled) {
    uiState.signalDiagnostics = null;
    uiState.signalPeakHold = null;
  }
  updateSignalDiagnosticsView();
  updateSelectedNodePeakMeter();
}

function initInterfaceCalibrationControls(): void {
  if (interfaceCalibrationToggle && interfaceCalibrationToggle.dataset.bound !== "true") {
    interfaceCalibrationToggle.dataset.bound = "true";
    interfaceCalibrationToggle.addEventListener("change", () => updateInterfaceCalibrationSettings());
  }
  if (interfaceCalibrationReferenceInput && interfaceCalibrationReferenceInput.dataset.bound !== "true") {
    interfaceCalibrationReferenceInput.dataset.bound = "true";
    interfaceCalibrationReferenceInput.addEventListener("change", () => updateInterfaceCalibrationSettings());
  }
}

function updateInterfaceCalibrationSettings(): void {
  const enabled = Boolean(interfaceCalibrationToggle?.checked ?? true);
  const rawReference = Number(interfaceCalibrationReferenceInput?.value ?? 12);
  const reference = Number.isFinite(rawReference) ? rawReference : 12.0;

  uiState.appSettings[INTERFACE_CALIBRATION_ENABLED_SETTING] = enabled;
  uiState.appSettings[INTERFACE_CALIBRATION_REFERENCE_SETTING] = reference;
  setAppSetting(INTERFACE_CALIBRATION_ENABLED_SETTING, enabled);
  setAppSetting(INTERFACE_CALIBRATION_REFERENCE_SETTING, reference);

  if (interfaceCalibrationReferenceInput) {
    interfaceCalibrationReferenceInput.disabled = !enabled;
    interfaceCalibrationReferenceInput.value = reference.toFixed(1);
  }
}

function updateSessionStatus(): void {
  if (!sessionStatus) return;

  if (!uiState.appSettings[API_KEY_SETTING]) {
    sessionStatus.textContent = "No API key saved.";
    return;
  }

  const session = uiState.tone3000Session;
  if (!session) {
    sessionStatus.textContent = "Session not started.";
    return;
  }

  const remainingSeconds = Math.max(0, Math.floor((session.expiresAt - Date.now()) / 1000));
  sessionStatus.textContent = `Session active. Expires in ${remainingSeconds}s.`;
}

type LibraryArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
};

type LibraryArchive = {
  formatVersion: number;
  createdAt: string;
  resourceMode: "used" | "all";
  presets: Preset[];
  blends: BlendDefinition[];
  resources: LibraryArchiveResource[];
};

function getLibraryResource(resourceType: string, resourceId: string): LibraryResource | undefined {
  const resources = uiState.resourceLibrary[resourceType] ?? [];
  return resources.find((res) => res.id === resourceId);
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
    addRef("nam", preset.audioFxModelId ?? undefined);
  }
  if (preset.irId) {
    addRef("ir", preset.irId ?? undefined);
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

function buildUsedResourceSet(): Set<string> {
  const used = new Set<string>();
  const presets = uiState.presets.map((preset) => clonePreset(uiState.presetCache.get(preset.id) ?? preset));
  const blends = (uiState.blendLibrary ?? []).map((blend) => JSON.parse(JSON.stringify(blend)) as BlendDefinition);
  const blendIds = new Set<string>();
  presets.forEach((preset) => {
    collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
  });
  const referencedBlends = blends.filter((blend) => blendIds.has(blend.id));

  presets.forEach((preset) => {
    collectPresetResourceRefs(preset, referencedBlends).forEach((ref) => {
      if (ref.type && ref.id) {
        used.add(`${ref.type}:${ref.id}`);
      }
    });
  });

  blends.forEach((blend) => {
    (blend.models ?? []).forEach((modelId) => {
      if (modelId) {
        used.add(`nam:${modelId}`);
      }
    });
  });

  return used;
}

async function exportLibraryArchive(): Promise<void> {
  const presets = uiState.presets.map((preset) => clonePreset(uiState.presetCache.get(preset.id) ?? preset));
  const blends = (uiState.blendLibrary ?? []).map((blend) => JSON.parse(JSON.stringify(blend)) as BlendDefinition);

  if (!presets.length && !blends.length) {
    showNotification("Export failed", "No presets or blends available");
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
  const namFolder = resourcesFolder.folder("nam");
  const irFolder = resourcesFolder.folder("ir");
  if (!namFolder || !irFolder) {
    showNotification("Export failed", "Unable to create resource folders");
    return;
  }

  const resourceMode = libraryExportResourcesSelect?.value === "all" ? "all" : "used";
  const exportResources: LibraryArchiveResource[] = [];
  const resourceEntries: Array<{ type: string; resource: LibraryResource }> = [];

  if (resourceMode === "all") {
    const library = uiState.resourceLibrary;
    (library.nam ?? []).forEach((resource) => resourceEntries.push({ type: "nam", resource }));
    (library.ir ?? []).forEach((resource) => resourceEntries.push({ type: "ir", resource }));
  } else {
    const blendIds = new Set<string>();
    presets.forEach((preset) => {
      collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
    });
    const referencedBlends = blends.filter((blend) => blendIds.has(blend.id));
    const refs = new Map<string, ResourceRef>();
    presets.forEach((preset) => {
      collectPresetResourceRefs(preset, referencedBlends).forEach((ref) => {
        const resourceType = ref.resourceType ?? ref.type ?? "";
        const resourceId = ref.resourceId ?? ref.id ?? "";
        if (!resourceType || !resourceId) {
          return;
        }
        refs.set(`${resourceType}:${resourceId}`, ref);
      });
    });
    refs.forEach((ref) => {
      const resourceType = ref.resourceType ?? ref.type ?? "";
      const resourceId = ref.resourceId ?? ref.id ?? "";
      if (!resourceType || !resourceId) {
        return;
      }
      const resource = getLibraryResource(resourceType, resourceId);
      if (resource && (resourceType === "nam" || resourceType === "ir")) {
        resourceEntries.push({ type: resourceType, resource });
      }
    });
  }

  let missingCount = 0;
  for (const entry of resourceEntries) {
    const fileName = buildArchiveFileName(entry.resource, entry.type);
    const data = await requestResourceData(entry.type, entry.resource.id);
    if (!data) {
      missingCount += 1;
      continue;
    }
    const hash = await sha256HexFromBase64(data);
    const targetFolder = entry.type === "ir" ? irFolder : namFolder;
    targetFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: entry.resource.id,
      name: entry.resource.name,
      category: entry.resource.category,
      type: entry.type,
      fileName,
      hash,
    });
  }

  const archive: LibraryArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    resourceMode,
    presets,
    blends,
    resources: exportResources,
  };

  zip.file("library.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);

  if (missingCount > 0) {
    showNotification("Export warning", `${missingCount} resources could not be read`);
  }

  postMessage({
    type: "saveLibraryArchive",
    fileName: `${sanitizeFilename("Soundshed-Library")}.soundshed-library.zip`,
    data,
  });
}

type LibraryItem = {
  type: string;
  id: string;
  name: string;
  category: string;
  description: string;
  filePath: string;
  metadata?: Record<string, string>;
  fileMissing?: boolean;
};

function getLibraryItems(): LibraryItem[] {
  const items: LibraryItem[] = [];
  const builtInModels = getAudioFxLibrary();
  const builtInIrs = getIrLibrary();

  builtInModels.forEach((model) => {
    items.push({
      type: "nam",
      id: model.id,
      name: humanizeId(model.id),
      category: "Built-in",
      description: "",
      filePath: model.filePath,
      metadata: { source: "built-in" },
    });
  });

  builtInIrs.forEach((ir) => {
    items.push({
      type: "ir",
      id: ir.id,
      name: humanizeId(ir.id),
      category: "Built-in",
      description: "",
      filePath: ir.filePath,
      metadata: { source: "built-in" },
    });
  });

  const library = uiState.resourceLibrary ?? {};
  Object.entries(library).forEach(([type, resources]) => {
    if (!Array.isArray(resources)) {
      return;
    }
    resources.forEach((res) => {
      if (!res || typeof res !== "object") {
        return;
      }
      const entry = res as import("./types.js").LibraryResource;
      const entryFilePath = entry.filePath ?? "";
      items.push({
        type,
        id: entry.id ?? "",
        name: entry.name ?? entry.id ?? "",
        category: entry.category ?? "Imported",
        description: entry.description ?? "",
        filePath: entryFilePath,
        metadata: entry.metadata ?? { source: "imported" },
        fileMissing: entry.fileMissing ?? entryFilePath.length === 0,
      });
    });
  });
  return dedupeLibraryItems(items);
}

function renderLibraryView(): void {
  if (!libraryResults) {
    return;
  }

  const resourcesHaveMissingFlags = Object.values(uiState.resourceLibrary ?? {}).some((entries) =>
    (entries ?? []).some((entry) => typeof entry?.fileMissing === "boolean"),
  );
  if (!resourcesHaveMissingFlags) {
    const now = Date.now();
    if (now - libraryStateRequestedAt > 1000) {
      libraryStateRequestedAt = now;
      postMessage({ type: "requestState" });
    }
  }

  const allItems = getLibraryItems();
  const query = (librarySearchInput?.value ?? "").trim().toLowerCase();
  const typeFilter = libraryTypeSelect?.value ?? "all";
  const sourceFilter = librarySourceSelect?.value ?? "all";
  const viewMode = libraryViewSelect?.value ?? "list";

  const typeFilteredItems = typeFilter === "all"
    ? allItems
    : allItems.filter((item) => item.type === typeFilter);

  updateCategoryOptions(typeFilteredItems);
  const categoryFilter = libraryCategorySelect?.value ?? "all";

  let filtered = typeFilteredItems;
  if (categoryFilter !== "all") {
    filtered = filtered.filter((item) => item.category === categoryFilter);
  }

  if (sourceFilter !== "all") {
    filtered = filtered.filter((item) => {
      const origin = inferResourceOrigin(item.filePath).toLowerCase();
      return sourceFilter === "imported" ? origin === "imported" : origin === "built-in";
    });
  }

  if (query) {
    filtered = filtered.filter((item) => {
      const haystack = `${item.name} ${item.id} ${item.category} ${item.description} ${item.filePath}`.toLowerCase();
      return haystack.includes(query);
    });
  }

  if (viewMode === "grouped") {
    renderGroupedLibraryView(filtered, allItems.length);
    return;
  }

  renderListLibraryView(filtered, allItems.length);
}

function bindLibraryActions(): void {
  if (!libraryResults) {
    return;
  }

  if (libraryResults.dataset.bound === "true") {
    return;
  }

  libraryResults.dataset.bound = "true";
  libraryResults.addEventListener("click", (event) => {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }
    const browseButton = target.closest(".equipment-library-browse") as HTMLButtonElement | null;
    if (!browseButton) {
      return;
    }

    const resourceType = browseButton.dataset.resourceType ?? "";
    const resourceId = browseButton.dataset.resourceId ?? "";
    if (!resourceType || !resourceId) {
      return;
    }

    const item = getLibraryItems().find((entry) => entry.type === resourceType && entry.id === resourceId);
    if (!item) {
      showNotification("Replace failed", "Resource not found in library.");
      return;
    }

    promptReplaceLibraryResource(item);
  });
}

function promptReplaceLibraryResource(item: LibraryItem): void {
  const input = document.createElement("input");
  input.type = "file";
  input.accept = item.type === "nam" ? ".nam,.json" : item.type === "ir" ? ".wav" : "*";
  input.addEventListener("change", () => {
    void replaceLibraryResourceFromInput(item, input);
  });
  input.click();
}

async function replaceLibraryResourceFromInput(item: LibraryItem, input: HTMLInputElement): Promise<void> {
  const file = input.files?.[0];
  input.value = "";
  if (!file) {
    return;
  }

  const buffer = await file.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const hash = await sha256HexFromBase64(data);
  const provider = item.metadata?.provider ?? "manual";

  postMessage({
    type: "importRemoteResource",
    provider,
    resourceType: item.type,
    resourceId: item.id,
    name: item.name,
    description: item.description,
    category: item.category,
    fileName: sanitizeFilename(file.name),
    data,
    hash,
    metadata: item.metadata ?? {},
  });

  showNotification("Resource updated", item.name || item.id);
}

function renderListLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  if (librarySummary) {
    librarySummary.textContent = `Showing ${filtered.length} of ${totalCount} resources`;
  }

  if (!filtered.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No resources match the current filters.</div>`;
    return;
  }

  const usedResources = buildUsedResourceSet();

  libraryResults.innerHTML = filtered
    .map((item) => renderLibraryItemRow(item, usedResources))
    .join("");
}

function renderGroupedLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  const grouped = groupLibraryItemsByTone(filtered);
  const usedResources = buildUsedResourceSet();
  if (librarySummary) {
    librarySummary.textContent = `Showing ${grouped.length} of ${totalCount} resources (grouped)`;
  }

  if (!grouped.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No grouped resources match the current filters.</div>`;
    return;
  }

  libraryResults.innerHTML = grouped
    .map((group) => {
      const typeLabel = group.types.size === 1
        ? Array.from(group.types)[0]
        : "Mixed";
      const categoryLabel = group.categories.size === 1
        ? Array.from(group.categories)[0]
        : "Multiple";
      const originLabel = group.origins.size === 1
        ? Array.from(group.origins)[0]
        : "Mixed";
      const modelCountLabel = `${group.count} models`;
      const usedCount = group.items.filter((item) => usedResources.has(`${item.type}:${item.id}`)).length;
      const usedBadge = usedCount > 0
        ? `<span class="equipment-library-used" title="${usedCount} used by presets or blends">${renderIcon("link", "equipment-library-used-icon")}</span>`
        : "";
      return `
        <div class="results-item equipment-library-item equipment-library-group" draggable="true" data-group-id="${escapeHtml(group.groupId)}">
          <div class="results-item-main equipment-library-item-main">
            <div class="results-item-title equipment-library-item-title">${escapeHtml(group.title)}${usedBadge}</div>
            <div class="results-item-meta equipment-library-item-meta">
              <span>${escapeHtml(typeLabel)}</span>
              <span>${escapeHtml(categoryLabel)}</span>
              <span>${escapeHtml(originLabel)}</span>
              <span>${escapeHtml(group.groupId)}</span>
              <span>${escapeHtml(modelCountLabel)}</span>
              ${group.items[0]?.metadata?.authorUsername ? `<span>by: ${escapeHtml(group.items[0].metadata.authorUsername)}</span>` : ""}
              ${(group.items[0]?.metadata?.sourceUrl ?? "").startsWith("https://www.tone3000.com/") ? `<a href="${escapeHtml(group.items[0].metadata!.sourceUrl!)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>` : ""}
            </div>
          </div>
          <div class="results-item-actions equipment-library-item-actions">
            ${isExperimentalFeaturesEnabled() ? `<button class="equipment-library-create-blend" data-group-id="${escapeHtml(group.groupId)}">Create Blend</button>` : ""}
            <button class="equipment-library-delete-group icon-btn danger" data-group-id="${escapeHtml(group.groupId)}" title="Delete Group"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg></button>
          </div>
        </div>
      `;
    })
    .join("");

  bindBlendCreateButtons(grouped);
  bindBlendDeleteButtons(grouped);
  bindBlendGroupDragHandlers(grouped);
}

function renderLibraryItemRow(item: LibraryItem, usedResources: Set<string>): string {
  const typeLabel = item.type === "ir" ? "Cab IR" : item.type === "nam" ? "Amp Model" : item.type.toUpperCase();
  const categoryLabel = item.category ? item.category : "Uncategorized";
  const originLabel = inferResourceOrigin(item.filePath);
  const metadataBadges = buildMetadataBadges(item.metadata);
  const missingBadge = item.fileMissing ? "<span class=\"equipment-library-missing\">Missing File</span>" : "";
  const browseLabel = item.fileMissing ? "Browse File" : "Replace File";
  const browseAction = `<button class="equipment-library-browse" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}">${browseLabel}</button>`;
  const usedKey = `${item.type}:${item.id}`;
  const usedBadge = usedResources.has(usedKey)
    ? `<span class="equipment-library-used" title="Used by preset">${renderIcon("link", "equipment-library-used-icon")}</span>`
    : "";
  return `
    <div class="results-item equipment-library-item">
      <div class="results-item-main equipment-library-item-main">
        <div class="results-item-title equipment-library-item-title">${escapeHtml(item.name || item.id)}${usedBadge}</div>
        <div class="results-item-meta equipment-library-item-meta">
          <span>${escapeHtml(typeLabel)}</span>
          <span>${escapeHtml(categoryLabel)}</span>
          <span>${escapeHtml(originLabel)}</span>
          ${missingBadge}
          <span>${escapeHtml(item.id)}</span>
          ${metadataBadges}
        </div>
        <div class="results-item-path equipment-library-item-path" title="${escapeHtml(item.filePath)}">${escapeHtml(item.filePath || "(no file path)")}</div>
      </div>
      <div class="results-item-actions equipment-library-item-actions">
        ${browseAction}
      </div>
    </div>
  `;
}

type ToneGroup = {
  groupId: string;
  title: string;
  count: number;
  types: Set<string>;
  categories: Set<string>;
  origins: Set<string>;
  modelIds: string[];
  items: LibraryItem[];
  gear?: string;
};

function groupLibraryItemsByTone(items: LibraryItem[]): ToneGroup[] {
  const groups = new Map<string, ToneGroup>();
  items.forEach((item) => {
    const toneId = item.metadata?.toneId ?? item.metadata?.groupId;
    const toneTitle = item.metadata?.toneTitle ?? item.metadata?.groupName;
    if (!toneId || !toneTitle) {
      return;
    }

    const key = `${toneId}:${toneTitle}`;
    const existing = groups.get(key);
    const origin = inferResourceOrigin(item.filePath);
    const gear = item.metadata?.gear ?? item.category;
    if (existing) {
      existing.count += 1;
      existing.types.add(item.type);
      existing.categories.add(item.category || "Uncategorized");
      existing.origins.add(origin);
      if (item.type === "nam") {
        existing.modelIds.push(item.id);
      }
      existing.items.push(item);
      if (!existing.gear && gear) {
        existing.gear = gear;
      }
    } else {
      groups.set(key, {
        groupId: toneId,
        title: toneTitle,
        count: 1,
        types: new Set([item.type]),
        categories: new Set([item.category || "Uncategorized"]),
        origins: new Set([origin]),
        modelIds: item.type === "nam" ? [item.id] : [],
        items: [item],
        gear: gear,
      });
    }
  });

  return Array.from(groups.values()).sort((a, b) => a.title.localeCompare(b.title));
}

function bindBlendCreateButtons(groups: ToneGroup[]): void {
  const buttons = libraryResults?.querySelectorAll(".equipment-library-create-blend");
  if (!buttons) {
    return;
  }

  buttons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const groupId = (btn as HTMLElement).dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }
      void createBlendFromGroup(group);
    });
  });
}

function bindBlendDeleteButtons(groups: ToneGroup[]): void {
  const buttons = libraryResults?.querySelectorAll(".equipment-library-delete-group");
  if (!buttons) {
    return;
  }

  const usedResources = buildUsedResourceSet();

  buttons.forEach((btn) => {
    btn.addEventListener("click", async () => {
      const groupId = (btn as HTMLElement).dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }

      const deletable = group.items.filter((item) =>
        inferResourceOrigin(item.filePath) === "Imported"
        && !usedResources.has(`${item.type}:${item.id}`)
      );
      const usedCount = group.items.filter((item) => usedResources.has(`${item.type}:${item.id}`)).length;
      const skipped = group.items.length - deletable.length;

      if (!deletable.length) {
        const reason = usedCount > 0
          ? "All resources in this group are used by presets or blends."
          : "No deletable imported resources found.";
        showNotification("Delete group", reason);
        return;
      }

      const message = `Delete ${deletable.length} resources from "${group.title}"?`
        + (usedCount ? ` ${usedCount} used resources will be kept.` : "")
        + (skipped ? ` ${skipped} non-deletable resources will be kept.` : "");

      const confirmed = await showConfirm(message, "Delete group");
      if (!confirmed) {
        return;
      }

      postMessage({
        type: "cleanupResourceLibrary",
        scope: "all",
        removeFiles: true,
        resources: deletable.map((item) => ({ type: item.type, id: item.id })),
      });
    });
  });
}

function bindBlendGroupDragHandlers(groups: ToneGroup[]): void {
  const items = libraryResults?.querySelectorAll(".equipment-library-group") as NodeListOf<HTMLElement> | null;
  if (!items) {
    return;
  }

  items.forEach((item) => {
    item.addEventListener("dragstart", (event: DragEvent) => {
      const groupId = item.dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group || !event.dataTransfer) {
        return;
      }

      const payload = {
        groupId: group.groupId,
        title: group.title,
        category: normalizeBlendCategory(group.gear),
        modelIds: group.modelIds,
        modelMappings: buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary),
      };
      event.dataTransfer.setData("application/x-resource-group", JSON.stringify(payload));
      event.dataTransfer.effectAllowed = "copy";
      item.classList.add("dragging");
      document.body.classList.add("fx-dragging");
    });

    item.addEventListener("dragend", () => {
      item.classList.remove("dragging");
      document.body.classList.remove("fx-dragging");
    });
  });
}

async function createBlendFromGroup(group: ToneGroup): Promise<void> {
  const id = typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const name = prompt("Blend name", group.title) ?? group.title;
  if (!name.trim()) {
    return;
  }

  const snapMode = await showConfirm("Snap between models? Click OK for snap, Cancel for interpolate.", "Blend Mode");

  const category = normalizeBlendCategory(group.gear);
  if (!group.modelIds.length) {
    showNotification("Blend creation failed", "No amp models found in this group.");
    return;
  }

  const modelMappings = buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary);
  const blend = {
    id,
    name: name.trim(),
    category,
    models: modelMappings.map((mapping) => mapping.id),
    modelMappings,
    blendMode: snapMode ? "snap" : "interpolate",
    toneGroupId: group.groupId,
    toneGroupTitle: group.title,
  };

  postMessage({
    type: "saveBlendDefinition",
    blend,
  });
}

async function cleanupUnusedResources(): Promise<void> {
  const scope = libraryCleanupSelect?.value ?? "all";
  const allItems = getLibraryItems();
  const usedResources = buildUsedResourceSet();

  const unused = allItems.filter((item) => !usedResources.has(`${item.type}:${item.id}`));
  const scoped = scope === "all"
    ? unused
    : unused.filter((item) => item.type === scope);
  const deletable = scoped.filter((item) => inferResourceOrigin(item.filePath) === "Imported");
  const skippedBuiltIn = scoped.length - deletable.length;

  if (!deletable.length) {
    showNotification("Cleanup", skippedBuiltIn ? "No unused imported resources found." : "No unused resources found.");
    return;
  }

  const scopeLabel = scope === "all" ? "all unused resources" : `unused ${scope.toUpperCase()} resources`;
  const message = `Remove ${deletable.length} ${scopeLabel}?`
    + (skippedBuiltIn ? ` ${skippedBuiltIn} built-in resources will be kept.` : "");

  const confirmed = await showConfirm(message, "Cleanup");
  if (!confirmed) {
    return;
  }

  postMessage({
    type: "cleanupResourceLibrary",
    scope,
    removeFiles: true,
    resources: deletable.map((item) => ({ type: item.type, id: item.id })),
  });
}

function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}

function updateCategoryOptions(items: LibraryItem[]): void {
  if (!libraryCategorySelect) {
    return;
  }

  const categories = Array.from(
    new Set(items.map((item) => item.category).filter((value) => value && value.trim().length > 0)),
  ).sort((a, b) => a.localeCompare(b));

  const previousSelection = libraryCategorySelect.value;
  const options = ["<option value=\"all\">All Categories</option>"]
    .concat(categories.map((category) => `
      <option value="${escapeHtml(category)}">${escapeHtml(category)}</option>
    `.trim()))
    .join("");

  libraryCategorySelect.innerHTML = options;
  if (previousSelection !== "all" && categories.includes(previousSelection)) {
    libraryCategorySelect.value = previousSelection;
  } else {
    libraryCategorySelect.value = "all";
  }
}

function inferResourceOrigin(filePath: string): string {
  if (!filePath) {
    return "Unknown";
  }
  const normalized = filePath.toLowerCase().replace(/\\/g, "/");
  if (normalized.includes("/settings/") || normalized.includes("/appdata/") || normalized.includes("/documents/")) {
    return "Imported";
  }
  return "Built-in";
}

function buildMetadataBadges(metadata?: Record<string, string>): string {
  if (!metadata) {
    return "";
  }

  const badges: string[] = [];
  const provider = metadata.provider;
  const toneTitle = metadata.toneTitle;
  const groupId = metadata.groupId;
  const groupName = metadata.groupName;
  const gear = metadata.gear;
  const modelName = metadata.modelName;
  const entryName = metadata.entryName;
  const sourceUrl = metadata.sourceUrl;
  const authorUsername = metadata.authorUsername;

  if (provider) badges.push(`<span>${escapeHtml(provider)}</span>`);
  if (toneTitle) badges.push(`<span>tone: ${escapeHtml(toneTitle)}</span>`);
  if (groupName) badges.push(`<span>group: ${escapeHtml(groupName)}</span>`);
  if (groupId) badges.push(`<span>groupId: ${escapeHtml(groupId)}</span>`);
  if (gear) badges.push(`<span>gear: ${escapeHtml(gear)}</span>`);
  if (modelName) badges.push(`<span>model: ${escapeHtml(modelName)}</span>`);
  if (entryName) badges.push(`<span>file: ${escapeHtml(entryName)}</span>`);
  if (authorUsername) badges.push(`<span>by: ${escapeHtml(authorUsername)}</span>`);
  const safeTone3000Url = sourceUrl?.startsWith("https://www.tone3000.com/") ? sourceUrl : null;
  if (safeTone3000Url) badges.push(`<a href="${escapeHtml(safeTone3000Url)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>`);

  return badges.join("");
}

function dedupeLibraryItems(items: LibraryItem[]): LibraryItem[] {
  const map = new Map<string, LibraryItem>();
  items.forEach((item) => {
    const key = `${item.type}:${item.id}`;
    const existing = map.get(key);
    if (!existing) {
      map.set(key, item);
      return;
    }
    const existingOrigin = inferResourceOrigin(existing.filePath);
    const currentOrigin = inferResourceOrigin(item.filePath);
    if (existingOrigin === "Built-in" && currentOrigin === "Imported") {
      map.set(key, item);
    }
  });
  return Array.from(map.values());
}

function humanizeId(value: string): string {
  if (!value) {
    return "Resource";
  }
  return value
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function getSettingValue(key: string): AppSettingValue {
  return uiState.appSettings?.[key] ?? null;
}

export function updateSettingsSessionStatus(): void {
  updateSessionStatus();
}

export function refreshSettingsUpdateBanner(): void {
  const banner = document.getElementById("settings-update-banner");
  if (!banner) return;

  const update = uiState.availableUpdate;
  if (!update) {
    banner.style.display = "none";
    return;
  }

  const titleEl = document.getElementById("settings-update-banner-title");
  const notesEl = document.getElementById("settings-update-banner-notes");
  const linkEl = document.getElementById("settings-update-banner-link") as HTMLAnchorElement | null;

  if (titleEl) {
    titleEl.textContent = `Software Update Version ${update.version} is available`;
  }

  if (notesEl && update.releaseNotes) {
    const firstLine = update.releaseNotes
      .split("\n")
      .map((l) => l.trim())
      .find((l) => l.length > 0) ?? "";
    notesEl.textContent = firstLine.replace(/^#+\s*/, "").replace(/\*\*/g, "");
  }

  if (linkEl && update.downloadUrl) {
    linkEl.href = update.downloadUrl;
  }

  banner.style.display = "";
}
