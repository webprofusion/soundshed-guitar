import { initializeControls, initializeInputModeControls, initializeAmpCabPowerControls, refreshEqModalVisualization } from "./controls.js";
import {
  initializePresetControls,
  initializePresets,
  initializeSaveAsButton,
  initializeSavePresetModal,
  initializePresetActionButtons,
  initializePresetTagFilterBar,
  renderActivePreset,
} from "./presets.js";
import { installFetchLogger, renderLogEntries } from "./logging.js";
import { updateDSPPerformancePlot } from "./views.js";
import { handleIncomingMessage } from "./messages.js";
import { requestSignalPathTest } from "./presets.js";
import { initializeTuner } from "./tuner.js";
import { initFxSelector, refreshFxSelector } from "./fxSelector.js";
import { themeSwitcher } from "./theme-switcher.js";
import { startUiSettingsTracking } from "./windowSettings.js";
import { renderFooterDemoAudioControls, bindFooterDemoAudioControls } from "./demoAudio.js";
import { initDiagnosticsToggle, initThemeSelect, initUserInputCalibrationControls } from "./settings.js";
import { postMessage } from "./bridge.js";
import { initializeMetronome } from "./metronome.js";
import { initializeBlendEditorModal } from "./signalPath.js";
import { initializeCustomEffectDesignerModal } from "./customEffectDesigner.js";
import { initializeDialogModals } from "./dialogs.js";
import { activateTab, initializeIconBarTabs, initializeTabButtons, switchMainPanel } from "./navigation.js";
import { initializeToneSharingPanel } from "./toneSharingPanel.js";
import { initializeRiffLibraryPanel } from "./riffLibrary.js";
import { initMultiRigTab } from "./multiPresetMixer.js";
import { initializeJamPanel } from "./jam.js";
import { applyBuildFlags } from "./buildFlags.js";
import { hideSplashScreen, initSplashScreen } from "./splash.js";
import { FEATURE_FLAGS_CHANGED_EVENT } from "./featureFlags.js";
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

function describeError(error: unknown): string {
  if (error instanceof Error) {
    return `${error.name}: ${error.message}`;
  }
  if (typeof error === "string") {
    return error;
  }
  try {
    return JSON.stringify(error);
  } catch {
    return String(error);
  }
}

function showBootstrapError(details: string): void {
  const splash = document.getElementById("splash-screen");
  if (!splash) return;

  const subtitle = splash.querySelector(".splash-subtitle");
  if (subtitle) {
    subtitle.textContent = "Startup failed";
  }

  let detailNode = splash.querySelector(".splash-error") as HTMLElement | null;
  if (!detailNode) {
    detailNode = document.createElement("p");
    detailNode.className = "splash-error";
    detailNode.style.marginTop = "10px";
    detailNode.style.fontSize = "12px";
    detailNode.style.maxWidth = "360px";
    detailNode.style.textAlign = "center";
    detailNode.style.opacity = "0.92";

    const splashContent = splash.querySelector(".splash-content");
    if (splashContent) {
      splashContent.appendChild(detailNode);
    }
  }

  if (detailNode) {
    detailNode.textContent = details;
  }
}

function reportBootstrapFailure(source: string, error: unknown): void {
  const details = describeError(error);
  console.error(`[JS] UI bootstrap failure (${source}):`, error);
  showBootstrapError(details);
  postMessage({ type: "uiBootstrapError", source, details });
}


async function bootstrap(): Promise<void> {
  initSplashScreen();

  installFetchLogger();
  renderLogEntries();

  applyBuildFlags();

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
  initializePresetTagFilterBar();
  initializeTuner();
  initializeMetronome();
  initializeRiffLibraryPanel();
  initializeJamPanel();
  initializeToneSharingPanel();
  initMultiRigTab();
  initializeCustomEffectDesignerModal();
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
  initUserInputCalibrationControls();
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => {
    refreshFxSelector();
    renderActivePreset();
  });

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

  // Intercept all https:// link clicks and open them in the system browser
  // instead of navigating the WebView.
  document.addEventListener("click", (event) => {
    const anchor = (event.target as Element | null)?.closest("a");
    if (!anchor) return;
    const href = anchor.getAttribute("href") ?? "";
    if (href.startsWith("https://") || href.startsWith("http://")) {
      event.preventDefault();
      postMessage({ type: "openUrl", url: href });
    }
  }, true);

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

  // Hide splash screen now that app is fully initialized
  await hideSplashScreen();
}

let bootstrapSettled = false;
let bootstrapFailureReported = false;

const reportBootstrapFailureOnce = (source: string, error: unknown): void => {
  if (bootstrapFailureReported) return;
  bootstrapFailureReported = true;
  reportBootstrapFailure(source, error);
};

window.addEventListener("error", (event) => {
  if (bootstrapSettled) return;
  reportBootstrapFailureOnce("window.error", event.error ?? event.message);
});

window.addEventListener("unhandledrejection", (event) => {
  if (bootstrapSettled) return;
  reportBootstrapFailureOnce("unhandledrejection", event.reason);
});

bootstrap()
  .then(() => {
    bootstrapSettled = true;
  })
  .catch((error) => {
    reportBootstrapFailureOnce("bootstrap", error);
    bootstrapSettled = true;
  });
