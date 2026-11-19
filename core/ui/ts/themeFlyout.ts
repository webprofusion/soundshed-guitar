/**
 * The footer's Theme button and the flyout it opens: the UI zoom, and every theme as a tile with
 * a picture of the app wearing it (docs/theme-system.md, "Choosing a theme").
 *
 * The pictures are images/themes/thumbnails/<theme>.jpg, square crops of the running app's
 * top-left corner captured by tools/theme-thumbnails/capture.mjs. A theme with no picture yet still gets a tile, with an
 * empty frame.
 *
 * Choosing a tile applies the theme at once and leaves the flyout open, so themes can be tried
 * one after another; a click outside, Escape or the button again closes it. The zoom picker is
 * the one settings/appearance.ts binds (initZoomControls), moved here from the footer.
 */
import { getUiZoom } from "./pointerDrag.js";
import { THEME_OPTIONS, themeSwitcher } from "./theme-switcher.js";
import type { ThemeName } from "./theme-switcher.js";

const VIEWPORT_MARGIN = 8;
const FLYOUT_GAP = 6;

let initialized = false;
let repositionFrame = 0;

const triggerEl = (): HTMLButtonElement | null => document.getElementById("footer-theme-btn") as HTMLButtonElement | null;
const flyoutEl = (): HTMLDivElement | null => document.getElementById("theme-flyout") as HTMLDivElement | null;
const gridEl = (): HTMLElement | null => flyoutEl()?.querySelector<HTMLElement>(".theme-flyout-grid") ?? null;

/** Where a theme's picture is, relative to the page. */
export function themeThumbnailUrl(theme: ThemeName): string {
  return `images/themes/thumbnails/${theme}.jpg`;
}

function isThemeName(value: unknown): value is ThemeName {
  return THEME_OPTIONS.some((option) => option.value === value);
}

function isOpen(): boolean {
  const flyout = flyoutEl();
  return Boolean(flyout && !flyout.hidden);
}

function buildTile(theme: ThemeName, label: string): HTMLButtonElement {
  const tile = document.createElement("button");
  tile.type = "button";
  tile.className = "theme-flyout-tile";
  tile.dataset.theme = theme;
  tile.setAttribute("role", "radio");
  tile.setAttribute("aria-checked", "false");
  tile.tabIndex = -1;

  const frame = document.createElement("span");
  frame.className = "theme-flyout-thumb";
  const image = document.createElement("img");
  image.alt = "";
  image.decoding = "async";
  image.draggable = false;
  // A theme added since the pictures were last captured keeps its empty frame.
  image.addEventListener("error", () => frame.classList.add("is-missing"), { once: true });
  image.src = themeThumbnailUrl(theme);
  frame.append(image);

  const name = document.createElement("span");
  name.className = "theme-flyout-name";
  name.textContent = label;

  tile.append(frame, name);
  return tile;
}

/** The tiles are built once, on the first open, so the pictures load only when wanted. */
function ensureTiles(): void {
  const grid = gridEl();
  if (!grid || grid.childElementCount > 0) {
    return;
  }
  grid.replaceChildren(...THEME_OPTIONS.map((option) => buildTile(option.value, option.label)));
}

/**
 * Marks the current theme. The tiles are updated in place, never rebuilt: the click that chose
 * one is still bubbling from it, and the footer sheet's outside-click check walks up from there.
 */
function syncSelection(theme: ThemeName = themeSwitcher.getCurrentTheme()): void {
  for (const tile of tilesOf(gridEl())) {
    const selected = tile.dataset.theme === theme;
    tile.setAttribute("aria-checked", String(selected));
    tile.tabIndex = selected ? 0 : -1;
  }
}

function tilesOf(grid: HTMLElement | null): HTMLButtonElement[] {
  return grid ? Array.from(grid.querySelectorAll<HTMLButtonElement>(".theme-flyout-tile")) : [];
}

/**
 * Fixed to the viewport and hung from the body, as the input mode flyout is: a theme that gives
 * the footer a filter or backdrop would otherwise become the box it is laid out in. Above the
 * button when there is more room there (the footer is at the bottom), and kept in the window.
 * The app's zoom is CSS zoom on the body, so the rects are brought into the body's pixels.
 */
function positionFlyout(): void {
  const flyout = flyoutEl();
  const trigger = triggerEl();
  if (!flyout || !trigger || flyout.hidden) {
    return;
  }

  const zoom = getUiZoom();
  const client = trigger.getBoundingClientRect();
  const rect = { left: client.left / zoom, top: client.top / zoom, bottom: client.bottom / zoom, right: client.right / zoom };
  const viewportWidth = window.innerWidth / zoom;
  const viewportHeight = window.innerHeight / zoom;
  const width = flyout.offsetWidth;
  const height = flyout.offsetHeight;

  // Right edges lined up, as the button sits at the right of the footer.
  const left = Math.min(
    Math.max(rect.right - width, VIEWPORT_MARGIN),
    Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width),
  );

  const spaceBelow = viewportHeight - rect.bottom - FLYOUT_GAP - VIEWPORT_MARGIN;
  const spaceAbove = rect.top - FLYOUT_GAP - VIEWPORT_MARGIN;
  const above = spaceAbove >= spaceBelow;
  const room = Math.max(160, Math.floor(above ? spaceAbove : spaceBelow));
  flyout.style.maxHeight = `${room}px`;
  const placedHeight = Math.min(height, room);
  const top = above ? Math.max(VIEWPORT_MARGIN, rect.top - FLYOUT_GAP - placedHeight) : rect.bottom + FLYOUT_GAP;

  flyout.style.left = `${Math.round(left)}px`;
  flyout.style.top = `${Math.round(top)}px`;
  flyout.classList.toggle("opens-above", above);
}

