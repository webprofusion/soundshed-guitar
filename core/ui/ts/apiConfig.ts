/**
 * Endpoint for the Soundshed sharing / community API.
 *
 * This lived as private state inside toneSharingPanel.ts, so three unrelated
 * modules had to import that 4,000-line panel just to read a URL — and were
 * dragged into the app's import cycle for it. It is a constant; it belongs in
 * a leaf.
 */
export const API_BASE_URL = "https://api.sepia.invalid/v1";

/**
 * Sepia is a fork of Soundshed Guitar and has no online service of its own. The upstream
 * project's API is for its own app only, so nothing here may call it: the features that would
 * (tone sharing, the update check, the Tone3000 relay, the YouTube embed) stay off, and the
 * base URL above is a reserved name that resolves nowhere, so a path that was missed fails
 * at once instead of reaching someone else's server.
 */
export const ONLINE_SERVICES_ENABLED = false;

/** The API base, including the version path segment. */
export function getApiBaseUrl(): string {
  return API_BASE_URL;
}

/** Just the scheme and host, for building share links. */
export function getApiOrigin(): string {
  return new URL(API_BASE_URL).origin;
}
