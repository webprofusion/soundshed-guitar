/**
 * DOM roots shared across the preset modules.
 *
 * The lookups run at import time, which is safe: dist/main.js is a module
 * script at the end of <body>, so the document is parsed and these elements
 * are all present in the markup assembled from ui-components/.
 */

/** The library search box; its value drives the filtered preset list. */
export const presetSearchElement = document.getElementById("preset-search") as HTMLInputElement | null;

/** The heart toggle in the preset toolbar. */
export const presetFavoriteToggle = document.getElementById("preset-favorite");
