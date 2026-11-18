/**
 * The Tone 3000 browser's results, kept so that looking at them again is not another request.
 *
 * Every page of results the browser has shown in the last hour is held in memory under the
 * request that produced it (mode, category, search, sort, page), so paging back or returning
 * to a category costs nothing. The one last on screen is also written to localStorage, so the
 * tab reopens on it after a restart. Nothing here fetches: tone3000Browser.ts asks this first
 * and goes to Tone3000 only on a miss, or when the user asks for the results again.
 *
 * It is a short-lived copy of what the user was just shown, not a copy of the catalogue: one
 * page per request, dropped after an hour.
 */

import type { Tone3000Tone } from "./tone3000ApiTypes.js";

/** Results older than this are fetched again the next time they are wanted. */
export const TONE3000_RESULTS_MAX_AGE_MS = 60 * 60 * 1000;

/** How many pages of results are kept. Each is at most one page (25 tones). */
const MAX_ENTRIES = 40;

const STORAGE_KEY = "guitarfx.tone3000Browser.lastResults.v1";

export interface Tone3000ResultsRequest {
  mode: "search" | "favorites";
  categoryId: string;
  query: string;
  sort: string;
  page: number;
}

export interface Tone3000CachedResults {
  request: Tone3000ResultsRequest;
  tones: Tone3000Tone[];
  /** The page the server said this is, and how many there are. */
  page: number;
  totalPages: number;
  fetchedAt: number;
}

const entries = new Map<string, Tone3000CachedResults>();

export function tone3000ResultsKey(request: Tone3000ResultsRequest): string {
  // Favourites ignore the category, the search and the sort.
  return request.mode === "favorites"
    ? JSON.stringify(["favorites", request.page])
    : JSON.stringify(["search", request.categoryId, request.query.trim().toLowerCase(), request.sort, request.page]);
}

export function isTone3000ResultsFresh(entry: Pick<Tone3000CachedResults, "fetchedAt">, now = Date.now()): boolean {
  const age = now - entry.fetchedAt;
  // A clock set back makes an entry look newer than now; treat that as stale.
  return age >= 0 && age < TONE3000_RESULTS_MAX_AGE_MS;
}

/** The results for a request, if they were fetched within the hour. */
export function getCachedTone3000Results(request: Tone3000ResultsRequest, now = Date.now()): Tone3000CachedResults | null {
  const key = tone3000ResultsKey(request);
  const entry = entries.get(key);
  if (!entry) {
    return null;
  }
  if (!isTone3000ResultsFresh(entry, now)) {
    entries.delete(key);
    return null;
  }
  return entry;
}

/** Keeps a page of results just fetched. */
export function storeTone3000Results(entry: Tone3000CachedResults): void {
  const key = tone3000ResultsKey(entry.request);
  entries.delete(key);
  entries.set(key, entry);
  while (entries.size > MAX_ENTRIES) {
    const oldest = entries.keys().next().value;
    if (oldest === undefined) {
      break;
    }
    entries.delete(oldest);
  }
}

function storage(): Storage | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    // Storage can be denied outright (a sandboxed page).
    return null;
  }
}

/** Records the results now on screen as the ones to reopen on. */
export function rememberLastTone3000Results(entry: Tone3000CachedResults): void {
  try {
    storage()?.setItem(STORAGE_KEY, JSON.stringify(entry));
  } catch {
    // Full or unavailable: the tab then just opens on a fresh search.
  }
}

function isRequest(value: unknown): value is Tone3000ResultsRequest {
  const request = value as Partial<Tone3000ResultsRequest> | null;
  return Boolean(request)
    && (request?.mode === "search" || request?.mode === "favorites")
    && typeof request.categoryId === "string"
    && typeof request.query === "string"
    && typeof request.sort === "string"
    && typeof request.page === "number" && Number.isFinite(request.page);
}

/**
 * The results that were on screen last time, if they are still within the hour. They go back
 * into the in-memory set, so asking for the same request finds them.
 */
export function loadLastTone3000Results(now = Date.now()): Tone3000CachedResults | null {
  let parsed: unknown;
  try {
    const raw = storage()?.getItem(STORAGE_KEY);
    if (!raw) {
      return null;
    }
    parsed = JSON.parse(raw);
  } catch {
    return null;
  }

  const entry = parsed as Partial<Tone3000CachedResults> | null;
  if (!entry || !isRequest(entry.request) || !Array.isArray(entry.tones)
    || typeof entry.fetchedAt !== "number" || typeof entry.page !== "number" || typeof entry.totalPages !== "number") {
    return null;
  }
  const results = entry as Tone3000CachedResults;
  if (!isTone3000ResultsFresh(results, now)) {
    return null;
  }
  storeTone3000Results(results);
  return results;
}

/** Forgets everything, e.g. when the Tone3000 account changes. */
export function clearTone3000ResultsCache(): void {
  entries.clear();
  try {
    storage()?.removeItem(STORAGE_KEY);
  } catch {
    // Nothing to forget.
  }
}

/** "just now", "12 min ago": how old the results on screen are. */
export function describeTone3000ResultsAge(fetchedAt: number, now = Date.now()): string {
  const minutes = Math.floor(Math.max(0, now - fetchedAt) / 60_000);
  if (minutes < 1) {
    return "just now";
  }
  if (minutes < 60) {
    return `${minutes} min ago`;
  }
  const hours = Math.floor(minutes / 60);
  return hours === 1 ? "over an hour ago" : `${hours} hours ago`;
}
