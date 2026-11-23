/**
 * The Tone3000 tone pictures the resource browser's local lists show: a Library row, and a
 * Folder file that is in the library, get its tone's picture when one is known.
 *
 * The engine keeps the pictures as small files in the profile's cache
 * (core/src/controller/ResourceThumbnailService.h) but makes no requests, so this fills it:
 *
 * - Rows ask the engine (`getResourceThumbnails`, batched) which picture each shows. A cached
 *   file loads through the editor's `resource-thumbnails/<file>` route.
 * - A picture the engine has not got is downloaded at the sized URL it gives (Tone3000's
 *   storage serves a 96 px copy; no credentials), shown from memory at once and handed to the
 *   engine to keep (`saveResourceThumbnail`).
 * - A tone whose picture was never recorded is looked up on Tone3000 through Soundshed's relay
 *   (it has no tone-by-id path, so by the tone's title, matching the id), at most MAX_LOOKUPS a
 *   session and one every LOOKUP_SPACING_MS, so the relay's shared key is not run down. What it
 *   finds, a picture or none, goes to the engine too, so the next session does not ask.
 *
 * Everything known is kept in memory for the session, so reopening the browser asks nothing.
 * Every row keeps a fixed picture slot on its left, as the Tone3000 tab's rows do
 * (`libraryThumbnailSlot`); one drawn before its picture shows an empty box there, which is
 * swapped for the picture in place when it comes. The native UIs do
 * the same through core/src/uiclient/ResourceThumbnails.h.
 */

import { postMessage } from "../bridge.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { extractTone3000Tones, SOUNDSHED_TONE3000_PROXY_API_BASE } from "../tone3000Api.js";
import { getTone3000ImageUrl } from "../tone3000Shared.js";
import type { LibraryResource } from "../types.js";
import { arrayBufferToBase64, escapeHtml } from "../utils.js";

type ThumbnailResourceType = "nam" | "ir";

interface Lookup {
  resourceType: ThumbnailResourceType;
  toneId: string;
  title: string;
  resourceIds: string[];
}

const ROUTE = "resource-thumbnails/";
const CACHE_FILE = /^[0-9a-f]{16}\.(jpg|png|webp|gif)$/;
/** Lookups a session may make; the native UIs keep the same limit. */
export const MAX_LOOKUPS = 12;
const LOOKUP_SPACING_MS = 1500;
const MAX_CONCURRENT_FETCHES = 3;
/** The engine keeps no thumbnail bigger than this. */
const MAX_BYTES = 512 * 1024;

const keyByResource = new Map<string, string>();
const srcByKey = new Map<string, string>();
const fetchByKey = new Map<string, { url: string; imageUrl: string }>();
const asked = new Set<string>();
const pendingAsks = new Map<ThumbnailResourceType, Set<string>>();
let askScheduled = false;

const fetchQueue: string[] = [];
const fetchStarted = new Set<string>();
let fetchesRunning = 0;

const lookupQueue: Lookup[] = [];
const lookedUp = new Set<string>();
let lookupsStarted = 0;
let lookupRunning = false;

function resourceKey(resourceType: string, resourceId: string): string {
  return `${resourceType}:${resourceId}`;
}

/** A library resource that came from Tone3000, so may have a tone picture. */
export function isThumbnailCandidate(resource: LibraryResource | undefined | null): boolean {
  const metadata = resource?.metadata;
  return Boolean(resource) && (metadata?.provider === "tone3000" || resource!.id.startsWith("tone3000:") || Boolean(metadata?.toneId));
}

/** What an `<img>` loads for this resource's picture, or null while none is known. */
export function libraryThumbnailSrc(resourceType: string, resourceId: string): string | null {
  const key = keyByResource.get(resourceKey(resourceType, resourceId));
  return key ? srcByKey.get(key) ?? null : null;
}

function imageMarkup(src: string): string {
  // A remote picture (a Folder file's Tone3000 match) is loaded as the details card loads it.
  const referrer = /^https:\/\//.test(src) ? ` referrerpolicy="no-referrer"` : "";
  return `<img class="resource-browser-item-thumb" src="${escapeHtml(src)}" alt="" loading="lazy" decoding="async"${referrer} />`;
}

/** The empty box a row's picture slot shows until (unless) a picture is known. */
const PLACEHOLDER_MARKUP = `<span class="resource-browser-item-thumb is-placeholder" aria-hidden="true"></span>`;

