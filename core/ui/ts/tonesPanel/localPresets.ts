/**
 * Tones → Your Presets: the presets on this machine as a full-size grid of cards, with the
 * folders down the side.
 *
 * It is a second view of the preset library, not a second library: the same filter rules as
 * the preset popover (presets/filter.ts), the same favourites and ratings, and a card loads
 * its preset the way a popover row does. What it keeps for itself is where it is looking —
 * its folder, search, tag and sort — so browsing here does not move the popover.
 */

import { createDeferredView } from "../deferredView.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "../featureFlags.js";
import { getPresetRating, isPresetFavorite, setPresetRating, toggleFavoritePreset } from "../presets/favorites.js";
import { getFilteredPresets } from "../presets/filter.js";
import { findFolderById, getPresetFolderPath, isVirtualPresetFolderId, sortPresetFoldersAlphabetically } from "../presets/folders.js";
import { applyPresetFromLibrary } from "../presets/load.js";
import { onPresetUIRendered } from "../presets/refresh.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { uiState } from "../state.js";
import type { Preset } from "../types.js";
import { escapeHtml, idAccentColor } from "../utils.js";
import { collectPresetTags, describePresetCount, flattenPresetFolders, isLocalPresetSort, presetInitials, sortLocalPresets } from "./localPresetsModel.js";
import type { LocalPresetSort } from "./localPresetsModel.js";

const NO_TAGS: ReadonlySet<string> = new Set<string>();

let folderId: string = PRESET_FOLDER_ALL_ID;
let query = "";
let tag = "";
let sort: LocalPresetSort = "name";

function byId<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function isLocalPresetsShown(): boolean {
  return Boolean(
    byId("panel-sharing")?.classList.contains("active")
    && byId("tones-tab-local")?.classList.contains("active"),
  );
}

const localPresetsView = createDeferredView(isLocalPresetsShown, renderLocalPresets);

/** A folder deleted while this tab was looking at it falls back to All Presets. */
function resolveFolderId(): string {
  if (!isVirtualPresetFolderId(folderId) && !findFolderById(uiState.presetFolders ?? [], folderId)) {
    folderId = PRESET_FOLDER_ALL_ID;
  }
  return folderId;
}

function presetsInFolder(id: string): Preset[] {
  return getFilteredPresets("", { folderId: id, tags: NO_TAGS });
}

function renderFolders(activeFolderId: string): void {
  const nav = byId("tones-local-folders");
  if (!nav) {
    return;
  }

  const rows = [
    { id: PRESET_FOLDER_RECENTS_ID, name: "Recents", depth: 0 },
    { id: PRESET_FOLDER_FAVORITES_ID, name: "Favourites", depth: 0 },
    { id: PRESET_FOLDER_ALL_ID, name: "All Presets", depth: 0 },
    ...flattenPresetFolders(sortPresetFoldersAlphabetically(uiState.presetFolders ?? [])),
  ];

  nav.innerHTML = rows
    .map((row) => {
      const active = row.id === activeFolderId;
      return `
        <button type="button" class="tone-sharing-nav-btn tab-btn tab-btn-vertical tones-local-folder${active ? " active" : ""}" data-folder-id="${escapeHtml(row.id)}" data-depth="${row.depth}" aria-pressed="${active ? "true" : "false"}">
          <span class="tones-local-folder-name">${escapeHtml(row.name)}</span>
          <span class="tones-local-folder-count">${presetsInFolder(row.id).length}</span>
        </button>
      `;
    })
    .join("");

  // Nested folders step in. Set here rather than in the markup: a style attribute is one more
  // thing to escape.
  nav.querySelectorAll<HTMLElement>(".tones-local-folder").forEach((button) => {
    const depth = Number(button.dataset.depth ?? 0);
    if (depth > 0) {
      button.style.paddingInlineStart = `${12 + depth * 14}px`;
    }
  });
}

