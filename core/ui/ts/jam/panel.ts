/**
 * Jam panel rendering: the section/tab chrome and the backing-track result grid.
 *
 * Rendering only — it reads jam state and writes the DOM, but never mutates
 * state or kicks off a search. Section switching and search live in ../jam.ts.
 */

import type { JamState, JamVideoSummary } from "../types.js";
import { escapeHtml } from "../utils.js";
import { renderRiffLibraryPanel } from "../riffLibrary.js";
import { renderPracticeToolPanel } from "../practiceTool.js";
import {
  ensureJamState,
  isBackingTracksFeatureEnabled,
  isFavorite,
  isPracticeToolFeatureEnabled,
  isRiffLibraryFeatureEnabled,
  isScalesFeatureEnabled,
  resolveBackingTracksTab,
  resolveJamSection,
} from "./state.js";

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
  const practiceToolSectionButton = document.getElementById("jam-section-practice-tool");
  const searchTab = document.getElementById("jam-backing-tab-search");
  const favoritesTab = document.getElementById("jam-backing-tab-favorites");
  const backingTracksPanel = document.getElementById("jam-section-panel-backing-tracks");
  const scalesPanel = document.getElementById("jam-section-panel-scales");
  const riffsPanel = document.getElementById("jam-section-panel-riffs");
  const practiceToolPanel = document.getElementById("jam-section-panel-practice-tool");
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
  const practiceToolEnabled = isPracticeToolFeatureEnabled();

  if (searchInput && searchInput.value !== jam.query) {
    searchInput.value = jam.query;
  }

  scalesSectionButton?.toggleAttribute("hidden", !scalesEnabled);
  riffsSectionButton?.toggleAttribute("hidden", !riffLibraryEnabled);
  practiceToolSectionButton?.toggleAttribute("hidden", !practiceToolEnabled);
  searchTab?.toggleAttribute("hidden", !backingTracksEnabled);
  favoritesTab?.toggleAttribute("hidden", !backingTracksEnabled);

  scalesSectionButton?.classList.toggle("active", resolvedSection === "scales");
  riffsSectionButton?.classList.toggle("active", resolvedSection === "riffs");
  practiceToolSectionButton?.classList.toggle("active", resolvedSection === "practiceTool");
  searchTab?.classList.toggle("active", resolvedSection === "backingTracks" && resolvedTab === "search");
  favoritesTab?.classList.toggle("active", resolvedSection === "backingTracks" && resolvedTab === "favorites");

  backingTracksPanel?.classList.toggle("active", resolvedSection === "backingTracks" && backingTracksEnabled);
  backingTracksPanel?.toggleAttribute("hidden", resolvedSection !== "backingTracks" || !backingTracksEnabled);
  scalesPanel?.classList.toggle("active", resolvedSection === "scales" && scalesEnabled);
  scalesPanel?.toggleAttribute("hidden", resolvedSection !== "scales" || !scalesEnabled);
  riffsPanel?.classList.toggle("active", resolvedSection === "riffs" && riffLibraryEnabled);
  riffsPanel?.toggleAttribute("hidden", resolvedSection !== "riffs" || !riffLibraryEnabled);
  practiceToolPanel?.classList.toggle("active", resolvedSection === "practiceTool" && practiceToolEnabled);
  practiceToolPanel?.toggleAttribute("hidden", resolvedSection !== "practiceTool" || !practiceToolEnabled);
  searchPanel?.classList.toggle("active", resolvedTab === "search" && backingTracksEnabled);
  searchPanel?.toggleAttribute("hidden", resolvedTab !== "search" || !backingTracksEnabled);
  favoritesPanel?.classList.toggle("active", resolvedTab === "favorites" && backingTracksEnabled);
  favoritesPanel?.toggleAttribute("hidden", resolvedTab !== "favorites" || !backingTracksEnabled);

  if (resolvedSection === "riffs") {
    renderRiffLibraryPanel();
    return;
  }

  if (resolvedSection === "practiceTool") {
    renderPracticeToolPanel();
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
