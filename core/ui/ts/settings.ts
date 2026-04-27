import { uiState, clonePreset } from "./state.js";
import { setAppSetting, postMessage } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { handleAppSettingUpdate } from "./tone3000.js";
import { updateSignalDiagnosticsView, updateDSPPerformancePlot } from "./views.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import { getAudioFxLibrary, getIrLibrary } from "./dataLibraries.js";
import { buildArchiveFileName, requestResourceData, sanitizeFilename, arrayBufferToBase64 } from "./archiveUtils.js";
import { sha256HexFromBase64 } from "./utils.js";
import type { AppSettingValue, Preset, BlendDefinition, ResourceRef, LibraryResource, UserInputCalibrationProfile } from "./types.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { themeSwitcher, type ThemeName } from "./theme-switcher.js";
import { renderIcon } from "./iconAssets.js";
import { showConfirm } from "./dialogs.js";
import { initCompositeEditor, renderCompositeList } from "./compositeEditor.js";
import { initLayoutManager, renderLayoutList } from "./layoutManager.js";
import { initBlendManager, renderBlendList } from "./blendManager.js";
import { updateSelectedNodePeakMeter } from "./signalPath.js";
import {
  FEATURE_DEFINITIONS,
  FEATURE_FLAGS_CHANGED_EVENT,
  FEATURE_GROUPS,
  Features,
  areAdvancedLibraryFeaturesEnabled,
  getFeatureSettingKey,
  isJamExperienceEnabled,
  isLibraryExperienceEnabled,
  isFeatureEnabled,
  type FeatureId,
} from "./featureFlags.js";

const API_KEY_SETTING = "tone3000.apiKey";
const DIAGNOSTICS_SETTING = "diagnostics.signalLevelsEnabled";
const USER_INPUT_CALIBRATION_PROFILES_SETTING = "audio.userInputCalibration.profiles";
const USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING = "audio.userInputCalibration.activeProfileId";
const USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS = -12.0;
const USER_INPUT_CALIBRATION_NONE_VALUE = "__none__";
const FACTORY_ARCHIVE_LOADING_SETTING = "factoryPresets.archiveLoadingEnabled";
const DSP_NOMINAL_LEVEL_SETTING = "audio.dsp.nominalOperatingLevelDbfs";
const DSP_PROTECTION_CEILING_SETTING = "audio.dsp.outputProtectionCeilingDbfs";
const DSP_NOMINAL_LEVEL_DEFAULT = -18.0;
const DSP_NOMINAL_LEVEL_MIN = -30.0;
const DSP_NOMINAL_LEVEL_MAX = -6.0;
const DSP_PROTECTION_CEILING_DEFAULT = -1.0;
const DSP_PROTECTION_CEILING_MIN = -6.0;
const DSP_PROTECTION_CEILING_MAX = 0.0;

const apiKeyInput = document.getElementById("tone3000-api-key-input") as HTMLInputElement | null;
const saveButton = document.getElementById("tone3000-api-key-save");
const clearButton = document.getElementById("tone3000-api-key-clear");
const sessionStatus = document.getElementById("tone3000-session-status");
const openAudioPreferencesButton = document.getElementById("open-audio-preferences");
const openAudioPreferencesRow = document.getElementById("open-audio-preferences-row");
const openAudioPreferencesHint = document.getElementById("open-audio-preferences-hint");
const userInputCalibrationToolbarTrigger = document.getElementById("user-input-calibration-toolbar-trigger") as HTMLButtonElement | null;
const userInputCalibrationToolbarMenu = document.getElementById("user-input-calibration-toolbar-menu") as HTMLDivElement | null;
const userInputCalibrationProfileSelect = document.getElementById("user-input-calibration-profile") as HTMLSelectElement | null;
const userInputCalibrationTrainButton = document.getElementById("user-input-calibration-train") as HTMLButtonElement | null;
const userInputCalibrationDeleteButton = document.getElementById("user-input-calibration-delete") as HTMLButtonElement | null;
const userInputCalibrationSummary = document.getElementById("user-input-calibration-summary") as HTMLElement | null;
const userInputCalibrationModal = document.getElementById("user-input-calibration-modal") as HTMLDivElement | null;
const userInputCalibrationCloseButton = document.getElementById("user-input-calibration-close") as HTMLButtonElement | null;
const userInputCalibrationCancelButton = document.getElementById("user-input-calibration-cancel") as HTMLButtonElement | null;
const userInputCalibrationSaveButton = document.getElementById("user-input-calibration-save") as HTMLButtonElement | null;
const userInputCalibrationResetButton = document.getElementById("user-input-calibration-reset") as HTMLButtonElement | null;
const userInputCalibrationNameInput = document.getElementById("user-input-calibration-name") as HTMLInputElement | null;
const userInputCalibrationDescriptionInput = document.getElementById("user-input-calibration-description") as HTMLTextAreaElement | null;
const userInputCalibrationLivePeak = document.getElementById("user-input-calibration-live-peak") as HTMLElement | null;
const userInputCalibrationCapturedPeak = document.getElementById("user-input-calibration-captured-peak") as HTMLElement | null;
const userInputCalibrationRecommendedTrim = document.getElementById("user-input-calibration-recommended-trim") as HTMLElement | null;
const userInputCalibrationStatus = document.getElementById("user-input-calibration-status") as HTMLElement | null;
const equipmentTabButtons = Array.from(document.querySelectorAll(".equipment-tab-btn"));
const equipmentTabPanels = Array.from(document.querySelectorAll(".equipment-tab-panel"));
const equipmentLibraryTabButton = document.querySelector('.equipment-tab-btn[data-equipment-tab="library"]') as HTMLElement | null;
const themeSelect = document.getElementById("theme-select") as HTMLSelectElement | null;
const librarySearchInput = document.getElementById("equipment-library-search") as HTMLInputElement | null;
const libraryTypeSelect = document.getElementById("equipment-library-type") as HTMLSelectElement | null;
const librarySourceSelect = document.getElementById("equipment-library-source") as HTMLSelectElement | null;
const libraryViewSelect = document.getElementById("equipment-library-view") as HTMLSelectElement | null;
const libraryCategorySelect = document.getElementById("equipment-library-category") as HTMLSelectElement | null;
const libraryCleanupSelect = document.getElementById("equipment-library-cleanup-scope") as HTMLSelectElement | null;
const libraryCleanupButton = document.getElementById("equipment-library-cleanup-btn") as HTMLButtonElement | null;
const libraryCleanupRow = document.getElementById("equipment-library-cleanup-row") as HTMLElement | null;
const libraryResults = document.getElementById("equipment-library-results");
const librarySummary = document.getElementById("equipment-library-summary");
const libraryTabButtons = Array.from(document.querySelectorAll(".library-tab-btn"));
const libraryTabPanels = Array.from(document.querySelectorAll(".library-tab-panel"));
const libraryExportButton = document.getElementById("library-export-btn");
const libraryExportResourcesSelect = document.getElementById("library-export-resources") as HTMLSelectElement | null;
const featureGroupsContainer = document.getElementById("settings-feature-groups") as HTMLElement | null;
const factoryArchiveLoadingToggle = document.getElementById("factory-archive-loading-toggle") as HTMLInputElement | null;
const dspNominalLevelInput = document.getElementById("dsp-nominal-level-input") as HTMLInputElement | null;
const dspProtectionCeilingInput = document.getElementById("dsp-protection-ceiling-input") as HTMLInputElement | null;
const factoryArchiveLoadingRow = document.getElementById("factory-archive-loading-row") as HTMLElement | null;
const factoryArchiveSettingsSection = document.getElementById("factory-archive-settings-section") as HTMLElement | null;
const updateCheckToggle = document.getElementById("update-check-toggle") as HTMLInputElement | null;
const advancedTabButton = document.querySelector('.library-tab-btn[data-library-tab="advanced"]') as HTMLElement | null;
const tone3000TabButton = document.querySelector('.library-tab-btn[data-library-tab="tone3000"]') as HTMLElement | null;
const resourceLibraryTabButton = document.querySelector('.library-tab-btn[data-library-tab="resources"]') as HTMLElement | null;
const compositeTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="composites"]') as HTMLElement | null;
const blendsTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="blends"]') as HTMLElement | null;
const layoutsTabButton = document.querySelector('.advanced-sub-tab-btn[data-advanced-tab="layouts"]') as HTMLElement | null;
const compositeTabPanel = document.getElementById("advanced-tab-composites") as HTMLElement | null;
const blendsTabPanel = document.getElementById("advanced-tab-blends") as HTMLElement | null;
const layoutsTabPanel = document.getElementById("advanced-tab-layouts") as HTMLElement | null;
const tone3000SettingsHeading = document.getElementById("settings-tone3000-heading") as HTMLElement | null;
const tone3000SettingsSection = document.getElementById("settings-tone3000-section") as HTMLElement | null;
const libraryToolsHeading = document.getElementById("settings-library-tools-heading") as HTMLElement | null;
const libraryToolsSection = document.getElementById("settings-library-tools-section") as HTMLElement | null;
const sharingPanelButton = document.querySelector('.icon-btn[data-panel="sharing"]') as HTMLElement | null;
const jamPanelButton = document.querySelector('.icon-btn[data-panel="jam"]') as HTMLElement | null;
const sharingPanel = document.getElementById("panel-sharing") as HTMLElement | null;
const jamPanel = document.getElementById("panel-jam") as HTMLElement | null;
const jamPlayerDock = document.getElementById("jam-player-dock") as HTMLElement | null;
const jamFloatingPlayerRoot = document.getElementById("jam-floating-player-root") as HTMLElement | null;
const footerRiffRecordButton = document.getElementById("footer-riff-record-btn") as HTMLElement | null;
let settingsInitialized = false;
let libraryFiltersInitialized = false;
let equipmentTabsInitialized = false;
let themeSelectInitialized = false;
let libraryTabsInitialized = false;
let userInputCalibrationControlsInitialized = false;
let dspLevelTargetsInitialized = false;
let libraryStateRequestedAt = 0;
let suppressViewStateUpdates = false;
let userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;
let userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;
let userInputCalibrationTrainingBypassActive = false;

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
  initUserInputCalibrationControls();
  initFeatureToggles();
  initDspLevelTargetControls();
  initFactoryArchiveLoadingToggle();
  initUpdateCheckToggle();
  initEquipmentTabs();
  initLibraryFilters();
  initLibraryCleanup();
  initThemeSelect();
  initLibraryTabs();
  initLibraryExport();

  refreshSettingsView();
  initTone3000Browser();
}

