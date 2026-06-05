import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { setAppSetting } from "./bridge.js";
import type { AppSettingValue, Tone3000Session } from "./types.js";
import type { Tone3000ApiSession } from "./tone3000ApiTypes.js";
import {
  buildTone3000ModelsUrl,
  extractTone3000Models,
  getTone3000ApiClientConfig,
  getTone3000SessionUrl,
  TONE3000_OFFICIAL_API_BASE,
} from "./tone3000Api.js";

const TONE3000_API_KEY_SETTING = "tone3000.apiKey";
const TONE3000_USE_SOUNDSHED_API_SETTING = "tone3000.useSoundshedToneSearchApi";
const SESSION_REFRESH_LEAD_MS = 60_000;
const SESSION_REFRESH_RETRY_MS = 60_000;
const SESSION_HEARTBEAT_MS = 60_000;

let sessionRequest: Promise<void> | null = null;
let sessionRefreshTimer: ReturnType<typeof globalThis.setTimeout> | null = null;
let sessionHeartbeatTimer: ReturnType<typeof globalThis.setInterval> | null = null;

function getApiKeyFromSettings(): string {
  const value: AppSettingValue = uiState.appSettings?.[TONE3000_API_KEY_SETTING] ?? null;
  return typeof value === "string" ? value.trim() : "";
}

function isSoundshedToneSearchApiEnabled(): boolean {
  const value: AppSettingValue = uiState.appSettings?.[TONE3000_USE_SOUNDSHED_API_SETTING] ?? null;
  return value === true;
}

function isProxyModeActive(): boolean {
  return getTone3000ApiClientConfig().usingProxy || isSoundshedToneSearchApiEnabled();
}

export function isTone3000AuthReady(): boolean {
  if (isProxyModeActive()) {
    return true;
  }
  return Boolean(uiState.tone3000Session?.accessToken);
}

function normalizeTone3000RequestUrl(input: string): string {
  if (!isProxyModeActive()) {
    return input;
  }

  try {
    const requestUrl = new URL(input);
    const officialBase = new URL(TONE3000_OFFICIAL_API_BASE);
    const isOfficialHost = requestUrl.host === officialBase.host;
    const apiPrefix = officialBase.pathname.endsWith("/")
      ? officialBase.pathname
      : `${officialBase.pathname}/`;

    if (!isOfficialHost || !requestUrl.pathname.startsWith(apiPrefix)) {
      return input;
    }

    const proxiedPath = requestUrl.pathname.slice(apiPrefix.length);
    const proxyBase = getTone3000ApiClientConfig().baseUrl;
    const proxyUrl = new URL(proxiedPath, proxyBase.endsWith("/") ? proxyBase : `${proxyBase}/`);
    proxyUrl.search = requestUrl.search;
    return proxyUrl.toString();
  } catch {
    return input;
  }
}

function maskApiKey(apiKey: string): string {
  if (apiKey.length <= 6) {
    return "***";
  }
  return `${apiKey.slice(0, 3)}***${apiKey.slice(-3)}`;
}

function clearSessionRefreshTimer(): void {
  if (!sessionRefreshTimer) {
    return;
  }
  globalThis.clearTimeout(sessionRefreshTimer);
  sessionRefreshTimer = null;
}

function clearSessionHeartbeatTimer(): void {
  if (!sessionHeartbeatTimer) {
    return;
  }
  globalThis.clearInterval(sessionHeartbeatTimer);
  sessionHeartbeatTimer = null;
}

function startSessionHeartbeat(): void {
  if (isProxyModeActive()) {
    clearSessionHeartbeatTimer();
    return;
  }

  if (sessionHeartbeatTimer) {
    return;
  }

  sessionHeartbeatTimer = globalThis.setInterval(() => {
    const apiKey = getApiKeyFromSettings();
    const usingProxy = isProxyModeActive();
    if ((!usingProxy && !apiKey) || !uiState.tone3000Session?.accessToken) {
      clearSessionHeartbeatTimer();
      return;
    }
    void ensureTone3000Session();
  }, SESSION_HEARTBEAT_MS);
}

