/**
 * Indirection for "the preset library changed, redraw it".
 *
 * The renderers live in presets.ts, because they need the dropdown, the
 * library list and the active-preset UI. Archive import finishes by asking for
 * that redraw — but importing presets.ts from presets/archive.ts (which
 * presets.ts itself imports) would create an import cycle between the two.
 *
 * So the seam states the relationship as it is: archive *requests* a refresh,
 * it does not own the rendering. presets.ts registers the implementation once,
 * at module load. Same pattern as signalPath/render.ts.
 */

import type { Preset } from "../types.js";

let refresh: ((activePreset: Preset | null) => void) | null = null;

/** Called once by presets.ts to supply the real refresh. */
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
