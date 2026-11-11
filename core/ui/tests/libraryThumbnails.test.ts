import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  libraryThumbnailAttr,
  libraryThumbnailImage,
  onResourceThumbnails,
  resetLibraryThumbnailsForTests,
} from "../ts/resourceBrowser/libraryThumbnails.js";
import type { LibraryResource } from "../ts/types.js";

const KEY = "0123456789abcdef";
const PICTURE = "https://api.tone3000.com/storage/v1/object/public/images/abc.jpg";
const SIZED = "https://api.tone3000.com/storage/v1/render/image/public/images/abc.jpg?width=96&height=96&resize=cover";

function resource(id: string, metadata: Record<string, string>): LibraryResource {
  return { id, name: id, category: "amp", metadata } as unknown as LibraryResource;
}

/** Lets queued timers and promise chains run. */
async function settle(ms = 0): Promise<void> {
  await vi.advanceTimersByTimeAsync(ms);
}

describe("resource browser tone thumbnails", () => {
  const originalFetch = globalThis.fetch;
  let sent: Array<Record<string, unknown>>;

  beforeEach(() => {
    vi.useFakeTimers();
    resetLibraryThumbnailsForTests();
    sent = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
    URL.createObjectURL = vi.fn(() => "blob:thumb");
    document.body.innerHTML = "";
  });

  afterEach(() => {
    delete window.IPlugSendMsg;
    globalThis.fetch = originalFetch;
    vi.useRealTimers();
  });

  it("leaves a resource that did not come from Tone3000 as it was", async () => {
    expect(libraryThumbnailAttr("nam", resource("local", { gearMake: "Fender" }))).toBe("");
    expect(libraryThumbnailImage("nam", resource("local", { gearMake: "Fender" }))).toBe("");
    await settle();
    expect(sent).toEqual([]);
  });

  it("asks once for the rows drawn, and shows a cached file through the editor's route", async () => {
    const rows = [resource("tone3000:1", { provider: "tone3000", toneId: "10" }), resource("tone3000:2", { provider: "tone3000" })];
    document.body.innerHTML = rows.map((r) => `<div class="row"${libraryThumbnailAttr("nam", r)}><span>info</span></div>`).join("");
    rows.forEach((r) => libraryThumbnailAttr("nam", r)); // drawn again: not asked again
    await settle();

    expect(sent).toEqual([{ type: "getResourceThumbnails", resourceType: "nam", resourceIds: ["tone3000:1", "tone3000:2"] }]);

    onResourceThumbnails({
      type: "resourceThumbnails",
      resourceType: "nam",
      thumbnails: { "tone3000:1": KEY },
      files: { [KEY]: { file: `${KEY}.jpg`, path: "C:/x" } },
      fetch: {},
      lookup: [],
    });

    // The row on screen is patched in place; a row drawn now has it from the start.
    const image = document.querySelector<HTMLImageElement>("[data-thumb-for='nam:tone3000:1'] > img.resource-browser-item-thumb");
    expect(image?.getAttribute("src")).toBe(`resource-thumbnails/${KEY}.jpg`);
    expect(document.querySelectorAll("img").length).toBe(1);
    expect(libraryThumbnailImage("nam", rows[0])).toContain(`resource-thumbnails/${KEY}.jpg`);
    expect(libraryThumbnailImage("nam", rows[1])).toBe("");
  });

  it("downloads a missing thumbnail without credentials and hands it to the engine", async () => {
    const bytes = new Uint8Array([0xff, 0xd8, 0xff, 0xe0]);
    const fetchMock = vi.fn(async () => new Response(bytes, { status: 200, headers: { "content-type": "image/jpeg" } }));
    globalThis.fetch = fetchMock as unknown as typeof fetch;
    document.body.innerHTML = `<div${libraryThumbnailAttr("nam", resource("tone3000:1", { provider: "tone3000" }))}></div>`;

    onResourceThumbnails({
      type: "resourceThumbnails",
      resourceType: "nam",
      thumbnails: { "tone3000:1": KEY },
      files: {},
      fetch: { [KEY]: { url: SIZED, imageUrl: PICTURE } },
      lookup: [],
    });
    await settle();

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, init] = fetchMock.mock.calls[0] as unknown as [string, RequestInit];
    expect(url).toBe(SIZED);
    expect(init.credentials).toBe("omit");
    expect(JSON.stringify(init.headers)).not.toMatch(/authorization|api-key/i);
    expect(sent).toContainEqual({ type: "saveResourceThumbnail", imageUrl: PICTURE, data: "/9j/4A==" });
    expect(document.querySelector("img")?.getAttribute("src")).toBe("blob:thumb");
  });

  // Sepia: this exercises an upstream online service, which is switched off here
  // (ONLINE_SERVICES_ENABLED in ts/apiConfig.ts). Kept, skipped, for when a fork runs its own.
  it.skip("looks a tone up on the relay by its title, one at a time, and records what it found", async () => {
    const fetchMock = vi.fn(async (url: string) => {
      const query = new URL(url).searchParams.get("query");
      const data = query === "Twin Pack"
        ? [{ id: 9, title: "Twin Pack II" }, { id: 20, title: "Twin Pack", images: ["https://api.tone3000.com/storage/v1/object/public/images/twin.png"] }]
        : [];
      return new Response(JSON.stringify({ data, page: 1, total_pages: 1 }), { status: 200 });
    });
    globalThis.fetch = fetchMock as unknown as typeof fetch;

    onResourceThumbnails({
      type: "resourceThumbnails",
      resourceType: "nam",
      thumbnails: {},
      files: {},
      fetch: {},
      lookup: [
        { toneId: "20", title: "Twin Pack", resourceIds: ["tone3000:3"] },
        { toneId: "30", title: "Plexi", resourceIds: ["tone3000:4"] },
      ],
    });
    await settle();

    expect(fetchMock).toHaveBeenCalledTimes(1); // the second waits its turn
    expect(String(fetchMock.mock.calls[0][0])).toMatch(/^https:\/\/api-guitar\.soundshed\.com\/v1\/resourcesearch\/tones\/search\?/);
    expect(sent).toContainEqual({
      type: "saveResourceThumbnail",
      toneId: "20",
      imageUrl: "https://api.tone3000.com/storage/v1/object/public/images/twin.png",
    });
    // Its resources are asked about again, now that the engine knows the picture.
    expect(sent).toContainEqual({ type: "getResourceThumbnails", resourceType: "nam", resourceIds: ["tone3000:3"] });

    await settle(1500);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(sent).toContainEqual({ type: "saveResourceThumbnail", toneId: "30", imageUrl: "" });
  });
});