function renderTagOptions(tags: string[]): void {
  const select = byId<HTMLSelectElement>("tones-local-tag");
  if (!select) {
    return;
  }
  // A tag no preset in this folder carries any more stops filtering.
  if (tag && !tags.includes(tag)) {
    tag = "";
  }
  select.innerHTML = [
    `<option value="">All tags</option>`,
    ...tags.map((entry) => `<option value="${escapeHtml(entry)}">${escapeHtml(entry)}</option>`),
  ].join("");
  select.value = tag;
  // Nothing to filter by when no preset here has a tag.
  select.hidden = tags.length === 0;
}

function renderCard(preset: Preset): string {
  const id = escapeHtml(preset.id);
  const name = preset.name?.trim() || "Untitled preset";
  const isActive = preset.id === uiState.activePresetId;
  const isLoading = preset.id === uiState.presetLoadingId;
  const favorite = isPresetFavorite(preset.id);
  const rating = getPresetRating(preset.id);
  const folderPath = getPresetFolderPath(preset.id) ?? "";
  const description = preset.description?.trim() ?? "";
  const tags = preset.tags ?? [];

  const stars = Array.from({ length: 5 }, (_, index) => {
    const value = index + 1;
    const lit = rating !== null && value <= rating;
    return `<button class="preset-rating-star${lit ? " active" : ""}" type="button" data-rating="${value}" aria-label="Rate ${value} of 5" aria-pressed="${lit ? "true" : "false"}">★</button>`;
  }).join("");

  const loadLabel = isLoading ? "Loading…" : isActive ? "Loaded" : "Load";

  return `
    <article class="tones-local-card${isActive ? " active" : ""}${isLoading ? " loading" : ""}" data-preset-id="${id}">
      <div class="tones-local-card-art">
        <span class="tones-local-card-category">${escapeHtml(preset.category ?? "")}</span>
        <span class="tones-local-card-initials" aria-hidden="true">${escapeHtml(presetInitials(name))}</span>
      </div>
      <div class="tones-local-card-body">
        <div class="tones-local-card-header">
          <h3 class="tones-local-card-title">${escapeHtml(name)}</h3>
          <button class="tones-local-card-fav${favorite ? " active" : ""}" type="button" data-action="favorite" aria-pressed="${favorite ? "true" : "false"}" title="${favorite ? "Remove from favourites" : "Add to favourites"}" aria-label="${favorite ? "Remove from favourites" : "Add to favourites"}">${favorite ? "♥" : "♡"}</button>
        </div>
        ${description ? `<p class="tones-local-card-description">${escapeHtml(description)}</p>` : ""}
        ${folderPath ? `<div class="tones-local-card-folder" title="${escapeHtml(folderPath)}">${escapeHtml(folderPath)}</div>` : ""}
        ${tags.length ? `<div class="tones-local-card-tags">${tags.map((entry) => `<span class="preset-item-tag">${escapeHtml(entry)}</span>`).join("")}</div>` : ""}
      </div>
      <div class="tones-local-card-footer">
        <div class="preset-rating-stars" role="group" aria-label="Rating">${stars}</div>
        <button class="tones-local-card-load" type="button" data-action="load"${isLoading ? " disabled" : ""}>${loadLabel}</button>
      </div>
    </article>
  `;
}

function renderEmpty(inFolder: number): string {
  if (inFolder > 0) {
    return `<div class="tones-local-empty">No presets match this search.</div>`;
  }
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    const label = folderId === PRESET_FOLDER_FAVORITES_ID
      ? "No favourites yet. Use the heart on a preset to keep it here."
      : folderId === PRESET_FOLDER_RECENTS_ID
        ? "Presets you load will show up here."
        : "This folder is empty.";
    return `<div class="tones-local-empty">${label}</div>`;
  }
  // The tab strip (tabs.ts) answers data-tones-open-tab, so this module need not import it.
  const browse = isFeatureEnabled(Features.ToneSharing)
    ? `<button type="button" class="btn btn-secondary" data-tones-open-tab="community">Browse Community Presets</button>`
    : "";
  return `<div class="tones-local-empty"><p>No presets on this machine yet.</p>${browse}</div>`;
}

