import { initializeControls, initializeInputModeControls, initializeAmpCabPowerControls, refreshEqModalVisualization } from "./controls.js";
import {
  initializePresetControls,
  initializePresets,
  initializeSaveAsButton,
  initializeSavePresetModal,
  initializePresetActionButtons,
  renderActivePreset,
} from "./presets.js";
import { installFetchLogger, renderLogEntries } from "./logging.js";
import { updateDSPPerformancePlot } from "./views.js";
import { handleIncomingMessage } from "./messages.js";
import { requestSignalPathTest } from "./presets.js";
import { initializeTuner } from "./tuner.js";
import { initFxSelector } from "./fxSelector.js";
import { themeSwitcher } from "./theme-switcher.js";
import { startUiSettingsTracking } from "./windowSettings.js";
import { renderFooterDemoAudioControls, bindFooterDemoAudioControls } from "./demoAudio.js";
import { initDiagnosticsToggle, initThemeSelect } from "./settings.js";
import { postMessage } from "./bridge.js";
import { initializeMetronome } from "./metronome.js";
import { initializeBlendEditorModal } from "./signalPath.js";
import { initializeDialogModals } from "./dialogs.js";
import { activateTab, initializeIconBarTabs, initializeTabButtons, switchMainPanel } from "./navigation.js";
import { initializeToneSharingPanel } from "./toneSharingPanel.js";
import { initializeRiffLibraryPanel } from "./riffLibrary.js";
const eqModal = document.getElementById("eq-modal");
const eqModalCloseBtn = document.getElementById("eq-modal-close");

function openEqModal(): void {
  if (!eqModal) return;
  eqModal.style.display = "flex";
  refreshEqModalVisualization();
  requestAnimationFrame(() => refreshEqModalVisualization());
}

function closeEqModal(): void {
  if (!eqModal) return;
  eqModal.style.display = "none";
}


async function bootstrap(): Promise<void> {
  installFetchLogger();
  renderLogEntries();

  initializeTabButtons();

  // Initialize theme switcher
  themeSwitcher; // Ensure singleton is created
  console.log("[JS] Theme switcher initialized:", themeSwitcher.getCurrentTheme());
  initThemeSelect();
  
  // Add theme switcher UI to icon bar

  activateTab("details");
  initializeControls();
  initializeInputModeControls();
  initializeAmpCabPowerControls();
  initializePresetControls();
  initializeIconBarTabs({ onEq: openEqModal });
  initializeDialogModals();
  initializeSavePresetModal();
  initializeSaveAsButton();
  initializePresetActionButtons();
  initializeTuner();
  initializeMetronome();
  initializeRiffLibraryPanel();
  initializeToneSharingPanel();
  if (eqModalCloseBtn) {
    eqModalCloseBtn.addEventListener("click", closeEqModal);
  }
  if (eqModal) {
    eqModal.addEventListener("mousedown", (event) => {
      if (event.target === eqModal) {
        closeEqModal();
      }
    });
  }
  initializeBlendEditorModal();
  initFxSelector();
  startUiSettingsTracking();
  initDiagnosticsToggle();

  // Initialize footer demo audio controls
  const footerDemoContainer = document.getElementById("footer-demo-audio-container");
  if (footerDemoContainer) {
    footerDemoContainer.innerHTML = renderFooterDemoAudioControls();
    bindFooterDemoAudioControls();
  }

  renderActivePreset();
  await initializePresets();

  window.IPlugReceiveData = (message: string) => {
    //console.log("[JS] IPlugReceiveData called with:", message.substring(0, 100));
    handleIncomingMessage(message);
  };
  console.log("[JS] IPlugReceiveData registered on window");

  postMessage({ type: "uiReady" });
  postMessage({ type: "getTheme" });
  postMessage({ type: "uiVisibility", visible: !document.hidden });

  document.addEventListener("visibilitychange", () => {
    postMessage({ type: "uiVisibility", visible: !document.hidden });
  });

  window.addEventListener("focus", () => {
    postMessage({ type: "uiVisibility", visible: true });
  });

  window.addEventListener("resize", () => {
    updateDSPPerformancePlot();
  });

  const signalTestButton = document.getElementById("run-signal-test");
  if (signalTestButton) {
    signalTestButton.addEventListener("click", requestSignalPathTest);
  }

  const footerSettingsButton = document.getElementById("footer-settings-btn");
  if (footerSettingsButton) {
    footerSettingsButton.addEventListener("click", () => {
      switchMainPanel("settings");
    });
  }
}

bootstrap();
