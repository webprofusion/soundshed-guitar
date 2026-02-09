import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { setAppSetting } from "./bridge.js";
import type { AppSettingValue, Tone3000Session } from "./types.js";

const TONE3000_API_KEY_SETTING = "tone3000.apiKey";
const TONE3000_SESSION_URL = "https://www.tone3000.com/api/v1/auth/session";

let sessionInitialized = false;
let sessionRequest: Promise<void> | null = null;

function getApiKeyFromSettings(): string {
  const value: AppSettingValue = uiState.appSettings?.[TONE3000_API_KEY_SETTING] ?? null;
  return typeof value === "string" ? value.trim() : "";
}

function maskApiKey(apiKey: string): string {
  if (apiKey.length <= 6) {
    return "***";
  }
  return `${apiKey.slice(0, 3)}***${apiKey.slice(-3)}`;
}

async function startSession(apiKey: string): Promise<void> {
  if (sessionRequest) {
    return sessionRequest;
  }

  sessionRequest = (async () => {
    try {
      const response = await fetch(TONE3000_SESSION_URL, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ api_key: apiKey }),
      });

      if (!response.ok) {
        const detail = await response.text();
        throw new Error(`HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
      }

      const data = (await response.json()) as {
        access_token?: string;
        refresh_token?: string;
        expires_in?: number;
      };

      if (!data.access_token || !data.refresh_token || !data.expires_in) {
        throw new Error("Session response missing required fields");
      }

      const session: Tone3000Session = {
        accessToken: data.access_token,
        refreshToken: data.refresh_token,
        expiresAt: Date.now() + data.expires_in * 1000,
      };

      uiState.tone3000Session = session;
      appendLog(`tone3000 session started (${maskApiKey(apiKey)})`);
      showNotification("Tone3000 session ready");
    } catch (error) {
      uiState.tone3000Session = null;
      const message = error instanceof Error ? error.message : String(error);
      appendLog(`tone3000 session failed (${maskApiKey(apiKey)}): ${message}`);
      showNotification("Tone3000 auth failed", message);
    } finally {
      sessionRequest = null;
    }
  })();

  return sessionRequest;
}

export async function ensureTone3000Session(): Promise<void> {
  if (sessionInitialized) {
    return;
  }
  sessionInitialized = true;

  let apiKey = getApiKeyFromSettings();
  if (!apiKey) {
    return;
  }

  await startSession(apiKey);
}

export async function handleAppSettingUpdate(key: string, value: AppSettingValue): Promise<void> {
  if (key !== TONE3000_API_KEY_SETTING) {
    return;
  }

  const normalized = typeof value === "string" ? value.trim() : "";
  if (!normalized) {
    uiState.tone3000Session = null;
    appendLog("tone3000 api key cleared");
    return;
  }

  await startSession(normalized);
}
