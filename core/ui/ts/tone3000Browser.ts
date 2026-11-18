import { uiState } from "./state.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { postMessage } from "./bridge.js";
import {
  ensureTone3000Session,
  isTone3000AuthReady,
  isTone3000ByokEnabled,
  tone3000AuthenticatedFetch,
} from "./tone3000.js";
import { escapeHtml } from "./utils.js";
import { openBlendEditorWithDefinition } from "./signalPathBlend.js";
import { Tone3000DetailsView } from "./tone3000DetailsView.js";
import {
  createTone3000BlendDefinition,
  backfillTone3000ResourceImages,
  fetchTone3000Models,
  getTone3000ImageUrl,
  importTone3000Models,
} from "./tone3000Shared.js";
import type { Tone3000Gear, Tone3000Model, Tone3000Platform, Tone3000Tone } from "./tone3000ApiTypes.js";
import {
  buildTone3000FavoritesUrl,
  buildTone3000SearchUrl,
  extractTone3000Tones,
  parseTone3000Pagination,
} from "./tone3000Api.js";
import {
  describeTone3000ResultsAge,
  getCachedTone3000Results,
  isTone3000ResultsFresh,
  loadLastTone3000Results,
  rememberLastTone3000Results,
  storeTone3000Results,
} from "./tone3000BrowserCache.js";
import type { Tone3000CachedResults, Tone3000ResultsRequest } from "./tone3000BrowserCache.js";

const PAGE_SIZE = 25;

const categoryListEl = document.getElementById("tone3000-category-list");
const resultsEl = document.getElementById("tone3000-results");
const searchControlsEl = document.getElementById("tone3000-search-controls");
const searchInputEl = document.getElementById("tone3000-search-input") as HTMLInputElement | null;
const sortSelectEl = document.getElementById("tone3000-sort-select") as HTMLSelectElement | null;
const searchButtonEl = document.getElementById("tone3000-search-button");
const searchModeTabEl = document.getElementById("tone3000-mode-search-tab") as HTMLButtonElement | null;
const favoritesModeTabEl = document.getElementById("tone3000-mode-favorites-tab") as HTMLButtonElement | null;
const paginationEl = document.getElementById("tone3000-pagination");
const prevButtonEl = document.getElementById("tone3000-prev-btn") as HTMLButtonElement | null;
const nextButtonEl = document.getElementById("tone3000-next-btn") as HTMLButtonElement | null;
const pageLabelEl = document.getElementById("tone3000-page-label");
const resultsAgeEl = document.getElementById("tone3000-results-age");
const refreshButtonEl = document.getElementById("tone3000-refresh-btn") as HTMLButtonElement | null;

interface CategoryConfig {
  id: string;
  label: string;
  gear?: Tone3000Gear;
  platform?: Tone3000Platform;
}

const CATEGORIES: CategoryConfig[] = [
  { id: "pedal", label: "Pedals (FX)", gear: "pedal", platform: "nam" },
  { id: "preamp", label: "Preamps", gear: "outboard", platform: "nam" },
  { id: "amp", label: "Amps", gear: "amp", platform: "nam" },
  { id: "full-rig", label: "Full Rigs", gear: "full-rig", platform: "nam" },
  { id: "cab", label: "Cab IRs", gear: "cab", platform: "ir" },
  { id: "space", label: "Room & Reverb IRs", gear: "space", platform: "ir" },
];

let activeCategory = CATEGORIES[0];
let activeQuery = "";
let currentTones: Tone3000Tone[] = [];
let currentPage = 1;
let totalPages = 1;
let detailsView: Tone3000DetailsView | null = null;
let favoritesMode = false;
/** The results on screen and when they were fetched; null while there are none (loading, an error). */
let shownResults: Tone3000CachedResults | null = null;

function canUseFavoritesMode(): boolean {
  return isTone3000ByokEnabled();
}

