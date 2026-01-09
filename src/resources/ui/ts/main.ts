import { initializeControls, initializeInputModeControls, initializeAmpCabPowerControls } from "./controls.js";
import {
  initializePresetControls,
  initializePresets,
  initializeSaveAsButton,
  initializeSavePresetModal,
  initializePresetActionButtons,
  renderActivePreset,
} from "./presets.js";
import { installFetchLogger, renderLogEntries } from "./logging.js";
import { handleIncomingMessage } from "./messages.js";
import { requestSignalPathTest } from "./presets.js";
import { initializeTuner } from "./tuner.js";
import { initFxSelector } from "./fxSelector.js";

const tabButtons = Array.from(document.querySelectorAll(".tab-button"));
const tabPanels = Array.from(document.querySelectorAll(".tab-panel"));
const iconBarButtons = Array.from(document.querySelectorAll(".icon-bar .icon-btn"));
const mainTabPanels = Array.from(document.querySelectorAll(".main-content .tab-panel"));

function activateTab(tabId: string): void {
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
}

function switchMainPanel(panelId: string): void {
  iconBarButtons.forEach((btn) => {
    const btnPanel = (btn as HTMLElement).dataset.panel;
    btn.classList.toggle("active", btnPanel === panelId);
  });

  mainTabPanels.forEach((panel) => {
    const isPanelMatch = (panel as HTMLElement).id === `panel-${panelId}`;
    panel.classList.toggle("active", isPanelMatch);
  });
}

function initializeIconBarTabs(): void {
  iconBarButtons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const panelId = (btn as HTMLElement).dataset.panel;
      if (panelId) {
        switchMainPanel(panelId);
      }
    });
  });
}

async function bootstrap(): Promise<void> {
  installFetchLogger();
  renderLogEntries();

  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tabId = (button as HTMLElement).dataset.tab ?? "";
      if (tabId) {
        activateTab(tabId);
      }
    });
  });

  activateTab("details");
  initializeControls();
  initializeInputModeControls();
  initializeAmpCabPowerControls();
  initializePresetControls();
  initializeIconBarTabs();
  initializeSavePresetModal();
  initializeSaveAsButton();
  initializePresetActionButtons();
  initializeTuner();
  initFxSelector();

  renderActivePreset();
  await initializePresets();

  window.IPlugReceiveData = (message: string) => {
    console.log("[JS] IPlugReceiveData called with:", message.substring(0, 100));
    handleIncomingMessage(message);
  };
  console.log("[JS] IPlugReceiveData registered on window");

  const signalTestButton = document.getElementById("run-signal-test");
  if (signalTestButton) {
    signalTestButton.addEventListener("click", requestSignalPathTest);
  }
}

bootstrap();
