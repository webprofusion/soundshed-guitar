import { getNavigationViewState, mergeNavigationViewState } from "./navigationState.js";
import { postMessage } from "./bridge.js";
import { isCompact } from "./compactMode.js";
import { applyCompactStage, isCompactStage, setCompactStage, setCompactStageDetailLabel } from "./compactStage.js";
import { initSettingsPanel, refreshTone3000ApiKeyField, activateEquipmentTab, activateLibraryTab, activateAdvancedSubTab, setSettingsViewStateSuppressed } from "./settings.js";
import { ensureTone3000Session } from "./tone3000.js";
import { handleJamPanelActivated, initializeJamPanel } from "./jam.js";
import { initializeToneSharingPanel } from "./toneSharingPanel.js";
import type { UiViewState } from "./types.js";
import { isJamEnabled } from "./buildFlags.js";
import { Features, isFeatureEnabled, isJamExperienceEnabled } from "./featureFlags.js";

function getTabButtons() { return Array.from(document.querySelectorAll(".tab-button")); }
function getTabPanels() { return Array.from(document.querySelectorAll(".tab-panel")); }
function getPanelSwitchButtons() { return Array.from(document.querySelectorAll(".icon-bar .icon-btn, .panel-switch")); }
function getMainTabPanels() { return Array.from(document.querySelectorAll(".main-content .tab-panel")); }
function getPadsFooterButtons(): HTMLButtonElement[] {
  const buttons = [
    document.getElementById("footer-pads-btn") as HTMLButtonElement | null,
    document.getElementById("footer-compact-pads-btn") as HTMLButtonElement | null,
  ];
  return buttons.filter((button): button is HTMLButtonElement => Boolean(button));
}
function getPlayPanel() { return document.getElementById("panel-visualizer") as HTMLElement | null; }
function getPerformancePanel() { return document.getElementById("panel-performance") as HTMLElement | null; }
function isPlayViewMode(value: unknown): value is "visualizer" | "pads" { return value === "visualizer" || value === "pads"; }

let pendingSend = false;
const sendDelayMs = 200;
let applyingViewState = false;
let playViewMode: "visualizer" | "pads" = "visualizer";

function updateUiViewState(update: UiViewState): void {
  if (!mergeNavigationViewState(update)) {
    return;
  }

  if (applyingViewState) {
    return;
  }
  if (pendingSend) {
    return;
  }

  pendingSend = true;
  window.setTimeout(() => {
    pendingSend = false;
    postMessage({ type: "uiViewStateChanged", viewState: getNavigationViewState() });
  }, sendDelayMs);
}

function applyPlayViewMode(mode: "visualizer" | "pads", persistState = true): void {
  playViewMode = mode;
  const playPanel = getPlayPanel();
  const performancePanel = getPerformancePanel();
  const padsFooterButtons = getPadsFooterButtons();
  const showPads = mode === "pads";

  if (playPanel) {
    playPanel.classList.toggle("show-performance-pads", showPads);
  }
  if (performancePanel) {
    performancePanel.classList.toggle("active", showPads);
    performancePanel.setAttribute("aria-hidden", showPads ? "false" : "true");
  }
  // The compact stage's second tab is whichever of these is behind it.
  setCompactStageDetailLabel(showPads ? "Setlist" : "Effect");
  padsFooterButtons.forEach((button) => {
    button.classList.toggle("is-active", showPads);
    button.setAttribute("aria-pressed", showPads ? "true" : "false");
  });
  if (persistState) {
    updateUiViewState({ playView: mode });
  }
}

function togglePlayViewMode(): void {
  if (getNavigationViewState().mainPanel !== "visualizer") {
    switchMainPanel("visualizer");
  }
  applyPlayViewMode(playViewMode === "pads" ? "visualizer" : "pads");
}

export function activateTab(tabId: string): void {
  const tabButtons = getTabButtons();
  const tabPanels = getTabPanels();
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
  const requestedSettingsTab = panelId === "library" ? "library" : null;
  const openPadsView = panelId === "performance";
  const normalizedPanelId = panelId === "performance"
    ? "visualizer"
    : panelId === "scalex"
    ? "sharing"
    : requestedSettingsTab
      ? "settings"
      : panelId;
  const effectivePanelId = (() => {
    if (normalizedPanelId === "jam" && (!isJamEnabled() || !isJamExperienceEnabled())) {
      return "visualizer";
    }
    if (normalizedPanelId === "sharing" && !isFeatureEnabled(Features.ToneSharing)) {
      return "visualizer";
    }
    return normalizedPanelId;
  })();

  // The compact stage tabs belong to Play alone, and CSS cannot see which panel
  // is active from outside .main-content.
  document.documentElement.dataset.mainPanel = effectivePanelId;

  const panelSwitchButtons = getPanelSwitchButtons();
  const mainTabPanels = getMainTabPanels();
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
  const fullHeightPanels = ["jam", "settings", "sharing", "advanced", "mixer"];
  const isFullHeight = fullHeightPanels.includes(effectivePanelId);

  if (signalPathBar) {
    signalPathBar.style.display = isFullHeight ? "none" : "";
  }
  if (mainContent) {
    mainContent.classList.toggle("full-height", isFullHeight);
  }

  if (effectivePanelId === "settings") {
    initSettingsPanel();
    if (requestedSettingsTab) {
      activateEquipmentTab(requestedSettingsTab);
    }
    if (isFeatureEnabled(Features.Tone3000)) {
      void ensureTone3000Session().then(() => refreshTone3000ApiKeyField());
    }
  }

  if (effectivePanelId === "jam") {
    initializeJamPanel();
    handleJamPanelActivated();
  }

  if (effectivePanelId === "sharing") {
    initializeToneSharingPanel();
  }

  if (openPadsView) {
    applyPlayViewMode("pads");
  }

  updateUiViewState({ mainPanel: effectivePanelId });
}

