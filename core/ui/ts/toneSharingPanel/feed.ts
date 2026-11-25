/**
 * The card grid: turning rows of items and packs into markup, paging through a
 * collection, and the debounced community search that drives it.
 *
 * Every request carries the browse request version it was issued under, so a
 * slow page that lands after the user has typed again is dropped rather than
 * rendered over the newer results.
 */

import { getAppSetting } from "../appSettingsStore.js";
import { escapeHtml, idAccentColor } from "../utils.js";
import { featureToggleButton, renderToneIconButton } from "./actionButtons.js";
import { apiFetch, isToneSharingAdmin } from "./api.js";
import { element } from "./dom.js";
import { buildModerationBadge, canFeature, formatCompactMetric, getToneSharingDisplayTags, resolveCreatorAvatarUrl, resolveCreatorProfileHandle, resolveToneSharingDownloadCount } from "./format.js";
import { observePackThumbnails } from "./images.js";
import { buildInstalledToneSharingLookup } from "./presetLinks.js";
import { requestBrowseReload } from "./refresh.js";
import { SHOW_TONE_SHARING_STATS, browseCollections, browseState, previewState, storageKeys } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingRow } from "./types.js";

let communitySearchTimer: ReturnType<typeof setTimeout> | undefined;

export function updateCommunityPresetSearchUi(): void {
  const row = element<HTMLElement>("tone-sharing-community-search");
  const input = element<HTMLInputElement>("tone-sharing-community-search-input");
  const clearButton = element<HTMLButtonElement>("tone-sharing-community-search-clear");
  if (!row || !input || !clearButton) {
    return;
  }

  const shouldShow = browseState.mode === "items" && browseState.activeSharedTarget === null;
  row.classList.toggle("tone-sharing-community-search--hidden", !shouldShow);

  if (input.value !== browseState.searchQuery) {
    input.value = browseState.searchQuery;
  }

  const tagFilter = element<HTMLSelectElement>("tone-sharing-community-tag-filter");
  if (tagFilter) tagFilter.value = browseState.tagFilter;
  clearButton.disabled = browseState.searchQuery.length === 0 && !browseState.tagFilter;
}

export type FeedLayout = "cards" | "list";

export function getFeedLayout(): FeedLayout {
  return getAppSetting(storageKeys.feedLayout) === "list" ? "list" : "cards";
}

/**
 * Cards or a list. The list is one line per preset or pack, for a small screen to show many
 * more of them; it is the feed's class, so it holds for every section and re-render.
 */
export function applyFeedLayout(): void {
  const layout = getFeedLayout();
  element<HTMLElement>("tone-sharing-feed")?.classList.toggle("tone-sharing-feed--list", layout === "list");
  document.querySelectorAll<HTMLButtonElement>("[data-tone-sharing-layout]").forEach((button) => {
    button.setAttribute("aria-pressed", button.dataset.toneSharingLayout === layout ? "true" : "false");
  });
}

