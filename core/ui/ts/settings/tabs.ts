/**
 * The three levels of tab in this panel — equipment, library, and the advanced
 * sub-tabs — and the feature flags that decide which of them exist.
 *
 * Resolving a tab id is separate from activating one because a tab can be
 * disabled while it is the one being restored; the resolver picks the fallback.
 */

import { initBlendManager, renderBlendList } from "../blendManager.js";
import { postMessage } from "../bridge.js";
import { flushShownViews } from "../deferredView.js";
import { initCompositeEditor, renderCompositeList } from "../compositeEditor.js";
import { Features, areAdvancedLibraryFeaturesEnabled, isFeatureEnabled } from "../featureFlags.js";
import { initLayoutManager, renderLayoutList } from "../layoutManager.js";
import { scheduleDSPPerformancePlotUpdate, updateSignalDiagnosticsView } from "../views.js";
import { equipmentTabButtons, equipmentTabPanels, libraryTabButtons, libraryTabPanels } from "./dom.js";
import { isSettingsViewStateSuppressed, updateSettingsViewState } from "./viewState.js";

let equipmentTabsInitialized = false;

let libraryTabsInitialized = false;

/** The Tone 3000 browser used to be the first of these; it is a Tones tab now (tonesPanel/tabs.ts). */
const LIBRARY_TAB_IDS = ["resources", "advanced"];

export function isLibraryTabEnabled(tabId: string): boolean {
  switch (tabId) {
    case "resources":
      return isFeatureEnabled(Features.ResourceLibrary);
    case "advanced":
      return areAdvancedLibraryFeaturesEnabled();
    default:
      // Not a tab here, e.g. "tone3000" in a view saved before it moved.
      return false;
  }
}

export function resolveLibraryTabId(preferredTabId: string): string | null {
  if (isLibraryTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return LIBRARY_TAB_IDS.find((tabId) => isLibraryTabEnabled(tabId)) ?? null;
}

export function isEquipmentTabEnabled(tabId: string): boolean {
  switch (tabId) {
    case "library":
      return isFeatureEnabled(Features.BlendTools);
    default:
      return true;
  }
}

export function resolveEquipmentTabId(preferredTabId: string): string {
  const orderedTabs = ["settings", "audio-midi", "library", "features", "performance", "help"];
  if (isEquipmentTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return orderedTabs.find((tabId) => isEquipmentTabEnabled(tabId)) ?? "settings";
}

export function isAdvancedSubTabEnabled(tabId: string): boolean {
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

export function resolveAdvancedSubTabId(preferredTabId: string): string | null {
  const orderedTabs = ["composites", "blends", "layouts"];
  if (isAdvancedSubTabEnabled(preferredTabId)) {
    return preferredTabId;
  }

  return orderedTabs.find((tabId) => isAdvancedSubTabEnabled(tabId)) ?? null;
}

export function initEquipmentTabs(): void {
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
    scheduleDSPPerformancePlotUpdate();
    updateSignalDiagnosticsView();
  }

  flushShownViews();
  updateSettingsViewState({ equipmentTab: resolvedTabId });
}

export function initLibraryTabs(): void {
  if (libraryTabsInitialized) {
    return;
  }
  libraryTabsInitialized = true;

  libraryTabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.libraryTab ?? "resources";
      activateLibraryTab(tabId);
    });
  });

  activateLibraryTab("resources");
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

  // The engine re-checks which files are missing each time the list is opened.
  if (resolvedTabId === "resources" && !isSettingsViewStateSuppressed()) {
    postMessage({ type: "requestResourceLibrary" });
  }

  if (resolvedTabId === "riffs") {
    postMessage({ type: "getRiffLibrary" });
  }

  if (resolvedTabId === "advanced") {
    initAdvancedSubTabs();
  }

  flushShownViews();
  updateSettingsViewState({ libraryTab: resolvedTabId });
}

let advancedSubTabsInitialized = false;

export function initAdvancedSubTabs(): void {
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

export function applyAdvancedSubTab(tabId: string, subTabButtons: HTMLElement[], subTabPanels: HTMLElement[]): void {
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