function syncBrowseModeUi(): void {
  const enabled = canUseFavoritesMode();
  if (favoritesModeTabEl) {
    favoritesModeTabEl.classList.toggle("active", favoritesMode);
    favoritesModeTabEl.setAttribute("aria-pressed", favoritesMode ? "true" : "false");
    favoritesModeTabEl.title = enabled
      ? "Show your Tone3000 favourites"
      : "Favourites require your own Tone3000 API key in Settings";
  }

  if (searchModeTabEl) {
    searchModeTabEl.classList.toggle("active", !favoritesMode);
    searchModeTabEl.setAttribute("aria-pressed", !favoritesMode ? "true" : "false");
  }

  if (searchControlsEl) {
    searchControlsEl.style.display = favoritesMode ? "none" : "";
  }

  if (categoryListEl) {
    categoryListEl.style.display = favoritesMode ? "none" : "";
  }
}

function renderFavoritesPrompt(): string {
  return `<div class="tone3000-empty">To view your favourites, add your own Tone3000 API key under Settings.</div>`;
}

function getToneImportStatus(tone: Tone3000Tone): { status: "imported" | "partial" | "none"; importedCount: number } {
  const toneId = String(tone.id);
  const seen = new Set<string>();
  const resourceTypes = ["nam", "ir"];

  resourceTypes.forEach((type) => {
    const resources = uiState.resourceLibrary[type] || [];
    resources.forEach((resource) => {
      if (resource.fileMissing) {
        return;
      }
      const metadata = resource.metadata || {};
      const resourceToneId = metadata.toneId || metadata.groupId;
      if (resourceToneId && String(resourceToneId) === toneId) {
        const modelKey = type === "ir"
          ? metadata.entryName || metadata.modelId || resource.id
          : metadata.modelId || resource.id;
        if (modelKey) {
          seen.add(String(modelKey));
        }
      }
    });
  });

  const importedCount = seen.size;
  const modelCount = tone.models_count ?? 0;
  if (modelCount > 0 && importedCount >= modelCount) {
    return { status: "imported", importedCount };
  }
  if (importedCount > 0) {
    return { status: "partial", importedCount };
  }
  return { status: "none", importedCount };
}

function formatCompactCount(value: number | null | undefined): string {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) {
    return "0";
  }

  const whole = Math.floor(value);
  if (whole >= 1_000_000) {
    const scaled = whole / 1_000_000;
    return `${(scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1)).replace(/\.0$/, "")}M`;
  }
  if (whole >= 1_000) {
    const scaled = whole / 1_000;
    return `${(scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1)).replace(/\.0$/, "")}K`;
  }
  return String(whole);
}

function getToneInitials(label: string): string {
  const normalized = label.trim().replace(/\s+/g, " ");
  if (!normalized) {
    return "T3";
  }
  const parts = normalized.split(" ").filter(Boolean);
  if (parts.length >= 2) {
    return `${parts[0][0]}${parts[1][0]}`.toUpperCase();
  }
  const compact = parts[0].replace(/[^A-Za-z0-9]/g, "").slice(0, 2).toUpperCase();
  return compact || "T3";
}

/**
 * Navigate the tone3000 browser to a specific query and optional category,
 * called externally (e.g. from the AI Tone Search panel).
 */
export function setTone3000Search(query: string, categoryId?: string): void {
  if (favoritesMode) {
    favoritesMode = false;
    syncBrowseModeUi();
  }

  if (categoryId) {
    const next = CATEGORIES.find((cat) => cat.id === categoryId);
    if (next) {
      activeCategory = next;
      renderCategories();
    }
  }
  if (searchInputEl) {
    searchInputEl.value = query;
  }
  void runSearch(1, { refresh: true });
}

/**
 * Puts the browser back on the results it showed last time, if they are under an hour old, so
 * that opening the tab draws them from the cache. Older than that, it opens on the default
 * search.
 */
