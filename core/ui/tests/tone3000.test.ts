import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { uiState } from "../ts/state.js";
import { downloadTone3000ResourceByReference } from "../ts/tone3000.js";

describe("downloadTone3000ResourceByReference", () => {
  const originalFetch = globalThis.fetch;

  beforeEach(() => {
    uiState.tone3000Session = {
      accessToken: "session-token",
      refreshToken: "",
      expiresAt: Date.now() + 60_000,
    };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("resolves a known model id without forcing an architecture filter", async () => {
    const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
      const url = typeof input === "string" ? input : input.toString();

      if (url.includes("/models") && !url.includes("/models/known-a1-model")) {
        return {
          ok: true,
          status: 200,
          json: async () => ({
            models: [
              { id: "known-a1-model", model_url: "https://tone3000.test/models/known-a1-model" },
            ],
          }),
          text: async () => "",
          headers: new Headers(),
        } as Response;
      }

      if (url.includes("/models/known-a1-model")) {
        return {
          ok: true,
          status: 200,
          arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
          headers: new Headers({ "content-type": "application/octet-stream" }),
          text: async () => "",
        } as Response;
      }

      throw new Error(`Unexpected fetch URL: ${url}`);
    });

    globalThis.fetch = fetchMock as typeof fetch;

    await downloadTone3000ResourceByReference({ toneId: "tone-123", modelId: "known-a1-model" });

    expect(fetchMock).toHaveBeenCalledTimes(2);
    const modelLookupUrl = fetchMock.mock.calls[0]?.[0];
    expect(String(modelLookupUrl)).toContain("tone_id=tone-123");
    expect(String(modelLookupUrl)).not.toContain("architecture=");
  });
});