function renderLocalPresets(): void {
  const grid = byId("tones-local-grid");
  if (!grid) {
    return;
  }

  const activeFolderId = resolveFolderId();
  renderFolders(activeFolderId);

  const inFolder = presetsInFolder(activeFolderId);
  renderTagOptions(collectPresetTags(inFolder));

  const filtered = getFilteredPresets(query, { folderId: activeFolderId, tags: tag ? new Set([tag]) : NO_TAGS });
  const presets = sortLocalPresets(filtered, sort, getPresetRating, activeFolderId === PRESET_FOLDER_RECENTS_ID);

  const summary = byId("tones-local-summary");
  if (summary) {
    summary.textContent = describePresetCount(presets.length, inFolder.length);
  }

  // Redrawing must not throw the reader back to the top of a long folder.
  const scrollTop = grid.scrollTop;
  grid.innerHTML = presets.length ? presets.map(renderCard).join("") : renderEmpty(inFolder.length);
  grid.querySelectorAll<HTMLElement>(".tones-local-card").forEach((card) => {
    card.style.setProperty("--tone-card-accent", idAccentColor(card.dataset.presetId ?? ""));
  });
  grid.scrollTop = scrollTop;
}

function handleGridClick(event: MouseEvent): void {
  const target = event.target as HTMLElement | null;
  const card = target?.closest<HTMLElement>(".tones-local-card");
  const presetId = card?.dataset.presetId;
  if (!target || !presetId) {
    return;
  }

  const star = target.closest<HTMLButtonElement>(".preset-rating-star");
  if (star) {
    const value = Number(star.dataset.rating ?? 0);
    // Clicking the current rating clears it, as in the preset popover.
    setPresetRating(presetId, getPresetRating(presetId) === value ? null : value);
    localPresetsView.request();
    return;
  }

  if (target.closest('[data-action="favorite"]')) {
    toggleFavoritePreset(presetId);
    localPresetsView.request();
    return;
  }

  // Anywhere else on the card loads it; the Load button is the same thing for the keyboard.
  // The load redraws the library, which redraws this (onPresetUIRendered).
  void applyPresetFromLibrary(presetId);
}

let bound = false;

/** Binds the tab's controls the first time it is shown, and draws it. */
export function initLocalPresetsBrowser(): void {
  if (!bound) {
    bound = true;

    byId("tones-local-folders")?.addEventListener("click", (event) => {
      const button = (event.target as HTMLElement | null)?.closest<HTMLElement>(".tones-local-folder");
      const next = button?.dataset.folderId;
      if (!next || next === folderId) {
        return;
      }
      folderId = next;
      const grid = byId("tones-local-grid");
      if (grid) {
        grid.scrollTop = 0;
      }
      localPresetsView.request();
    });

    byId<HTMLInputElement>("tones-local-search")?.addEventListener("input", (event) => {
      query = (event.target as HTMLInputElement).value;
      localPresetsView.request();
    });

    byId<HTMLSelectElement>("tones-local-tag")?.addEventListener("change", (event) => {
      tag = (event.target as HTMLSelectElement).value;
      localPresetsView.request();
    });

    byId<HTMLSelectElement>("tones-local-sort")?.addEventListener("change", (event) => {
      const value = (event.target as HTMLSelectElement).value;
      sort = isLocalPresetSort(value) ? value : "name";
      localPresetsView.request();
    });

    byId("tones-local-grid")?.addEventListener("click", handleGridClick);

    onPresetUIRendered(() => localPresetsView.request());
    // Factory Preset Packs decides whether the factory presets are listed at all.
    document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => localPresetsView.request());
  }

  localPresetsView.request();
}
