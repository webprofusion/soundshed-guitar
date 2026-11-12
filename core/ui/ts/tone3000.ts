import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { updateAppSetting } from "./appSettingsStore.js";
import type { AppSettings, AppSettingValue, Tone3000Session } from "./types.js";
import type { Tone3000ApiSession, Tone3000Architecture, Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import {
  buildTone3000ModelLookupUrl,
  buildTone3000ToneUrl,
  extractTone3000Models,
  fetchAllTone3000ModelPages,
  getTone3000ApiClientConfig,
  getTone3000SessionUrl,
  TONE3000_OFFICIAL_API_BASE,
} from "./tone3000Api.js";

const TONE3000_API_KEY_SETTING = "tone3000.apiKey";
const TONE3000_USE_SOUNDSHED_API_SETTING = "tone3000.useSoundshedToneSearchApi";
const SESSION_REFRESH_LEAD_MS = 60_000;
const SESSION_REFRESH_RETRY_MS = 60_000;
const SESSION_HEARTBEAT_MS = 60_000;
const DEFAULT_MODEL_RESOLUTION_ARCHITECTURE: Tone3000Architecture = "2";

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

export function isTone3000ProxyModeEnabled(): boolean {
  return isProxyModeActive();
}

export function isTone3000ByokEnabled(): boolean {
  if (isProxyModeActive()) {
    return false;
  }
  return Boolean(getApiKeyFromSettings()) || Boolean(uiState.tone3000Session?.accessToken);
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

/**
 * True when `url` is on the official Tone3000 API's origin or, in proxy mode, the configured
 * proxy's. Nothing else may be fetched through tone3000AuthenticatedFetch: its callers pass
 * URLs from API responses and from imported preset archives (`modelUrl`), and in BYOK mode
 * the session's bearer token goes with the request.
 */
export function isTone3000RequestUrlAllowed(url: string): boolean {
  let origin: string;
  try {
    origin = new URL(url).origin;
  } catch {
    return false;
  }
  const allowed = [new URL(TONE3000_OFFICIAL_API_BASE).origin];
  const config = getTone3000ApiClientConfig();
  if (config.usingProxy) {
    try {
      allowed.push(new URL(config.baseUrl).origin);
    } catch {
      // A malformed proxy setting allows nothing extra.
    }
  }
  return allowed.includes(origin);
}

function describeRequestHost(url: string): string {
  try {
    return new URL(url).host || url;
  } catch {
    return "an invalid URL";
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
  if (!isTone3000RequestUrlAllowed(requestUrl)) {
    appendLog(`tone3000 request refused: ${describeRequestHost(requestUrl)} is not the Tone3000 API or the configured proxy`);
    throw new Error(`Refusing a Tone3000 request to ${describeRequestHost(requestUrl)}`);
  }
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
  /** The file's content hash (metadata.contentHash), to find the model when its ids are gone. */
  contentHash?: string;
};

const CONTENT_HASH_PATTERN = /^(nam|ir)-content-v1:[0-9a-f]{64}$/;

/** Whether `value` is a content hash Tone3000 can look up. */
export function isContentHash(value: string | null | undefined): value is string {
  return typeof value === "string" && CONTENT_HASH_PATTERN.test(value);
}

/**
 * Tone3000 models whose content hash is `contentHash`: the same weights (NAM) or samples (IR)
 * whatever the file is called or its metadata says. Reposts share a hash, so there can be
 * several, oldest first; none is an empty list. Only the first page (25) is read: more reposts
 * than that add nothing a caller would choose between.
 */
export async function lookupTone3000ModelsByContentHash(contentHash: string): Promise<Tone3000Model[]> {
  if (!isContentHash(contentHash)) {
    return [];
  }
  const response = await tone3000AuthenticatedFetch(buildTone3000ModelLookupUrl(contentHash));
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Tone3000 content hash lookup failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
  }
  return extractTone3000Models(await response.json()) as Tone3000Model[];
}

/** One tone by id, or null when Tone3000 does not show it (private, deleted). */
export async function fetchTone3000Tone(toneId: string | number): Promise<Tone3000Tone | null> {
  const response = await tone3000AuthenticatedFetch(buildTone3000ToneUrl(toneId));
  if (response.status === 403 || response.status === 404) {
    return null;
  }
  if (!response.ok) {
    const detail = await response.text().catch(() => "");
    throw new Error(`Tone3000 tone lookup failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
  }
  const payload = (await response.json()) as unknown;
  const tone = payload && typeof payload === "object" && "data" in payload && !Array.isArray((payload as { data: unknown }).data)
    ? (payload as { data: unknown }).data
    : payload;
  return tone && typeof tone === "object" && "id" in tone ? (tone as Tone3000Tone) : null;
}

/**
 * Of the models sharing a content hash, the one to use: one in the tone the caller already knows
 * (the same pack, its model re-uploaded under a new id), else the oldest, the likeliest original.
 */
export function chooseContentHashMatch<T extends { tone_id?: number | string | null }>(models: T[], preferredToneId?: string): T | null {
  const preferred = preferredToneId?.trim();
  return (preferred ? models.find((model) => String(model.tone_id ?? "") === preferred) : undefined) ?? models[0] ?? null;
}

async function fetchTone3000ModelsByToneId(
  toneId: string,
  architecture?: Tone3000Architecture,
): Promise<Tone3000ModelLookup[]> {
  return fetchAllTone3000ModelPages(async (url) => {
    const response = await tone3000AuthenticatedFetch(url);
    if (!response.ok) {
      const detail = await response.text().catch(() => "");
      throw new Error(`Tone3000 model lookup failed: HTTP ${response.status}${detail ? ` - ${detail}` : ""}`);
    }
    return response.json();
  }, toneId, architecture);
}

export async function saveTone3000ApiKey(apiKey: string): Promise<boolean> {
  const normalized = apiKey.trim();
  if (!normalized) {
    return false;
  }

  updateAppSetting(TONE3000_API_KEY_SETTING, normalized);
  if (isProxyModeActive()) {
    return true;
  }

  await startSession(normalized, { force: true });
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
  try {
    return await downloadTone3000ResourceByLink(reference);
  } catch (error) {
    // The link is gone (the model re-uploaded under a new id, its tone moved or made private): a
    // model with the same content hash is the same file, wherever it is now.
    if (!isContentHash(reference.contentHash)) {
      throw error;
    }
    let match: Tone3000Model | null = null;
    try {
      match = chooseContentHashMatch(await lookupTone3000ModelsByContentHash(reference.contentHash), reference.toneId);
    } catch (lookupError) {
      appendLog(`tone3000 content hash lookup failed: ${lookupError instanceof Error ? lookupError.message : String(lookupError)}`);
    }
    if (!match?.model_url) {
      throw error;
    }
    appendLog(`tone3000 reference ${reference.toneId ?? "?"}/${reference.modelId ?? "?"} found by content hash as model ${match.id}`);
    return downloadTone3000ResourceByModelUrl(match.model_url);
  }
}

/** By the reference's model URL, or else its tone and model ids. */
async function downloadTone3000ResourceByLink(reference: Tone3000ArchiveReference): Promise<ArrayBuffer> {
  // The reference comes from an archive someone shared: a modelUrl anywhere but Tone3000 is
  // ignored, and the model is looked up by its ids instead.
  if (reference.modelUrl && isTone3000RequestUrlAllowed(reference.modelUrl)) {
    return downloadTone3000ResourceByModelUrl(reference.modelUrl);
  }
  if (reference.modelUrl) {
    appendLog(`tone3000 archive modelUrl ignored: ${describeRequestHost(reference.modelUrl)} is not Tone3000`);
  }

  if (!isProxyModeActive() && !uiState.tone3000Session?.accessToken) {
    throw new Error("Tone3000 session required to download this resource");
  }
  return downloadTone3000ResourceByIds(reference);
}

async function downloadTone3000ResourceByIds(reference: Tone3000ArchiveReference): Promise<ArrayBuffer> {
  const toneId = reference.toneId?.trim() ?? "";
  const modelId = reference.modelId?.trim() ?? "";
  if (!toneId || !modelId) {
    throw new Error("Tone3000 resource reference is missing toneId or modelId");
  }

  // Resolve known model ids without an architecture filter first so A1/other
  // legacy model variants are still discoverable from shared references.
  const allModels = await fetchTone3000ModelsByToneId(toneId);
  let model = allModels.find((entry) => String(entry.id ?? "") === modelId);
  if (!model) {
    // Compatibility fallback for environments that still prefer the A2 view.
    const defaultModels = await fetchTone3000ModelsByToneId(toneId, DEFAULT_MODEL_RESOLUTION_ARCHITECTURE);
    model = defaultModels.find((entry) => String(entry.id ?? "") === modelId);
  }
  const modelUrl = typeof model?.model_url === "string" ? model.model_url : "";
  if (!modelUrl) {
    throw new Error("Tone3000 resource is no longer available for this tone");
  }

  return downloadTone3000ResourceByModelUrl(modelUrl);
}

/**
 * Follows a key or connection mode another instance changed. The key is shared by every
 * instance, so one that was cleared or replaced elsewhere must stop being used here too;
 * left alone, the session's refresh timer would keep renewing it with the old key. The
 * session is only dropped: the next request starts one from the current settings.
 */
export function applySharedTone3000Settings(previous: AppSettings | null | undefined): void {
  const previousKey = previous?.[TONE3000_API_KEY_SETTING];
  const keyBefore = typeof previousKey === "string" ? previousKey.trim() : "";
  const proxyBefore = previous?.[TONE3000_USE_SOUNDSHED_API_SETTING] === true;
  if (keyBefore === getApiKeyFromSettings() && proxyBefore === isSoundshedToneSearchApiEnabled()) {
    return;
  }
  clearSessionTimers();
  if (uiState.tone3000Session) {
    uiState.tone3000Session = null;
    appendLog("tone3000 key or connection mode changed in another instance; session dropped");
  }
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
