/**
 * The tone sharing HTTP client: URL building, the authenticated fetch, and the
 * session id that travels with it.
 *
 * `apiFetch` is the only place a request is made, so it is also the only place
 * that has to notice the server handing back a rotated session.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { storageKeys, toneSharingState } from "./state.js";

export function getApiOrigin(): string {
  try {
    return new URL(toneSharingState.apiBase).origin;
  } catch {
    return "https://api.sepia.invalid";
  }
}

export function isToneSharingAdmin(): boolean {
  return toneSharingState.user?.role === "admin";
}

export function normalizeSettingString(value: unknown): string {
  return typeof value === "string" ? value.trim() : "";
}

export function persistToneSharingSession(value: string): void {
  updateAppSetting(storageKeys.sessionId, value || null);
}

export function updateToneSharingSession(sessionId: unknown): void {
  const normalized = normalizeSettingString(sessionId);
  if (normalized === toneSharingState.sessionId) {
    return;
  }
  toneSharingState.sessionId = normalized;
  persistToneSharingSession(normalized);
}

export function buildApiUrl(pathOrUrl: string): string {
  if (/^https?:\/\//i.test(pathOrUrl)) {
    return pathOrUrl;
  }

  const base = toneSharingState.apiBase.endsWith("/") ? toneSharingState.apiBase.slice(0, -1) : toneSharingState.apiBase;
  const normalizedPath = pathOrUrl.startsWith("/") ? pathOrUrl : `/${pathOrUrl}`;

  try {
    const parsedBase = new URL(base);
    const basePath = parsedBase.pathname.replace(/\/+$/, "");
    const pathAlreadyIncludesBase =
      !!basePath &&
      basePath !== "/" &&
      (normalizedPath === basePath || normalizedPath.startsWith(`${basePath}/`));

    if (pathAlreadyIncludesBase) {
      return `${parsedBase.origin}${normalizedPath}`;
    }
  } catch {
  }

  return `${base}${normalizedPath}`;
}

/** True when `url` is on the Tone Sharing API's own origin, the only place the session goes. */
export function isToneSharingApiUrl(url: string): boolean {
  try {
    return new URL(url).origin === getApiOrigin();
  } catch {
    return false;
  }
}

/**
 * fetch() for a Tone Sharing path, or an absolute URL the server handed back (a download,
 * a pack thumbnail). The session id and cookies go only to the API's own origin: an absolute
 * URL in a pack's metadata may point anywhere, and must not be handed the session.
 */
export function toneSharingFetch(pathOrUrl: string, init: RequestInit = {}): Promise<Response> {
  const url = buildApiUrl(pathOrUrl);
  const ownOrigin = isToneSharingApiUrl(url);
  const headers = new Headers(init.headers ?? {});
  headers.delete("x-session-id");
  if (ownOrigin && toneSharingState.sessionId) {
    headers.set("x-session-id", toneSharingState.sessionId);
  }
  return fetch(url, { ...init, headers, credentials: ownOrigin ? "include" : "omit" });
}

export async function apiFetch<T = unknown>(path: string, init: RequestInit = {}): Promise<T> {
  const request = async (includeSessionId: boolean): Promise<{ response: Response; payload: Record<string, unknown> | null }> => {
    const headers = new Headers(init.headers ?? {});
    if (includeSessionId && toneSharingState.sessionId) {
      headers.set("x-session-id", toneSharingState.sessionId);
    }
    const response = await fetch(`${toneSharingState.apiBase}${path}`, {
      ...init,
      headers,
      credentials: "include"
    });
    const payload = await response.json().catch(() => null) as Record<string, unknown> | null;
    return { response, payload };
  };

  const startedWithSessionId = Boolean(toneSharingState.sessionId);
  let usedFallbackWithoutSessionId = false;
  let { response, payload } = await request(startedWithSessionId);
  if (startedWithSessionId && response.status === 401) {
    usedFallbackWithoutSessionId = true;
    ({ response, payload } = await request(false));
  }

  const payloadData = payload?.data && typeof payload.data === "object" ? payload.data as Record<string, unknown> : null;
  const responseSessionId = normalizeSettingString(response.headers.get("x-session-id"));
  const payloadSessionId = normalizeSettingString(payload?.sessionId) || normalizeSettingString(payloadData?.sessionId);
  if (responseSessionId || payloadSessionId) {
    updateToneSharingSession(responseSessionId || payloadSessionId);
  } else if (usedFallbackWithoutSessionId && response.ok) {
    // Keep cookie-based auth working when an old stored header token has expired.
    updateToneSharingSession("");
  } else if (response.status === 401 && startedWithSessionId) {
    updateToneSharingSession("");
  }

  if (!response.ok || !payload || payload.ok === false) {
    const errorPayload = payload?.error;
    const errorMessage = errorPayload && typeof errorPayload === "object" ? (errorPayload as { message?: unknown }).message : undefined;
    const message = typeof errorMessage === "string" && errorMessage.trim()
      ? errorMessage
      : `Request failed (${response.status})`;
    throw new Error(message);
  }

  return payload.data as T;
}

export async function parseApiErrorMessage(response: Response): Promise<string> {
  const fallback = `Request failed (${response.status})`;
  try {
    const payload = await response.json() as { error?: { message?: string } };
    return payload?.error?.message || fallback;
  } catch {
    return fallback;
  }
}
