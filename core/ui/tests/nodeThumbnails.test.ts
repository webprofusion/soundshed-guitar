// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from "vitest";
import { nodeThumbnailImage, resetNodeThumbnailsForTests } from "../ts/signalPath/nodeThumbnails";

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

describe("nodeThumbnailImage", () => {
  afterEach(() => {
    resetNodeThumbnailsForTests();
    document.body.innerHTML = "";
  });

  it("draws the original first, then puts the smooth copy into the nodes on screen", async () => {
    const maker = vi.fn(async () => "blob:copy");
    resetNodeThumbnailsForTests(maker);

    const first = nodeThumbnailImage("../images/cab.png");
    expect(first).toContain('src="../images/cab.png"');
    document.body.innerHTML = first + nodeThumbnailImage("../images/cab.png");
    expect(maker).toHaveBeenCalledTimes(1);

    await flush();
    const sources = [...document.querySelectorAll("img")].map((image) => image.getAttribute("src"));
    expect(sources).toEqual(["blob:copy", "blob:copy"]);
    expect(nodeThumbnailImage("../images/cab.png")).toContain('src="blob:copy"');
    expect(maker).toHaveBeenCalledTimes(1);
  });

  it("keeps the original, and does not try again, when no copy can be made", async () => {
    const maker = vi.fn(async () => {
      throw new Error("no codecs");
    });
    resetNodeThumbnailsForTests(maker);

    nodeThumbnailImage("data:image/png;base64,AAAA");
    await flush();
    expect(nodeThumbnailImage("data:image/png;base64,AAAA")).toContain('src="data:image/png;base64,AAAA"');
    expect(maker).toHaveBeenCalledTimes(1);
  });
});