function forceDiagnosticsEnabled(): void {
  if (uiState.appSettings[DIAGNOSTICS_SETTING] !== true) {
    uiState.appSettings[DIAGNOSTICS_SETTING] = true;
    setAppSetting(DIAGNOSTICS_SETTING, true);
  }
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

function initFeatureToggles(): void {
  if (!featureGroupsContainer) {
    return;
  }

  if (!featureGroupsContainer.innerHTML.trim()) {
    featureGroupsContainer.innerHTML = FEATURE_GROUPS.map((group) => {
      const featureRows = group.featureIds.map((featureId) => {
        const feature = FEATURE_DEFINITIONS.find((entry) => entry.id === featureId);
        if (!feature) {
          return "";
        }

        return `
          <div class="settings-row">
            <label for="feature-toggle-${feature.id}">${escapeHtml(feature.label)}</label>
            <label class="toggle-switch">
              <input type="checkbox" id="feature-toggle-${feature.id}" data-feature-id="${feature.id}" />
              <span class="toggle-slider"></span>
            </label>
          </div>
          <div class="settings-hint">${escapeHtml(feature.description)}</div>
        `;
      }).join("");

      return `
        <div class="settings-section" data-feature-group="${group.id}">
          <h3>${escapeHtml(group.title)}</h3>
          <div class="settings-hint">${escapeHtml(group.description)}</div>
          ${featureRows}
        </div>
      `;
    }).join("");
  }

  if (featureGroupsContainer.dataset.bound === "true") {
    return;
  }

  featureGroupsContainer.dataset.bound = "true";
  featureGroupsContainer.addEventListener("change", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLInputElement)) {
      return;
    }

    const featureId = target.dataset.featureId as FeatureId | undefined;
    if (!featureId) {
      return;
    }

    const enabled = Boolean(target.checked);
    const key = getFeatureSettingKey(featureId);
    uiState.appSettings[key] = enabled;
    setAppSetting(key, enabled);
    syncFeatureVisibility();
    renderLibraryView();
    updateSelectedNodePeakMeter();
    document.dispatchEvent(new CustomEvent(FEATURE_FLAGS_CHANGED_EVENT, {
      detail: { featureId, enabled },
    }));
  });
}

function refreshFeatureToggleStates(): void {
  if (!featureGroupsContainer) {
    return;
  }

  featureGroupsContainer.querySelectorAll<HTMLInputElement>("input[data-feature-id]").forEach((input) => {
    const featureId = input.dataset.featureId as FeatureId | undefined;
    if (!featureId) {
      return;
    }

    const feature = FEATURE_DEFINITIONS.find((entry) => entry.id === featureId);
    if (!feature) {
      return;
    }

    input.checked = feature ? Boolean(getFeatureSettingValue(featureId)) : false;
  });
}

function getFeatureSettingValue(featureId: FeatureId): boolean {
  return isFeatureEnabled(featureId);
}

function setElementVisibility(element: HTMLElement | null, visible: boolean): void {
  if (!element) {
    return;
  }

  element.toggleAttribute("hidden", !visible);
  if (!visible) {
    element.classList.remove("active");
  }
}

function setSectionVisibility(heading: HTMLElement | null, section: HTMLElement | null, visible: boolean): void {
  setElementVisibility(heading, visible);
  setElementVisibility(section, visible);
}

function ensureVisibleMainPanel(): void {
  const activePanel = document.querySelector<HTMLElement>(".main-content .tab-panel.active");
  if (!activePanel) {
    return;
  }

  if (!activePanel.hasAttribute("hidden")) {
    return;
  }

  (document.querySelector('.icon-btn[data-panel="visualizer"]') as HTMLButtonElement | null)?.click();
}

function isLibraryTabEnabled(tabId: string): boolean {
  switch (tabId) {
    case "tone3000":
      return isFeatureEnabled(Features.Tone3000);
    case "resources":
      return isFeatureEnabled(Features.ResourceLibrary);
    case "advanced":
      return areAdvancedLibraryFeaturesEnabled();
    default:
      return true;
  }
}

