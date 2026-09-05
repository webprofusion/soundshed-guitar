/**
 * Jam panel state, persistence and feature gating.
 *
 * The leaf of the jam feature: everything here reads or writes `uiState.jam`
 * and the app settings behind it, and nothing here touches the panel DOM or
 * triggers a render. The render modules and the orchestrator in ../jam.ts all
 * depend on this file; it depends on none of them, which is what keeps the
 * feature free of import cycles.
 */

import { setAppSetting } from "../bridge.js";
import { uiState } from "../state.js";
import type { AppSettingValue, JamPlayerState, JamState, JamVideoSummary } from "../types.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";

export const API_KEY_SETTING = "jam.youtubeApiKey";
export const FAVORITES_SETTING = "jam.favorites";
export const SEARCH_CACHE_SETTING = "jam.searchCache";
export const PLAYER_UI_SETTING = "jam.playerUi";
export const PLAYER_WIDTH = 420;
export const PLAYER_MINIMIZED_WIDTH = 280;
export const PLAYER_PADDING = 24;
export const DEFAULT_PLAYER_Y = 96;
export const DEFAULT_JAM_QUERY = "backing track";
export const SEARCH_MAX_RESULTS = 12;

export type JamSectionId = "backingTracks" | "scales" | "riffs" | "practiceTool";
export type BackingTracksTabId = "search" | "favorites";
type LegacyJamState = JamState & { activeSection?: JamSectionId; activeTab?: BackingTracksTabId | "riffs" };
export type JamSearchCache = {
  query: string;
  normalizedQuery: string;
  results: JamVideoSummary[];
};

export function ensureJamState(): JamState {
  if (!uiState.jam) {
    uiState.jam = {
      activeSection: "backingTracks",
      activeTab: "search",
      query: DEFAULT_JAM_QUERY,
      results: [],
      favorites: [],
      loading: false,
      error: "",
      apiKeyAvailable: false,
      player: {
        open: false,
        minimized: false,
        x: 0,
        y: DEFAULT_PLAYER_Y,
        width: PLAYER_WIDTH,
        currentVideo: null,
      },
    };
  }

  const jam = uiState.jam as LegacyJamState;
  const legacyActiveTab = (uiState.jam as { activeTab?: BackingTracksTabId | "riffs" }).activeTab;
  if (
    jam.activeSection !== "backingTracks" &&
    jam.activeSection !== "scales" &&
    jam.activeSection !== "riffs" &&
    jam.activeSection !== "practiceTool"
  ) {
    jam.activeSection = legacyActiveTab === "riffs" ? "riffs" : "backingTracks";
  }
  if (jam.activeTab !== "search" && jam.activeTab !== "favorites") {
    jam.activeTab = "search";
  }

  return jam as JamState;
}

export function isJamVideoSummary(value: unknown): value is JamVideoSummary {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return typeof record.videoId === "string"
    && typeof record.title === "string"
    && typeof record.channelTitle === "string"
    && typeof record.thumbnailUrl === "string";
}

export function getApiKey(): string {
  const value = uiState.appSettings?.[API_KEY_SETTING];
  return typeof value === "string" ? value.trim() : "";
}

export function normalizeSearchQuery(rawQuery: string): string {
  const trimmed = rawQuery.trim().replace(/\s+/g, " ");
  if (!trimmed) {
    return "";
  }
  if (/\bbacking\s+track\b/i.test(trimmed)) {
    return trimmed;
  }
  return `${trimmed} backing track`;
}

export function normalizeFavorites(value: unknown): JamVideoSummary[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter(isJamVideoSummary);
}

export function normalizeSearchCache(value: unknown): JamSearchCache | null {
  if (!value || typeof value !== "object") {
    return null;
  }

  const record = value as Record<string, unknown>;
  const normalizedQuery = typeof record.normalizedQuery === "string"
    ? normalizeSearchQuery(record.normalizedQuery)
    : "";
  if (!normalizedQuery) {
    return null;
  }

  const query = typeof record.query === "string" && record.query.trim()
    ? record.query.trim()
    : normalizedQuery;

  return {
    query,
    normalizedQuery,
    results: normalizeFavorites(record.results),
  };
}

export function normalizePlayerState(value: unknown, current: JamPlayerState): JamPlayerState {
  if (!value || typeof value !== "object") {
    return current;
  }

  const record = value as Record<string, unknown>;
  return {
    ...current,
    minimized: typeof record.minimized === "boolean" ? record.minimized : current.minimized,
    x: typeof record.x === "number" && Number.isFinite(record.x) ? record.x : current.x,
    y: typeof record.y === "number" && Number.isFinite(record.y) ? record.y : current.y,
    width: typeof record.width === "number" && Number.isFinite(record.width) ? record.width : current.width,
  };
}

