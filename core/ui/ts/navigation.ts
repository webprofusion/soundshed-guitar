import { uiState } from "./state.js";
import { postMessage } from "./bridge.js";
import { initLibraryFilters, initLibraryTabs, initSettingsPanel, updateSettingsSessionStatus, activateEquipmentTab, activateLibraryTab, activateAdvancedSubTab, setSettingsViewStateSuppressed } from "./settings.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import { ensureTone3000Session } from "./tone3000.js";
import type { UiViewState } from "./types.js";
import { isJamEnabled } from "./buildFlags.js";

const tabButtons = Array.from(document.querySelectorAll(".tab-button"));
const tabPanels = Array.from(document.querySelectorAll(".tab-panel"));
const panelSwitchButtons = Array.from(document.querySelectorAll(".icon-bar .icon-btn, .panel-switch"));
const mainTabPanels = Array.from(document.querySelectorAll(".main-content .tab-panel"));
let tone3000BrowserInitialized = false;

let pendingSend = false;
const sendDelayMs = 200;
let applyingViewState = false;

function mergeViewState(base: UiViewState, update: UiViewState): UiViewState {
  const settings = {
    ...base.settings,
    ...update.settings,
  };
  return {
    ...base,
    ...update,
    settings,
  };
}

function updateUiViewState(update: UiViewState): void {
  const current = uiState.uiViewState ?? {};
  const next = mergeViewState(current, update);

  if (JSON.stringify(current) === JSON.stringify(next)) {
    return;
  }

  uiState.uiViewState = next;
  if (applyingViewState) {
    return;
  }
  if (pendingSend) {
    return;
  }

  pendingSend = true;
  window.setTimeout(() => {
    pendingSend = false;
    postMessage({ type: "uiViewStateChanged", viewState: uiState.uiViewState });
  }, sendDelayMs);
}

export function activateTab(tabId: string): void {
  if (!tabButtons.length || !tabPanels.length) {
    return;
  }

  tabButtons.forEach((button) => {
    const isActive = (button as HTMLElement).dataset.tab === tabId;
    button.classList.toggle("active", isActive);
  });

  tabPanels.forEach((panel) => {
    const isDetailsPanel = (panel as HTMLElement).id === "preset-details" && tabId === "details";
    const isLogPanel = (panel as HTMLElement).id === "log-panel" && tabId === "logs";
    panel.classList.toggle("active", isDetailsPanel || isLogPanel);
  });

  updateUiViewState({ presetTab: tabId });
}

export function switchMainPanel(panelId: string): void {
  const normalizedPanelId = panelId === "scalex" ? "sharing" : panelId;
  const effectivePanelId = !isJamEnabled() && normalizedPanelId === "jam"
    ? "visualizer"
    : normalizedPanelId;

  panelSwitchButtons.forEach((btn) => {
    const btnPanel = (btn as HTMLElement).dataset.panel;
    btn.classList.toggle("active", btnPanel === effectivePanelId);
  });

  mainTabPanels.forEach((panel) => {
    const isPanelMatch = (panel as HTMLElement).id === `panel-${effectivePanelId}`;
    panel.classList.toggle("active", isPanelMatch);
  });

  // Hide signal path bar for full-height panels (everything except visualizer)
  const signalPathBar = document.getElementById("signal-path-bar");
  const mainContent = document.querySelector(".main-content") as HTMLElement | null;
  const fullHeightPanels = ["library", "jam", "settings", "sharing", "advanced", "mixer"];
  const isFullHeight = fullHeightPanels.includes(effectivePanelId);

  if (signalPathBar) {
    signalPathBar.style.display = isFullHeight ? "none" : "";
  }
  if (mainContent) {
    mainContent.classList.toggle("full-height", isFullHeight);
  }

  if (effectivePanelId === "settings") {
    initSettingsPanel();
    void ensureTone3000Session().then(() => updateSettingsSessionStatus());
  }

  if (effectivePanelId === "library") {
    initLibraryTabs();
    initLibraryFilters();
    if (!tone3000BrowserInitialized) {
      initTone3000Browser();
      tone3000BrowserInitialized = true;
    }
    void ensureTone3000Session();
  }

  updateUiViewState({ mainPanel: effectivePanelId });
}

export function initializeIconBarTabs(options?: { onEq?: () => void; onMetronome?: () => void }): void {
  panelSwitchButtons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const panelId = (btn as HTMLElement).dataset.panel;
      if (!panelId) {
        return;
      }
      if (panelId === "metronome") {
        if (options?.onMetronome) {
          options.onMetronome();
        }
        return;
      }
      if (panelId === "eq") {
        if (options?.onEq) {
          options.onEq();
        }
        return;
      }
      switchMainPanel(panelId);
    });
  });
}

export function initializeTabButtons(): void {
  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.tab ?? "";
      if (tabId) {
        activateTab(tabId);
      }
    });
  });
}

export function applyUiViewState(state?: UiViewState): void {
  if (!state) {
    return;
  }
  const current = uiState.uiViewState ?? {};
  const next = mergeViewState(current, state);
  uiState.uiViewState = next;

  applyingViewState = true;
  if (next.mainPanel) {
    switchMainPanel(next.mainPanel);
  }
  if (next.presetTab) {
    activateTab(next.presetTab);
  }
  if (next.settings) {
    setSettingsViewStateSuppressed(true);
    if (next.settings.equipmentTab) {
      activateEquipmentTab(next.settings.equipmentTab);
    }
    if (next.settings.libraryTab) {
      activateLibraryTab(next.settings.libraryTab);
    }
    if (next.settings.advancedTab) {
      activateAdvancedSubTab(next.settings.advancedTab);
    }
    setSettingsViewStateSuppressed(false);
  }
  applyingViewState = false;
}
