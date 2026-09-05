/**
 * Jam panel entry point: search orchestration, section/tab switching, DOM
 * wiring and lifecycle.
 *
 * The feature is split across ./jam/, layered so the imports only ever run one
 * way — state -> panel -> player, with this file on top:
 *
 *   jam/state.ts    jam state, persistence, feature gating (imports nothing local)
 *   jam/search.ts   YouTube client; takes a query, returns videos
 *   jam/panel.ts    section chrome and the results grid
 *   jam/player.ts   floating player, its dock and favourite action
 */

import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { uiState } from "./state.js";
import { isJamEnabled } from "./buildFlags.js";
import { FEATURE_FLAGS_CHANGED_EVENT, isJamExperienceEnabled } from "./featureFlags.js";
import { renderRiffLibraryPanel } from "./riffLibrary.js";
import { renderPracticeToolPanel } from "./practiceTool.js";
import { fetchBackingTracks } from "./jam/search.js";
import { renderJamPanel } from "./jam/panel.js";
import { openPlayer, renderFloatingPlayer, toggleFavorite } from "./jam/player.js";
import {
  DEFAULT_JAM_QUERY,
  FAVORITES_SETTING,
  PLAYER_UI_SETTING,
  PLAYER_WIDTH,
  SEARCH_CACHE_SETTING,
  clampPlayerPosition,
  ensureJamState,
  findVideoById,
  getApiKey,
  getCachedSearch,
  isBackingTracksFeatureEnabled,
  isJamPanelVisible,
  normalizeFavorites,
  normalizePlayerState,
  normalizeSearchCache,
  normalizeSearchQuery,
  persistSearchCache,
  resolveBackingTracksTab,
  resolveJamSection,
  type BackingTracksTabId,
  type JamSectionId,
} from "./jam/state.js";

export { renderJamPanel } from "./jam/panel.js";
export { renderFloatingPlayer } from "./jam/player.js";

let initialized = false;
let searchRequestId = 0;
let initialSearchTriggered = false;

function maybeRunInitialSearch(): void {
  const jam = ensureJamState();
  if (!isJamPanelVisible() || !isBackingTracksFeatureEnabled() || jam.activeSection !== "backingTracks" || jam.activeTab !== "search") {
    return;
  }

  jam.apiKeyAvailable = getApiKey().length > 0;
  if (!jam.apiKeyAvailable || getCachedSearch(jam.query) || jam.results.length > 0 || jam.favorites.length > 0 || jam.loading || initialSearchTriggered) {
    return;
  }

  initialSearchTriggered = true;
  void runSearch();
}

function setActiveSection(section: JamSectionId): void {
  const jam = ensureJamState();
  jam.activeSection = resolveJamSection(section);
  jam.activeTab = resolveBackingTracksTab(jam.activeTab);
  renderJamPanel();

  if (jam.activeSection === "riffs") {
    renderRiffLibraryPanel();
    return;
  }

  if (jam.activeSection === "practiceTool") {
    renderPracticeToolPanel();
    return;
  }

  if (jam.activeSection === "scales") {
    return;
  }

  if (jam.activeTab === "search") {
    maybeRunInitialSearch();
  }
}

function setBackingTracksTab(tab: BackingTracksTabId): void {
  const jam = ensureJamState();
  jam.activeSection = resolveJamSection("backingTracks");
  jam.activeTab = resolveBackingTracksTab(tab);
  renderJamPanel();
  if (jam.activeTab === "search") {
    maybeRunInitialSearch();
  }
}

async function runSearch(): Promise<void> {
  const jam = ensureJamState();
  const query = normalizeSearchQuery(jam.query);
  if (!query) {
    jam.error = "Enter a search term to find backing tracks.";
    jam.results = [];
    renderJamPanel();
    return;
  }

  const cached = getCachedSearch(jam.query);
  if (cached) {
    jam.results = cached.results;
    jam.loading = false;
    jam.error = jam.results.length === 0 ? "No matching backing tracks found." : "";
    appendLog(`jam search cache hit → ${cached.normalizedQuery}`);
    renderJamPanel();
    return;
  }

  const apiKey = getApiKey();
  if (!apiKey) {
    jam.error = "Jam search is unavailable in this build.";
    jam.results = [];
    renderJamPanel();
    return;
  }

  const requestId = ++searchRequestId;
  jam.loading = true;
  jam.error = "";
  renderJamPanel();

  try {
    const results = await fetchBackingTracks(query, apiKey);
    if (requestId !== searchRequestId) {
      return;
    }

    jam.results = results;
    jam.loading = false;
    jam.error = jam.results.length === 0 ? "No matching backing tracks found." : "";
    persistSearchCache(jam.query, jam.results);
    appendLog(`jam search → ${query}`);
  } catch (error) {
    if (requestId !== searchRequestId) {
      return;
    }
    jam.loading = false;
    jam.results = [];
    jam.error = error instanceof Error ? error.message : String(error);
    appendLog(`jam search failed → ${jam.error}`);
    showNotification("Jam search failed", jam.error);
  }

  renderJamPanel();
}