function restoreLastView(): void {
  const last = loadLastTone3000Results();
  if (!last) {
    return;
  }
  if (last.request.mode === "favorites") {
    if (!canUseFavoritesMode()) {
      return;
    }
    favoritesMode = true;
  } else {
    activeCategory = CATEGORIES.find((category) => category.id === last.request.categoryId) ?? activeCategory;
    if (searchInputEl) {
      searchInputEl.value = last.request.query;
    }
    if (sortSelectEl && Array.from(sortSelectEl.options).some((option) => option.value === last.request.sort)) {
      sortSelectEl.value = last.request.sort;
    }
  }
  currentPage = last.request.page;
}

/**
 * Called when the browser's tab comes back on screen. Fetches only if what is showing has
 * gone stale (over an hour old), or if the last search had no way to reach Tone3000 and there
 * may be one now. Results still within the hour are left as they are.
 */
export function refreshTone3000BrowserOnShow(): void {
  if (waitingForAccess) {
    retryTone3000BrowserAccess(true);
    return;
  }
  if (shownResults && !isTone3000ResultsFresh(shownResults)) {
    void runSearch(currentPage);
    return;
  }
  renderResultsAge();
}

function renderResultsAge(): void {
  if (resultsAgeEl) {
    resultsAgeEl.textContent = shownResults ? `Updated ${describeTone3000ResultsAge(shownResults.fetchedAt)}` : "";
  }
}

export function initTone3000Browser(): void {
  if (!detailsView) {
    detailsView = new Tone3000DetailsView({
      // Results shown from the cache have not started a session; the first thing that needs
      // Tone3000 does.
      fetchModels: async (tone) => {
        await ensureTone3000Session();
        return fetchTone3000Models(tone);
      },
      onImport: async (tone, models, createBlend) => {
        const imported = await importTone3000Models(tone, models);
        if (createBlend) {
          const blendIds = imported.importedNamIds.length ? Array.from(new Set(imported.importedNamIds)) : [];
          if (!blendIds.length) {
            showNotification("Blend creation failed", "No NAM models were imported.");
            return;
          }
          const blend = createTone3000BlendDefinition(tone, blendIds, imported.importedNamNames);
          postMessage({
            type: "saveBlendDefinition",
            blend,
          });
          openBlendEditorWithDefinition(blend);
        }
      },
    });
  }

  renderCategories();
  if (resultsEl) {
    resultsEl.addEventListener("click", (event) => {
      const target = event.target as HTMLElement | null;
      if (!target) return;
      const detailsButton = target.closest(".tone3000-details-btn") as HTMLButtonElement | null;
      if (detailsButton) {
        const toneId = detailsButton.dataset.toneId;
        const tone = currentTones.find((item) => String(item.id) === toneId);
        if (!tone) {
          showNotification("Details unavailable", "Tone not found");
          return;
        }
        detailsView?.open(tone, {
          allowMultipleSelection: true,
          allowBlendCreation: true,
        });
        return;
      }
      const button = target.closest(".tone3000-import-btn") as HTMLButtonElement | null;
      if (!button) return;

      const toneId = button.dataset.toneId;
      const tone = currentTones.find((item) => String(item.id) === toneId);
      if (!tone) {
        showNotification("Import failed", "Tone not found");
        return;
      }

      showNotification("Import started", tone.title ?? "Tone3000");
      void importToneModels(button, tone);
    });
  }
  // Search, Enter in the field and Refresh ask Tone3000 again. Everything else (a category,
  // the sort, a page, the mode) reuses results fetched within the hour.
  searchButtonEl?.addEventListener("click", () => void runSearch(1, { refresh: true }));
  refreshButtonEl?.addEventListener("click", () => void runSearch(currentPage, { refresh: true }));
  searchModeTabEl?.addEventListener("click", () => {
    if (favoritesMode) {
      favoritesMode = false;
      syncBrowseModeUi();
      void runSearch(1);
    }
  });
  favoritesModeTabEl?.addEventListener("click", () => {
    if (!favoritesMode) {
      favoritesMode = true;
      syncBrowseModeUi();
      void runSearch(1);
    }
  });
  searchInputEl?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      void runSearch(1, { refresh: true });
    }
  });
  sortSelectEl?.addEventListener("change", () => void runSearch());

  prevButtonEl?.addEventListener("click", () => {
    if (currentPage > 1) {
      void runSearch(currentPage - 1);
    }
  });

  nextButtonEl?.addEventListener("click", () => {
    if (currentPage < totalPages) {
      void runSearch(currentPage + 1);
    }
  });

  restoreLastView();
  renderCategories();
  renderResultsAge();
  void runSearch(currentPage);
}

