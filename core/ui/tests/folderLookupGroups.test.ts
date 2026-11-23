/**
 * The Folder tab's "Look up details" (folderTone3000.ts lookUpFolderFiles) and the tone groups it
 * leads to (folderGroups.ts): ten files a click, each tone fetched once, files already looked up
 * skipped; files sharing a tone gathered at the first one's place, a tone with one file left as a
 * plain row, and a file a later click matches joining the group already shown.
 */
// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const postMessage = vi.fn();
const showNotification = vi.fn();
vi.mock("../ts/bridge.js", async (importOriginal) => ({ ...(await importOriginal<Record<string, unknown>>()), postMessage }));
vi.mock("../ts/notifications.js", async (importOriginal) => ({ ...(await importOriginal<Record<string, unknown>>()), showNotification }));
window.IPlugSendMsg = () => {};

const { uiState } = await import("../ts/state.js");
const folderTone3000 = await import("../ts/resourceBrowser/folderTone3000.js");
const { folderFilesInListOrder, folderVirtualEntries, groupFolderFiles, renderFolderGroupHeader, sharedTonePrefix } = await import("../ts/resourceBrowser/folderGroups.js");
const { renderGroupHeader } = await import("../ts/resourceBrowser/groupHeader.js");

const hashOf = (index: number) => `nam-content-v1:${index.toString(16).padStart(64, "0")}`;

const file = (index: number) => ({
  name: `capture ${index}.nam`,
  path: `D:/Captures/capture ${index}.nam`,
  resourceType: "nam" as const,
  metadata: { contentHash: hashOf(index) } as Record<string, string>,
});

function json(body: unknown, status = 200): Response {
  return { ok: status >= 200 && status < 300, status, json: async () => body, text: async () => JSON.stringify(body), headers: new Headers() } as Response;
}

/** Tone3000 as the proxy answers it: `toneByFile` says which tone each file index belongs to. */
function serveTones(toneByFile: Map<number, number>) {
  const urls: string[] = [];
  globalThis.fetch = vi.fn(async (input: RequestInfo | URL) => {
    const url = new URL(String(input));
    urls.push(url.pathname);
    const path = url.pathname.replace("/v1/resourcesearch/", "");
    if (path === "models/lookup") {
      const index = parseInt((url.searchParams.get("content_hash") ?? "").split(":")[1] ?? "", 16);
      const toneId = toneByFile.get(index);
      return json({ data: toneId ? [{ id: 1000 + index, tone_id: toneId, name: `m${index}`, model_url: "", content_hash: hashOf(index) }] : [] });
    }
    const tone = /^tones\/(\d+)$/.exec(path);
    if (tone) {
      const id = Number(tone[1]);
      return json({ id, title: `Tone ${id}`, gear: "amp", user: { id: "u", username: "maker" }, images: [`https://cdn.tone3000.com/${id}.jpg`] });
    }
    throw new Error(`Unexpected fetch ${url}`);
  }) as typeof fetch;
  return urls;
}

const details = (path: string) => folderTone3000.folderFileTone3000Metadata(path);

