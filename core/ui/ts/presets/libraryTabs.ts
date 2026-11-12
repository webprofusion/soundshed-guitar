/**
 * The library popover's tabs: Presets, Multi-Rig (with the feature on) and, at
 * compact density, Setlists.
 *
 * At full density the setlists fold under the preset list, opened by their own
 * bar. On a small screen that bar took a fixed row under a list that had room for
 * one or two cards, so at compact the setlists are a third tab with the whole
 * body to themselves. Leaving compact density on the Setlists tab goes back to
 * Presets, where the fold is.
 */

import { isCompact } from "../compactMode.js";
import { uiState } from "../state.js";
import {
  presetLibraryMultiRigPanel,
  presetLibraryMultiRigTab,
  presetLibraryPresetsPanel,
  presetLibraryPresetsTab,
  presetLibrarySetlistsTab,
  presetLibraryTabs,
  setlistAddCurrentButton,
  setlistCollapsible,
  setlistPanel,
} from "./dom.js";

export type PresetLibraryTab = "presets" | "multiRig" | "setlists";

let currentTab: PresetLibraryTab = "presets";
let initialized = false;

export function getPresetLibraryTab(): PresetLibraryTab {
  return currentTab;
}

export function selectPresetLibraryTab(tab: PresetLibraryTab): void {
  const compact = isCompact();
  const next: PresetLibraryTab = tab === "setlists" && !compact ? "presets" : tab;
  currentTab = next;

  const buttons: Array<[HTMLElement | null, PresetLibraryTab]> = [
    [presetLibraryPresetsTab, "presets"],
    [presetLibraryMultiRigTab, "multiRig"],
    [presetLibrarySetlistsTab, "setlists"],
  ];
  for (const [button, id] of buttons) {
    button?.classList.toggle("active", id === next);
    button?.setAttribute("aria-selected", String(id === next));
  }

  if (presetLibraryPresetsPanel) {
    presetLibraryPresetsPanel.hidden = next !== "presets";
  }
  if (presetLibraryMultiRigPanel) {
    presetLibraryMultiRigPanel.hidden = next !== "multiRig";
  }
  // Setlists only make sense against the regular preset library: at full density
  // they fold under its list, at compact they are their own tab.
  if (setlistCollapsible) {
    setlistCollapsible.hidden = compact ? next !== "setlists" : next !== "presets";
    setlistCollapsible.classList.toggle("is-tab", compact);
  }
  if (compact && next === "setlists") {
    setlistPanel?.setAttribute("aria-hidden", "false");
  } else if (setlistPanel && setlistCollapsible) {
    setlistPanel.setAttribute("aria-hidden", String(!setlistCollapsible.classList.contains("open")));
  }
  syncSetlistAddCurrentButton();
}

/** The tab row shows whenever there is more than one tab: Multi-Rig on, or compact density. */
export function syncPresetLibraryTabsVisibility(multiRigEnabled: boolean): void {
  const showTabs = multiRigEnabled || isCompact();
  if (presetLibraryTabs) {
    presetLibraryTabs.hidden = !showTabs;
    presetLibraryTabs.setAttribute("aria-hidden", String(!showTabs));
  }
  if (presetLibrarySetlistsTab) {
    presetLibrarySetlistsTab.hidden = !isCompact();
    presetLibrarySetlistsTab.tabIndex = isCompact() ? 0 : -1;
  }
  if (!multiRigEnabled && currentTab === "multiRig") {
    selectPresetLibraryTab("presets");
  } else {
    selectPresetLibraryTab(currentTab);
  }
}

/** "+ Current preset" adds the preset playing to the setlist being edited. */
export function syncSetlistAddCurrentButton(): void {
  if (setlistAddCurrentButton) {
    setlistAddCurrentButton.disabled = !uiState.activePresetId || !uiState.activeSetlistId;
  }
}

/**
 * Binds the Presets and Setlists tabs (the Multi-Rig tab is bound by
 * multiPresetMixer.ts, which owns that list) and follows density changes.
 */
export function initPresetLibraryTabs(isMultiRigEnabled: () => boolean, addCurrentPresetToSetlist: () => void): void {
  if (initialized) {
    return;
  }
  initialized = true;
  presetLibraryPresetsTab?.addEventListener("click", () => selectPresetLibraryTab("presets"));
  presetLibrarySetlistsTab?.addEventListener("click", () => selectPresetLibraryTab("setlists"));
  setlistAddCurrentButton?.addEventListener("click", addCurrentPresetToSetlist);
  window.addEventListener("densityChanged", () => syncPresetLibraryTabsVisibility(isMultiRigEnabled()));
  syncPresetLibraryTabsVisibility(isMultiRigEnabled());
}
