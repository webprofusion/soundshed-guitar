/**
 * The small copies of artwork the signal chain's nodes draw.
 *
 * A node's picture is drawn about 40 CSS px across, while stock equipment images are well over
 * 1,000 px and layout thumbnails 280 px. Shrinking that much in one draw, as the browser does for
 * an <img>, aliases badly (jagged edges, shimmering detail). So each picture is resampled once,
 * with the browser's high-quality filter, to just over the size it is drawn at on this screen,
 * and nodes draw that copy instead.
 *
 * The first render draws the original; when the copy is ready it is put into the nodes on screen
 * in place, and later renders use it at once. Copies live for the session as object URLs.
 */

import { escapeHtml } from "../utils.js";

/** Shortest edge, in CSS px, a copy keeps: the compact tile (40 px) covered, with a little spare. */
const COPY_SHORT_EDGE_CSS = 44;
/** A source no more than this many times the copy's size is drawn well enough as it is. */
const MIN_SHRINK = 2;

/** Makes the copy of `url` whose shortest edge is `shortEdge` px; null when there is no gain or no way. */
export type NodeThumbnailMaker = (url: string, shortEdge: number) => Promise<string | null>;

const keyBySource = new Map<string, number>();
/** Per key and copy size: the copy's URL once made, or null when the original is drawn. */
const copyByKeyAndEdge = new Map<string, string | null>();
let maker: NodeThumbnailMaker = makeThumbnailCopy;

function copyShortEdge(): number {
  const ratio = typeof devicePixelRatio === "number" && devicePixelRatio > 0 ? Math.min(devicePixelRatio, 4) : 1;
  return Math.ceil(COPY_SHORT_EDGE_CSS * ratio);
}

/** The `<img>` for a node's artwork: the smooth copy when there is one, else the original while it is made. */
export function nodeThumbnailImage(url: string): string {
  let key = keyBySource.get(url);
  if (key === undefined) {
    key = keyBySource.size + 1;
    keyBySource.set(url, key);
  }
  const edge = copyShortEdge();
  const copyKey = `${key}@${edge}`;
  if (!copyByKeyAndEdge.has(copyKey)) {
    // Recorded before it is made, so a re-render while it is under way starts no second one.
    copyByKeyAndEdge.set(copyKey, null);
    void maker(url, edge)
      .catch(() => null)
      .then((copy) => {
        if (!copy) return;
        copyByKeyAndEdge.set(copyKey, copy);
        patchImages(key!, copy);
      });
  }
  const src = copyByKeyAndEdge.get(copyKey) ?? url;
  return `<img class="node-layout-thumb" src="${escapeHtml(src)}" data-thumb-key="${key}" alt="" />`;
}

function patchImages(key: number, src: string): void {
  if (typeof document === "undefined") return;
  document.querySelectorAll<HTMLImageElement>(`img.node-layout-thumb[data-thumb-key="${key}"]`).forEach((image) => {
    image.src = src;
  });
}

/** Resamples `url` with the browser's high-quality filter, as a WebP object URL. */
async function makeThumbnailCopy(url: string, shortEdge: number): Promise<string | null> {
  if (typeof createImageBitmap !== "function" || typeof OffscreenCanvas === "undefined"
    || typeof URL === "undefined" || typeof URL.createObjectURL !== "function") {
    return null;
  }
  const response = await fetch(url);
  if (!response.ok) return null;
  const source = await response.blob();
  const probe = await createImageBitmap(source);
  const scale = shortEdge / Math.min(probe.width, probe.height);
  const width = Math.max(1, Math.round(probe.width * scale));
  const height = Math.max(1, Math.round(probe.height * scale));
  probe.close();
  if (scale * MIN_SHRINK > 1) return null;

  const bitmap = await createImageBitmap(source, { resizeWidth: width, resizeHeight: height, resizeQuality: "high" });
  const canvas = new OffscreenCanvas(width, height);
  const context = canvas.getContext("2d");
  if (!context) {
    bitmap.close();
    return null;
  }
  context.drawImage(bitmap, 0, 0);
  bitmap.close();
  return URL.createObjectURL(await canvas.convertToBlob({ type: "image/webp", quality: 0.92 }));
}

export function resetNodeThumbnailsForTests(testMaker?: NodeThumbnailMaker): void {
  keyBySource.clear();
  copyByKeyAndEdge.clear();
  maker = testMaker ?? makeThumbnailCopy;
}
