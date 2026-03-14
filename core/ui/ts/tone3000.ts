import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { setAppSetting } from "./bridge.js";
import type { AppSettingValue, Tone3000Session } from "./types.js";

const TONE3000_API_KEY_SETTING = "tone3000.apiKey";
const TONE3000_API_BASE = "https://www.tone3000.com/api/v1";
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

type Tone3000ModelLookup = {
  id?: string | number;
  model_url?: string;
};

type Tone3000ArchiveReference = {
  toneId?: string;
  modelId?: string;
  modelUrl?: string;
};

async function fetchTone3000ModelsByToneId(toneId: string, accessToken: string): Promise<Tone3000ModelLookup[]> {
  const response = await fetch(`${TONE3000_API_BASE}/models?tone_id=${encodeURIComponent(toneId)}&page=1&page_size=100`, {
    headers: {
      Authorization: `Bearer ${accessToken}`,
    },
  });
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Tone3000 model lookup failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
  }

  const data = await response.json();
  if (Array.isArray(data?.models)) {
    return data.models as Tone3000ModelLookup[];
  }
  if (Array.isArray(data?.data)) {
    return data.data as Tone3000ModelLookup[];
  }
  if (Array.isArray(data?.results)) {
    return data.results as Tone3000ModelLookup[];
  }
  if (Array.isArray(data)) {
    return data as Tone3000ModelLookup[];
  }
  return [];
}

export async function saveTone3000ApiKey(apiKey: string): Promise<boolean> {
  const normalized = apiKey.trim();
  if (!normalized) {
    return false;
  }

  uiState.appSettings[TONE3000_API_KEY_SETTING] = normalized;
  setAppSetting(TONE3000_API_KEY_SETTING, normalized);
  await startSession(normalized);
  return Boolean(uiState.tone3000Session?.accessToken);
}

/**
 * Download a tone3000 resource file using the current authenticated session.
 * Used during preset archive import when resources carry a tone3000 model URL.
 */
export async function downloadTone3000ResourceByModelUrl(modelUrl: string): Promise<ArrayBuffer> {
  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    throw new Error("Tone3000 session required to download this resource");
  }
  const response = await fetch(modelUrl, {
    headers: { Authorization: `Bearer ${session.accessToken}` },
  });
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Tone3000 download failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
  }
  return response.arrayBuffer();
}

export async function downloadTone3000ResourceByReference(reference: Tone3000ArchiveReference): Promise<ArrayBuffer> {
  if (reference.modelUrl) {
    return downloadTone3000ResourceByModelUrl(reference.modelUrl);
  }

  const session = uiState.tone3000Session;
  if (!session?.accessToken) {
    throw new Error("Tone3000 session required to download this resource");
  }
  const toneId = reference.toneId?.trim() ?? "";
  const modelId = reference.modelId?.trim() ?? "";
  if (!toneId || !modelId) {
    throw new Error("Tone3000 resource reference is missing toneId or modelId");
  }

  const models = await fetchTone3000ModelsByToneId(toneId, session.accessToken);
  const model = models.find((entry) => String(entry.id ?? "") === modelId);
  const modelUrl = typeof model?.model_url === "string" ? model.model_url : "";
  if (!modelUrl) {
    throw new Error("Tone3000 resource is no longer available for this tone");
  }

  return downloadTone3000ResourceByModelUrl(modelUrl);
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