function renderCategories(): void {
  if (!categoryListEl) return;
  categoryListEl.innerHTML = CATEGORIES.map((category) => {
    const activeClass = category.id === activeCategory.id ? "active" : "";
    return `
      <button class="tone3000-category ${activeClass}" data-category="${escapeHtml(category.id)}">
        <span>${category.label}</span>
      </button>
    `;
  }).join("");

  categoryListEl.querySelectorAll(".tone3000-category").forEach((button) => {
    button.addEventListener("click", () => {
      const id = (button as HTMLElement).dataset.category;
      const next = CATEGORIES.find((cat) => cat.id === id);
      if (!next) return;
      activeCategory = next;
      renderCategories();
      void runSearch();
    });
  });
}

/** Bumped by every search, so a reply that is not the latest search's is dropped. */
let searchSequence = 0;

/** The last search stopped at "Add a Tone3000 API key": there was no way to reach Tone3000 yet. */
let waitingForAccess = false;

/**
 * Searches again if the last search had no access and there is some now. The browser can be
 * shown before the app settings that say how to reach Tone3000 have arrived (a saved view
 * that opens on this tab), and nothing else would take it off that message.
 *
 * `startSession` also tries when there is a key but no session yet, which costs a request to
 * Tone3000: for when the tab is shown again, not for every settings refresh.
 */
export function retryTone3000BrowserAccess(startSession = false): void {
  if (waitingForAccess && (isTone3000AuthReady() || (startSession && isTone3000ByokEnabled()))) {
    void runSearch();
  }
}

/** Draws a page of results, fetched just now or taken from the cache, and makes it the one to reopen on. */
function showResults(results: Tone3000CachedResults): void {
  shownResults = results;
  currentPage = results.page;
  totalPages = results.totalPages;
  renderPagination(false);
  renderResults(results.tones);
  renderResultsAge();
  rememberLastTone3000Results(results);
}

/**
 * Shows the results for the browser's current mode, category, search and sort.
 *
 * Results fetched within the hour are shown from the cache and nothing is sent. `refresh`
 * asks Tone3000 regardless: it is what Search and Refresh do.
 */