export async function renderFeedRows(rows: ToneSharingRow[], requestVersion = browseState.requestVersion): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  const useTiledLayout = browseState.mode === "items" || browseState.mode === "packs";
  feed.classList.toggle("tone-sharing-feed--tiled", useTiledLayout);
  applyFeedLayout();
  const installedLookup = buildInstalledToneSharingLookup();
  // My Content already has the owner's edit and delete; admins get theirs everywhere else.
  const adminTools = isToneSharingAdmin() && browseState.mode !== "installed";
  const adminEditTools = adminTools && browseState.mode !== "mine";

  if (!rows.length) {
    feed.innerHTML = `<div class="tone-sharing-status">No content yet. Publish the first tone.</div>`;
    updateBrowseFooter();
    return;
  }

  const rowHtml = await Promise.all(
    rows.map(async (row) => {
      const itemHtml = await Promise.all(
        row.items.map(async (item) => {
          let cardClass = "tone-sharing-card-item";
          const cardStyleFragments: string[] = [];
          const itemAccent = item.kind === "item" ? idAccentColor(item.id) : "#8f77ff";
          cardStyleFragments.push(`--tone-card-accent: ${itemAccent}`);

          // Pack thumbnails are loaded lazily (only when the card scrolls into view) to
          // avoid fetching every thumbnail up front when the Tones tab opens.
          let lazyPackThumbnailUrl: string | null = null;
          if (item.kind === "pack") {
            cardClass += " tone-sharing-pack-hero";
            lazyPackThumbnailUrl = item.thumbnailUrl ?? (item.thumbnailAssetId ? `/packs/${item.id}/thumbnail` : null);
          }

          const isPreviewing = previewState.itemId === item.id;
          const styleAttr = cardStyleFragments.length
            ? ` style="${escapeHtml(cardStyleFragments.join("; "))}"`
            : "";
          const lazyThumbAttr = lazyPackThumbnailUrl
            ? ` data-pack-thumbnail="${escapeHtml(lazyPackThumbnailUrl)}"`
            : "";
          const reviewMode = browseState.mode === "review";
          const isInstalled = item.kind === "item"
            ? installedLookup.itemIds.has(item.id)
            : installedLookup.packIds.has(item.id);
          const creatorData = item as unknown as Record<string, unknown>;
          const creatorHandle = resolveCreatorProfileHandle(creatorData);
          const creatorAvatarUrl = resolveCreatorAvatarUrl(creatorData);
          const itemDownloadCount = item.kind === "item"
            ? resolveToneSharingDownloadCount(item as unknown as Record<string, unknown>) ?? 0
            : 0;
          const typeLabel = item.kind === "item" ? (item.type ?? "preset") : "pack";
          const itemId = escapeHtml(item.id);
          const safeTitle = escapeHtml(item.title);
          const safeTypeLabel = escapeHtml(typeLabel);
          const showTypeChip = typeLabel.trim().toLowerCase() !== "preset";
          const metaChips = [
            ...(showTypeChip
              ? [`<span class="tone-sharing-meta-chip">${safeTypeLabel}</span>`]
              : []),
            ...(item.kind === "item"
              ? [`<span class="tone-sharing-meta-chip">${formatCompactMetric(itemDownloadCount)} downloads</span>`]
              : []),
          ];
          const creatorDisplayName = typeof item.creatorDisplayName === "string" ? item.creatorDisplayName.trim() : "";
          const creatorLineLabel = creatorDisplayName && creatorHandle && creatorDisplayName !== creatorHandle
            ? `${creatorDisplayName} (${creatorHandle})`
            : (creatorDisplayName || creatorHandle || (creatorAvatarUrl ? "Creator" : ""));
          const safeCreatorLineLabel = escapeHtml(creatorLineLabel);
          const safeCreatorAvatarUrl = creatorAvatarUrl ? escapeHtml(creatorAvatarUrl) : "";
          const showCreatorIdentity = Boolean(creatorLineLabel || creatorAvatarUrl);
          const creatorIdentityMarkup = showCreatorIdentity
            ? `<div class="tone-sharing-card-creator">`
              + `${creatorAvatarUrl ? `<img class="tone-sharing-card-creator-avatar" src="${safeCreatorAvatarUrl}" alt="Creator avatar" loading="lazy" />` : ""}`
              + `${creatorLineLabel ? `<span class="tone-sharing-card-creator-handle">${safeCreatorLineLabel}</span>` : ""}`
              + `</div>`
            : "";
          const safeDescription = typeof item.description === "string" && item.description.trim().length > 0
            ? escapeHtml(item.description.trim())
            : "";
          const descriptionClass = item.kind === "pack"
            ? "tone-sharing-card-item-description tone-sharing-card-item-description--pack"
            : "tone-sharing-card-item-description";
          const moderationBadge = buildModerationBadge(item.moderationStatus);
          // Only the creator's own lists and the admin's carry a note; public listings never do.
          const moderationNoteText = item.moderationNote?.notes?.trim() ?? "";
          const moderationNoteMarkup = moderationNoteText
            ? `<div class="tone-sharing-card-moderation-note"><span class="tone-sharing-card-moderation-note-label">Moderator</span>${escapeHtml(moderationNoteText)}</div>`
            : "";
          const itemStats = SHOW_TONE_SHARING_STATS && item.kind === "item"
            ? [
                `<span class="tone-sharing-card-stat"><span class="tone-sharing-card-stat-label">Fav</span><strong>${formatCompactMetric(item.favoriteCount)}</strong></span>`,
                `<span class="tone-sharing-card-stat"><span class="tone-sharing-card-stat-label">Rating</span><strong>${typeof item.averageRating === "number" && Number.isFinite(item.averageRating) ? `${item.averageRating.toFixed(1)}/5` : "--"}</strong></span>`,
                `<span class="tone-sharing-card-stat"><span class="tone-sharing-card-stat-label">Votes</span><strong>${formatCompactMetric(item.ratingCount)}</strong></span>`,
              ].join("")
            : "";
          const displayTags = item.kind === "item" ? getToneSharingDisplayTags(item.tags) : [];
          const tagsMarkup = displayTags.length > 0
            ? `<div class="tone-sharing-card-tags">${displayTags.map((tag) => `<span class="tone-sharing-tag-badge">${escapeHtml(tag)}</span>`).join("")}</div>`
            : "";
          const bottomMetaMarkup = (tagsMarkup || creatorIdentityMarkup)
            ? `<div class="tone-sharing-card-bottom-meta">${tagsMarkup}${creatorIdentityMarkup}</div>`
            : "";
          const actionsMarkup = item.kind === "item"
            ? `
                        ${renderToneIconButton({ kind: "action", value: "preview", icon: "preview", label: isPreviewing ? "Previewing preset" : "Preview preset", active: isPreviewing })}
                        ${renderToneIconButton({ kind: "action", value: "download", icon: "download", label: isInstalled ? "Preset already installed" : "Download preset", primary: true, disabled: isInstalled })}
                        ${renderToneIconButton({ kind: "action", value: "share", icon: "share", label: "Share preset" })}`
            : `
                        ${renderToneIconButton({ kind: "action", value: "view", icon: "view", label: "Open pack" })}
                        ${renderToneIconButton({ kind: "action", value: "download", icon: "download", label: isInstalled ? "Pack already installed" : "Download pack", primary: true, disabled: isInstalled })}
                        ${renderToneIconButton({ kind: "action", value: "share", icon: "share", label: "Share pack" })}`;
          return `
                  <div class="${escapeHtml(cardClass)}${isPreviewing ? " is-previewing" : ""}" data-kind="${item.kind}" data-id="${itemId}" data-title="${safeTitle}"${styleAttr}${lazyThumbAttr}>
                    <div class="tone-sharing-card-item-content">
                      <div class="tone-sharing-card-item-header">
                        <div class="tone-sharing-card-item-title">${safeTitle}</div>
                        ${moderationBadge}
                      </div>
                      <div class="tone-sharing-card-item-meta">
                        ${metaChips.join("")}
                      </div>
                      ${itemStats ? `<div class="tone-sharing-card-item-stats">${itemStats}</div>` : ""}
                      ${safeDescription ? `<div class="${descriptionClass}">${safeDescription}</div>` : ""}
                      ${moderationNoteMarkup}
                      ${bottomMetaMarkup}
                    </div>
                    <div class="tone-sharing-card-item-actions">
                      ${actionsMarkup}
                      ${reviewMode ? `${renderToneIconButton({ kind: "action", value: "approve", icon: "approve", label: "Approve", primary: true })}${renderToneIconButton({ kind: "action", value: "reject", icon: "reject", label: "Reject" })}` : ""}
                      ${browseState.mode === "mine" && item.kind === "pack" ? renderToneIconButton({ kind: "action", value: "edit-pack", icon: "edit", label: "Edit pack" }) : ""}
                      ${browseState.mode === "mine" ? renderToneIconButton({ kind: "action", value: "delete", icon: "delete", label: "Delete" }) : ""}
                      ${adminTools && canFeature(item.moderationStatus) ? featureToggleButton("action", Boolean(item.featured)) : ""}
                      ${adminEditTools ? renderToneIconButton({ kind: "action", value: "admin-edit", icon: "edit", label: "Edit (admin)" }) : ""}
                      ${adminEditTools ? renderToneIconButton({ kind: "action", value: "admin-delete", icon: "delete", label: "Delete (admin)" }) : ""}
                    </div>
                  </div>
                `;
        })
      );

      return `
        <div class="tone-sharing-row${row.strip ? " tone-sharing-row--strip" : ""}">
          <div class="tone-sharing-row-title">${escapeHtml(row.title)}</div>
          <div class="tone-sharing-row-track">
            ${itemHtml.join("")}
          </div>
        </div>
      `;
    })
  );

  if (requestVersion !== browseState.requestVersion) return;
  feed.innerHTML = rowHtml.join("");
  observePackThumbnails(feed);
  updateBrowseFooter();
}

