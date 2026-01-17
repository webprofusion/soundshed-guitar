import { uiState } from "./state.js";
import { setAppSetting } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { handleAppSettingUpdate } from "./tone3000.js";
import { updateSignalDiagnosticsView } from "./views.js";
import { initTone3000Browser } from "./tone3000Browser.js";
import type { AppSettingValue } from "./types.js";

const API_KEY_SETTING = "tone3000.apiKey";
const DIAGNOSTICS_SETTING = "diagnostics.signalLevelsEnabled";

const apiKeyInput = document.getElementById("tone3000-api-key-input") as HTMLInputElement | null;
const saveButton = document.getElementById("tone3000-api-key-save");
const clearButton = document.getElementById("tone3000-api-key-clear");
const sessionStatus = document.getElementById("tone3000-session-status");
const diagnosticsToggle = document.getElementById("signal-diagnostics-toggle") as HTMLInputElement | null;
let settingsInitialized = false;

export function initSettingsPanel(): void {
  if (settingsInitialized) {
    return;
  }
  settingsInitialized = true;
  saveButton?.addEventListener("click", () => void saveApiKey());
  clearButton?.addEventListener("click", () => void clearApiKey());
  initDiagnosticsToggle();

  refreshSettingsView();
  initTone3000Browser();
}

export function initDiagnosticsToggle(): void {
  if (!diagnosticsToggle) {
    return;
  }

  if (diagnosticsToggle.dataset.bound === "true") {
    return;
  }

  diagnosticsToggle.dataset.bound = "true";
  diagnosticsToggle.addEventListener("change", () => void updateDiagnosticsSetting());
}

export function refreshSettingsView(): void {
  const stored = getSettingValue(API_KEY_SETTING);
  if (apiKeyInput) {
    apiKeyInput.value = "";
    apiKeyInput.placeholder = stored ? "API key stored" : "Enter your Tone3000 API key";
  }
  if (diagnosticsToggle) {
    diagnosticsToggle.checked = Boolean(getSettingValue(DIAGNOSTICS_SETTING));
  }
  updateSessionStatus();
  updateSignalDiagnosticsView();
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

function updateDiagnosticsSetting(): void {
  const enabled = Boolean(diagnosticsToggle?.checked);
  uiState.appSettings[DIAGNOSTICS_SETTING] = enabled;
  setAppSetting(DIAGNOSTICS_SETTING, enabled);
  if (!enabled) {
    uiState.signalDiagnostics = null;
  }
  updateSignalDiagnosticsView();
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

function getSettingValue(key: string): AppSettingValue {
  return uiState.appSettings?.[key] ?? null;
}

export function updateSettingsSessionStatus(): void {
  updateSessionStatus();
}