async function runSearch(page = 1, options: { refresh?: boolean } = {}): Promise<void> {
  if (!resultsEl) return;

  // Searches overlap: change the category, the sort or the page while one is in flight and the
  // older reply could land last and overwrite the newer, filtered by the category current then.
  const sequence = ++searchSequence;
  const isStale = (): boolean => sequence !== searchSequence;
  const category = activeCategory;

  syncBrowseModeUi();

  const useFavoritesMode = favoritesMode;
  if (useFavoritesMode && !canUseFavoritesMode()) {
    shownResults = null;
    resultsEl.innerHTML = renderFavoritesPrompt();
    updatePagination(false);
    renderResultsAge();
    return;
  }

  activeQuery = useFavoritesMode ? "" : (searchInputEl?.value.trim() ?? "");
  currentPage = page;

  const request: Tone3000ResultsRequest = {
    mode: useFavoritesMode ? "favorites" : "search",
    categoryId: category.id,
    query: activeQuery,
    sort: sortSelectEl?.value ?? "popular",
    page,
  };

  // Before the session: in key mode starting one is a request to Tone3000 too.
  if (!options.refresh) {
    const cached = getCachedTone3000Results(request);
    if (cached) {
      waitingForAccess = false;
      showResults(cached);
      return;
    }
  }

  await ensureTone3000Session();

  if (isStale()) return;

  waitingForAccess = !isTone3000AuthReady();
  if (waitingForAccess) {
    shownResults = null;
    resultsEl.innerHTML = `<div class="tone3000-empty">Add a Tone3000 API key to browse models.</div>`;
    renderResultsAge();
    return;
  }

  shownResults = null;
  resultsEl.innerHTML = `<div class="tone3000-empty">Loading...</div>`;
  updatePagination(true);
  renderResultsAge();

  try {
    const params = new URLSearchParams({
      page: String(page),
      page_size: PAGE_SIZE.toString(),
    });
    if (!useFavoritesMode) {
      if (activeQuery) {
        params.set("query", activeQuery);
      }
      if (category.gear) {
        params.set("gear", category.gear);
      }
      const sortValue = request.sort;
      if (sortValue === "popular") {
        params.set("sort", "downloads-all-time");
      } else if (sortValue === "recent") {
        params.set("sort", "newest");
      } else if (sortValue === "trending") {
        params.set("sort", "trending");
      } else if (sortValue === "name") {
        params.set("sort", "best-match");
      }
    }

    const endpoint = useFavoritesMode ? buildTone3000FavoritesUrl(params) : buildTone3000SearchUrl(params);
    const response = await tone3000AuthenticatedFetch(endpoint);

    if (isStale()) return;

    if (!response.ok) {
      if (useFavoritesMode && (response.status === 401 || response.status === 403 || response.status === 404)) {
        throw new Error("Favourites are only available in BYOK direct API mode.");
      }
      throw new Error(`Search failed: ${response.status}`);
    }

    const data = await response.json();

    if (isStale()) return;

    const tones = extractTone3000Tones(data);
    backfillTone3000ResourceImages(tones);

    const filtered = useFavoritesMode
      ? tones
      : category.platform
      ? tones.filter((tone) => {
          const platform = (tone.platform ?? "").toLowerCase();
          const platforms = (tone as { platforms?: string[] }).platforms;
          if (!platform && !Array.isArray(platforms)) {
            return true;
          }
          if (platform === category.platform) {
            return true;
          }
          return Array.isArray(platforms)
            ? platforms.some((value) => value.toLowerCase() === category.platform)
            : false;
        })
      : tones;

    updatePagination(false, data);
    const results: Tone3000CachedResults = { request, tones: filtered, page: currentPage, totalPages, fetchedAt: Date.now() };
    storeTone3000Results(results);
    showResults(results);
  } catch (error) {
    if (isStale()) return;

    const message = error instanceof Error ? error.message : String(error);
    resultsEl.innerHTML = `<div class="tone3000-empty">${escapeHtml(message)}</div>`;
    updatePagination(false);
  }
}

/** Takes the page and page count from a reply (or, with none, leaves them at the current page) and draws them. */
function updatePagination(loading: boolean, data?: Record<string, unknown>): void {
  const parsed = parseTone3000Pagination(data, currentPage, PAGE_SIZE);
  currentPage = parsed.page;
  totalPages = parsed.total ? parsed.totalPages : currentPage;
  renderPagination(loading);
}

function renderPagination(loading: boolean): void {
  if (!paginationEl || !pageLabelEl || !prevButtonEl || !nextButtonEl) {
    return;
  }

  // The page label alone dims, not the row: the results' age and Refresh sit in it too.
  pageLabelEl.style.opacity = loading ? "0.6" : "1";

  pageLabelEl.textContent = `Page ${currentPage}${totalPages ? ` of ${totalPages}` : ""}`;
  prevButtonEl.disabled = loading || currentPage <= 1;
  nextButtonEl.disabled = loading || (totalPages ? currentPage >= totalPages : false);
}