export function updateActiveSharedFilter(): void {
  const container = element<HTMLElement>("tone-sharing-active-filter");
  const label = element<HTMLElement>("tone-sharing-active-filter-label");
  if (!container || !label) {
    return;
  }

  if (!browseState.activeSharedTarget) {
    container.classList.add("tone-sharing-active-filter--hidden");
    label.textContent = "Filtered to a shared tone.";
    return;
  }

  container.classList.remove("tone-sharing-active-filter--hidden");
  label.textContent = browseState.activeSharedTarget.kind === "pack"
    ? `Filtered to shared pack ${browseState.activeSharedTarget.id}`
    : `Filtered to shared preset ${browseState.activeSharedTarget.id}`;
}

export function updateBrowseFooter(): void {
  const footer = element<HTMLElement>("tone-sharing-feed-footer");
  const prevButton = element<HTMLButtonElement>("tone-sharing-page-prev");
  const button = element<HTMLButtonElement>("tone-sharing-load-more");
  const nextButton = element<HTMLButtonElement>("tone-sharing-page-next");
  const label = element<HTMLElement>("tone-sharing-feed-footer-label");
  if (!footer || !prevButton || !button || !nextButton || !label) {
    return;
  }

  const supportsPaging = browseState.activeSharedTarget === null && (browseState.mode === "items" || browseState.mode === "packs");
  if (!supportsPaging) {
    footer.classList.add("tone-sharing-feed-footer--hidden");
    label.textContent = "";
    prevButton.classList.add("tone-sharing-feed-footer-btn--hidden");
    nextButton.classList.add("tone-sharing-feed-footer-btn--hidden");
    button.classList.remove("tone-sharing-feed-footer-btn--hidden");
    prevButton.disabled = false;
    button.disabled = false;
    nextButton.disabled = false;
    button.textContent = "Load More";
    return;
  }

  footer.classList.remove("tone-sharing-feed-footer--hidden");
  const loadingLabel = browseCollections.loadingMore ? "Loading..." : "Load More";
  button.textContent = loadingLabel;

  const loadedCount = browseState.mode === "items" ? browseCollections.items.length : browseCollections.packs.length;
  if (browseState.mode === "packs") {
    button.classList.add("tone-sharing-feed-footer-btn--hidden");
    prevButton.classList.remove("tone-sharing-feed-footer-btn--hidden");
    nextButton.classList.remove("tone-sharing-feed-footer-btn--hidden");
    prevButton.disabled = browseCollections.loadingMore || browseCollections.page <= 1;
    nextButton.disabled = browseCollections.loadingMore || !browseCollections.hasMore;
    prevButton.textContent = browseCollections.loadingMore ? "Loading..." : "Previous";
    nextButton.textContent = browseCollections.loadingMore ? "Loading..." : "Next";
    label.textContent = loadedCount > 0
      ? `Page ${browseCollections.page}${browseCollections.hasMore ? "" : " · end of results"}`
      : "";
    return;
  }

  prevButton.classList.add("tone-sharing-feed-footer-btn--hidden");
  nextButton.classList.add("tone-sharing-feed-footer-btn--hidden");
  button.classList.remove("tone-sharing-feed-footer-btn--hidden");
  button.disabled = browseCollections.loadingMore || !browseCollections.hasMore;

  label.textContent = browseCollections.hasMore
    ? `${loadedCount} loaded`
    : loadedCount > 0
      ? `${loadedCount} loaded, end of results`
      : "";
}