/**
 * A row's fixed picture slot, as the Tone3000 tab's rows have: the resource's tone picture when
 * one is known, else `fallbackSrc` (an https picture the caller already has, such as a Folder
 * file's Tone3000 match), else an empty box of the same size that the picture replaces when it
 * arrives. Every row gets the slot, so titles line up whether or not a row has a picture.
 */
export function libraryThumbnailSlot(resourceType: string, resource: LibraryResource | undefined | null, fallbackSrc?: string): string {
  const src = (resource && isThumbnailCandidate(resource) ? libraryThumbnailSrc(resourceType, resource.id) : null)
    ?? (fallbackSrc && /^https:\/\//.test(fallbackSrc.trim()) ? fallbackSrc.trim() : null);
  return src ? imageMarkup(src) : PLACEHOLDER_MARKUP;
}

/**
 * ` data-thumb-for="…"` for a row showing a resource that may have a tone picture, so a picture
 * arriving later finds the row; the row's rendering is also what asks the engine about it. Empty
 * for anything else, so those rows look as they always have.
 */
export function libraryThumbnailAttr(resourceType: string, resource: LibraryResource | undefined | null): string {
  if (!resource || !isThumbnailCandidate(resource)) {
    return "";
  }
  requestLibraryThumbnails(resourceType, [resource.id]);
  return ` data-thumb-for="${escapeHtml(resourceKey(resourceType, resource.id))}"`;
}

/** Asks the engine which pictures these resources show, once a session each (batched). */
export function requestLibraryThumbnails(resourceType: string, resourceIds: Iterable<string>): void {
  if (resourceType !== "nam" && resourceType !== "ir") {
    return;
  }
  for (const id of resourceIds) {
    const key = resourceKey(resourceType, id);
    if (!id || asked.has(key)) {
      continue;
    }
    asked.add(key);
    let pending = pendingAsks.get(resourceType);
    if (!pending) {
      pending = new Set();
      pendingAsks.set(resourceType, pending);
    }
    pending.add(id);
  }
  if (pendingAsks.size && !askScheduled) {
    askScheduled = true;
    setTimeout(flushAsks, 0);
  }
}

function flushAsks(): void {
  askScheduled = false;
  for (const [resourceType, ids] of pendingAsks) {
    postMessage({ type: "getResourceThumbnails", resourceType, resourceIds: Array.from(ids) });
  }
  pendingAsks.clear();
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : {};
}

/** The engine's answer to getResourceThumbnails. */
export function onResourceThumbnails(payload: Record<string, unknown>): void {
  const resourceType = payload.resourceType;
  if (resourceType !== "nam" && resourceType !== "ir") {
    return;
  }

  for (const [id, key] of Object.entries(asRecord(payload.thumbnails))) {
    if (typeof key === "string") {
      keyByResource.set(resourceKey(resourceType, id), key);
    }
  }

  for (const [key, entry] of Object.entries(asRecord(payload.files))) {
    const file = asRecord(entry).file;
    if (typeof file === "string" && CACHE_FILE.test(file) && !srcByKey.has(key)) {
      srcByKey.set(key, `${ROUTE}${file}`);
    }
  }

  for (const [key, entry] of Object.entries(asRecord(payload.fetch))) {
    const { url, imageUrl } = asRecord(entry);
    if (typeof url === "string" && /^https?:\/\//.test(url) && typeof imageUrl === "string" && !srcByKey.has(key) && !fetchStarted.has(key)) {
      fetchByKey.set(key, { url, imageUrl });
      fetchStarted.add(key);
      fetchQueue.push(key);
    }
  }

  for (const entry of Array.isArray(payload.lookup) ? payload.lookup : []) {
    const { toneId, title, resourceIds } = asRecord(entry);
    if (typeof toneId === "string" && toneId && typeof title === "string" && title && !lookedUp.has(toneId)) {
      lookedUp.add(toneId);
      lookupQueue.push({
        resourceType,
        toneId,
        title,
        resourceIds: Array.isArray(resourceIds) ? resourceIds.filter((id): id is string => typeof id === "string") : [],
      });
    }
  }

  patchRows();
  pumpFetches();
  pumpLookups();
}

/** Puts a picture that is now known into the rows on screen that were drawn without it. */
function patchRows(): void {
  if (typeof document === "undefined") {
    return;
  }
  document.querySelectorAll<HTMLElement>("[data-thumb-for]").forEach((row) => {
    if (row.querySelector(":scope > img.resource-browser-item-thumb")) {
      return;
    }
    const key = keyByResource.get(row.dataset.thumbFor ?? "");
    const src = key ? srcByKey.get(key) : undefined;
    if (!src) {
      return;
    }
    const placeholder = row.querySelector(":scope > .resource-browser-item-thumb.is-placeholder");
    if (placeholder) {
      placeholder.outerHTML = imageMarkup(src);
      return;
    }
    row.insertAdjacentHTML("afterbegin", imageMarkup(src));
  });
}

function networkAllowed(): boolean {
  return isFeatureEnabled(Features.Tone3000) && typeof fetch === "function";
}

function pumpFetches(): void {
  while (networkAllowed() && fetchesRunning < MAX_CONCURRENT_FETCHES && fetchQueue.length) {
    const key = fetchQueue.shift()!;
    const job = fetchByKey.get(key);
    if (!job) {
      continue;
    }
    fetchesRunning += 1;
    void fetchThumbnail(key, job.url, job.imageUrl).finally(() => {
      fetchesRunning -= 1;
      pumpFetches();
    });
  }
}

async function fetchThumbnail(key: string, url: string, imageUrl: string): Promise<void> {
  try {
    // Tone3000's storage answers WebP, the smallest, when asked; both UIs draw it.
    const response = await fetch(url, { credentials: "omit", headers: { Accept: "image/webp,image/*;q=0.8" } });
    if (!response.ok) {
      return;
    }
    const buffer = await response.arrayBuffer();
    if (!buffer.byteLength || buffer.byteLength > MAX_BYTES) {
      return;
    }
    postMessage({ type: "saveResourceThumbnail", imageUrl, data: arrayBufferToBase64(buffer) });
    if (typeof URL !== "undefined" && typeof URL.createObjectURL === "function") {
      const blob = new Blob([buffer], { type: response.headers.get("content-type") ?? "image/jpeg" });
      srcByKey.set(key, URL.createObjectURL(blob));
      patchRows();
    }
  } catch {
    // No network, or the host refused: the row keeps its plain look, and nothing is retried.
  }
}

function pumpLookups(): void {
  if (lookupRunning || !lookupQueue.length || lookupsStarted >= MAX_LOOKUPS || !networkAllowed()) {
    return;
  }
  const lookup = lookupQueue.shift()!;
  lookupsStarted += 1;
  lookupRunning = true;
  void lookUpTone(lookup).finally(() => {
    setTimeout(() => {
      lookupRunning = false;
      pumpLookups();
    }, LOOKUP_SPACING_MS);
  });
}

async function lookUpTone(lookup: Lookup): Promise<void> {
  try {
    const params = new URLSearchParams({ page: "1", page_size: "20", query: lookup.title });
    const response = await fetch(`${SOUNDSHED_TONE3000_PROXY_API_BASE}/tones/search?${params.toString()}`, { credentials: "omit" });
    if (!response.ok) {
      return;
    }
    const tone = extractTone3000Tones(await response.json()).find((candidate) => String(candidate.id) === lookup.toneId);
    // Not among the results counts as no picture; the engine tries again in a month.
    const imageUrl = tone ? getTone3000ImageUrl(tone) ?? "" : "";
    postMessage({ type: "saveResourceThumbnail", toneId: lookup.toneId, imageUrl: imageUrl.startsWith("data:") ? "" : imageUrl });
    if (imageUrl && !imageUrl.startsWith("data:")) {
      lookup.resourceIds.forEach((id) => asked.delete(resourceKey(lookup.resourceType, id)));
      requestLibraryThumbnails(lookup.resourceType, lookup.resourceIds);
    }
  } catch {
    // The relay could not be reached: asked again next session.
  }
}

/** Forgets everything; for tests. */
export function resetLibraryThumbnailsForTests(): void {
  keyByResource.clear();
  srcByKey.clear();
  fetchByKey.clear();
  asked.clear();
  pendingAsks.clear();
  askScheduled = false;
  fetchQueue.length = 0;
  fetchStarted.clear();
  fetchesRunning = 0;
  lookupQueue.length = 0;
  lookedUp.clear();
  lookupsStarted = 0;
  lookupRunning = false;
}