function resolveLibraryTabId(preferredTabId: string): string | null {
  const orderedTabs = ["tone3000", "resources", "advanced"];
  if (isLibraryTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return orderedTabs.find((tabId) => isLibraryTabEnabled(tabId)) ?? null;
}

function isEquipmentTabEnabled(tabId: string): boolean {
  switch (tabId) {
    case "library":
      return isLibraryExperienceEnabled();
    default:
      return true;
  }
}

function resolveEquipmentTabId(preferredTabId: string): string {
  const orderedTabs = ["settings", "library", "features", "performance", "help"];
  if (isEquipmentTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return orderedTabs.find((tabId) => isEquipmentTabEnabled(tabId)) ?? "settings";
}

function isAdvancedSubTabEnabled(tabId: string): boolean {
  switch (tabId) {
    case "composites":
      return isFeatureEnabled(Features.CompositeEffects);
    case "blends":
      return isFeatureEnabled(Features.BlendTools);
    case "layouts":
      return isFeatureEnabled(Features.EffectLayout);
    default:
      return true;
  }
}

function resolveAdvancedSubTabId(preferredTabId: string): string | null {
  const orderedTabs = ["composites", "blends", "layouts"];
  if (isAdvancedSubTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return orderedTabs.find((tabId) => isAdvancedSubTabEnabled(tabId)) ?? null;
}

function syncFeatureVisibility(): void {
  refreshFeatureToggleStates();

  const tone3000Enabled = isFeatureEnabled(Features.Tone3000);
  const resourceLibraryEnabled = isFeatureEnabled(Features.ResourceLibrary);
  const riffLibraryEnabled = isFeatureEnabled(Features.RiffLibrary);
  const toneSharingEnabled = isFeatureEnabled(Features.ToneSharing);
  const jamEnabled = isFeatureEnabled(Features.Jam);
  const jamExperienceEnabled = isJamExperienceEnabled();
  const resourceCleanupEnabled = isFeatureEnabled(Features.ResourceCleanup);
  const factoryPresetArchivesEnabled = isFeatureEnabled(Features.FactoryPresetArchives);
  const debugStateCaptureEnabled = isFeatureEnabled(Features.DebugStateCapture);
  const libraryEnabled = isLibraryExperienceEnabled();
  const advancedLibraryEnabled = areAdvancedLibraryFeaturesEnabled();
  const footerDebugCaptureButton = document.getElementById("footer-capture-debug-state-btn") as HTMLElement | null;

  setSectionVisibility(tone3000SettingsHeading, tone3000SettingsSection, tone3000Enabled);
  setSectionVisibility(libraryToolsHeading, libraryToolsSection, resourceLibraryEnabled);

  setElementVisibility(tone3000TabButton, tone3000Enabled);
  setElementVisibility(resourceLibraryTabButton, resourceLibraryEnabled);
  setElementVisibility(advancedTabButton, advancedLibraryEnabled);

  setElementVisibility(compositeTabButton, isFeatureEnabled(Features.CompositeEffects));
  setElementVisibility(compositeTabPanel, isFeatureEnabled(Features.CompositeEffects));
  setElementVisibility(blendsTabButton, isFeatureEnabled(Features.BlendTools));
  setElementVisibility(blendsTabPanel, isFeatureEnabled(Features.BlendTools));
  setElementVisibility(layoutsTabButton, isFeatureEnabled(Features.EffectLayout));
  setElementVisibility(layoutsTabPanel, isFeatureEnabled(Features.EffectLayout));

  setElementVisibility(equipmentLibraryTabButton, libraryEnabled);
  setElementVisibility(sharingPanelButton, toneSharingEnabled);
  setElementVisibility(sharingPanel, toneSharingEnabled);
  setElementVisibility(jamPanelButton, jamExperienceEnabled);
  setElementVisibility(jamPanel, jamExperienceEnabled);
  setElementVisibility(jamPlayerDock, jamEnabled);
  setElementVisibility(jamFloatingPlayerRoot, jamEnabled);
  setElementVisibility(footerRiffRecordButton, riffLibraryEnabled);
  setElementVisibility(footerDebugCaptureButton, debugStateCaptureEnabled);

  if (factoryArchiveSettingsSection) {
    factoryArchiveSettingsSection.toggleAttribute("hidden", !factoryPresetArchivesEnabled);
  }
  if (factoryArchiveLoadingRow) {
    factoryArchiveLoadingRow.toggleAttribute("hidden", !factoryPresetArchivesEnabled);
  }
  if (factoryArchiveLoadingToggle) {
    factoryArchiveLoadingToggle.disabled = !factoryPresetArchivesEnabled;
  }

  updateResourceCleanupVisibility(resourceCleanupEnabled);

  const activeLibraryTab = libraryTabButtons.find((button) => button.classList.contains("active"))?.getAttribute("data-library-tab") ?? "tone3000";
  const resolvedLibraryTab = resolveLibraryTabId(activeLibraryTab);
  if (resolvedLibraryTab && resolvedLibraryTab !== activeLibraryTab) {
    activateLibraryTab(resolvedLibraryTab);
  }

  const activeEquipmentTab = equipmentTabButtons.find((button) => button.classList.contains("active"))?.getAttribute("data-equipment-tab") ?? "settings";
  const resolvedEquipmentTab = resolveEquipmentTabId(activeEquipmentTab);
  if (resolvedEquipmentTab !== activeEquipmentTab) {
    activateEquipmentTab(resolvedEquipmentTab);
  }

  const activeAdvancedSubTab = Array.from(document.querySelectorAll<HTMLElement>(".advanced-sub-tab-btn")).find((button) => button.classList.contains("active"))?.dataset.advancedTab ?? "composites";
  const resolvedAdvancedSubTab = resolveAdvancedSubTabId(activeAdvancedSubTab);
  if (advancedLibraryEnabled && resolvedAdvancedSubTab && resolvedAdvancedSubTab !== activeAdvancedSubTab) {
    activateAdvancedSubTab(resolvedAdvancedSubTab);
  }

  ensureVisibleMainPanel();
  window.dispatchEvent(new Event("resize"));
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
  const resolvedTabId = resolveEquipmentTabId(tabId);

  equipmentTabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.equipmentTab === resolvedTabId;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-selected", isActive ? "true" : "false");
  });

  equipmentTabPanels.forEach((panel) => {
    const isMatch = (panel as HTMLElement).id === `equipment-tab-${resolvedTabId}`;
    panel.classList.toggle("active", isMatch);
    panel.toggleAttribute("hidden", !isMatch);
  });

  if (resolvedTabId === "performance") {
    updateDSPPerformancePlot();
  }

  updateSettingsViewState({ equipmentTab: resolvedTabId });
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
  const resolvedTabId = resolveLibraryTabId(tabId);

  libraryTabButtons.forEach((button) => {
    const isActive = resolvedTabId !== null && (button as HTMLElement).dataset.libraryTab === resolvedTabId;
    button.classList.toggle("active", isActive);
  });

  libraryTabPanels.forEach((panel) => {
    const isMatch = resolvedTabId !== null && (panel as HTMLElement).id === `library-tab-${resolvedTabId}`;
    panel.classList.toggle("active", isMatch);
  });

  if (!resolvedTabId) {
    return;
  }

  if (resolvedTabId === "resources" && !suppressViewStateUpdates) {
    postMessage({ type: "requestState" });
  }

  if (resolvedTabId === "riffs") {
    postMessage({ type: "getRiffLibrary" });
  }

  if (resolvedTabId === "advanced") {
    initAdvancedSubTabs();
  }

  updateSettingsViewState({ libraryTab: resolvedTabId });
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
  const resolvedTabId = resolveAdvancedSubTabId(tabId);

  subTabButtons.forEach((b) => {
    b.classList.toggle("active", resolvedTabId !== null && (b as HTMLElement).dataset.advancedTab === resolvedTabId);
  });
  subTabPanels.forEach((p) => {
    p.classList.toggle("active", resolvedTabId !== null && (p as HTMLElement).id === `advanced-tab-${resolvedTabId}`);
  });

  if (!resolvedTabId) {
    return;
  }

  if (resolvedTabId === "composites") {
    renderCompositeList();
  } else if (resolvedTabId === "blends") {
    renderBlendList();
  } else if (resolvedTabId === "layouts") {
    renderLayoutList();
  }

  updateSettingsViewState({ advancedTab: resolvedTabId });
}

const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";

function initUpdateCheckToggle(): void {
  if (!updateCheckToggle || updateCheckToggle.dataset.bound === "true") return;
  updateCheckToggle.dataset.bound = "true";
  updateCheckToggle.addEventListener("change", () => {
    const enabled = Boolean(updateCheckToggle.checked);
    uiState.appSettings[UPDATE_CHECK_ENABLED_SETTING] = enabled;
    setAppSetting(UPDATE_CHECK_ENABLED_SETTING, enabled);
  });
}

function initFactoryArchiveLoadingToggle(): void {
  if (!factoryArchiveLoadingToggle || factoryArchiveLoadingToggle.dataset.bound === "true") return;
  factoryArchiveLoadingToggle.dataset.bound = "true";
  factoryArchiveLoadingToggle.addEventListener("change", () => {
    const enabled = Boolean(factoryArchiveLoadingToggle.checked);
    uiState.appSettings[FACTORY_ARCHIVE_LOADING_SETTING] = enabled;
    setAppSetting(FACTORY_ARCHIVE_LOADING_SETTING, enabled);
  });
}

function sanitizeNumericSetting(value: number, min: number, max: number, fallback: number): number {
  if (!Number.isFinite(value)) {
    return fallback;
  }
  return Math.min(max, Math.max(min, value));
}

function bindImmediateNumericSetting(
  input: HTMLInputElement | null,
  key: string,
  min: number,
  max: number,
  fallback: number,
): void {
  if (!input || input.dataset.bound === "true") return;
  input.dataset.bound = "true";

  const applyValue = (normalizeText: boolean) => {
    const parsed = Number.parseFloat(input.value);
    if (!Number.isFinite(parsed)) {
      if (normalizeText) {
        const stored = Number(getSettingValue(key));
        input.value = sanitizeNumericSetting(stored, min, max, fallback).toFixed(1);
      }
      return;
    }

    const sanitized = sanitizeNumericSetting(parsed, min, max, fallback);
    uiState.appSettings[key] = sanitized;
    setAppSetting(key, sanitized);

    if (normalizeText) {
      input.value = sanitized.toFixed(1);
    }
  };

  input.addEventListener("input", () => applyValue(false));
  input.addEventListener("change", () => applyValue(true));
  input.addEventListener("blur", () => applyValue(true));
}

function initDspLevelTargetControls(): void {
  if (dspLevelTargetsInitialized) {
    return;
  }
  dspLevelTargetsInitialized = true;

  bindImmediateNumericSetting(
    dspNominalLevelInput,
    DSP_NOMINAL_LEVEL_SETTING,
    DSP_NOMINAL_LEVEL_MIN,
    DSP_NOMINAL_LEVEL_MAX,
    DSP_NOMINAL_LEVEL_DEFAULT,
  );

  bindImmediateNumericSetting(
    dspProtectionCeilingInput,
    DSP_PROTECTION_CEILING_SETTING,
    DSP_PROTECTION_CEILING_MIN,
    DSP_PROTECTION_CEILING_MAX,
    DSP_PROTECTION_CEILING_DEFAULT,
  );
}

function updateResourceCleanupVisibility(enabled: boolean): void {
  if (libraryCleanupRow) {
    libraryCleanupRow.toggleAttribute("hidden", !enabled);
  }
  if (libraryCleanupSelect) {
    libraryCleanupSelect.disabled = !enabled;
  }
  if (libraryCleanupButton) {
    libraryCleanupButton.disabled = !enabled;
  }
}

export function initDiagnosticsToggle(): void {
  forceDiagnosticsEnabled();

  const applyBtn = document.getElementById("apply-designed-peak-btn") as HTMLButtonElement | null;
  if (applyBtn && applyBtn.dataset.bound !== "true") {
    applyBtn.dataset.bound = "true";
    applyBtn.addEventListener("click", () => {
      const peakDbfs = (applyBtn as HTMLButtonElement & { _peakDbfs?: number })._peakDbfs;
      if (peakDbfs == null || !isFinite(peakDbfs)) {
        showNotification("No peak value available yet — play audio first");
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
  forceDiagnosticsEnabled();
  refreshUserInputCalibrationView();
  if (themeSelect) {
    themeSelect.value = themeSwitcher.getCurrentTheme();
  }
  refreshFeatureToggleStates();
  if (factoryArchiveLoadingToggle) {
    const factoryArchiveLoadingEnabled = getSettingValue(FACTORY_ARCHIVE_LOADING_SETTING);
    factoryArchiveLoadingToggle.checked = factoryArchiveLoadingEnabled === null ? true : Boolean(factoryArchiveLoadingEnabled);
  }
  if (dspNominalLevelInput) {
    const nominalLevel = sanitizeNumericSetting(
      Number(getSettingValue(DSP_NOMINAL_LEVEL_SETTING)),
      DSP_NOMINAL_LEVEL_MIN,
      DSP_NOMINAL_LEVEL_MAX,
      DSP_NOMINAL_LEVEL_DEFAULT,
    );
    dspNominalLevelInput.value = nominalLevel.toFixed(1);
  }
  if (dspProtectionCeilingInput) {
    const protectionCeiling = sanitizeNumericSetting(
      Number(getSettingValue(DSP_PROTECTION_CEILING_SETTING)),
      DSP_PROTECTION_CEILING_MIN,
      DSP_PROTECTION_CEILING_MAX,
      DSP_PROTECTION_CEILING_DEFAULT,
    );
    dspProtectionCeilingInput.value = protectionCeiling.toFixed(1);
  }
  if (updateCheckToggle) {
    const updateCheckEnabled = getSettingValue(UPDATE_CHECK_ENABLED_SETTING);
    updateCheckToggle.checked = updateCheckEnabled === null ? true : Boolean(updateCheckEnabled);
  }
  const showAudioPreferences = Boolean(uiState.environment?.standalone);
  openAudioPreferencesRow?.toggleAttribute("hidden", !showAudioPreferences);
  openAudioPreferencesHint?.toggleAttribute("hidden", !showAudioPreferences);
  syncFeatureVisibility();
  updateSessionStatus();
  updateSignalDiagnosticsView();
  updateCurrentVersionDisplay();
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

function readUserInputCalibrationProfiles(): UserInputCalibrationProfile[] {
  const raw = getSettingValue(USER_INPUT_CALIBRATION_PROFILES_SETTING);
  if (!Array.isArray(raw)) {
    return [];
  }

  return raw.flatMap((entry) => {
    if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
      return [];
    }

    const candidate = entry as Record<string, unknown>;
    const id = typeof candidate.id === "string" ? candidate.id.trim() : "";
    const name = typeof candidate.name === "string" ? candidate.name.trim() : "";
    if (!id || !name) {
      return [];
    }

    const capturedPeakDbfs = Number(candidate.capturedPeakDbfs);
    const targetPeakDbfsRaw = Number(candidate.targetPeakDbfs);
    const targetPeakDbfs = Number.isFinite(targetPeakDbfsRaw)
      ? targetPeakDbfsRaw
      : USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS;
    const gainDbRaw = Number(candidate.gainDb);
    const gainDb = Number.isFinite(gainDbRaw)
      ? gainDbRaw
      : (Number.isFinite(capturedPeakDbfs) ? targetPeakDbfs - capturedPeakDbfs : 0.0);

    return [{
      id,
      name,
      description: typeof candidate.description === "string" ? candidate.description : "",
      capturedPeakDbfs: Number.isFinite(capturedPeakDbfs) ? capturedPeakDbfs : Number.NaN,
      targetPeakDbfs,
      gainDb: Math.max(-24.0, Math.min(24.0, gainDb)),
      createdAt: typeof candidate.createdAt === "string" ? candidate.createdAt : undefined,
      updatedAt: typeof candidate.updatedAt === "string" ? candidate.updatedAt : undefined,
    } satisfies UserInputCalibrationProfile];
  });
}

function readActiveUserInputCalibrationProfileId(): string | null {
  const raw = getSettingValue(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING);
  if (typeof raw !== "string") {
    return null;
  }
  const trimmed = raw.trim();
  return trimmed ? trimmed : null;
}

function getActiveUserInputCalibrationProfile(): UserInputCalibrationProfile | null {
  const profiles = readUserInputCalibrationProfiles();
  const activeProfileId = readActiveUserInputCalibrationProfileId();
  if (!activeProfileId) {
    return null;
  }
  return profiles.find((profile) => profile.id === activeProfileId) ?? null;
}

function serializeUserInputCalibrationProfiles(profiles: UserInputCalibrationProfile[]): AppSettingValue {
  return profiles.map((profile) => {
    const payload: Record<string, AppSettingValue> = {
      id: profile.id,
      name: profile.name,
      description: profile.description,
      capturedPeakDbfs: profile.capturedPeakDbfs,
      targetPeakDbfs: profile.targetPeakDbfs,
      gainDb: profile.gainDb,
    };
    if (profile.createdAt) {
      payload.createdAt = profile.createdAt;
    }
    if (profile.updatedAt) {
      payload.updatedAt = profile.updatedAt;
    }
    return payload;
  });
}

function persistUserInputCalibrationState(profiles: UserInputCalibrationProfile[], activeProfileId: string | null): void {
  const serializedProfiles = serializeUserInputCalibrationProfiles(profiles);
  uiState.appSettings[USER_INPUT_CALIBRATION_PROFILES_SETTING] = serializedProfiles;
  uiState.appSettings[USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING] = activeProfileId;
  setAppSetting(USER_INPUT_CALIBRATION_PROFILES_SETTING, serializedProfiles);
  setAppSetting(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING, activeProfileId);
  refreshUserInputCalibrationView();
}

function setUserInputCalibrationTrainingBypassActive(active: boolean): void {
  if (userInputCalibrationTrainingBypassActive === active) {
    return;
  }

  userInputCalibrationTrainingBypassActive = active;
  postMessage({ type: "setUserInputCalibrationTrainingActive", active });
  refreshUserInputCalibrationView();
}

function readDisplayedUserInputCalibrationProfileId(): string | null {
  if (userInputCalibrationTrainingBypassActive) {
    return null;
  }

  return readActiveUserInputCalibrationProfileId();
}

function formatDbfsValue(value: number): string {
  return Number.isFinite(value) ? `${value.toFixed(1)} dBFS` : "-";
}

function formatSignedDbValue(value: number): string {
  if (!Number.isFinite(value)) {
    return "-";
  }
  const sign = value >= 0 ? "+" : "";
  return `${sign}${value.toFixed(1)} dB`;
}

function getCurrentRawInputPeakDbfs(): number {
  return uiState.signalDiagnostics?.rawInput?.peakDbfs
    ?? uiState.signalDiagnostics?.input?.peakDbfs
    ?? Number.NEGATIVE_INFINITY;
}

function isUserInputCalibrationModalOpen(): boolean {
  return Boolean(userInputCalibrationModal && userInputCalibrationModal.style.display !== "none");
}

function isUserInputCalibrationToolbarMenuOpen(): boolean {
  return Boolean(userInputCalibrationToolbarMenu && !userInputCalibrationToolbarMenu.hidden);
}

function positionUserInputCalibrationToolbarMenu(): void {
  if (!userInputCalibrationToolbarMenu || !userInputCalibrationToolbarTrigger) {
    return;
  }

  const offsetParent = userInputCalibrationToolbarMenu.offsetParent;
  if (!(offsetParent instanceof HTMLElement)) {
    return;
  }

  const viewportMargin = 8;
  const parentRect = offsetParent.getBoundingClientRect();
  const triggerRect = userInputCalibrationToolbarTrigger.getBoundingClientRect();
  const menuWidth = userInputCalibrationToolbarMenu.offsetWidth;

  const centeredLeft = (triggerRect.left + (triggerRect.width / 2)) - parentRect.left - (menuWidth / 2);
  const minLeft = viewportMargin - parentRect.left;
  const maxLeft = window.innerWidth - viewportMargin - parentRect.left - menuWidth;
  const clampedLeft = Math.min(Math.max(centeredLeft, minLeft), Math.max(minLeft, maxLeft));

  userInputCalibrationToolbarMenu.style.left = `${Math.round(clampedLeft)}px`;
}

function closeUserInputCalibrationToolbarMenu(): void {
  if (userInputCalibrationToolbarMenu) {
    userInputCalibrationToolbarMenu.hidden = true;
  }
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "false");
}

function openUserInputCalibrationToolbarMenu(): void {
  if (!userInputCalibrationToolbarMenu) {
    return;
  }

  userInputCalibrationToolbarMenu.hidden = false;
  positionUserInputCalibrationToolbarMenu();
  userInputCalibrationToolbarTrigger?.setAttribute("aria-expanded", "true");
}

function toggleUserInputCalibrationToolbarMenu(): void {
  if (isUserInputCalibrationToolbarMenuOpen()) {
    closeUserInputCalibrationToolbarMenu();
    return;
  }

  openUserInputCalibrationToolbarMenu();
}

function refreshUserInputCalibrationTrainingView(): void {
  if (userInputCalibrationLivePeak) {
    userInputCalibrationLivePeak.textContent = formatDbfsValue(userInputCalibrationLivePeakDbfs);
  }
  if (userInputCalibrationCapturedPeak) {
    userInputCalibrationCapturedPeak.textContent = formatDbfsValue(userInputCalibrationTrainingPeakDbfs);
  }

  const recommendedTrimDb = Number.isFinite(userInputCalibrationTrainingPeakDbfs)
    ? Math.max(-24.0, Math.min(24.0, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS - userInputCalibrationTrainingPeakDbfs))
    : Number.NaN;
  if (userInputCalibrationRecommendedTrim) {
    userInputCalibrationRecommendedTrim.textContent = formatSignedDbValue(recommendedTrimDb);
  }

  const rawInputClipped = Boolean(uiState.signalDiagnostics?.rawInput?.clipped);
  const trimmedName = userInputCalibrationNameInput?.value.trim() ?? "";
  const canSave = trimmedName.length > 0 && Number.isFinite(userInputCalibrationTrainingPeakDbfs) && !rawInputClipped;

  if (userInputCalibrationSaveButton) {
    userInputCalibrationSaveButton.disabled = !canSave;
  }

  if (!userInputCalibrationStatus) {
    return;
  }

  if (rawInputClipped) {
    userInputCalibrationStatus.textContent = "Input clipped during training. Back the interface gain down and capture again.";
    return;
  }
  if (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)) {
    userInputCalibrationStatus.textContent = "Play your loudest picking or strumming to capture a training peak.";
    return;
  }
  userInputCalibrationStatus.textContent = `Captured ${formatDbfsValue(userInputCalibrationTrainingPeakDbfs)}. Saving this profile will apply ${formatSignedDbValue(recommendedTrimDb)} toward a ${formatDbfsValue(USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS)} target.`;
}

function resetUserInputCalibrationTrainingPeak(): void {
  userInputCalibrationLivePeakDbfs = getCurrentRawInputPeakDbfs();
  userInputCalibrationTrainingPeakDbfs = Number.isFinite(userInputCalibrationLivePeakDbfs)
    ? userInputCalibrationLivePeakDbfs
    : Number.NEGATIVE_INFINITY;
  refreshUserInputCalibrationTrainingView();
}

function openUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal || isUserInputCalibrationModalOpen()) {
    return;
  }

  closeUserInputCalibrationToolbarMenu();
  setUserInputCalibrationTrainingBypassActive(true);
  userInputCalibrationNameInput && (userInputCalibrationNameInput.value = "");
  userInputCalibrationDescriptionInput && (userInputCalibrationDescriptionInput.value = "");
  userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationModal.style.display = "flex";
  refreshUserInputCalibrationTrainingView();
  userInputCalibrationNameInput?.focus();
}

function closeUserInputCalibrationTrainingModal(): void {
  if (!userInputCalibrationModal) {
    return;
  }
  userInputCalibrationModal.style.display = "none";
  userInputCalibrationLivePeakDbfs = Number.NEGATIVE_INFINITY;
  userInputCalibrationTrainingPeakDbfs = Number.NEGATIVE_INFINITY;
  setUserInputCalibrationTrainingBypassActive(false);
}

async function deleteActiveUserInputCalibrationProfile(): Promise<void> {
  const activeProfile = getActiveUserInputCalibrationProfile();
  if (!activeProfile) {
    return;
  }

  await deleteUserInputCalibrationProfile(activeProfile.id);
}

async function deleteUserInputCalibrationProfile(profileId: string): Promise<void> {
  const profiles = readUserInputCalibrationProfiles();
  const profile = profiles.find((entry) => entry.id === profileId) ?? null;
  if (!profile) {
    return;
  }

  const confirmed = await showConfirm(
    `Delete input calibration profile "${profile.name}"?`,
    "Delete Input Calibration",
  );
  if (!confirmed) {
    return;
  }

  const activeProfileId = readActiveUserInputCalibrationProfileId();
  const remainingProfiles = profiles.filter((entry) => entry.id !== profile.id);
  persistUserInputCalibrationState(remainingProfiles, activeProfileId === profile.id ? null : activeProfileId);
  showNotification("Input calibration deleted", profile.name);
}

function saveUserInputCalibrationProfile(): void {
  const name = userInputCalibrationNameInput?.value.trim() ?? "";
  if (!name) {
    showNotification("Input calibration name required");
    return;
  }

  if (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)) {
    showNotification("No training level captured", "Play your guitar first.");
    return;
  }

  if (uiState.signalDiagnostics?.rawInput?.clipped) {
    showNotification("Input clipped during training", "Reduce interface gain and capture again.");
    return;
  }

  const gainDb = Math.max(-24.0, Math.min(24.0, USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS - userInputCalibrationTrainingPeakDbfs));
  const timestamp = new Date().toISOString();
  const profile: UserInputCalibrationProfile = {
    id: globalThis.crypto?.randomUUID?.() ?? `input-cal-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    name,
    description: userInputCalibrationDescriptionInput?.value.trim() ?? "",
    capturedPeakDbfs: userInputCalibrationTrainingPeakDbfs,
    targetPeakDbfs: USER_INPUT_CALIBRATION_TARGET_PEAK_DBFS,
    gainDb,
    createdAt: timestamp,
    updatedAt: timestamp,
  };

  const profiles = readUserInputCalibrationProfiles();
  profiles.push(profile);
  persistUserInputCalibrationState(profiles, profile.id);
  closeUserInputCalibrationTrainingModal();
  showNotification("Input calibration saved", `${profile.name} • ${formatSignedDbValue(profile.gainDb)}`);
}

function applyUserInputCalibrationProfileSelection(activeProfileId: string | null): void {
  uiState.appSettings[USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING] = activeProfileId;
  setAppSetting(USER_INPUT_CALIBRATION_ACTIVE_PROFILE_SETTING, activeProfileId);
  refreshUserInputCalibrationView();
}

function handleUserInputCalibrationProfileSelection(): void {
  const selectedValue = userInputCalibrationProfileSelect?.value ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
}

function renderUserInputCalibrationToolbarMenu(profiles: UserInputCalibrationProfile[], activeProfile: UserInputCalibrationProfile | null): void {
  if (!userInputCalibrationToolbarMenu) {
    return;
  }

  userInputCalibrationToolbarMenu.innerHTML = "";

  const header = document.createElement("div");
  header.className = "input-calibration-toolbar-menu-header";

  const title = document.createElement("span");
  title.className = "input-calibration-toolbar-menu-title";
  title.textContent = "Input Calibration";
  header.appendChild(title);

  const subtitle = document.createElement("span");
  subtitle.className = "input-calibration-toolbar-menu-subtitle";
  subtitle.textContent = userInputCalibrationTrainingBypassActive
    ? "Training bypass active"
    : (activeProfile ? `${activeProfile.name} active` : "No active calibration");
  header.appendChild(subtitle);

  userInputCalibrationToolbarMenu.appendChild(header);

  const appendProfileItem = (profile: UserInputCalibrationProfile | null): void => {
    const itemRow = document.createElement("div");
    itemRow.className = "input-calibration-toolbar-row";

    const button = document.createElement("button");
    button.type = "button";
    button.className = "input-calibration-toolbar-item";
    button.dataset.profileId = profile?.id ?? USER_INPUT_CALIBRATION_NONE_VALUE;
    button.setAttribute("role", "menuitemradio");

    const isActive = profile ? activeProfile?.id === profile.id : !activeProfile;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-checked", isActive ? "true" : "false");

    const copy = document.createElement("span");
    copy.className = "input-calibration-toolbar-item-copy";

    const name = document.createElement("span");
    name.className = "input-calibration-toolbar-item-name";
    name.textContent = profile?.name ?? "No calibration";
    copy.appendChild(name);

    const note = document.createElement("span");
    note.className = "input-calibration-toolbar-item-note";
    note.textContent = profile
      ? (profile.description || `Captured ${formatDbfsValue(profile.capturedPeakDbfs)}`)
      : "Use raw input without a global trim";
    copy.appendChild(note);

    button.appendChild(copy);

    const meta = document.createElement("span");
    meta.className = "input-calibration-toolbar-item-meta";
    meta.textContent = isActive ? "ACTIVE" : (profile ? formatSignedDbValue(profile.gainDb) : "OFF");
    button.appendChild(meta);

    itemRow.appendChild(button);

    if (profile) {
      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.className = "input-calibration-toolbar-item-delete";
      deleteButton.dataset.action = "delete";
      deleteButton.dataset.profileId = profile.id;
      deleteButton.setAttribute("role", "menuitem");
      deleteButton.setAttribute("aria-label", `Delete ${profile.name}`);
      deleteButton.title = `Delete ${profile.name}`;
      deleteButton.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg>';
      itemRow.appendChild(deleteButton);
    }

    userInputCalibrationToolbarMenu.appendChild(itemRow);
  };

  appendProfileItem(null);
  profiles.forEach((profile) => appendProfileItem(profile));

  const divider = document.createElement("div");
  divider.className = "input-calibration-toolbar-menu-divider";
  userInputCalibrationToolbarMenu.appendChild(divider);

  const trainButton = document.createElement("button");
  trainButton.type = "button";
  trainButton.className = "input-calibration-toolbar-item action";
  trainButton.dataset.action = "train";
  trainButton.setAttribute("role", "menuitem");

  const trainCopy = document.createElement("span");
  trainCopy.className = "input-calibration-toolbar-item-copy";

  const trainName = document.createElement("span");
  trainName.className = "input-calibration-toolbar-item-name";
  trainName.textContent = "Train Input Level";
  trainCopy.appendChild(trainName);

  const trainNote = document.createElement("span");
  trainNote.className = "input-calibration-toolbar-item-note";
  trainNote.textContent = "Open the training dialog and save a new profile";
  trainCopy.appendChild(trainNote);

  trainButton.appendChild(trainCopy);

  const trainMeta = document.createElement("span");
  trainMeta.className = "input-calibration-toolbar-item-meta";
  trainMeta.textContent = "TRAIN";
  trainButton.appendChild(trainMeta);

  userInputCalibrationToolbarMenu.appendChild(trainButton);

  if (userInputCalibrationToolbarTrigger) {
    userInputCalibrationToolbarTrigger.classList.toggle("has-active-calibration", Boolean(activeProfile));
    userInputCalibrationToolbarTrigger.title = activeProfile
      ? `Input calibration options (${activeProfile.name})`
      : "Input calibration options";
    userInputCalibrationToolbarTrigger.setAttribute(
      "aria-label",
      activeProfile
        ? `Input calibration options. Active profile ${activeProfile.name}`
        : "Input calibration options",
    );
  }
}

async function handleUserInputCalibrationToolbarMenuClick(event: MouseEvent): Promise<void> {
  const target = event.target;
  if (!(target instanceof HTMLElement)) {
    return;
  }

  const deleteButton = target.closest(".input-calibration-toolbar-item-delete") as HTMLButtonElement | null;
  if (deleteButton) {
    const profileId = deleteButton.dataset.profileId?.trim();
    if (!profileId) {
      return;
    }

    await deleteUserInputCalibrationProfile(profileId);
    return;
  }

  const item = target.closest(".input-calibration-toolbar-item") as HTMLButtonElement | null;
  if (!item) {
    return;
  }

  if (item.dataset.action === "train") {
    openUserInputCalibrationTrainingModal();
    return;
  }

  const selectedValue = item.dataset.profileId ?? USER_INPUT_CALIBRATION_NONE_VALUE;
  const activeProfileId = selectedValue === USER_INPUT_CALIBRATION_NONE_VALUE ? null : selectedValue;
  applyUserInputCalibrationProfileSelection(activeProfileId);
  closeUserInputCalibrationToolbarMenu();
}

function refreshUserInputCalibrationView(): void {
  const profiles = readUserInputCalibrationProfiles();
  const persistedActiveProfileId = readActiveUserInputCalibrationProfileId();
  const activeProfileId = readDisplayedUserInputCalibrationProfileId();
  const activeProfile = profiles.find((profile) => profile.id === activeProfileId) ?? null;
  const persistedActiveProfile = profiles.find((profile) => profile.id === persistedActiveProfileId) ?? null;

  if (userInputCalibrationProfileSelect) {
    userInputCalibrationProfileSelect.innerHTML = "";

    const noneOption = document.createElement("option");
    noneOption.value = USER_INPUT_CALIBRATION_NONE_VALUE;
    noneOption.textContent = "No calibration";
    userInputCalibrationProfileSelect.appendChild(noneOption);

    profiles.forEach((profile) => {
      const option = document.createElement("option");
      option.value = profile.id;
      option.textContent = profile.name;
      userInputCalibrationProfileSelect.appendChild(option);
    });

    userInputCalibrationProfileSelect.value = activeProfile ? activeProfile.id : USER_INPUT_CALIBRATION_NONE_VALUE;
  }

  if (userInputCalibrationDeleteButton) {
    userInputCalibrationDeleteButton.disabled = !activeProfile;
  }

  renderUserInputCalibrationToolbarMenu(profiles, activeProfile);

  if (userInputCalibrationSummary) {
    const currentPeakDbfs = getCurrentRawInputPeakDbfs();
    if (userInputCalibrationTrainingBypassActive) {
      userInputCalibrationSummary.textContent = persistedActiveProfile
        ? `${persistedActiveProfile.name} temporarily bypassed while training a new input level profile.`
        : "Input calibration temporarily bypassed while training a new profile.";
    } else if (activeProfile) {
      const description = activeProfile.description ? `${activeProfile.description}. ` : "";
      const currentPeak = Number.isFinite(currentPeakDbfs) ? ` Current raw peak: ${formatDbfsValue(currentPeakDbfs)}.` : "";
      userInputCalibrationSummary.textContent = `${activeProfile.name}. ${description}Applies ${formatSignedDbValue(activeProfile.gainDb)} toward ${formatDbfsValue(activeProfile.targetPeakDbfs)} from a captured ${formatDbfsValue(activeProfile.capturedPeakDbfs)}.${currentPeak}`;
    } else {
      userInputCalibrationSummary.textContent = "No global input calibration active. Train one profile for each guitar or interface gain position you want Soundshed to normalize.";
    }
  }

  if (isUserInputCalibrationToolbarMenuOpen()) {
    positionUserInputCalibrationToolbarMenu();
  }

  if (isUserInputCalibrationModalOpen()) {
    refreshUserInputCalibrationTrainingView();
  }
}

export function initUserInputCalibrationControls(): void {
  if (userInputCalibrationControlsInitialized) {
    refreshUserInputCalibrationView();
    return;
  }

  userInputCalibrationControlsInitialized = true;
  userInputCalibrationProfileSelect?.addEventListener("change", handleUserInputCalibrationProfileSelection);
  userInputCalibrationTrainButton?.addEventListener("click", openUserInputCalibrationTrainingModal);
  userInputCalibrationDeleteButton?.addEventListener("click", () => void deleteActiveUserInputCalibrationProfile());
  userInputCalibrationToolbarTrigger?.addEventListener("click", toggleUserInputCalibrationToolbarMenu);
  userInputCalibrationToolbarMenu?.addEventListener("click", (event) => void handleUserInputCalibrationToolbarMenuClick(event));
  userInputCalibrationCloseButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationCancelButton?.addEventListener("click", closeUserInputCalibrationTrainingModal);
  userInputCalibrationResetButton?.addEventListener("click", resetUserInputCalibrationTrainingPeak);
  userInputCalibrationSaveButton?.addEventListener("click", saveUserInputCalibrationProfile);
  userInputCalibrationNameInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationDescriptionInput?.addEventListener("input", refreshUserInputCalibrationTrainingView);
  userInputCalibrationModal?.addEventListener("mousedown", (event) => {
    if (event.target === userInputCalibrationModal) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  document.addEventListener("mousedown", (event) => {
    const target = event.target;
    if (!(target instanceof Node)) {
      return;
    }

    if (userInputCalibrationToolbarMenu?.contains(target) || userInputCalibrationToolbarTrigger?.contains(target)) {
      return;
    }

    closeUserInputCalibrationToolbarMenu();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") {
      return;
    }

    if (isUserInputCalibrationToolbarMenuOpen()) {
      closeUserInputCalibrationToolbarMenu();
    }

    if (isUserInputCalibrationModalOpen()) {
      closeUserInputCalibrationTrainingModal();
    }
  });
  window.addEventListener("resize", () => {
    if (isUserInputCalibrationToolbarMenuOpen()) {
      positionUserInputCalibrationToolbarMenu();
    }
  });
  refreshUserInputCalibrationView();
}

export function handleUserInputCalibrationDiagnosticsUpdate(): void {
  userInputCalibrationLivePeakDbfs = getCurrentRawInputPeakDbfs();
  if (!isUserInputCalibrationModalOpen()) {
    return;
  }

  if (Number.isFinite(userInputCalibrationLivePeakDbfs)
      && (!Number.isFinite(userInputCalibrationTrainingPeakDbfs)
        || userInputCalibrationLivePeakDbfs > userInputCalibrationTrainingPeakDbfs)) {
    userInputCalibrationTrainingPeakDbfs = userInputCalibrationLivePeakDbfs;
  }

  refreshUserInputCalibrationTrainingView();
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
      const origin = inferResourceOrigin(item.filePath, item.metadata).toLowerCase();
      return origin === sourceFilter;
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

    const editButton = target.closest(".equipment-library-edit") as HTMLButtonElement | null;
    if (editButton) {
      const resourceType = editButton.dataset.resourceType ?? "";
      const resourceId = editButton.dataset.resourceId ?? "";
      const item = getLibraryItems().find((entry) => entry.type === resourceType && entry.id === resourceId);
      if (!item) {
        showNotification("Edit failed", "Resource not found in library.");
        return;
      }
      void promptEditLibraryResource(item);
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

    if (inferResourceOrigin(item.filePath, item.metadata) === "Local") {
      postMessage({
        type: "browseLibraryResourcePath",
        resourceType: item.type,
        resourceId: item.id,
      });
      return;
    }

    promptReplaceLibraryResource(item);
  });

  const updateDropActive = (active: boolean) => {
    libraryResults.classList.toggle("riff-drop-active", active);
  };

  const extractSupportedLocalFiles = (event: DragEvent): File[] => {
    return Array.from(event.dataTransfer?.files ?? []).filter((file) => inferDroppedLocalResourceType(file.name) !== null);
  };

  libraryResults.addEventListener("dragover", (event) => {
    const files = extractSupportedLocalFiles(event);
    if (!files.length) {
      return;
    }
    event.preventDefault();
    updateDropActive(true);
    if (event.dataTransfer) {
      event.dataTransfer.dropEffect = "copy";
    }
  });

  libraryResults.addEventListener("dragleave", (event) => {
    if (event.target === libraryResults) {
      updateDropActive(false);
    }
  });

  libraryResults.addEventListener("drop", (event) => {
    const files = extractSupportedLocalFiles(event);
    updateDropActive(false);
    if (!files.length) {
      return;
    }
    event.preventDefault();
    void importDroppedLibraryFiles(files);
  });
}

function inferDroppedLocalResourceType(fileName: string): "nam" | "ir" | null {
  const lower = fileName.trim().toLowerCase();
  if (lower.endsWith(".wav") || lower.endsWith(".ir")) {
    return "ir";
  }
  if (lower.endsWith(".nam") || lower.endsWith(".json")) {
    return "nam";
  }
  return null;
}

async function importDroppedLibraryFiles(files: File[]): Promise<void> {
  for (const file of files) {
    await importDroppedLibraryFile(file);
  }
}

async function importDroppedLibraryFile(file: File): Promise<void> {
  const resourceType = inferDroppedLocalResourceType(file.name);
  if (!resourceType) {
    return;
  }

  const buffer = await file.arrayBuffer();
  const data = arrayBufferToBase64(buffer);
  const hash = await sha256HexFromBase64(data);
  const nativePath = typeof (file as File & { path?: unknown }).path === "string"
    ? String((file as File & { path?: unknown }).path)
    : "";
  const label = file.name.replace(/\.[^.]+$/, "").trim() || file.name;

  postMessage({
    type: "saveLocalLibraryResource",
    resourceType,
    ...(nativePath ? { filePath: nativePath } : { data }),
    fileName: sanitizeFilename(file.name),
    name: label,
    category: "Local",
    hash,
    metadata: {
      provider: "local",
    },
  });
}

async function promptEditLibraryResource(item: LibraryItem): Promise<void> {
  if (inferResourceOrigin(item.filePath, item.metadata) === "Built-in") {
    showNotification("Edit unavailable", "Built-in resources cannot be edited.");
    return;
  }

  const name = window.prompt("Resource title", item.name);
  if (name === null) {
    return;
  }
  const category = window.prompt("Category", item.category);
  if (category === null) {
    return;
  }
  const description = window.prompt("Description", item.description ?? "");
  if (description === null) {
    return;
  }
  const metadataText = window.prompt("Metadata JSON", JSON.stringify(item.metadata ?? {}, null, 2));
  if (metadataText === null) {
    return;
  }

  let metadata: Record<string, string> = {};
  const trimmedMetadata = metadataText.trim();
  if (trimmedMetadata) {
    try {
      const parsed = JSON.parse(trimmedMetadata) as Record<string, unknown>;
      if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
        throw new Error("Metadata must be a JSON object.");
      }
      metadata = Object.fromEntries(
        Object.entries(parsed)
          .filter(([, value]) => value !== null && value !== undefined)
          .map(([key, value]) => [key, String(value)]),
      );
    } catch (error) {
      showNotification("Invalid metadata", error instanceof Error ? error.message : "Metadata must be valid JSON.");
      return;
    }
  }

  postMessage({
    type: "updateLibraryResource",
    resourceType: item.type,
    resourceId: item.id,
    name: name.trim() || item.id,
    category: category.trim(),
    description,
    metadata,
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
            ${isFeatureEnabled(Features.BlendTools) ? `<button class="equipment-library-create-blend" data-group-id="${escapeHtml(group.groupId)}">Create Blend</button>` : ""}
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
  const originLabel = inferResourceOrigin(item.filePath, item.metadata);
  const metadataBadges = buildMetadataBadges(item.metadata);
  const missingBadge = item.fileMissing ? "<span class=\"equipment-library-missing\">Missing File</span>" : "";
  const browseLabel = originLabel === "Local"
    ? (item.fileMissing ? "Set Path" : "Change Path")
    : (item.fileMissing ? "Browse File" : "Replace File");
  const browseAction = `<button class="equipment-library-browse" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}">${browseLabel}</button>`;
  const editAction = originLabel !== "Built-in"
    ? `<button class="equipment-library-edit" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}">Edit</button>`
    : "";
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
        ${editAction}
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
    const origin = inferResourceOrigin(item.filePath, item.metadata);
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
        inferResourceOrigin(item.filePath, item.metadata) === "Imported"
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
  if (!isFeatureEnabled(Features.ResourceCleanup)) {
    showNotification("Cleanup unavailable", "Enable Resource Cleanup Tools in Settings > Features to use Resource Library cleanup.");
    return;
  }

  const scope = libraryCleanupSelect?.value ?? "all";
  const allItems = getLibraryItems();
  const usedResources = buildUsedResourceSet();

  const unused = allItems.filter((item) => !usedResources.has(`${item.type}:${item.id}`));
  const scoped = scope === "all"
    ? unused
    : unused.filter((item) => item.type === scope);
  const deletable = scoped.filter((item) => inferResourceOrigin(item.filePath, item.metadata) === "Imported");
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

function inferResourceOrigin(filePath: string, metadata?: Record<string, string>): string {
  const provider = (metadata?.provider ?? metadata?.archiveProvider ?? metadata?.source ?? "").trim().toLowerCase();
  if (provider === "tone3000") {
    return "Tone3000";
  }

  const importedProviders = new Set([
    "presetarchive",
    "blendarchive",
    "factory-archives",
    "remote",
    "generatedpack",
    "zipimport",
    "tonesharingapi",
    "imported",
  ]);
  if (provider && importedProviders.has(provider)) {
    return "Imported";
  }

  if (!filePath) {
    return "Unknown";
  }
  const normalized = filePath.toLowerCase().replace(/\\/g, "/");
  if (normalized.includes("/content/tone3000/")) {
    return "Tone3000";
  }
  if (normalized.includes("/content/presetarchive/")
    || normalized.includes("/content/blendarchive/")
    || normalized.includes("/content/factory-archives/")
    || normalized.includes("/content/remote/")
    || normalized.includes("/imports/")) {
    return "Imported";
  }
  if (normalized.includes("/settings/") || normalized.includes("/appdata/") || normalized.includes("/documents/")) {
    return "Local";
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
  const originRank = (origin: string): number => {
    if (origin === "Imported") return 3;
    if (origin === "Local") return 2;
    if (origin === "Built-in") return 1;
    return 0;
  };

  items.forEach((item) => {
    const key = `${item.type}:${item.id}`;
    const existing = map.get(key);
    if (!existing) {
      map.set(key, item);
      return;
    }
    const existingOrigin = inferResourceOrigin(existing.filePath, existing.metadata);
    const currentOrigin = inferResourceOrigin(item.filePath, item.metadata);
    if (originRank(currentOrigin) > originRank(existingOrigin)) {
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

/** Updates the current version display in the Software Updates section. */
function updateCurrentVersionDisplay(): void {
  const versionEl = document.getElementById("current-version-display");
  if (!versionEl) return;

  const version = uiState.environment?.version ?? "Unknown";
  versionEl.textContent = version;
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