describe("Folder tab: look up details and tone groups", () => {
  const originalFetch = globalThis.fetch;

  beforeEach(() => {
    postMessage.mockClear();
    showNotification.mockClear();
    folderTone3000.resetFolderTone3000ForTests();
    uiState.resourceLibrary = { ...uiState.resourceLibrary, nam: [] };
    uiState.tone3000Session = { accessToken: "token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = { ...uiState.appSettings, "tone3000.useSoundshedToneSearchApi": true };
  });

  afterEach(() => {
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("looks up ten files a click, fetches each tone once, and skips files already looked up", async () => {
    const files = Array.from({ length: 12 }, (_, index) => file(index));
    const urls = serveTones(new Map([[0, 10], [1, 10], [2, 10], [3, 20], [11, 10]]));
    const progress: Array<[number, number]> = [];

    const first = await folderTone3000.lookUpFolderFiles(files, (done, total) => progress.push([done, total]));
    expect(first).toEqual({ found: 4, looked: 10, remaining: 2 });
    expect(progress[0]).toEqual([0, 10]);
    expect(progress.at(-1)).toEqual([10, 10]);
    expect(urls.filter((url) => url.endsWith("models/lookup"))).toHaveLength(10);
    expect(urls.filter((url) => url.endsWith("tones/10"))).toHaveLength(1);
    // The engine is not asked to hash a file whose hash is already known.
    expect(postMessage).not.toHaveBeenCalledWith(expect.objectContaining({ type: "hashResourceFile" }));
    expect(showNotification).toHaveBeenCalledTimes(1);
    expect(showNotification).toHaveBeenCalledWith("Details found", "4 of 10 files matched a published tone. Click again for the next 2.");
    expect(files.filter(folderTone3000.needsFolderLookup).map((entry) => entry.name)).toEqual(["capture 10.nam", "capture 11.nam"]);

    const second = await folderTone3000.lookUpFolderFiles(files, () => {});
    expect(second).toEqual({ found: 1, looked: 2, remaining: 0 });
    expect(urls.filter((url) => url.endsWith("models/lookup"))).toHaveLength(12);
    expect(files.some(folderTone3000.needsFolderLookup)).toBe(false);
    // Nothing left: a further click asks nothing.
    expect(await folderTone3000.lookUpFolderFiles(files, () => {})).toEqual({ found: 0, looked: 0, remaining: 0 });
    expect(urls.filter((url) => url.endsWith("models/lookup"))).toHaveLength(12);
  });

  it("stops at the first failed lookup and says so once", async () => {
    globalThis.fetch = vi.fn(async () => {
      throw new Error("offline");
    }) as typeof fetch;
    const result = await folderTone3000.lookUpFolderFiles([file(0), file(1)], () => {});
    expect(result.looked).toBe(0);
    expect(globalThis.fetch).toHaveBeenCalledTimes(1);
    expect(showNotification).toHaveBeenCalledTimes(1);
    expect(showNotification.mock.calls[0][0]).toBe("Lookup failed");
    // Failed files are offered again.
    expect(folderTone3000.needsFolderLookup(file(0))).toBe(true);
  });

  it("groups files sharing a tone at the first one's place, and a later match joins the group", async () => {
    const files = Array.from({ length: 12 }, (_, index) => file(index));
    serveTones(new Map([[1, 10], [4, 10], [3, 20], [11, 10]]));
    await folderTone3000.lookUpFolderFiles(files, () => {});

    let items = groupFolderFiles(files, (entry) => details(entry.path));
    expect(items.map((item) => (item.kind === "file" ? item.file.name : `group ${item.group.key} x${item.group.files.length}`))).toEqual([
      "capture 0.nam",
      "group tone:10 x2",
      "capture 2.nam",
      // Tone 20 has one file here: a plain row.
      "capture 3.nam",
      "capture 5.nam", "capture 6.nam", "capture 7.nam", "capture 8.nam", "capture 9.nam", "capture 10.nam", "capture 11.nam",
    ]);

    // The second click matches capture 11 to tone 10: it joins the group shown, not a new one.
    await folderTone3000.lookUpFolderFiles(files, () => {});
    items = groupFolderFiles(files, (entry) => details(entry.path));
    const group = items.find((item) => item.kind === "group");
    expect(group?.kind === "group" && group.group.files.map((entry) => entry.name)).toEqual(["capture 1.nam", "capture 4.nam", "capture 11.nam"]);
    expect(items.filter((item) => item.kind === "group")).toHaveLength(1);
    // Next/prev walks a group's files together.
    expect(folderFilesInListOrder(items).slice(0, 5).map((entry) => entry.name))
      .toEqual(["capture 0.nam", "capture 1.nam", "capture 4.nam", "capture 11.nam", "capture 2.nam"]);
  });

  it("draws a group closed unless opened, searched or holding the selection, with creator and count", () => {
    const tone = { toneId: "10", toneTitle: "Tone 10", authorUsername: "maker", imageUrl: "https://cdn.tone3000.com/10.jpg", tone3000Category: "amp" };
    const files = [file(1), file(2), file(3)];
    const items = groupFolderFiles(files, (entry) => (entry === files[2] ? undefined : tone));
    const closed = folderVirtualEntries(items, { toggled: new Map(), openAll: false, selectedPath: "" });
    expect(closed.map((entry) => entry.key)).toEqual(["group:tone:10", `file:${files[2].path}`]);

    const selected = folderVirtualEntries(items, { toggled: new Map(), openAll: false, selectedPath: files[1].path });
    expect(selected.map((entry) => entry.kind)).toEqual(["group", "file", "file", "file"]);
    expect(selected[1]).toMatchObject({ groupMember: true });
    expect(folderVirtualEntries(items, { toggled: new Map(), openAll: true, selectedPath: "" })).toHaveLength(4);
    // The user's own choice wins over the default.
    expect(folderVirtualEntries(items, { toggled: new Map([["tone:10", false]]), openAll: true, selectedPath: "" })).toHaveLength(2);

    const header = closed[0];
    if (header.kind !== "group") throw new Error("expected a group");
    const html = renderFolderGroupHeader(header, () => undefined);
    expect(html).toContain('data-kind="group"');
    expect(html).toContain("Tone 10");
    expect(html).toContain("by maker");
    expect(html).toContain("2 models");
    expect(html).toContain('src="https://cdn.tone3000.com/10.jpg"');
    expect(html).toContain('aria-expanded="false"');
  });

  it("names a tone with no saved title from its models' names, and its creator from any member", () => {
    expect(sharedTonePrefix(["Peavey - 5150 II - LEAD - G7.0", "Peavey - 5150 II - CLEAN - G1.5"])).toBe("Peavey - 5150 II");
    expect(sharedTonePrefix(["ProCo Rat - ProCo RAT", "ProCo Rat - ProCo RAT"])).toBe("ProCo Rat");
    expect(sharedTonePrefix(["Alpha", "Beta"])).toBe("");
    expect(sharedTonePrefix(["Only - One"])).toBe("");

    const files = [file(1), file(2)];
    const bare = { toneId: "26704", provider: "tone3000" };
    const full = { toneId: "26704", provider: "tone3000", creatorName: "hk" };
    const items = groupFolderFiles(files, (entry) => (entry === files[0] ? bare : full));
    const [header] = folderVirtualEntries(items, { toggled: new Map(), openAll: false, selectedPath: "" });
    if (header.kind !== "group") throw new Error("expected a group");
    const names = new Map([[files[0].path, "ProCo Rat - ProCo RAT"], [files[1].path, "ProCo Rat - ProCo RAT v2"]]);
    const html = renderFolderGroupHeader(header, (entry) => ({ id: entry.path, name: names.get(entry.path) ?? "", category: "", description: "", metadata: {} }));
    expect(html).toContain(">ProCo Rat<");
    expect(html).toContain("by hk");
    expect(html).not.toContain("by tone3000");
  });

  it("puts a group header's chevron after its picture and text", () => {
    const html = renderGroupHeader({ key: "k", title: "Pack", meta: ["", "3 models"], open: true, thumbAttr: "", thumbSlot: "<span class=\"thumb\"></span>" });
    expect(html.indexOf("thumb")).toBeLessThan(html.indexOf("resource-browser-pack-info"));
    expect(html.indexOf("resource-browser-pack-info")).toBeLessThan(html.indexOf("resource-browser-pack-expand"));
    expect(html).toContain('aria-expanded="true"');
    // Empty meta parts are dropped.
    expect(html).not.toContain("<span></span>");
  });
});