function clearSessionTimers(): void {
  clearSessionRefreshTimer();
  clearSessionHeartbeatTimer();
}

function scheduleSessionRefresh(apiKey: string): void {
  clearSessionRefreshTimer();

  const session = uiState.tone3000Session;
  if (!session?.expiresAt) {
    return;
  }

  startSessionHeartbeat();

  const msUntilRefresh = Math.max(5_000, session.expiresAt - Date.now() - SESSION_REFRESH_LEAD_MS);
  sessionRefreshTimer = globalThis.setTimeout(() => {
    void refreshSession(apiKey);
  }, msUntilRefresh);
}

async function refreshSession(apiKey: string): Promise<void> {
  const normalized = apiKey.trim();
  const usingProxy = isProxyModeActive();
  if (usingProxy) {
    clearSessionTimers();
    return;
  }

  if (!normalized) {
    clearSessionTimers();
    return;
  }

  await startSession(normalized || "proxy", { force: true });
  if (uiState.tone3000Session?.accessToken) {
    scheduleSessionRefresh(normalized || "proxy");
    return;
  }

  clearSessionTimers();
  sessionRefreshTimer = globalThis.setTimeout(() => {
    void refreshSession(normalized || "proxy");
  }, SESSION_REFRESH_RETRY_MS);
}

type StartSessionOptions = {
  force?: boolean;
};

function parseSessionResponse(data: unknown): Tone3000Session {
  const payload = (data ?? {}) as Partial<Tone3000ApiSession>;

  const accessToken = typeof payload.access_token === "string" ? payload.access_token : "";
  const expiresInSeconds = typeof payload.expires_in === "number" ? payload.expires_in : 0;
  if (!accessToken || !expiresInSeconds) {
    throw new Error("Session response missing required fields");
  }

  const refreshToken = typeof payload.refresh_token === "string" ? payload.refresh_token : "";
  return {
    accessToken,
    refreshToken,
    expiresAt: Date.now() + expiresInSeconds * 1000,
  };
}

function withAuthorizationHeader(headers: HeadersInit | undefined, accessToken: string): Headers {
  const merged = new Headers(headers ?? {});
  merged.set("Authorization", `Bearer ${accessToken}`);
  return merged;
}

