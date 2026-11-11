/**
 * Sepia has no online service, and must never call the upstream project's: its API is for its
 * own app only. These hold the web UI to that at the three places a URL is chosen.
 */
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";

import { API_BASE_URL, ONLINE_SERVICES_ENABLED } from "../ts/apiConfig.js";
import { getTone3000ApiClientConfig, TONE3000_OFFICIAL_API_BASE } from "../ts/tone3000Api.js";
import { uiState } from "../ts/state.js";

function sourceFiles(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    return statSync(path).isDirectory() ? sourceFiles(path) : [path];
  });
}

describe("online services", () => {
  it("are off, and the API base resolves nowhere", () => {
    expect(ONLINE_SERVICES_ENABLED).toBe(false);
    expect(new URL(API_BASE_URL).hostname.endsWith(".invalid")).toBe(true);
  });

  it("leave Tone3000 on its official API even when the relay setting is on", () => {
    uiState.appSettings = { "tone3000.useSoundshedToneSearchApi": true } as typeof uiState.appSettings;
    const config = getTone3000ApiClientConfig();
    expect(config.usingProxy).toBe(false);
    expect(config.baseUrl).toBe(TONE3000_OFFICIAL_API_BASE);
  });

  it("leave Jam on its offline tools: no YouTube backing-track search", async () => {
    const { isBackingTracksFeatureEnabled, resolveJamSection } = await import("../ts/jam/state.js");
    expect(isBackingTracksFeatureEnabled()).toBe(false);
    expect(resolveJamSection("backingTracks")).not.toBe("backingTracks");
  });

  it("name no upstream host anywhere in the UI source or markup", () => {
    const root = join(__dirname, "..");
    const files = [...sourceFiles(join(root, "ts")), ...sourceFiles(join(root, "ui-components")), join(root, "index.template.html")];
    const offenders = files.filter((file) => /soundshed\.com/i.test(readFileSync(file, "utf8")));
    expect(offenders).toEqual([]);
  });
});