function renderResults(tones: Tone3000Tone[]): void {
  if (!resultsEl) return;

  currentTones = tones;

  if (!tones.length) {
    const emptyLabel = favoritesMode
      ? "No favourite tones found. Favourite them on Tone3000 first, then refresh."
      : "No tones found in this category.";
    resultsEl.innerHTML = `<div class="tone3000-empty">${escapeHtml(emptyLabel)}</div>`;
    return;
  }

  resultsEl.innerHTML = tones
    .map((tone) => {
      const toneId = String(tone.id);
      const modelCount = tone.models_count ?? 0;
      const equipmentImageUrl = getEquipmentImageUrl(tone);
      const importStatus = getToneImportStatus(tone);
      const statusLabel =
        importStatus.status === "imported"
          ? "Imported"
          : importStatus.status === "partial"
            ? "Partial"
            : "";
      const statusBadge = statusLabel ? `<span class="tone3000-status">${escapeHtml(statusLabel)}</span>` : "";
      const buttonLabel = importStatus.status === "none" ? "Import" : "Re-import";
      const safeTitle = escapeHtml(tone.title?.trim() || "Untitled Tone");
      const safeGear = escapeHtml(tone.gear ?? "tone");
      const safePlatform = escapeHtml((tone.platform ?? "unknown").toUpperCase());
      const creatorRaw = (tone.user?.username ?? "community").replace(/^@+/, "");
      const safeCreator = escapeHtml(creatorRaw || "community");
      const imageMarkup = equipmentImageUrl
        ? `
          <div class="tone3000-item-hero tone3000-item-hero--image">
            <img src="${escapeHtml(equipmentImageUrl)}" alt="${escapeHtml(tone.gear ?? "Equipment")}" loading="lazy" />
          </div>
        `
        : `
          <div class="tone3000-item-hero tone3000-item-hero--placeholder">
            <span class="tone3000-item-hero-initials">${escapeHtml(getToneInitials(tone.title ?? "Tone"))}</span>
          </div>
        `;

      return `
        <div class="results-item tone3000-item" data-tone-id="${escapeHtml(toneId)}">
          ${imageMarkup}
          <div class="results-item-main tone3000-item-main">
            <div class="tone3000-item-header">
              <div class="results-item-title tone3000-item-title">${safeTitle}</div>
              ${statusBadge}
            </div>
            <div class="results-item-meta tone3000-item-meta">
              <span class="tone3000-meta-chip">${safeGear}</span>
              <span class="tone3000-meta-chip">${safePlatform}</span>
              <span class="tone3000-meta-chip">@${safeCreator}</span>
            </div>
            <div class="tone3000-item-stats">
              <span><strong>${formatCompactCount(modelCount)}</strong> models</span>
              <span><strong>${formatCompactCount(tone.downloads_count ?? 0)}</strong> downloads</span>
            </div>
          </div>
          <div class="results-item-actions tone3000-item-actions">
            <button class="tone3000-details-btn" data-tone-id="${escapeHtml(toneId)}" type="button">Details</button>
            <button class="tone3000-import-btn" data-tone-id="${escapeHtml(toneId)}" type="button">${buttonLabel}</button>
          </div>
        </div>
      `;
    })
    .join("");

}

function getEquipmentImageUrl(tone: Tone3000Tone): string | null {
  return getTone3000ImageUrl(tone);
}

async function fetchToneModels(tone: Tone3000Tone): Promise<Tone3000Model[]> {
  return fetchTone3000Models(tone);
}

async function importToneModels(button: HTMLButtonElement, tone: Tone3000Tone): Promise<void> {
  // Results shown from the cache have not started a session yet.
  await ensureTone3000Session();
  if (!isTone3000AuthReady()) {
    showNotification("Tone3000 session missing");
    return;
  }

  button.disabled = true;
  button.textContent = "Importing...";

  try {
    const models = await fetchToneModels(tone);
    if (!models.length) {
      throw new Error("No models found for tone");
    }

    await importTone3000Models(tone, models, (completed, total) => {
      button.textContent = `Importing ${completed}/${total}...`;
    });

    appendLog(`tone3000 import complete (${tone.title})`);
    showNotification("Import complete", tone.title ?? "Tone3000");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    appendLog(`tone3000 import failed: ${message}`);
    showNotification("Import failed", message);
  } finally {
    button.disabled = false;
    renderResults(currentTones);
  }
}
