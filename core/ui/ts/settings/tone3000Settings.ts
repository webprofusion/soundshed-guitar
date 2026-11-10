/**
 * The Tone3000 account settings: the API key, and the choice between talking to
 * Tone3000 directly or through the Soundshed proxy.
 */

import { getApiBaseUrl } from "../apiConfig.js";
import { updateAppSetting } from "../appSettingsStore.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { handleAppSettingUpdate, saveTone3000ApiKey } from "../tone3000.js";
import { getTone3000ApiClientConfig } from "../tone3000Api.js";
import { apiKeyInput, saveButton, tone3000ApiKeyRow, tone3000ApiModeStatus, tone3000ProxyHealthCheckButton, tone3000ProxyHealthRow, tone3000ProxyHealthStatus, tone3000ProxyInfoHint, tone3000UseSoundshedApiToggle } from "./dom.js";
import { API_KEY_SETTING, TONE3000_USE_SOUNDSHED_API_SETTING } from "./keys.js";
import { getSettingValue } from "./values.js";

/**
 * Says whether a key is stored, without ever putting the key in the field. The key can be
 * saved from outside this panel (the prompt a Tone3000 import raises) or by another instance,
 * so this runs whenever the panel is shown, not only when it is first built.
 */
export function refreshTone3000ApiKeyField(): void {
  if (!apiKeyInput) {
    return;
  }
  const stored = getSettingValue(API_KEY_SETTING);
  apiKeyInput.placeholder = typeof stored === "string" && stored.trim() ? "API key stored" : "Enter your Tone3000 API key";
}

export function initTone3000UseSoundshedApiToggle(): void {
  // Bound to a local so the null check narrows inside the listener; a
  // narrowing on an imported binding does not survive into a closure.
  const toggle = tone3000UseSoundshedApiToggle;
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.addEventListener("change", () => {
    const enabled = Boolean(toggle.checked);
    updateAppSetting(TONE3000_USE_SOUNDSHED_API_SETTING, enabled);
    void handleAppSettingUpdate(TONE3000_USE_SOUNDSHED_API_SETTING, enabled);
    applyTone3000ModeVisibility(enabled);
    updateTone3000ApiModeStatus();
  });
}

export function updateTone3000ApiModeStatus(): void {
  if (!tone3000ApiModeStatus) {
    return;
  }

  const config = getTone3000ApiClientConfig();
  tone3000ApiModeStatus.textContent = config.usingProxy
    ? "Connection mode: relay API"
    : "Connection mode: Direct Tone3000 API with user API Key";
}

export function applyTone3000ModeVisibility(proxyEnabled: boolean): void {
  tone3000ApiKeyRow?.toggleAttribute("hidden", proxyEnabled);
  tone3000ProxyInfoHint?.toggleAttribute("hidden", !proxyEnabled);
  tone3000ApiModeStatus?.toggleAttribute("hidden", !proxyEnabled);
  tone3000ProxyHealthRow?.toggleAttribute("hidden", !proxyEnabled);
  tone3000ProxyHealthStatus?.toggleAttribute("hidden", !proxyEnabled);
}

export function updateTone3000ProxyHealthStatus(message: string): void {
  if (!tone3000ProxyHealthStatus) {
    return;
  }
  tone3000ProxyHealthStatus.textContent = message;
}

export function initTone3000ProxyHealthCheck(): void {
  if (!tone3000ProxyHealthCheckButton || tone3000ProxyHealthCheckButton.dataset.bound === "true") {
    return;
  }

  tone3000ProxyHealthCheckButton.dataset.bound = "true";
  tone3000ProxyHealthCheckButton.addEventListener("click", async () => {
    if (!tone3000ProxyHealthCheckButton) {
      return;
    }

    tone3000ProxyHealthCheckButton.disabled = true;
    updateTone3000ProxyHealthStatus("API health: checking...");

    try {
      const apiBase = getApiBaseUrl().trim().replace(/\/+$/, "");
      const response = await fetch(`${apiBase}/resourcesearch/health`, {
        method: "GET",
        credentials: "include",
      });

      let payload: unknown = null;
      try {
        payload = await response.json();
      } catch {
        payload = null;
      }

      if (!response.ok) {
        const reported = (payload && typeof payload === "object" && "error" in payload)
          ? (payload as { error?: { message?: unknown } }).error?.message
          : undefined;
        const message = typeof reported === "string" && reported ? reported : `HTTP ${response.status}`;
        updateTone3000ProxyHealthStatus(`API health: failed (${message})`);
        return;
      }

      updateTone3000ProxyHealthStatus("API health: OK");
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      updateTone3000ProxyHealthStatus(`API health: failed (${message})`);
    } finally {
      tone3000ProxyHealthCheckButton.disabled = false;
    }
  });
}

export async function saveApiKey(): Promise<void> {
  const apiKey = apiKeyInput?.value.trim() ?? "";
  if (!apiKey) {
    showNotification("Tone3000 API key required");
    return;
  }

  // Remember the currently stored key so we can roll back if the new key fails auth.
  const previousStored = getSettingValue(API_KEY_SETTING);
  const rollbackKey = typeof previousStored === "string" && previousStored.trim()
    ? previousStored.trim()
    : null;

  const saveBtn = saveButton as HTMLButtonElement | null;
  saveBtn?.setAttribute("disabled", "true");
  showNotification("Verifying Tone3000 API key\u2026");

  try {
    // saveTone3000ApiKey persists the key and verifies it by starting an authenticated
    // Tone3000 session; it returns true only when authentication succeeds (or proxy mode).
    const authenticated = await saveTone3000ApiKey(apiKey);
    if (authenticated) {
      appendLog("tone3000 api key saved and verified");
      if (apiKeyInput) {
        apiKeyInput.value = "";
      }
      refreshTone3000ApiKeyField();
      showNotification("Tone3000 API key saved");
      return;
    }

    // Authentication failed: roll back to the previously stored key so an invalid key
    // is not retained or used for subsequent requests.
    updateAppSetting(API_KEY_SETTING, rollbackKey);
    await handleAppSettingUpdate(API_KEY_SETTING, rollbackKey);
    refreshTone3000ApiKeyField();
    appendLog("tone3000 api key rejected (authentication failed)");
    showNotification("Tone3000 API key is invalid");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    appendLog(`tone3000 api key verification error: ${message}`);
    showNotification("Tone3000 API key is invalid");
  } finally {
    saveBtn?.removeAttribute("disabled");
  }
}

export async function clearApiKey(): Promise<void> {
  updateAppSetting(API_KEY_SETTING, null);
  if (apiKeyInput) {
    apiKeyInput.value = "";
  }
  refreshTone3000ApiKeyField();

  await handleAppSettingUpdate(API_KEY_SETTING, null);
}
