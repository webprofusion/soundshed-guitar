import { initializeControls, initializeInputModeControls, initializeAmpCabPowerControls, refreshEqModalVisualization } from "./controls.js";
import {
  initializePresetControls,
  initializePresets,
  initializeSaveAsButton,
  initializeSavePresetModal,
  initializePresetActionButtons,
  initializePresetTagFilterBar,
  handleDroppedPresetPack,
  renderActivePreset,
} from "./presets.js";
import { installFetchLogger, renderLogEntries } from "./logging.js";
import { scheduleDSPPerformancePlotUpdate } from "./views.js";
import { handleIncomingMessage } from "./messages.js";
import { requestSignalPathTest } from "./presets.js";
import { initializeTuner } from "./tuner.js";
import { initFxSelector, refreshFxSelector } from "./fxSelector.js";
import { themeSwitcher } from "./theme-switcher.js";
import { startUiSettingsTracking } from "./windowSettings.js";
import { renderFooterDemoAudioControls, bindFooterDemoAudioControls } from "./demoAudio.js";
import { initDiagnosticsToggle, initThemeSelect, initZoomControls, initUserInputCalibrationControls } from "./settings.js";
import { postMessage } from "./bridge.js";
import { initializeMetronome } from "./metronome.js";
import { initializeAutomationPanel } from "./automationPanel.js";
import { initializeBlendEditorModal, initSignalPathResize, renderSignalPathBar, createNamIrGlobalFileDropHandler } from "./signalPath.js";
import { initializeCustomEffectDesignerModal } from "./customEffectDesigner.js";
import { initializeDialogModals } from "./dialogs.js";
import { activateTab, initializeControlBarTabs, initializeIconBarTabs, initializePlayFooterPadsToggle, initializeTabButtons, switchMainPanel, initControlBarCollapse, initSignalPathCollapse } from "./navigation.js";
import { handleDroppedRiffAudioFiles, initializeRiffLibraryPanel } from "./riffLibrary.js";
import { initializeEarPracticePlayerPanel } from "./earPracticePlayer.js";
import { initializeGlobalFileDrop, registerGlobalFileDropHandler } from "./fileDrop.js";
import { initMultiRigTab } from "./multiPresetMixer.js";
import { applyBuildFlags } from "./buildFlags.js";
import { hideSplashScreen, initSplashScreen } from "./splash.js";
import { FEATURE_FLAGS_CHANGED_EVENT } from "./featureFlags.js";
import { initAlpineStores, startAlpine } from "./alpine.js";
import { initializePerformancePads } from "./performancePads.js";
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

function initFooterActionsPopup(): void {
  const footer = document.querySelector<HTMLElement>(".footer-bar");
  const toggle = document.getElementById("footer-actions-toggle") as HTMLButtonElement | null;
  const panel = document.getElementById("footer-actions-panel");
  if (!footer || !toggle || !panel || toggle.dataset.bound === "true") {
    return;
  }

  toggle.dataset.bound = "true";
  const compactQuery = window.matchMedia("(max-width: 680px)");

  const setOpen = (open: boolean) => {
    footer.classList.toggle("footer-actions-open", open);
    toggle.setAttribute("aria-expanded", open ? "true" : "false");
    panel.setAttribute("aria-hidden", compactQuery.matches && !open ? "true" : "false");
  };

  const syncMode = () => {
    if (!compactQuery.matches) {
      setOpen(false);
      panel.setAttribute("aria-hidden", "false");
      return;
    }
    setOpen(false);
  };

  toggle.addEventListener("click", () => {
    setOpen(!footer.classList.contains("footer-actions-open"));
  });

  document.addEventListener("click", (event) => {
    if (!compactQuery.matches || !footer.classList.contains("footer-actions-open")) {
      return;
    }
    const target = event.target as Node | null;
    if (target && footer.contains(target)) {
      return;
    }
    setOpen(false);
  });

  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || !footer.classList.contains("footer-actions-open")) {
      return;
    }
    setOpen(false);
    if (compactQuery.matches) {
      toggle.focus();
    }
  });

  compactQuery.addEventListener("change", syncMode);
  syncMode();
}


async function bootstrap(): Promise<void> {
  initSplashScreen();

  installFetchLogger();
  renderLogEntries();

  applyBuildFlags();

  // Alpine integration - register stores early so x-data expressions in HTML can use them.
  initAlpineStores();
  // Explicit start is safe even if the defer script auto-initializes.
  startAlpine();

  initializeTabButtons();
  initializeControlBarTabs();
  initControlBarCollapse();
  initSignalPathCollapse();

  // Ensure listeners for icon bar tabs (Play/Tones/Jam/Settings) are attached early
  initializeIconBarTabs({ onEq: openEqModal });

  // Ensure Alpine store reflects the initial panel (for :class in header component)
  try {
    const A = (window as any).Alpine;
    const ui = A && A.store && A.store('ui');
    if (ui) ui.mainPanel = 'visualizer';
  } catch {}

  // Force initial main panel visibility via legacy logic (adds .active)
  switchMainPanel('visualizer');

  // Ensure signal visualisation shows its (placeholder) content early
  try { renderSignalPathBar(); } catch {}
  try { initSignalPathResize(); } catch {}

  // Initialize theme switcher
  themeSwitcher; // Ensure singleton is created
  console.log("[JS] Theme switcher initialized:", themeSwitcher.getCurrentTheme());
  initThemeSelect();
  initZoomControls();
  
  // Add theme switcher UI to icon bar

  activateTab("details");
  initializeControls();
  initializeInputModeControls();
  initializeAmpCabPowerControls();
  initializePresetControls();
  initializeDialogModals();
  initializeSavePresetModal();
  initializeSaveAsButton();
  initializePresetActionButtons();
  initializePresetTagFilterBar();
  initializeTuner();
  initializeMetronome();
  initializeAutomationPanel();
  initializeRiffLibraryPanel();
  initializeEarPracticePlayerPanel();
  registerGlobalFileDropHandler({
    id: "preset-pack-drop",
    priority: 200,
    handle: (files) => handleDroppedPresetPack(files),
  });
  registerGlobalFileDropHandler({
    id: "riff-audio-drop",
    priority: 100,
    handle: (files) => handleDroppedRiffAudioFiles(files),
  });
  registerGlobalFileDropHandler({
    id: "nam-ir-resource-drop",
    priority: 150,
    handle: createNamIrGlobalFileDropHandler(),
  });
  initializeGlobalFileDrop();
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
  // Re-apply after settings tracking starts so host-restored height is respected.
  try { initSignalPathResize(); } catch {}
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
  initFooterActionsPopup();
  initializePlayFooterPadsToggle();
  initializePerformancePads();

  renderActivePreset();
  await initializePresets();

  window.IPlugReceiveData = (message: string) => {
    //console.log("[JS] IPlugReceiveData called with:", message.substring(0, 100));
    handleIncomingMessage(message);
  };
  console.log("[JS] IPlugReceiveData registered on window");

  postMessage({ type: "uiReady" });
  postMessage({ type: "getEffectCatalog" });
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
    scheduleDSPPerformancePlotUpdate();
  });

  window.addEventListener("themeChanged", () => {
    refreshEqModalVisualization();
    scheduleDSPPerformancePlotUpdate();
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
