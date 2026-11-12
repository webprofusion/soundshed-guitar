/**
 * Every element the preset toolbar, library popover and setlist panel talk to,
 * looked up once.
 *
 * The lookups run at import time, which is safe: dist/main.js is a module script
 * at the end of <body>, so the document is parsed and these elements are all in
 * the markup assembled from ui-components/.
 */

/** The library search box; its value drives the filtered preset list. */
export const presetSearchElement = document.getElementById("preset-search") as HTMLInputElement | null;

/** The heart toggle in the preset toolbar. */
export const presetFavoriteToggle = document.getElementById("preset-favorite");

export const presetChooserLabel = document.getElementById("preset-chooser-label") as HTMLButtonElement | null;

export const prevPresetBtn = document.getElementById("prev-preset");

export const nextPresetBtn = document.getElementById("next-preset");

export const presetUndoBtn = document.getElementById("preset-undo") as HTMLButtonElement | null;

export const presetRedoBtn = document.getElementById("preset-redo") as HTMLButtonElement | null;

/** The same two steps as entries in the preset actions menu, for compact density where the buttons are hidden. */
export const presetHistoryBackItem = document.getElementById("preset-history-back-item") as HTMLButtonElement | null;

export const presetHistoryForwardItem = document.getElementById("preset-history-forward-item") as HTMLButtonElement | null;

export const randomPresetBtn = document.getElementById("preset-random-btn");

export const presetExtraActionsBtn = document.getElementById("preset-extra-actions-btn") as HTMLButtonElement | null;

export const presetExtraActionsMenu = document.getElementById("preset-extra-actions-menu") as HTMLDivElement | null;

export const presetSelector = document.getElementById("preset-selector");

export const presetSelectorStatus = document.getElementById("preset-selector-status") as HTMLElement | null;

export const presetLibraryPopover = document.getElementById("preset-library-popover");

export const presetLibraryCloseButton = document.getElementById("preset-library-close-btn") as HTMLButtonElement | null;

export const presetControlBar = presetLibraryPopover?.closest<HTMLElement>(".control-bar") ?? null;

export const presetLibraryTabs = document.querySelector(".preset-library-tabs") as HTMLElement | null;

export const presetLibraryPresetsPanel = document.getElementById("preset-library-presets-panel") as HTMLElement | null;

export const presetLibraryMultiRigTab = document.getElementById("preset-lib-tab-multi-rig") as HTMLButtonElement | null;

export const presetLibraryPresetsTab = document.getElementById("preset-lib-tab-presets") as HTMLButtonElement | null;

export const presetLibraryMultiRigPanel = document.getElementById("preset-library-multi-rig-panel") as HTMLElement | null;

/** The library's preset rows; rendered by views.ts, re-rendered on every filter. */
export const presetListElement = document.getElementById("preset-list");

/** The library's folder tree, rendered alongside the rows. */
export const presetFolderTreeElement = document.getElementById("preset-folder-tree");

export const presetFolderNameInput = document.getElementById("preset-folder-name") as HTMLInputElement | null;

export const presetFolderAddButton = document.getElementById("preset-folder-add") as HTMLButtonElement | null;

export const presetFolderRenameButton = document.getElementById("preset-folder-rename") as HTMLButtonElement | null;

export const presetFolderDeleteButton = document.getElementById("preset-folder-delete") as HTMLButtonElement | null;

export const presetExportFolderButton = document.getElementById("preset-export-folder-btn") as HTMLButtonElement | null;

export const setlistNameInput = document.getElementById("setlist-name-input") as HTMLInputElement | null;

export const setlistBankInput = document.getElementById("setlist-bank-input") as HTMLInputElement | null;

export const setlistAddButton = document.getElementById("setlist-add-btn");

export const setlistListElement = document.getElementById("setlist-list");

export const setlistSlotsElement = document.getElementById("setlist-slots");

export const setlistEditorHeader = document.getElementById("setlist-editor-header");

export const setlistCollapsible = document.getElementById("setlist-collapsible");

export const setlistToggle = document.getElementById("setlist-toggle");

export const setlistPanel = document.getElementById("setlist-panel");

export const presetExportSessionButton = document.getElementById("preset-export-session-btn") as HTMLButtonElement | null;

export const presetExitSessionButton = document.getElementById("preset-exit-session-btn") as HTMLButtonElement | null;
