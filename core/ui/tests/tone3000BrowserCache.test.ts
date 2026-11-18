import { beforeEach, describe, expect, it } from "vitest";
import {
  TONE3000_RESULTS_MAX_AGE_MS,
  clearTone3000ResultsCache,
  describeTone3000ResultsAge,
  getCachedTone3000Results,
  isTone3000ResultsFresh,
  loadLastTone3000Results,
  rememberLastTone3000Results,
  storeTone3000Results,
  tone3000ResultsKey,
} from "../ts/tone3000BrowserCache.js";
import type { Tone3000CachedResults, Tone3000ResultsRequest } from "../ts/tone3000BrowserCache.js";
import type { Tone3000Tone } from "../ts/tone3000ApiTypes.js";

const NOW = 1_800_000_000_000;
const MINUTE = 60_000;

const request = (overrides: Partial<Tone3000ResultsRequest> = {}): Tone3000ResultsRequest => ({
  mode: "search",
  categoryId: "amp",
  query: "",
  sort: "popular",
  page: 1,
  ...overrides,
});

const results = (overrides: Partial<Tone3000CachedResults> = {}): Tone3000CachedResults => ({
  request: request(),
  tones: [{ id: 1, title: "Plexi" } as unknown as Tone3000Tone],
  page: 1,
  totalPages: 4,
  fetchedAt: NOW,
  ...overrides,
});

beforeEach(() => {
  clearTone3000ResultsCache();
});

describe("tone3000ResultsKey", () => {
  it("tells apart the category, the search, the sort and the page", () => {
    const keys = new Set([
      tone3000ResultsKey(request()),
      tone3000ResultsKey(request({ categoryId: "cab" })),
      tone3000ResultsKey(request({ query: "plexi" })),
      tone3000ResultsKey(request({ sort: "recent" })),
      tone3000ResultsKey(request({ page: 2 })),
    ]);
    expect(keys.size).toBe(5);
  });

  it("treats a search the same whatever its case or padding", () => {
    expect(tone3000ResultsKey(request({ query: " Plexi " }))).toBe(tone3000ResultsKey(request({ query: "plexi" })));
  });

  it("keys favourites by page alone", () => {
    expect(tone3000ResultsKey(request({ mode: "favorites", categoryId: "amp", query: "x" })))
      .toBe(tone3000ResultsKey(request({ mode: "favorites", categoryId: "cab", query: "" })));
    expect(tone3000ResultsKey(request({ mode: "favorites" }))).not.toBe(tone3000ResultsKey(request()));
  });
});

describe("the results kept in memory", () => {
  it("answers the same request within the hour and not after it", () => {
    storeTone3000Results(results());
    expect(getCachedTone3000Results(request(), NOW + 59 * MINUTE)?.tones).toHaveLength(1);
    expect(getCachedTone3000Results(request(), NOW + TONE3000_RESULTS_MAX_AGE_MS)).toBeNull();
    // Once found stale it is gone, even for an earlier clock.
    expect(getCachedTone3000Results(request(), NOW + MINUTE)).toBeNull();
  });

  it("misses a request it has not seen", () => {
    storeTone3000Results(results());
    expect(getCachedTone3000Results(request({ page: 2 }), NOW)).toBeNull();
    expect(getCachedTone3000Results(request({ categoryId: "cab" }), NOW)).toBeNull();
  });

  it("replaces a request's results with newer ones", () => {
    storeTone3000Results(results());
    storeTone3000Results(results({ totalPages: 9, fetchedAt: NOW + MINUTE }));
    expect(getCachedTone3000Results(request(), NOW + 2 * MINUTE)?.totalPages).toBe(9);
  });

  it("drops the oldest pages past its limit", () => {
    for (let page = 1; page <= 45; page += 1) {
      storeTone3000Results(results({ request: request({ page }), page }));
    }
    expect(getCachedTone3000Results(request({ page: 1 }), NOW)).toBeNull();
    expect(getCachedTone3000Results(request({ page: 45 }), NOW)?.page).toBe(45);
  });

  it("does not trust results dated in the future", () => {
    expect(isTone3000ResultsFresh({ fetchedAt: NOW + MINUTE }, NOW)).toBe(false);
  });
});

describe("the results to reopen on", () => {
  it("come back within the hour, and answer their request again", () => {
    rememberLastTone3000Results(results({ request: request({ query: "plexi", page: 3 }), page: 3 }));
    const last = loadLastTone3000Results(NOW + 30 * MINUTE);
    expect(last?.request).toEqual(request({ query: "plexi", page: 3 }));
    expect(getCachedTone3000Results(request({ query: "plexi", page: 3 }), NOW + 30 * MINUTE)?.page).toBe(3);
  });

  it("are not offered once they are an hour old", () => {
    rememberLastTone3000Results(results());
    expect(loadLastTone3000Results(NOW + TONE3000_RESULTS_MAX_AGE_MS + 1)).toBeNull();
    expect(getCachedTone3000Results(request(), NOW)).toBeNull();
  });

  it("ignore anything stored that is not results", () => {
    localStorage.setItem("guitarfx.tone3000Browser.lastResults.v1", "{not json");
    expect(loadLastTone3000Results(NOW)).toBeNull();
    localStorage.setItem("guitarfx.tone3000Browser.lastResults.v1", JSON.stringify({ request: { mode: "search" }, tones: [], fetchedAt: NOW }));
    expect(loadLastTone3000Results(NOW)).toBeNull();
  });

  it("are forgotten with the rest", () => {
    storeTone3000Results(results());
    rememberLastTone3000Results(results());
    clearTone3000ResultsCache();
    expect(loadLastTone3000Results(NOW)).toBeNull();
    expect(getCachedTone3000Results(request(), NOW)).toBeNull();
  });
});

describe("describeTone3000ResultsAge", () => {
  it("counts in minutes", () => {
    expect(describeTone3000ResultsAge(NOW, NOW + 20_000)).toBe("just now");
    expect(describeTone3000ResultsAge(NOW, NOW + 12 * MINUTE)).toBe("12 min ago");
    expect(describeTone3000ResultsAge(NOW, NOW + 61 * MINUTE)).toBe("over an hour ago");
    expect(describeTone3000ResultsAge(NOW, NOW + 130 * MINUTE)).toBe("2 hours ago");
  });
});