function schedulePosition(): void {
  if (repositionFrame || !isOpen()) {
    return;
  }
  repositionFrame = requestAnimationFrame(() => {
    repositionFrame = 0;
    positionFlyout();
  });
}

export function closeThemeFlyout(returnFocus = false): void {
  const flyout = flyoutEl();
  if (flyout) {
    flyout.hidden = true;
  }
  const trigger = triggerEl();
  trigger?.setAttribute("aria-expanded", "false");
  if (returnFocus) {
    trigger?.focus();
  }
}

export function openThemeFlyout(focus = false): void {
  const flyout = flyoutEl();
  if (!flyout) {
    return;
  }
  ensureTiles();
  syncSelection();
  flyout.hidden = false;
  positionFlyout();
  triggerEl()?.setAttribute("aria-expanded", "true");
  if (focus) {
    tilesOf(gridEl()).find((tile) => tile.tabIndex === 0)?.focus();
  }
}

function chooseTheme(theme: ThemeName): void {
  if (theme !== themeSwitcher.getCurrentTheme()) {
    themeSwitcher.setTheme(theme);
  }
  // themeChanged marks the tile too; this covers a theme that was already on.
  syncSelection(theme);
}

/** Arrow keys walk the tiles in reading order; Up and Down move a row of the grid. */
function moveFocus(event: KeyboardEvent): boolean {
  const tiles = tilesOf(gridEl());
  const current = tiles.indexOf(document.activeElement as HTMLButtonElement);
  if (current < 0 || tiles.length === 0) {
    return false;
  }

  const firstTop = tiles[0].offsetTop;
  const perRow = Math.max(1, tiles.filter((tile) => tile.offsetTop === firstTop).length);
  let next = current;
  switch (event.key) {
    case "ArrowRight": next = current + 1; break;
    case "ArrowLeft": next = current - 1; break;
    case "ArrowDown": next = current + perRow; break;
    case "ArrowUp": next = current - perRow; break;
    case "Home": next = 0; break;
    case "End": next = tiles.length - 1; break;
    default: return false;
  }
  tiles[Math.min(Math.max(next, 0), tiles.length - 1)].focus();
  return true;
}

export function initThemeFlyout(): void {
  const trigger = triggerEl();
  const flyout = flyoutEl();
  if (initialized || !trigger || !flyout) {
    return;
  }
  initialized = true;

  document.body.append(flyout);

  // detail is 0 for a click made with the keyboard, which then puts focus on the current theme.
  trigger.addEventListener("click", (event) => {
    if (isOpen()) {
      closeThemeFlyout();
    } else {
      openThemeFlyout(event.detail === 0);
    }
  });

  flyout.addEventListener("click", (event) => {
    const tile = (event.target as Element | null)?.closest<HTMLButtonElement>(".theme-flyout-tile");
    if (tile && isThemeName(tile.dataset.theme)) {
      chooseTheme(tile.dataset.theme);
    }
  });

  flyout.addEventListener("keydown", (event) => {
    if ((event.target as Element | null)?.closest(".theme-flyout-tile") && moveFocus(event)) {
      event.preventDefault();
    }
  });

  // Captured, so Escape closes the flyout and goes no further: the footer sheet the button may
  // sit in stays open.
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || !isOpen()) {
      return;
    }
    const active = document.activeElement;
    closeThemeFlyout(Boolean(active && (trigger.contains(active) || flyout.contains(active))));
    event.stopPropagation();
    event.preventDefault();
  }, true);

  document.addEventListener("pointerdown", (event) => {
    if (!isOpen() || !(event.target instanceof Node)) {
      return;
    }
    if (flyoutEl()?.contains(event.target) || triggerEl()?.contains(event.target)) {
      return;
    }
    closeThemeFlyout();
  }, true);

  window.addEventListener("themeChanged", ((event: CustomEvent) => {
    if (isThemeName(event.detail?.theme)) {
      syncSelection(event.detail.theme);
    }
    // A theme can change the footer's height, and so where the button is.
    schedulePosition();
  }) as EventListener);

  // A new zoom scales the button and the flyout both.
  window.addEventListener("uiSettingsApplied", schedulePosition);
  window.addEventListener("resize", schedulePosition);
  document.addEventListener("scroll", (event) => {
    if (!(event.target instanceof Node) || !flyoutEl()?.contains(event.target)) {
      schedulePosition();
    }
  }, true);
}
