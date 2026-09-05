/**
 * The draggable floating YouTube player, its dock, and the actions that open,
 * close, minimise and favourite from it.
 *
 * Sits above ./panel.js in the import graph: favouriting has to refresh both the
 * player chrome and the results grid, and this is the module that can reach
 * both. The panel never imports back, so the jam feature stays acyclic.
 */

import type { JamVideoSummary } from "../types.js";
import { escapeHtml } from "../utils.js";
import { getApiBaseUrl } from "../apiConfig.js";
import { getXMarkSvg, renderIcon } from "../iconAssets.js";
import { renderJamPanel } from "./panel.js";
import {
  PLAYER_MINIMIZED_WIDTH,
  PLAYER_PADDING,
  PLAYER_WIDTH,
  clampPlayerPosition,
  ensureJamState,
  isFavorite,
  persistPlayerUi,
  toggleFavoriteVideo,
} from "./state.js";

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

/** Flips favourite state and refreshes both surfaces that display it. */
export function toggleFavorite(video: JamVideoSummary): void {
  const jam = ensureJamState();
  toggleFavoriteVideo(video);
  renderJamPanel();
  // Only worth redrawing the player when it is showing this very video.
  if (jam.player.currentVideo?.videoId === video.videoId) {
    renderFloatingPlayer();
  }
}

export function openPlayer(video: JamVideoSummary): void {
  const jam = ensureJamState();
  jam.player.open = true;
  jam.player.minimized = false;
  jam.player.currentVideo = video;
  jam.player.width = PLAYER_WIDTH;
  clampPlayerPosition(jam.player);
  persistPlayerUi();
  renderFloatingPlayer();
}

export function closePlayer(): void {
  const jam = ensureJamState();
  jam.player.open = false;
  jam.player.currentVideo = null;
  jam.player.minimized = false;
  persistPlayerUi();
  renderFloatingPlayer();
}

export function toggleMinimized(): void {
  const jam = ensureJamState();
  jam.player.minimized = !jam.player.minimized;
  clampPlayerPosition(jam.player);
  persistPlayerUi();
  renderFloatingPlayer();
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
            <button type="button" id="jam-player-favorite" class="jam-favorite-toggle">${renderIcon("star", "jam-player-favorite-icon")}</button>
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
  const favoriteButton = document.getElementById("jam-player-favorite") as HTMLButtonElement | null;
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
  if (favoriteButton) {
    const favorite = isFavorite(video.videoId);
    const label = favorite ? "Remove from favourites" : "Add to favourites";
    favoriteButton.classList.toggle("is-active", favorite);
    favoriteButton.setAttribute("aria-pressed", favorite ? "true" : "false");
    favoriteButton.title = label;
    favoriteButton.setAttribute("aria-label", label);
    favoriteButton.onclick = () => toggleFavorite(video);
  }
  if (panel && header) {
    bindFloatingPlayerDrag(panel, header);
  }
}