function bindPanelActions(): void {
  const searchInput = document.getElementById("jam-search-input") as HTMLInputElement | null;
  const searchButton = document.getElementById("jam-search-button");
  const resultsHosts = [document.getElementById("jam-results"), document.getElementById("jam-favorites-results")];

  document.getElementById("jam-section-scales")?.addEventListener("click", () => setActiveSection("scales"));
  document.getElementById("jam-section-riffs")?.addEventListener("click", () => setActiveSection("riffs"));
  document.getElementById("jam-section-practice-tool")?.addEventListener("click", () => setActiveSection("practiceTool"));
  document.getElementById("jam-backing-tab-search")?.addEventListener("click", () => setBackingTracksTab("search"));
  document.getElementById("jam-backing-tab-favorites")?.addEventListener("click", () => setBackingTracksTab("favorites"));

  searchInput?.addEventListener("input", () => {
    ensureJamState().query = searchInput.value;
  });
  searchInput?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      void runSearch();
    }
  });
  searchButton?.addEventListener("click", () => void runSearch());

  resultsHosts.forEach((resultsHost) => {
    resultsHost?.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) {
        return;
      }
      const actionElement = target.closest<HTMLElement>("[data-jam-action]");
      const videoId = actionElement?.dataset.videoId ?? target.closest<HTMLElement>("[data-video-id]")?.dataset.videoId ?? "";
      const video = findVideoById(videoId);
      if (!video) {
        return;
      }

      if (actionElement?.dataset.jamAction === "favorite") {
        toggleFavorite(video);
        return;
      }

      if (actionElement?.dataset.jamAction === "play") {
        openPlayer(video);
      }
    });
  });
}

export function applyJamAppSettings(): void {
  if (!isJamEnabled() || !isJamExperienceEnabled()) {
    return;
  }

  const jam = ensureJamState();
  jam.activeSection = resolveJamSection(jam.activeSection);
  jam.activeTab = resolveBackingTracksTab(jam.activeTab);
  if (!jam.query.trim()) {
    jam.query = DEFAULT_JAM_QUERY;
  }
  jam.apiKeyAvailable = getApiKey().length > 0;
  jam.favorites = normalizeFavorites(uiState.appSettings?.[FAVORITES_SETTING]);
  const searchCache = normalizeSearchCache(uiState.appSettings?.[SEARCH_CACHE_SETTING]);
  const currentNormalizedQuery = normalizeSearchQuery(jam.query);
  if (searchCache && !jam.loading && (jam.results.length === 0 || searchCache.normalizedQuery === currentNormalizedQuery)) {
    if (!jam.query.trim() || jam.query === DEFAULT_JAM_QUERY || searchCache.normalizedQuery === currentNormalizedQuery) {
      jam.query = searchCache.query;
    }
    jam.results = searchCache.results;
    jam.error = jam.results.length === 0 ? "No matching backing tracks found." : "";
  }
  jam.player = normalizePlayerState(uiState.appSettings?.[PLAYER_UI_SETTING], jam.player);
  jam.player.width = PLAYER_WIDTH;
  clampPlayerPosition(jam.player);
  renderJamPanel();
  renderFloatingPlayer();
}

function handleFeatureFlagsChanged(): void {
  if (!initialized) {
    return;
  }

  const jam = ensureJamState();
  jam.activeSection = resolveJamSection(jam.activeSection);
  jam.activeTab = resolveBackingTracksTab(jam.activeTab);
  if (!isBackingTracksFeatureEnabled()) {
    jam.loading = false;
  }
  renderJamPanel();
  if (isJamPanelVisible()) {
    maybeRunInitialSearch();
  }
}

export function handleJamPanelActivated(): void {
  if (!initialized || !isJamEnabled() || !isJamExperienceEnabled()) {
    return;
  }

  const jam = ensureJamState();
  jam.activeSection = resolveJamSection(jam.activeSection);
  jam.activeTab = resolveBackingTracksTab(jam.activeTab);
  renderJamPanel();
  maybeRunInitialSearch();
}

export function initializeJamPanel(): void {
  if (!isJamEnabled()) {
    return;
  }

  if (initialized) {
    return;
  }
  initialized = true;
  bindPanelActions();
  window.addEventListener("resize", () => renderFloatingPlayer());
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, handleFeatureFlagsChanged);
  applyJamAppSettings();
}