async function startSession(apiKey: string, options?: StartSessionOptions): Promise<void> {
  if (sessionRequest) {
    return sessionRequest;
  }

  const force = options?.force ?? false;
  const existing = uiState.tone3000Session;
  if (!force && existing?.accessToken && (existing.expiresAt - Date.now()) > SESSION_REFRESH_LEAD_MS) {
    scheduleSessionRefresh(apiKey);
    return;
  }

  sessionRequest = (async () => {
    try {
      const response = await fetch(getTone3000SessionUrl(), {
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

      const session = parseSessionResponse(await response.json());

      uiState.tone3000Session = session;
      appendLog(`tone3000 session started (${maskApiKey(apiKey)})`);
      scheduleSessionRefresh(apiKey);
    } catch (error) {
      uiState.tone3000Session = null;
      const message = error instanceof Error ? error.message : String(error);
      appendLog(`tone3000 session failed (${maskApiKey(apiKey)}): ${message}`);
      clearSessionTimers();
    } finally {
      sessionRequest = null;
    }
  })();

  return sessionRequest;
}

export async function ensureTone3000Session(): Promise<void> {
  const usingProxy = isProxyModeActive();
  if (usingProxy) {
    clearSessionTimers();
    uiState.tone3000Session = null;
    return;
  }

  const apiKey = getApiKeyFromSettings();
  if (!apiKey) {
    clearSessionTimers();
    uiState.tone3000Session = null;
    return;
  }

  const session = uiState.tone3000Session;
  if (session?.accessToken && (session.expiresAt - Date.now()) > SESSION_REFRESH_LEAD_MS) {
    scheduleSessionRefresh(apiKey || "proxy");
    return;
  }

  await startSession(apiKey || "proxy");
}

export async function tone3000AuthenticatedFetch(input: string, init?: RequestInit): Promise<Response> {
  const requestUrl = normalizeTone3000RequestUrl(input);
  if (isProxyModeActive()) {
    return fetch(requestUrl, init);
  }

  await ensureTone3000Session();
  let session = uiState.tone3000Session;
  if (!session?.accessToken) {
    throw new Error("Tone3000 session required");
  }

  let response = await fetch(requestUrl, {
    ...init,
    headers: withAuthorizationHeader(init?.headers, session.accessToken),
  });

  if (response.status !== 401) {
    return response;
  }

  const usingProxy = isProxyModeActive();
  const apiKey = getApiKeyFromSettings();
  if (!apiKey && !usingProxy) {
    return response;
  }

  await startSession(apiKey || "proxy", { force: true });
  session = uiState.tone3000Session;
  if (!session?.accessToken) {
    return response;
  }

  response = await fetch(requestUrl, {
    ...init,
    headers: withAuthorizationHeader(init?.headers, session.accessToken),
  });
  return response;
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

async function fetchTone3000ModelsByToneId(toneId: string): Promise<Tone3000ModelLookup[]> {
  const response = await tone3000AuthenticatedFetch(buildTone3000ModelsUrl(toneId));
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Tone3000 model lookup failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
  }

  const data = await response.json();
  return extractTone3000Models(data) as Tone3000ModelLookup[];
}

export async function saveTone3000ApiKey(apiKey: string): Promise<boolean> {
  const normalized = apiKey.trim();
  if (!normalized) {
    return false;
  }

  uiState.appSettings[TONE3000_API_KEY_SETTING] = normalized;
  setAppSetting(TONE3000_API_KEY_SETTING, normalized);
  if (isProxyModeActive()) {
    return true;
  }

  await startSession(normalized);
  return Boolean(uiState.tone3000Session?.accessToken);
}

/**
 * Download a tone3000 resource file using the current authenticated session.
 * Used during preset archive import when resources carry a tone3000 model URL.
 */
export async function downloadTone3000ResourceByModelUrl(modelUrl: string): Promise<ArrayBuffer> {
  const response = await tone3000AuthenticatedFetch(modelUrl);
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

  if (!isProxyModeActive() && !uiState.tone3000Session?.accessToken) {
    throw new Error("Tone3000 session required to download this resource");
  }
  const toneId = reference.toneId?.trim() ?? "";
  const modelId = reference.modelId?.trim() ?? "";
  if (!toneId || !modelId) {
    throw new Error("Tone3000 resource reference is missing toneId or modelId");
  }

  const models = await fetchTone3000ModelsByToneId(toneId);
  const model = models.find((entry) => String(entry.id ?? "") === modelId);
  const modelUrl = typeof model?.model_url === "string" ? model.model_url : "";
  if (!modelUrl) {
    throw new Error("Tone3000 resource is no longer available for this tone");
  }

  return downloadTone3000ResourceByModelUrl(modelUrl);
}

export async function handleAppSettingUpdate(key: string, value: AppSettingValue): Promise<void> {
  if (key !== TONE3000_API_KEY_SETTING && key !== TONE3000_USE_SOUNDSHED_API_SETTING) {
    return;
  }

  if (key === TONE3000_USE_SOUNDSHED_API_SETTING) {
    uiState.tone3000Session = null;
    clearSessionTimers();
    await ensureTone3000Session();
    return;
  }

  const normalized = typeof value === "string" ? value.trim() : "";
  if (!normalized && !isSoundshedToneSearchApiEnabled()) {
    uiState.tone3000Session = null;
    clearSessionTimers();
    appendLog("tone3000 api key cleared");
    return;
  }

  if (isProxyModeActive()) {
    uiState.tone3000Session = null;
    return;
  }

  await startSession(normalized || "proxy");
}
