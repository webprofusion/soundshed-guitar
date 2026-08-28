/**
 * Endpoint for the Soundshed sharing / community API.
 *
 * This lived as private state inside toneSharingPanel.ts, so three unrelated
 * modules had to import that 4,000-line panel just to read a URL — and were
 * dragged into the app's import cycle for it. It is a constant; it belongs in
 * a leaf.
 */
export const API_BASE_URL = "https://api-guitar.soundshed.com/v1";

/** The API base, including the version path segment. */
export function getApiBaseUrl(): string {
  return API_BASE_URL;
}

/** Just the scheme and host, for building share links. */
export function getApiOrigin(): string {
  return new URL(API_BASE_URL).origin;
}
