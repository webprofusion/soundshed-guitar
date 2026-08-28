import { setAppSetting } from "./bridge.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { uiState } from "./state.js";
import type { AppSettingValue, JamPlayerState, JamState, JamVideoSummary } from "./types.js";
import { escapeHtml } from "./utils.js";
import { getApiBaseUrl } from "./toneSharingPanel.js";
import { isJamEnabled } from "./buildFlags.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled, isJamExperienceEnabled } from "./featureFlags.js";
import { renderRiffLibraryPanel } from "./riffLibrary.js";
import { renderEarPracticePlayerPanel } from "./earPracticePlayer.js";
import { getXMarkSvg } from "./iconAssets.js";

const API_KEY_SETTING = "jam.youtubeApiKey";
const FAVORITES_SETTING = "jam.favorites";
const SEARCH_CACHE_SETTING = "jam.searchCache";
const PLAYER_UI_SETTING = "jam.playerUi";
const PLAYER_WIDTH = 420;
const PLAYER_MINIMIZED_WIDTH = 280;
const PLAYER_PADDING = 24;
const DEFAULT_PLAYER_Y = 96;
const DEFAULT_JAM_QUERY = "backing track";
const SEARCH_MAX_RESULTS = 12;

let initialized = false;
let searchRequestId = 0;
let initialSearchTriggered = false;

type JamSectionId = "backingTracks" | "scales" | "riffs" | "player";
type BackingTracksTabId = "search" | "favorites";
type LegacyJamState = JamState & { activeSection?: JamSectionId; activeTab?: BackingTracksTabId | "riffs" };
type JamSearchCache = {
  query: string;
  normalizedQuery: string;
  results: JamVideoSummary[];
};

function ensureJamState(): JamState {
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
    jam.activeSection !== "player"
  ) {
    jam.activeSection = legacyActiveTab === "riffs" ? "riffs" : "backingTracks";
  }
  if (jam.activeTab !== "search" && jam.activeTab !== "favorites") {
    jam.activeTab = "search";
  }

  return jam as JamState;
}

function isJamVideoSummary(value: unknown): value is JamVideoSummary {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return typeof record.videoId === "string"
    && typeof record.title === "string"
    && typeof record.channelTitle === "string"
    && typeof record.thumbnailUrl === "string";
}

function getApiKey(): string {
  const value = uiState.appSettings?.[API_KEY_SETTING];
  return typeof value === "string" ? value.trim() : "";
}

function normalizeFavorites(value: unknown): JamVideoSummary[] {
  if (!Array.isArray(value)) {
    return [];
  }
  return value.filter(isJamVideoSummary);
}