export function resetBrowseCollections(): void {
  browseState.requestVersion++;
  clearTimeout(communitySearchTimer);
  browseCollections.page = 1;
  browseCollections.pageSize = 36;
  browseCollections.hasMore = false;
  browseCollections.loadingMore = false;
  browseCollections.items = [];
  browseCollections.packs = [];
  browseCollections.featuredRows = [];
}

export async function renderStandardBrowseCollection(): Promise<void> {
  if (browseState.mode === "items") {
    const items = browseCollections.items;
    const searching = browseState.searchQuery.trim().length > 0 || Boolean(browseState.tagFilter);
    if (!items.length && searching) {
      const feed = element<HTMLElement>("tone-sharing-feed");
      if (feed) {
        feed.innerHTML = `<div class="tone-sharing-status">No community presets match${browseState.searchQuery.trim() ? ` "${escapeHtml(browseState.searchQuery.trim())}"` : ""}${browseState.tagFilter ? ` with the tag "${escapeHtml(browseState.tagFilter)}"` : ""}.</div>`;
      }
      updateBrowseFooter();
      return;
    }

    // Presets opens on the featured packs over the community presets; a search replaces both.
    const rows = searching ? [] : [...browseCollections.featuredRows];
    if (items.length > 0) {
      rows.push(buildSingleRow(searching ? "Search Results" : "Community Presets", items, "item"));
    }
    await renderFeedRows(rows);
    return;
  }

  if (browseState.mode === "packs") {
    await renderFeedRows([buildSingleRow("Latest Packs", browseCollections.packs, "pack")]);
  }
}

