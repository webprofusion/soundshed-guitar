/**
 * The feature-flag toggles, and everything that has to appear or disappear when
 * one of them changes.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { FEATURE_DEFINITIONS, FEATURE_FLAGS_CHANGED_EVENT, FEATURE_GROUPS, Features, areAdvancedLibraryFeaturesEnabled, getFeatureSettingKey, isFeatureEnabled, isJamExperienceEnabled } from "../featureFlags.js";
import type { FeatureId } from "../featureFlags.js";
import { syncPickupMatchFromState } from "../pickupMatchGlobal.js";
import { updateSelectedNodePeakMeter } from "../signalPath.js";
import { escapeHtml } from "../utils.js";
import { advancedTabButton, blendsTabButton, blendsTabPanel, compositeTabButton, compositeTabPanel, equipmentLibraryTabButton, equipmentTabButtons, factoryArchiveExportEffectPresetsToggle, factoryArchiveLoadingRow, factoryArchiveLoadingToggle, factoryArchiveSettingsSection, featureGroupsContainer, footerRiffRecordButton, jamFloatingPlayerRoot, jamPanel, jamPanelButton, jamPlayerDock, layoutsTabButton, layoutsTabPanel, libraryTabButtons, libraryToolsHeading, libraryToolsSection, resourceLibraryTabButton, tone3000SettingsHeading, tone3000SettingsSection } from "./dom.js";
import { syncUserInputCalibrationToolbarTabs } from "./inputCalibration.js";
import { updateResourceCleanupVisibility } from "./libraryCleanup.js";
import { refreshLibraryView } from "./libraryView.js";
import { activateAdvancedSubTab, activateEquipmentTab, activateLibraryTab, resolveAdvancedSubTabId, resolveEquipmentTabId, resolveLibraryTabId } from "./tabs.js";

export function initFeatureToggles(): void {
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
          <div class="settings-row" data-feature-row="${feature.id}">
            <label for="feature-toggle-${feature.id}">${escapeHtml(feature.label)}</label>
            <label class="toggle-switch">
              <input type="checkbox" id="feature-toggle-${feature.id}" data-feature-id="${feature.id}" />
              <span class="toggle-slider"></span>
            </label>
          </div>
          <div class="settings-hint" data-feature-hint="${feature.id}">${escapeHtml(feature.description)}</div>
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
    updateAppSetting(key, enabled);
    syncFeatureVisibility();
    refreshLibraryView();
    updateSelectedNodePeakMeter();
    document.dispatchEvent(new CustomEvent(FEATURE_FLAGS_CHANGED_EVENT, {
      detail: { featureId, enabled },
    }));
  });
}

export function refreshFeatureToggleStates(): void {
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

  updateResourceLibraryFeatureVisibility();
}

export function updateResourceLibraryFeatureVisibility(): void {
  if (!featureGroupsContainer) {
    return;
  }

  const resourceLibraryRow = featureGroupsContainer.querySelector<HTMLElement>(`[data-feature-row="${Features.ResourceLibrary}"]`);
  const resourceLibraryHint = featureGroupsContainer.querySelector<HTMLElement>(`[data-feature-hint="${Features.ResourceLibrary}"]`);
  const shouldHide = !isFeatureEnabled(Features.BlendTools);

  resourceLibraryRow?.toggleAttribute("hidden", shouldHide);
  resourceLibraryHint?.toggleAttribute("hidden", shouldHide);
}

export function getFeatureSettingValue(featureId: FeatureId): boolean {
  return isFeatureEnabled(featureId);
}

export function setElementVisibility(element: HTMLElement | null, visible: boolean): void {
  if (!element) {
    return;
  }

  element.toggleAttribute("hidden", !visible);
  if (!visible) {
    element.classList.remove("active");
  }
}

export function setSectionVisibility(heading: HTMLElement | null, section: HTMLElement | null, visible: boolean): void {
  setElementVisibility(heading, visible);
  setElementVisibility(section, visible);
}

export function ensureVisibleMainPanel(): void {
  const activePanel = document.querySelector<HTMLElement>(".main-content .tab-panel.active");
  if (!activePanel) {
    return;
  }

  if (!activePanel.hasAttribute("hidden")) {
    return;
  }

  (document.querySelector('.icon-btn[data-panel="visualizer"]') as HTMLButtonElement | null)?.click();
}

export function syncFeatureVisibility(): void {
  refreshFeatureToggleStates();

  const tone3000Enabled = isFeatureEnabled(Features.Tone3000);
  const resourceLibraryEnabled = isFeatureEnabled(Features.ResourceLibrary);
  const riffLibraryEnabled = isFeatureEnabled(Features.RiffLibrary);
  const jamEnabled = isFeatureEnabled(Features.Jam);
  const jamExperienceEnabled = isJamExperienceEnabled();
  const resourceCleanupEnabled = isFeatureEnabled(Features.ResourceCleanup);
  const factoryPresetArchivesEnabled = isFeatureEnabled(Features.FactoryPresetArchives);
  const debugStateCaptureEnabled = isFeatureEnabled(Features.DebugStateCapture);
  const libraryEnabled = isFeatureEnabled(Features.BlendTools);
  const advancedLibraryEnabled = areAdvancedLibraryFeaturesEnabled();
  const footerDebugCaptureButton = document.getElementById("footer-capture-debug-state-btn") as HTMLElement | null;

  setSectionVisibility(tone3000SettingsHeading, tone3000SettingsSection, tone3000Enabled);
  setSectionVisibility(libraryToolsHeading, libraryToolsSection, resourceLibraryEnabled);

  setElementVisibility(resourceLibraryTabButton, resourceLibraryEnabled);
  setElementVisibility(advancedTabButton, advancedLibraryEnabled);

  setElementVisibility(compositeTabButton, isFeatureEnabled(Features.CompositeEffects));
  setElementVisibility(compositeTabPanel, isFeatureEnabled(Features.CompositeEffects));
  setElementVisibility(blendsTabButton, isFeatureEnabled(Features.BlendTools));
  setElementVisibility(blendsTabPanel, isFeatureEnabled(Features.BlendTools));
  setElementVisibility(layoutsTabButton, isFeatureEnabled(Features.EffectLayout));
  setElementVisibility(layoutsTabPanel, isFeatureEnabled(Features.EffectLayout));

  setElementVisibility(equipmentLibraryTabButton, libraryEnabled);
  // Tones stays whatever is switched off: its Community Presets and Tone 3000 tabs follow
  // their features (tonesPanel/tabs.ts), and Your Presets needs none.
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
  if (factoryArchiveExportEffectPresetsToggle) {
    factoryArchiveExportEffectPresetsToggle.disabled = !factoryPresetArchivesEnabled;
  }

  updateResourceCleanupVisibility(resourceCleanupEnabled);
  // Pickup Match: the input flyout's tab and the indicator beside IN, and off while hidden.
  syncUserInputCalibrationToolbarTabs();
  syncPickupMatchFromState();

  const activeLibraryTab = libraryTabButtons.find((button) => button.classList.contains("active"))?.getAttribute("data-library-tab") ?? "resources";
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
