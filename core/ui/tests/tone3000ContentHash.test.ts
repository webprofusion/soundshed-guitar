/**
 * Finding a library model on Tone3000 by its content hash (tone3000ContentHash.ts): the lookup
 * request, which match is taken, what a match writes on the row, the details panel's button, and
 * an archive's reference fetched by its content hash when its Tone3000 ids are gone.
 */
// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const postMessage = vi.fn();
vi.mock("../ts/bridge.js", async (importOriginal) => ({ ...(await importOriginal<Record<string, unknown>>()), postMessage }));
window.IPlugSendMsg = () => {};

const { uiState } = await import("../ts/state.js");
const { downloadTone3000ResourceByReference, lookupTone3000ModelsByContentHash } = await import("../ts/tone3000.js");
const { applyTone3000Match, canLookUpOnTone3000, findTone3000ByContentHash, withTone3000Match } = await import("../ts/tone3000ContentHash.js");
const { renderLibraryItemDetailsPanel } = await import("../ts/resourceBrowser/libraryDetails.js");

const HASH = "nam-content-v1:d50094f3d0f986666128704c36fe70df8a95f52f85c8a508435ed009e7d2ee1f";
const PROXY = "https://api-guitar.soundshed.com/v1/resourcesearch";

const tone = (id: number, title: string, username = "creator") => ({
  id,
  title,
  gear: "amp",
  license: "cc-by",
  user: { id: `u${id}`, username },
});
const model = (id: number, toneId: number) => ({
  id,
  tone_id: toneId,
  name: `VOX_AC30_CLEAN ${id}`,
  model_url: `https://www.tone3000.com/api/v1/models/${id}/download/m.nam`,
  architecture_version: "2",
  content_hash: HASH,
});

function json(body: unknown, status = 200): Response {
  return { ok: status >= 200 && status < 300, status, json: async () => body, text: async () => JSON.stringify(body), headers: new Headers() } as Response;
}

/** Answers by path; records each URL asked for. */
function serve(routes: Record<string, () => Response>) {
  const urls: string[] = [];
  globalThis.fetch = vi.fn(async (input: RequestInfo | URL) => {
    const url = new URL(String(input));
    urls.push(url.toString());
    const path = url.pathname.replace("/v1/resourcesearch/", "").replace("/api/v1/", "");
    const route = routes[path];
    if (!route) {
      throw new Error(`Unexpected fetch ${url}`);
    }
    return route();
  }) as typeof fetch;
  return urls;
}

const localRow = () => ({
  id: "local-1",
  name: "AC30 clean",
  category: "amp",
  description: "",
  filePath: "C:/models/ac30.nam",
  metadata: { provider: "local", contentHash: HASH, architectureVersion: "A2", namName: "AC30" } as Record<string, string>,
});