export function clampPlayerPosition(state: JamPlayerState): void {
  const width = state.minimized ? PLAYER_MINIMIZED_WIDTH : state.width;
  const maxX = Math.max(PLAYER_PADDING, window.innerWidth - width - PLAYER_PADDING);
  const maxY = Math.max(PLAYER_PADDING, window.innerHeight - 80);
  if (!Number.isFinite(state.x) || state.x <= 0) {
    state.x = maxX;
  }
  state.x = Math.min(Math.max(PLAYER_PADDING, state.x), maxX);
  state.y = Math.min(Math.max(PLAYER_PADDING, state.y), maxY);
}

export function persistFavorites(): void {
  const jam = ensureJamState();
  const payload = jam.favorites.map((favorite) => ({
    videoId: favorite.videoId,
    title: favorite.title,
    channelTitle: favorite.channelTitle,
    thumbnailUrl: favorite.thumbnailUrl,
  }));
  uiState.appSettings[FAVORITES_SETTING] = payload as unknown as AppSettingValue;
  setAppSetting(FAVORITES_SETTING, payload);
}

export function persistSearchCache(query: string, results: JamVideoSummary[]): void {
  const normalizedQuery = normalizeSearchQuery(query);
  if (!normalizedQuery) {
    return;
  }

  const payload = {
    query: query.trim() || normalizedQuery,
    normalizedQuery,
    results: results.map((result) => ({
      videoId: result.videoId,
      title: result.title,
      channelTitle: result.channelTitle,
      thumbnailUrl: result.thumbnailUrl,
    })),
  };
  uiState.appSettings[SEARCH_CACHE_SETTING] = payload as unknown as AppSettingValue;
  setAppSetting(SEARCH_CACHE_SETTING, payload);
}

export function getCachedSearch(query: string): JamSearchCache | null {
  const normalizedQuery = normalizeSearchQuery(query);
  if (!normalizedQuery) {
    return null;
  }

  const cached = normalizeSearchCache(uiState.appSettings?.[SEARCH_CACHE_SETTING]);
  if (!cached || cached.normalizedQuery !== normalizedQuery) {
    return null;
  }

  return cached;
}

export function persistPlayerUi(): void {
  const jam = ensureJamState();
  const payload = {
    minimized: jam.player.minimized,
    x: Math.round(jam.player.x),
    y: Math.round(jam.player.y),
    width: jam.player.width,
  };
  uiState.appSettings[PLAYER_UI_SETTING] = payload as unknown as AppSettingValue;
  setAppSetting(PLAYER_UI_SETTING, payload);
}

export function isFavorite(videoId: string): boolean {
  return ensureJamState().favorites.some((favorite) => favorite.videoId === videoId);
}

/**
 * Adds or removes a favourite and persists the result. Re-rendering is left to
 * the caller: both the results grid and the floating player show favourite
 * state, and which of them needs refreshing depends on where the click came from.
 */
export function toggleFavoriteVideo(video: JamVideoSummary): void {
  const jam = ensureJamState();
  const existingIndex = jam.favorites.findIndex((favorite) => favorite.videoId === video.videoId);
  if (existingIndex >= 0) {
    jam.favorites.splice(existingIndex, 1);
  } else {
    jam.favorites = [video, ...jam.favorites.filter((favorite) => favorite.videoId !== video.videoId)];
  }
  persistFavorites();
}

export function findVideoById(videoId: string): JamVideoSummary | undefined {
  const jam = ensureJamState();
  return jam.results.find((video) => video.videoId === videoId)
    ?? jam.favorites.find((video) => video.videoId === videoId);
}

export function isBackingTracksFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.Jam);
}

export function isRiffLibraryFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.RiffLibrary);
}

export function isPracticeToolFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.PracticeTool);
}

export function isScalesFeatureEnabled(): boolean {
  return true;
}

export function resolveJamSection(preferredSection: JamSectionId): JamSectionId {
  const orderedSections: JamSectionId[] = ["backingTracks", "scales", "riffs", "practiceTool"];
  const enabledSections = orderedSections.filter((sectionId) => {
    if (sectionId === "riffs") {
      return isRiffLibraryFeatureEnabled();
    }
    if (sectionId === "scales") {
      return isScalesFeatureEnabled();
    }
    if (sectionId === "practiceTool") {
      return isPracticeToolFeatureEnabled();
    }
    return isBackingTracksFeatureEnabled();
  });

  if (enabledSections.includes(preferredSection)) {
    return preferredSection;
  }

  return enabledSections[0] ?? "backingTracks";
}

export function resolveBackingTracksTab(preferredTab: BackingTracksTabId): BackingTracksTabId {
  return preferredTab === "favorites" ? "favorites" : "search";
}

export function isJamPanelVisible(): boolean {
  return document.getElementById("panel-jam")?.classList.contains("active") ?? false;
}