export function initializePlayFooterPadsToggle(): void {
  const padsFooterButtons = getPadsFooterButtons();
  if (!padsFooterButtons.length) {
    return;
  }
  padsFooterButtons.forEach((button) => {
    if (button.dataset.bound === "true") {
      return;
    }
    button.dataset.bound = "true";
    button.addEventListener("click", togglePlayViewMode);
  });
  applyPlayViewMode(playViewMode, false);
}

/**
 * Remember which compact stage tab the user left on.
 *
 * compactStage.ts announces the change rather than writing it down itself: view
 * state is this module's to own, and importing it back the other way would put a
 * cycle through a module the shell loads before anything else.
 */
export function initCompactStagePersistence(): void {
  window.addEventListener("compactStageChanged", (event) => {
    const stage = (event as CustomEvent<{ stage?: unknown }>).detail?.stage;
    if (isCompactStage(stage)) {
      updateUiViewState({ compactStage: stage });
    }
  });
}

/**
 * The landscape rail's Chain and Effect buttons: each opens Play on that half.
 *
 * Bound here rather than in compactStage.ts because they switch the main panel as
 * well, and that leaf cannot import this module back. Which one is lit is read
 * off the root by css/compact/rail.css, so there is no state to keep in step.
 */
export function initCompactStageDestinations(): void {
  document.querySelectorAll<HTMLButtonElement>("[data-compact-destination]").forEach((button) => {
    if (button.dataset.bound === "true") {
      return;
    }
    button.dataset.bound = "true";
    button.addEventListener("click", () => {
      const stage = button.dataset.compactDestination;
      if (!isCompactStage(stage)) {
        return;
      }
      if (document.documentElement.dataset.mainPanel !== "visualizer") {
        switchMainPanel("visualizer");
      }
      setCompactStage(stage);
    });
  });
}

export function initializeIconBarTabs(options?: { onEq?: () => void; onMetronome?: () => void }): void {
  const panelSwitchButtons = getPanelSwitchButtons();
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
  const tabButtons = getTabButtons();
  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.tab ?? "";
      if (tabId) {
        activateTab(tabId);
      }
    });
  });
}