describe("Tone3000 content hash lookup", () => {
  const originalFetch = globalThis.fetch;

  beforeEach(() => {
    postMessage.mockClear();
    uiState.tone3000Session = { accessToken: "token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = { ...uiState.appSettings, "tone3000.useSoundshedToneSearchApi": true };
  });

  afterEach(() => {
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("asks the proxy's models/lookup, and asks nothing for a value that is not a content hash", async () => {
    const urls = serve({ "models/lookup": () => json({ data: [model(579453, 57741)], page: 1, page_size: 25, total: 1, total_pages: 1 }) });
    const models = await lookupTone3000ModelsByContentHash(HASH);
    expect(models.map((entry) => entry.id)).toEqual([579453]);
    expect(urls[0]).toBe(`${PROXY}/models/lookup?content_hash=${encodeURIComponent(HASH)}&page=1&page_size=25`);

    expect(await lookupTone3000ModelsByContentHash("nam-content-v1:abc")).toEqual([]);
    expect(urls).toHaveLength(1);
  });

  it("prefers a model in the tone the row names, and skips a tone it cannot read", async () => {
    serve({
      "models/lookup": () => json({ data: [model(1, 10), model(2, 20), model(3, 30)] }),
      "tones/10": () => json({ error: "private" }, 403),
      "tones/20": () => json(tone(20, "Vox AC30 + Boosters (A2)")),
      "tones/30": () => json(tone(30, "Repost")),
    });
    const row = localRow();
    expect((await findTone3000ByContentHash(row))?.tone.title).toBe("Vox AC30 + Boosters (A2)");

    row.metadata.toneId = "30";
    const match = await findTone3000ByContentHash(row);
    expect(match?.model.id).toBe(3);
    expect(match?.others).toBe(2);
  });

  it("finds nothing when Tone3000 has no model with the same content", async () => {
    serve({ "models/lookup": () => json({ data: [], page: 1, page_size: 25, total: 0, total_pages: 0 }) });
    expect(await findTone3000ByContentHash(localRow())).toBeNull();
  });

  it("takes Tone3000's attribution and ids, and keeps what the engine read from the file", () => {
    const metadata = withTone3000Match(localRow().metadata, { tone: tone(57741, "Vox AC30 + Boosters (A2)", "amalgam"), model: model(579453, 57741), others: 0 });
    expect(metadata).toMatchObject({
      provider: "tone3000",
      providerKey: "579453",
      toneId: "57741",
      modelId: "579453",
      toneTitle: "Vox AC30 + Boosters (A2)",
      authorUsername: "amalgam",
      licence: "cc-by",
      contentHash: HASH,
      architectureVersion: "A2",
      namName: "AC30",
      tone3000MatchedBy: "contentHash",
    });
  });

  it("saves a match with updateLibraryResource, metadata and all", () => {
    const row = localRow();
    applyTone3000Match("nam", row, { tone: tone(57741, "Vox"), model: model(579453, 57741), others: 0 });
    expect(postMessage).toHaveBeenCalledWith(expect.objectContaining({
      type: "updateLibraryResource",
      resourceType: "nam",
      resourceId: "local-1",
      metadata: expect.objectContaining({ toneId: "57741", contentHash: HASH }),
    }));
    expect(row.metadata.toneId).toBe("57741");
  });

  it("offers the lookup for local and Tone3000 rows with a content hash only", () => {
    expect(canLookUpOnTone3000(localRow())).toBe(true);
    expect(canLookUpOnTone3000({ ...localRow(), metadata: { provider: "tone3000", contentHash: HASH } })).toBe(true);
    expect(canLookUpOnTone3000({ ...localRow(), metadata: { provider: "local" } })).toBe(false);
    expect(canLookUpOnTone3000({ ...localRow(), metadata: { provider: "soundshed", contentHash: HASH } })).toBe(false);

    const panel = renderLibraryItemDetailsPanel(localRow(), { tone3000Lookup: true });
    expect(panel).toContain("resource-browser-details-tone3000-lookup-btn");
    expect(panel).toContain("Content Hash");
    expect(renderLibraryItemDetailsPanel(localRow())).not.toContain("tone3000-lookup-btn");
  });

  it("fetches an archive's model by its content hash when its tone no longer lists it", async () => {
    const urls = serve({
      models: () => json({ data: [model(9, 57741)] }),
      "models/lookup": () => json({ data: [model(579453, 57741)] }),
      "models/579453/download/m.nam": () => ({ ok: true, status: 200, arrayBuffer: async () => new Uint8Array([7]).buffer, text: async () => "", headers: new Headers() } as Response),
    });
    const bytes = await downloadTone3000ResourceByReference({ toneId: "57741", modelId: "334639", contentHash: HASH });
    expect(new Uint8Array(bytes)).toEqual(new Uint8Array([7]));
    expect(urls.some((url) => url.includes("models/lookup"))).toBe(true);

    // Without a content hash the reference fails as before.
    await expect(downloadTone3000ResourceByReference({ toneId: "57741", modelId: "334639" })).rejects.toThrow(/no longer available/);
  });

  it("writes the content hash into a shared preset's model reference", async () => {
    const { buildArchiveRemoteRef } = await import("../ts/presets/remoteArchiveResources.js");
    const row = { ...localRow(), metadata: { provider: "tone3000", providerKey: "579453", toneId: "57741", modelId: "579453", contentHash: HASH } };
    expect(buildArchiveRemoteRef(row, "nam").ref.contentHash).toBe(HASH);
  });
});
