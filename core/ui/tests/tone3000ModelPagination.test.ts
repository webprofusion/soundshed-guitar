import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { uiState } from "../ts/state.js";
import { downloadTone3000ResourceByReference } from "../ts/tone3000.js";
import { fetchAllTone3000ModelPages, TONE3000_MODELS_PAGE_SIZE } from "../ts/tone3000Api.js";
import type { Tone3000Model, Tone3000Tone } from "../ts/tone3000ApiTypes.js";
import { fetchTone3000Models } from "../ts/tone3000Shared.js";

function makeModels(count: number): Tone3000Model[] {
  return Array.from({ length: count }, (_, index) => ({
    id: index + 1,
    name: `Model ${index + 1}`,
    model_url: `https://tone3000.test/models/${index + 1}`,
  }));
}

function queryOf(url: string, key: string): string {
  return new URL(url).searchParams.get(key) ?? "";
}

/// One page of `models` the way the Tone3000 API returns it.
function pageResponse(models: Tone3000Model[], url: string) {
  const page = Number(queryOf(url, "page"));
  const pageSize = Number(queryOf(url, "page_size"));
  return {
    data: models.slice((page - 1) * pageSize, page * pageSize),
    page,
    page_size: pageSize,
    total: models.length,
    total_pages: Math.ceil(models.length / pageSize),
  };
}

function jsonResponse(body: unknown): Response {
  return { ok: true, status: 200, json: async () => body, text: async () => "", headers: new Headers() } as Response;
}

describe("fetchAllTone3000ModelPages", () => {
  it("follows total_pages past the first page", async () => {
    const models = makeModels(TONE3000_MODELS_PAGE_SIZE * 2 + 50);
    const fetchJson = vi.fn(async (url: string) => pageResponse(models, url));

    const fetched = await fetchAllTone3000ModelPages(fetchJson, "tone-1", "2");

    expect(fetched.map((model) => model.id)).toEqual(models.map((model) => model.id));
    const urls = fetchJson.mock.calls.map(([url]) => url);
    expect(urls.map((url) => queryOf(url, "page"))).toEqual(["1", "2", "3"]);
    for (const url of urls) {
      expect(queryOf(url, "tone_id")).toBe("tone-1");
      expect(queryOf(url, "architecture")).toBe("2");
    }
  });

  it("asks once when the first page is the only one", async () => {
    const models = makeModels(5);
    const fetchJson = vi.fn(async (url: string) => pageResponse(models, url));

    expect(await fetchAllTone3000ModelPages(fetchJson, "tone-1")).toHaveLength(5);
    expect(fetchJson).toHaveBeenCalledTimes(1);
  });

  it("keeps going until a short page when no page count is reported", async () => {
    const models = makeModels(TONE3000_MODELS_PAGE_SIZE + 30);
    const fetchJson = vi.fn(async (url: string) => ({ models: pageResponse(models, url).data }));

    expect(await fetchAllTone3000ModelPages(fetchJson, "tone-1")).toHaveLength(models.length);
    expect(fetchJson).toHaveBeenCalledTimes(2);
  });

  it("stops when a page brings nothing new, as from an API that ignores page", async () => {
    const models = makeModels(TONE3000_MODELS_PAGE_SIZE);
    const fetchJson = vi.fn(async () => models);

    expect(await fetchAllTone3000ModelPages(fetchJson, "tone-1")).toHaveLength(models.length);
    expect(fetchJson).toHaveBeenCalledTimes(2);
  });

  it("passes a failed page on to the caller", async () => {
    const fetchJson = vi.fn(async () => {
      throw new Error("HTTP 500");
    });

    await expect(fetchAllTone3000ModelPages(fetchJson, "tone-1")).rejects.toThrow("HTTP 500");
  });
});

describe("Tone3000 model listings", () => {
  const originalFetch = globalThis.fetch;
  const models = makeModels(TONE3000_MODELS_PAGE_SIZE + 20);

  beforeEach(() => {
    uiState.tone3000Session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
  });

  afterEach(() => {
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("lists every model of a tone with more than one page of them", async () => {
    globalThis.fetch = vi.fn(async (input: RequestInfo | URL) =>
      jsonResponse(pageResponse(models, String(input)))) as typeof fetch;

    const fetched = await fetchTone3000Models({ id: "tone-1", title: "Tone" } as Tone3000Tone);

    expect(fetched).toHaveLength(models.length);
  });

  it("finds a shared model that is on a later page", async () => {
    const wanted = models[models.length - 1];
    const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url === wanted.model_url) {
        return {
          ok: true,
          status: 200,
          arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
          text: async () => "",
          headers: new Headers(),
        } as Response;
      }
      return jsonResponse(pageResponse(models, url));
    });
    globalThis.fetch = fetchMock as typeof fetch;

    const bytes = await downloadTone3000ResourceByReference({ toneId: "tone-1", modelId: String(wanted.id) });

    expect(bytes.byteLength).toBe(3);
    expect(fetchMock.mock.calls.map(([input]) => String(input))).toContain(wanted.model_url);
  });
});