export function initializeControlBarTabs(): void {
  const bar = document.querySelector<HTMLElement>(".control-bar");
  const tabs = Array.from(document.querySelectorAll<HTMLButtonElement>("[data-control-bar-tab]"));
  if (!bar || tabs.length === 0) {
    return;
  }

  const panels = Array.from(bar.querySelectorAll<HTMLElement>(".control-bar-panel"));
  const compactQuery = window.matchMedia("(max-width: 980px)");
  let activeTabId = "preset";
  let compactControlsOpen = false;

  /*
    Three layouts share these two panels.

    Desktop lays both out side by side and the tab strip is hidden. Narrow (the
    980px query) turns them into a real tab pair — one visible at a time. Compact
    keeps the preset row on screen permanently, because stepping presets is a
    performance action and must never be a menu away, and shows the input/output
    trims as a sheet over the content instead. The Controls tab is that sheet's
    toggle at this density, so it is the one case where picking a "tab" does not
    hide its sibling.
  */
  const applyPanelVisibility = () => {
    if (isCompact()) {
      bar.dataset.compactControls = compactControlsOpen ? "open" : "closed";
      panels.forEach((panel) => {
        const isControls = panel.id === "control-bar-controls-panel";
        panel.classList.toggle("is-active", isControls ? compactControlsOpen : true);
        panel.hidden = isControls && !compactControlsOpen;
      });
      return;
    }

    delete bar.dataset.compactControls;
    panels.forEach((panel) => {
      const isActive = panel.id === `control-bar-${activeTabId}-panel`;
      panel.classList.toggle("is-active", isActive);
      panel.hidden = compactQuery.matches && !isActive;
    });
  };

  const applyTabVisuals = () => {
    tabs.forEach((tab) => {
      const isActive = tab.dataset.controlBarTab === activeTabId;
      tab.classList.toggle("is-active", isActive);
      tab.setAttribute("aria-selected", isActive ? "true" : "false");
      tab.tabIndex = isActive ? 0 : -1;
      if (tab.dataset.controlBarTab === "controls") {
        tab.setAttribute("aria-expanded", compactControlsOpen ? "true" : "false");
      }
    });
  };

  const setCompactControlsOpen = (open: boolean) => {
    compactControlsOpen = open;
    applyTabVisuals();
    applyPanelVisibility();
  };

  const activateControlBarTab = (tabId: string) => {
    if (isCompact()) {
      setCompactControlsOpen(tabId === "controls" ? !compactControlsOpen : false);
      // The Controls button is the sheet's toggle here, not a tab selection, so
      // the preset row stays the selected panel underneath it.
      if (tabId === "controls") {
        return;
      }
    }

    activeTabId = tabId;
    applyTabVisuals();
    applyPanelVisibility();
  };

  const syncControlBarTabMode = () => {
    if (!isCompact()) {
      compactControlsOpen = false;
    }
    applyTabVisuals();
    applyPanelVisibility();
  };

  document.addEventListener("click", (event) => {
    if (!compactControlsOpen || !isCompact()) {
      return;
    }
    const target = event.target as Node | null;
    if (target && bar.contains(target)) {
      return;
    }
    // A popover the bar opened but hung from the body (the input mode flyout) is still the bar.
    if (target instanceof Element && target.closest("[data-control-bar-popover]")) {
      return;
    }
    setCompactControlsOpen(false);
  });

  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || !compactControlsOpen || !isCompact()) {
      return;
    }
    setCompactControlsOpen(false);
  });

  tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      const tabId = tab.dataset.controlBarTab;
      if (tabId) {
        activateControlBarTab(tabId);
      }
    });

    tab.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") {
        return;
      }

      event.preventDefault();
      const direction = event.key === "ArrowRight" ? 1 : -1;
      // Compact hides the Preset tab — arrowing onto it would move focus to
      // something the user cannot see.
      const visible = tabs.filter((candidate) => candidate.offsetParent !== null);
      const from = visible.indexOf(tab);
      if (visible.length < 2 || from === -1) {
        return;
      }
      const next = visible[(from + direction + visible.length) % visible.length];
      next.focus();
      const tabId = next.dataset.controlBarTab;
      if (tabId) {
        activateControlBarTab(tabId);
      }
    });
  });

  compactQuery.addEventListener("change", syncControlBarTabMode);
  window.addEventListener("densityChanged", syncControlBarTabMode);
  activateControlBarTab("preset");
}

export function applyUiViewState(state?: UiViewState): void {
  if (!state) {
    return;
  }
  mergeNavigationViewState(state);
  const next = getNavigationViewState();

  applyingViewState = true;
  if (next.mainPanel) {
    switchMainPanel(next.mainPanel);
  }
  if (isCompactStage(next.compactStage)) {
    applyCompactStage(next.compactStage);
  }
  if (isPlayViewMode(next.playView)) {
    applyPlayViewMode(next.playView, false);
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

const CONTROL_BAR_COLLAPSED_KEY = "guitarfx.controlBarCollapsed";

export function initControlBarCollapse(): void {
  const bar = document.querySelector<HTMLElement>(".control-bar");
  const btn = document.getElementById("control-bar-collapse-btn");
  if (!bar || !btn) return;

  const apply = (collapsed: boolean) => {
    bar.classList.toggle("is-collapsed", collapsed);
    btn.setAttribute("aria-expanded", collapsed ? "false" : "true");
    btn.setAttribute("aria-label", collapsed ? "Expand controls" : "Collapse controls");
    btn.title = collapsed ? "Expand controls" : "Collapse controls";
  };

  // Restore saved state
  apply(localStorage.getItem(CONTROL_BAR_COLLAPSED_KEY) === "true");

  btn.addEventListener("click", () => {
    const collapsed = !bar.classList.contains("is-collapsed");
    apply(collapsed);
    localStorage.setItem(CONTROL_BAR_COLLAPSED_KEY, String(collapsed));
  });
}

const SIGNAL_PATH_COLLAPSED_KEY = "guitarfx.signalPathCollapsed";

export function initSignalPathCollapse(): void {
  const bar = document.getElementById("signal-path-bar");
  const btn = document.getElementById("signal-path-collapse-btn");
  if (!bar || !btn) return;

  const apply = (collapsed: boolean) => {
    bar.classList.toggle("is-collapsed", collapsed);
    btn.setAttribute("aria-expanded", collapsed ? "false" : "true");
    btn.setAttribute("aria-label", collapsed ? "Expand signal chain" : "Collapse signal chain");
    btn.title = collapsed ? "Expand signal chain" : "Collapse signal chain";
  };

  // Restore saved state
  apply(localStorage.getItem(SIGNAL_PATH_COLLAPSED_KEY) === "true");

  btn.addEventListener("click", () => {
    const collapsed = !bar.classList.contains("is-collapsed");
    apply(collapsed);
    localStorage.setItem(SIGNAL_PATH_COLLAPSED_KEY, String(collapsed));
  });
}