export async function loadStandardBrowsePage(page: number, append = false): Promise<void> {
  const pageSize = browseCollections.pageSize;
  const requestVersion = browseState.requestVersion;

  if (browseState.mode === "items") {
    const query = browseState.searchQuery.trim();
    const tagParam = browseState.tagFilter ? `&tag=${encodeURIComponent(browseState.tagFilter)}` : "";
    // The featured packs come with the first page of the unsearched results. They are a garnish:
    // a /home that fails leaves the presets to show on their own.
    const withFeatured = page === 1 && !append && !query && !browseState.tagFilter;
    const [response, home] = await Promise.all([
      apiFetch<{ page?: number; pageSize?: number; items: ToneSharingItem[] }>(`/items?page=${page}&pageSize=${pageSize}&q=${encodeURIComponent(query)}${tagParam}`),
      withFeatured ? apiFetch<{ rows?: ToneSharingRow[] }>("/home").catch(() => null) : Promise.resolve(null),
    ]);
    if (requestVersion !== browseState.requestVersion || browseState.mode !== "items" || browseState.activeSharedTarget !== null) return;
    if (!append) {
      browseCollections.featuredRows = Array.isArray(home?.rows) ? featuredPackRows(home.rows) : [];
    }
    browseCollections.page = response.page ?? page;
    browseCollections.pageSize = response.pageSize ?? pageSize;
    browseCollections.items = append ? [...browseCollections.items, ...response.items] : response.items.slice();
    browseCollections.hasMore = response.items.length >= browseCollections.pageSize;
    await renderStandardBrowseCollection();
    return;
  }

  if (browseState.mode === "packs") {
    const response = await apiFetch<{ page?: number; pageSize?: number; totalPages?: number; totalCount?: number; packs: ToneSharingPack[] }>(`/packs?page=${page}&pageSize=${pageSize}`);
    if (requestVersion !== browseState.requestVersion || browseState.mode !== "packs" || browseState.activeSharedTarget !== null) return;
    browseCollections.page = response.page ?? page;
    browseCollections.pageSize = response.pageSize ?? pageSize;
    browseCollections.packs = response.packs.slice();
    if (typeof response.totalPages === "number" && Number.isFinite(response.totalPages) && response.totalPages > 0) {
      browseCollections.hasMore = browseCollections.page < Math.floor(response.totalPages);
    } else if (typeof response.totalCount === "number" && Number.isFinite(response.totalCount) && response.totalCount >= 0) {
      browseCollections.hasMore = (browseCollections.page * browseCollections.pageSize) < Math.floor(response.totalCount);
    } else {
      browseCollections.hasMore = response.packs.length >= browseCollections.pageSize;
    }
    await renderStandardBrowseCollection();
  }
}