function normalizeSearchCache(value: unknown): JamSearchCache | null {
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

function normalizePlayerState(value: unknown, current: JamPlayerState): JamPlayerState {
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

function clampPlayerPosition(state: JamPlayerState): void {
  const width = state.minimized ? PLAYER_MINIMIZED_WIDTH : state.width;
  const maxX = Math.max(PLAYER_PADDING, window.innerWidth - width - PLAYER_PADDING);
  const maxY = Math.max(PLAYER_PADDING, window.innerHeight - 80);
  if (!Number.isFinite(state.x) || state.x <= 0) {
    state.x = maxX;
  }
  state.x = Math.min(Math.max(PLAYER_PADDING, state.x), maxX);
  state.y = Math.min(Math.max(PLAYER_PADDING, state.y), maxY);
}

function getDockHostRect(): DOMRect | null {
  const dockHost = document.getElementById("jam-player-dock");
  if (!(dockHost instanceof HTMLElement)) {
    return null;
  }

  const rect = dockHost.getBoundingClientRect();
  if (rect.width < 1 || rect.height < 1) {
    return null;
  }

  return rect;
}

function setDockHostActive(active: boolean): void {
  const dockHost = document.getElementById("jam-player-dock");
  if (!(dockHost instanceof HTMLElement)) {
    return;
  }

  dockHost.classList.toggle("is-active", active);
  dockHost.setAttribute("aria-hidden", active ? "false" : "true");
}

function persistFavorites(): void {
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

function persistSearchCache(query: string, results: JamVideoSummary[]): void {
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

function getCachedSearch(query: string): JamSearchCache | null {
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

function persistPlayerUi(): void {
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

function isFavorite(videoId: string): boolean {
  return ensureJamState().favorites.some((favorite) => favorite.videoId === videoId);
}

function isBackingTracksFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.Jam);
}

function isRiffLibraryFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.RiffLibrary);
}

function isEarPracticePlayerFeatureEnabled(): boolean {
  return isFeatureEnabled(Features.EarPracticePlayer);
}

function isScalesFeatureEnabled(): boolean {
  return true;
}

function resolveJamSection(preferredSection: JamSectionId): JamSectionId {
  const orderedSections: JamSectionId[] = ["backingTracks", "scales", "riffs", "player"];
  const enabledSections = orderedSections.filter((sectionId) => {
    if (sectionId === "riffs") {
      return isRiffLibraryFeatureEnabled();
    }
    if (sectionId === "scales") {
      return isScalesFeatureEnabled();
    }
    if (sectionId === "player") {
      return isEarPracticePlayerFeatureEnabled();
    }
    return isBackingTracksFeatureEnabled();
  });

  if (enabledSections.includes(preferredSection)) {
    return preferredSection;
  }

  return enabledSections[0] ?? "backingTracks";
}

function resolveBackingTracksTab(preferredTab: BackingTracksTabId): BackingTracksTabId {
  return preferredTab === "favorites" ? "favorites" : "search";
}

function isJamPanelVisible(): boolean {
  return document.getElementById("panel-jam")?.classList.contains("active") ?? false;
}

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

  if (jam.activeSection === "player") {
    renderEarPracticePlayerPanel();
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

function normalizeSearchQuery(rawQuery: string): string {
  const trimmed = rawQuery.trim().replace(/\s+/g, " ");
  if (!trimmed) {
    return "";
  }
  if (/\bbacking\s+track\b/i.test(trimmed)) {
    return trimmed;
  }
  return `${trimmed} backing track`;
}

function getBestThumbnail(snippet: Record<string, unknown> | undefined): string {
  const thumbnails = snippet?.thumbnails as Record<string, { url?: string }> | undefined;
  return thumbnails?.high?.url
    ?? thumbnails?.medium?.url
    ?? thumbnails?.default?.url
    ?? "";
}

function normalizeSearchResults(payload: unknown): JamVideoSummary[] {
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

function describeFetchError(payload: unknown): string {
  if (!payload || typeof payload !== "object") {
    return "Request failed.";
  }

  const error = (payload as { error?: { message?: string } }).error;
  if (typeof error?.message === "string" && error.message.trim()) {
    return error.message.trim();
  }
  return "Request failed.";
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

  const url = new URL("https://www.googleapis.com/youtube/v3/search");
  url.searchParams.set("part", "snippet");
  url.searchParams.set("type", "video");
  url.searchParams.set("videoEmbeddable", "true");
  url.searchParams.set("videoSyndicated", "true");
  url.searchParams.set("maxResults", SEARCH_MAX_RESULTS.toString());
  url.searchParams.set("q", query);
  url.searchParams.set("key", apiKey);

  try {
    const response = await fetch(url.toString());
    const payload = await response.json().catch(() => null);
    if (requestId !== searchRequestId) {
      return;
    }
    if (!response.ok) {
      throw new Error(describeFetchError(payload));
    }

    jam.results = normalizeSearchResults(payload);
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

function toggleFavorite(video: JamVideoSummary): void {
  const jam = ensureJamState();
  const existingIndex = jam.favorites.findIndex((favorite) => favorite.videoId === video.videoId);
  if (existingIndex >= 0) {
    jam.favorites.splice(existingIndex, 1);
  } else {
    jam.favorites = [video, ...jam.favorites.filter((favorite) => favorite.videoId !== video.videoId)];
  }
  persistFavorites();
  renderJamPanel();
}

function openPlayer(video: JamVideoSummary): void {
  const jam = ensureJamState();
  jam.player.open = true;
  jam.player.minimized = false;
  jam.player.currentVideo = video;
  jam.player.width = PLAYER_WIDTH;
  clampPlayerPosition(jam.player);
  persistPlayerUi();
  renderFloatingPlayer();
}

function closePlayer(): void {
  const jam = ensureJamState();
  jam.player.open = false;
  jam.player.currentVideo = null;
  jam.player.minimized = false;
  persistPlayerUi();
  renderFloatingPlayer();
}

function toggleMinimized(): void {
  const jam = ensureJamState();
  jam.player.minimized = !jam.player.minimized;
  clampPlayerPosition(jam.player);
  persistPlayerUi();
  renderFloatingPlayer();
}

function renderSearchStatus(jam: JamState): string {
  if (!isBackingTracksFeatureEnabled()) {
    return '<div class="jam-empty-state">Backing tracks are disabled in Settings &gt; Features.</div>';
  }
  if (jam.loading) {
    return '<div class="jam-empty-state">Searching YouTube for backing tracks…</div>';
  }
  if (jam.error) {
    return `<div class="jam-empty-state jam-empty-state-error">${escapeHtml(jam.error)}</div>`;
  }
  if (jam.activeTab === "favorites" && jam.favorites.length === 0) {
    return '<div class="jam-empty-state">No favourites yet. Star a backing track to pin it here.</div>';
  }
  if (jam.activeTab === "search" && jam.results.length === 0) {
    return '<div class="jam-empty-state">Search for an artist, song, style, or key. "Backing track" is added automatically.</div>';
  }
  return "";
}

function renderResultCard(video: JamVideoSummary): string {
  const favorite = isFavorite(video.videoId);
  const videoId = escapeHtml(video.videoId);
  const title = escapeHtml(video.title);
  const channel = escapeHtml(video.channelTitle);
  const thumbSrc = video.thumbnailUrl ? escapeHtml(video.thumbnailUrl) : "";
  return `
    <article class="jam-result-card" data-video-id="${videoId}">
      <button class="jam-result-art" type="button" data-jam-action="play" data-video-id="${videoId}" aria-label="Play ${title}">
        ${thumbSrc
          ? `<img class="jam-result-thumb" src="${thumbSrc}" alt="" loading="lazy" />`
          : '<div class="jam-result-thumb-fallback"></div>'}
        <span class="jam-result-play-overlay" aria-hidden="true">
          <svg viewBox="0 0 16 16" fill="currentColor" width="20" height="20" aria-hidden="true"><polygon points="4,2 13,8 4,14"/></svg>
        </span>
      </button>
      <div class="jam-result-content">
        <div class="jam-result-title">${title}</div>
        <div class="jam-result-meta">
          <span class="jam-meta-chip">${channel}</span>
        </div>
      </div>
      <div class="jam-result-actions">
        <button class="jam-favorite-toggle${favorite ? " is-active" : ""}" type="button" data-jam-action="favorite" data-video-id="${videoId}" aria-pressed="${favorite ? "true" : "false"}" aria-label="${favorite ? "Remove from favourites" : "Add to favourites"}" title="${favorite ? "Remove from favourites" : "Add to favourites"}">${favorite ? "★" : "☆"}</button>
      </div>
    </article>
  `;
}

export function renderJamPanel(): void {
  const jam = ensureJamState();
  const resolvedSection = resolveJamSection(jam.activeSection);
  const resolvedTab = resolveBackingTracksTab(jam.activeTab);
  const searchResultsHost = document.getElementById("jam-results");
  const favoritesResultsHost = document.getElementById("jam-favorites-results");
  const searchInput = document.getElementById("jam-search-input") as HTMLInputElement | null;
  const scalesSectionButton = document.getElementById("jam-section-scales");
  const riffsSectionButton = document.getElementById("jam-section-riffs");
  const playerSectionButton = document.getElementById("jam-section-player");
  const searchTab = document.getElementById("jam-backing-tab-search");
  const favoritesTab = document.getElementById("jam-backing-tab-favorites");
  const backingTracksPanel = document.getElementById("jam-section-panel-backing-tracks");
  const scalesPanel = document.getElementById("jam-section-panel-scales");
  const riffsPanel = document.getElementById("jam-section-panel-riffs");
  const playerPanel = document.getElementById("jam-section-panel-player");
  const searchPanel = document.getElementById("jam-backing-tab-panel-search");
  const favoritesPanel = document.getElementById("jam-backing-tab-panel-favorites");

  if (!searchResultsHost || !favoritesResultsHost) {
    return;
  }

  jam.activeSection = resolvedSection;
  jam.activeTab = resolvedTab;

  const backingTracksEnabled = isBackingTracksFeatureEnabled();
  const scalesEnabled = isScalesFeatureEnabled();
  const riffLibraryEnabled = isRiffLibraryFeatureEnabled();
  const earPracticePlayerEnabled = isEarPracticePlayerFeatureEnabled();

  if (searchInput && searchInput.value !== jam.query) {
    searchInput.value = jam.query;
  }

  scalesSectionButton?.toggleAttribute("hidden", !scalesEnabled);
  riffsSectionButton?.toggleAttribute("hidden", !riffLibraryEnabled);
  playerSectionButton?.toggleAttribute("hidden", !earPracticePlayerEnabled);
  searchTab?.toggleAttribute("hidden", !backingTracksEnabled);
  favoritesTab?.toggleAttribute("hidden", !backingTracksEnabled);

  scalesSectionButton?.classList.toggle("active", resolvedSection === "scales");
  riffsSectionButton?.classList.toggle("active", resolvedSection === "riffs");
  playerSectionButton?.classList.toggle("active", resolvedSection === "player");
  searchTab?.classList.toggle("active", resolvedSection === "backingTracks" && resolvedTab === "search");
  favoritesTab?.classList.toggle("active", resolvedSection === "backingTracks" && resolvedTab === "favorites");

  backingTracksPanel?.classList.toggle("active", resolvedSection === "backingTracks" && backingTracksEnabled);
  backingTracksPanel?.toggleAttribute("hidden", resolvedSection !== "backingTracks" || !backingTracksEnabled);
  scalesPanel?.classList.toggle("active", resolvedSection === "scales" && scalesEnabled);
  scalesPanel?.toggleAttribute("hidden", resolvedSection !== "scales" || !scalesEnabled);
  riffsPanel?.classList.toggle("active", resolvedSection === "riffs" && riffLibraryEnabled);
  riffsPanel?.toggleAttribute("hidden", resolvedSection !== "riffs" || !riffLibraryEnabled);
  playerPanel?.classList.toggle("active", resolvedSection === "player" && earPracticePlayerEnabled);
  playerPanel?.toggleAttribute("hidden", resolvedSection !== "player" || !earPracticePlayerEnabled);
  searchPanel?.classList.toggle("active", resolvedTab === "search" && backingTracksEnabled);
  searchPanel?.toggleAttribute("hidden", resolvedTab !== "search" || !backingTracksEnabled);
  favoritesPanel?.classList.toggle("active", resolvedTab === "favorites" && backingTracksEnabled);
  favoritesPanel?.toggleAttribute("hidden", resolvedTab !== "favorites" || !backingTracksEnabled);

  if (resolvedSection === "riffs") {
    renderRiffLibraryPanel();
    return;
  }

  if (resolvedSection === "player") {
    renderEarPracticePlayerPanel();
    return;
  }

  if (resolvedSection === "scales") {
    return;
  }

  const source = resolvedTab === "favorites" ? jam.favorites : jam.results;
  const status = renderSearchStatus(jam);
  const targetHost = resolvedTab === "favorites" ? favoritesResultsHost : searchResultsHost;
  targetHost.innerHTML = status || source.map(renderResultCard).join("");
}

function bindFloatingPlayerDrag(panel: HTMLElement, handle: HTMLElement): void {
  if (handle.dataset.dragBound === "true") {
    return;
  }
  handle.dataset.dragBound = "true";
  const jam = ensureJamState();

  // Handle mouse dragging
  handle.addEventListener("mousedown", (event) => {
    if (jam.player.minimized) {
      return;
    }
    if ((event.target as HTMLElement).closest("button")) {
      return;
    }
    event.preventDefault();
    const startX = event.clientX;
    const startY = event.clientY;
    const initialX = jam.player.x;
    const initialY = jam.player.y;

    const onMove = (moveEvent: MouseEvent) => {
      jam.player.x = initialX + (moveEvent.clientX - startX);
      jam.player.y = initialY + (moveEvent.clientY - startY);
      clampPlayerPosition(jam.player);
      panel.style.left = `${jam.player.x}px`;
      panel.style.top = `${jam.player.y}px`;
    };

    const onUp = () => {
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onUp);
      persistPlayerUi();
    };

    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
  });

  // Handle touch dragging
  handle.addEventListener("touchstart", (event) => {
    if (jam.player.minimized) {
      return;
    }
    if ((event.target as HTMLElement).closest("button")) {
      return;
    }
    const touch = event.touches[0];
    if (!touch) return;
    
    event.preventDefault();
    const startX = touch.clientX;
    const startY = touch.clientY;
    const initialX = jam.player.x;
    const initialY = jam.player.y;

    const onMove = (moveEvent: TouchEvent) => {
      const touch = moveEvent.touches[0];
      if (!touch) return;
      jam.player.x = initialX + (touch.clientX - startX);
      jam.player.y = initialY + (touch.clientY - startY);
      clampPlayerPosition(jam.player);
      panel.style.left = `${jam.player.x}px`;
      panel.style.top = `${jam.player.y}px`;
    };

    const onEnd = () => {
      window.removeEventListener("touchmove", onMove);
      window.removeEventListener("touchend", onEnd);
      persistPlayerUi();
    };

    window.addEventListener("touchmove", onMove, { passive: false });
    window.addEventListener("touchend", onEnd);
  }, { passive: false });
}

export function renderFloatingPlayer(): void {
  const jam = ensureJamState();
  const root = document.getElementById("jam-floating-player-root");
  if (!root) {
    return;
  }

  if (!jam.player.open || !jam.player.currentVideo) {
    setDockHostActive(false);
    root.innerHTML = "";
    return;
  }

  clampPlayerPosition(jam.player);

  const dockActive = jam.player.minimized;
  setDockHostActive(dockActive);
  const dockRect = jam.player.minimized ? getDockHostRect() : null;
  const rawWidth = jam.player.minimized ? Math.round(dockRect?.width ?? PLAYER_MINIMIZED_WIDTH) : jam.player.width;
  const width = Math.max(160, Math.min(rawWidth, window.innerWidth - PLAYER_PADDING * 2));
  const video = jam.player.currentVideo;
  // Embed via a first-party wrapper page served from our API origin. The app
  // WebView has an opaque origin on WebKit (macOS/Linux); YouTube refuses to
  // initialise when its immediate parent frame has such an origin. The wrapper
  // gives the YouTube iframe a valid https parent origin so the player works.
  const src = `${getApiBaseUrl()}/embed/youtube?v=${encodeURIComponent(video.videoId)}`;

  let panel = root.querySelector<HTMLElement>(".jam-floating-player");
  const currentVideoId = panel?.dataset.videoId ?? "";
  if (!panel || currentVideoId !== video.videoId) {
    root.innerHTML = `
      <div class="jam-floating-player" data-video-id="${escapeHtml(video.videoId)}">
        <div class="jam-floating-player-header" id="jam-floating-player-header">
          <div class="jam-floating-player-meta">
            <div class="jam-floating-player-title" id="jam-floating-player-title"></div>
            <div class="jam-floating-player-channel" id="jam-floating-player-channel"></div>
          </div>
          <div class="jam-floating-player-actions">
            <button type="button" id="jam-player-minimize"></button>
            <button type="button" id="jam-player-close" aria-label="Close player" title="Close player">${getXMarkSvg()}</button>
          </div>
        </div>
        <div class="jam-floating-player-frame">
          <iframe
            id="jam-player-iframe"
            src="${src}"
            title="${escapeHtml(video.title)}"
            allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
            sandbox="allow-scripts allow-same-origin allow-presentation"
            referrerpolicy="strict-origin-when-cross-origin"
            allowfullscreen>
          </iframe>
        </div>
      </div>
    `;
    panel = root.querySelector<HTMLElement>(".jam-floating-player") ?? null;
  }

  const header = document.getElementById("jam-floating-player-header");
  const title = document.getElementById("jam-floating-player-title");
  const channel = document.getElementById("jam-floating-player-channel");
  const minimizeButton = document.getElementById("jam-player-minimize") as HTMLButtonElement | null;
  const closeButton = document.getElementById("jam-player-close") as HTMLButtonElement | null;
  const iframe = document.getElementById("jam-player-iframe") as HTMLIFrameElement | null;

  if (panel) {
    panel.dataset.videoId = video.videoId;
    panel.classList.toggle("is-minimized", jam.player.minimized);
    panel.classList.toggle("is-docked", Boolean(dockRect));
    if (dockRect) {
      // Keep the docked bar fully on-screen so its restore/close icons stay
      // reachable even when the header is tight and the dock host overflows.
      const maxLeft = Math.max(PLAYER_PADDING, window.innerWidth - width - PLAYER_PADDING);
      const dockedLeft = Math.min(Math.max(PLAYER_PADDING, Math.round(dockRect.left)), maxLeft);
      panel.style.left = `${dockedLeft}px`;
      panel.style.top = `${Math.round(dockRect.top)}px`;
    } else {
      panel.style.left = `${jam.player.x}px`;
      panel.style.top = `${jam.player.y}px`;
    }
    panel.style.width = `${width}px`;
  }
  if (title) {
    title.textContent = video.title;
    title.title = video.title;
  }
  if (channel) {
    channel.textContent = video.channelTitle;
    channel.title = video.channelTitle;
  }
  if (iframe) {
    iframe.title = video.title;
    if (!iframe.src || iframe.src !== src) {
      iframe.src = src;
    }
  }
  if (minimizeButton) {
    minimizeButton.textContent = jam.player.minimized ? "▢" : "—";
    minimizeButton.setAttribute("aria-label", jam.player.minimized ? "Restore player" : "Minimize player");
    minimizeButton.title = jam.player.minimized ? "Restore player" : "Minimize player";
    minimizeButton.onclick = () => toggleMinimized();
  }
  if (closeButton) {
    closeButton.onclick = () => closePlayer();
  }
  if (panel && header) {
    bindFloatingPlayerDrag(panel, header);
  }
}

function findVideoById(videoId: string): JamVideoSummary | undefined {
  const jam = ensureJamState();
  return jam.results.find((video) => video.videoId === videoId)
    ?? jam.favorites.find((video) => video.videoId === videoId);
}

function bindPanelActions(): void {
  const searchInput = document.getElementById("jam-search-input") as HTMLInputElement | null;
  const searchButton = document.getElementById("jam-search-button");
  const resultsHosts = [document.getElementById("jam-results"), document.getElementById("jam-favorites-results")];

  document.getElementById("jam-section-scales")?.addEventListener("click", () => setActiveSection("scales"));
  document.getElementById("jam-section-riffs")?.addEventListener("click", () => setActiveSection("riffs"));
  document.getElementById("jam-section-player")?.addEventListener("click", () => setActiveSection("player"));
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