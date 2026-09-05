/**
 * YouTube search client for backing tracks.
 *
 * Deliberately free of jam state and DOM: it takes a query and a key, and gives
 * back video summaries or throws. The orchestrator in ../jam.ts owns the cache
 * lookup, the loading flag and the re-render — which is what lets this module
 * avoid importing the panel, and the panel avoid importing this.
 */

import type { JamVideoSummary } from "../types.js";
import { SEARCH_MAX_RESULTS } from "./state.js";

function getBestThumbnail(snippet: Record<string, unknown> | undefined): string {
  const thumbnails = snippet?.thumbnails as Record<string, { url?: string }> | undefined;
  return thumbnails?.high?.url
    ?? thumbnails?.medium?.url
    ?? thumbnails?.default?.url
    ?? "";
}

export function normalizeSearchResults(payload: unknown): JamVideoSummary[] {
  if (!payload || typeof payload !== "object") {
    return [];
  }

  const items = (payload as { items?: unknown[] }).items;
  if (!Array.isArray(items)) {
    return [];
  }

  return items
    .map((item) => {
      const entry = item as Record<string, unknown>;
      const snippet = entry.snippet as Record<string, unknown> | undefined;
      const id = entry.id as Record<string, unknown> | undefined;
      const videoId = typeof id?.videoId === "string" ? id.videoId : "";
      if (!videoId || !snippet) {
        return null;
      }
      return {
        videoId,
        title: typeof snippet.title === "string" ? snippet.title : videoId,
        channelTitle: typeof snippet.channelTitle === "string" ? snippet.channelTitle : "Unknown channel",
        thumbnailUrl: getBestThumbnail(snippet),
      } satisfies JamVideoSummary;
    })
    .filter((entry): entry is JamVideoSummary => Boolean(entry));
}

export function describeFetchError(payload: unknown): string {
  if (!payload || typeof payload !== "object") {
    return "Request failed.";
  }

  const error = (payload as { error?: { message?: string } }).error;
  if (typeof error?.message === "string" && error.message.trim()) {
    return error.message.trim();
  }
  return "Request failed.";
}

/**
 * Queries the YouTube data API for embeddable backing tracks.
 *
 * Throws with the API's own message when the response is not ok, so callers can
 * surface it verbatim.
 */
export async function fetchBackingTracks(query: string, apiKey: string): Promise<JamVideoSummary[]> {
  const url = new URL("https://www.googleapis.com/youtube/v3/search");
  url.searchParams.set("part", "snippet");
  url.searchParams.set("type", "video");
  url.searchParams.set("videoEmbeddable", "true");
  url.searchParams.set("videoSyndicated", "true");
  url.searchParams.set("maxResults", SEARCH_MAX_RESULTS.toString());
  url.searchParams.set("q", query);
  url.searchParams.set("key", apiKey);

  const response = await fetch(url.toString());
  const payload = await response.json().catch(() => null);
  if (!response.ok) {
    throw new Error(describeFetchError(payload));
  }

  return normalizeSearchResults(payload);
}
