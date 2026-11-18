/**
 * Indirection for "the preset library changed, redraw it".
 *
 * The renderers live in library.ts, because they need the dropdown, the
 * library list and the active-preset UI. Archive import finishes by asking for
 * that redraw, and so do load, save, the popover and the folder and setlist
 * controls after they change something — but library.ts imports several of
 * those to bind its rows, so importing it back would tie them into a cycle.
 *
 * So the seam states the relationship as it is: those modules *request* a
 * redraw, they do not own the rendering. library.ts and actions.ts register the
 * implementations once, at module load. Same pattern as signalPath/render.ts.
 */

import type { Preset } from "../types.js";

let refresh: ((activePreset: Preset | null) => void) | null = null;

/** Called once by actions.ts to supply the real refresh. */
export function setPresetLibraryRefresher(fn: (activePreset: Preset | null) => void): void {
  refresh = fn;
}

/**
 * Redraws the preset dropdown, the library list and the active-preset UI.
 * Pass the preset that should end up selected, or null to leave it alone.
 */
export function requestPresetLibraryRefresh(activePreset: Preset | null = null): void {
  refresh?.(activePreset);
}

let renderUi: ((preset: Preset | null) => void) | null = null;

/** Called once by library.ts to supply the real renderer. */
export function setPresetUIRenderer(fn: (preset: Preset | null) => void): void {
  renderUi = fn;
}

/**
 * Redraws the library rows, the details pane for `preset` and the signal path
 * bar. Unlike requestPresetLibraryRefresh it does not refilter first.
 */
export function requestPresetUIRender(preset: Preset | null): void {
  renderUi?.(preset);
}

const renderListeners = new Set<() => void>();

/**
 * For another view of the preset library (Tones' Your Presets): `listener` runs each time
 * the library is redrawn, which is whenever the presets, the folders, the favourites, the
 * ratings or the active preset may have changed.
 */
export function onPresetUIRendered(listener: () => void): void {
  renderListeners.add(listener);
}

/** Called by library.ts at the end of its redraw. */
export function notifyPresetUIRendered(): void {
  renderListeners.forEach((listener) => listener());
}