export function buildSingleRow(title: string, entries: Array<{ id: string; title: string; type?: string }>, kind: "item" | "pack"): ToneSharingRow {
  return {
    id: `generated-${title.toLowerCase().replace(/\s+/g, "-")}`,
    slug: `generated-${title.toLowerCase().replace(/\s+/g, "-")}`,
    title,
    items: entries.map((entry) => ({
      id: entry.id,
      kind,
      title: entry.title,
      type: entry.type ?? null,
      featured: Boolean((entry as ToneSharingItem | ToneSharingPack).featured),
      moderationStatus: (entry as ToneSharingItem | ToneSharingPack).moderationStatus,
      moderationNote: (entry as ToneSharingItem | ToneSharingPack).moderationNote ?? null,
      creatorEmail: (entry as ToneSharingItem | ToneSharingPack).creatorEmail ?? null,
      creatorDisplayName: (entry as ToneSharingItem | ToneSharingPack).creatorDisplayName ?? null,
      creatorHandle: (entry as ToneSharingItem | ToneSharingPack).creatorHandle ?? null,
      creatorProfileHandle: (entry as ToneSharingItem | ToneSharingPack).creatorProfileHandle ?? null,
      profileHandle: (entry as ToneSharingItem | ToneSharingPack).profileHandle ?? null,
      creatorAvatarUrl: (entry as ToneSharingItem | ToneSharingPack).creatorAvatarUrl ?? null,
      favoriteCount: kind === "item" ? (entry as ToneSharingItem).favoriteCount : undefined,
      ratingCount: kind === "item" ? (entry as ToneSharingItem).ratingCount : undefined,
      averageRating: kind === "item" ? (entry as ToneSharingItem).averageRating ?? null : undefined,
      currentUserFavorite: kind === "item" ? (entry as ToneSharingItem).currentUserFavorite : undefined,
      currentUserRating: kind === "item" ? (entry as ToneSharingItem).currentUserRating ?? null : undefined,
      downloadCount: kind === "item"
        ? resolveToneSharingDownloadCount((entry as unknown as Record<string, unknown>)) ?? 0
        : undefined,
      description: kind === "pack" ? (entry as ToneSharingPack).description ?? null : (entry as ToneSharingItem).description ?? null,
      tags: kind === "item" ? (entry as ToneSharingItem).tags ?? null : null,
      thumbnailUrl: kind === "pack"
        ? ((entry as ToneSharingPack).thumbnailUrl ?? ((entry as ToneSharingPack).thumbnailAssetId ? `/packs/${entry.id}/thumbnail` : null))
        : null
    }))
  };
}

/**
 * The featured packs of /home, each row a strip: over the presets they are a line to scroll
 * along, not a grid to scroll past. Its preset rows are left out: Presets lists those itself,
 * searchable and paged.
 */
export function featuredPackRows(rows: ToneSharingRow[]): ToneSharingRow[] {
  return rows
    .map((row) => ({ ...row, strip: true, items: row.items.filter((item) => item.kind === "pack") }))
    .filter((row) => row.items.length > 0);
}

export function scheduleCommunitySearch(delay = 250): void {
  resetBrowseCollections();
  const version = browseState.requestVersion;
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (feed) feed.innerHTML = '<div class="tone-sharing-status" role="status">Searching...</div>';
  updateBrowseFooter();
  communitySearchTimer = setTimeout(() => {
    if (version !== browseState.requestVersion || browseState.mode !== "items" || browseState.activeSharedTarget !== null) return;
    void requestBrowseReload();
  }, delay);
}
