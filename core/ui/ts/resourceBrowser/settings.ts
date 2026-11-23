/**
 * The app-setting keys the browser persists under, and the sizing constants
 * behind the virtualised folder list.
 */

export const RESOURCE_FAVORITES_SETTING = "resources.favorites";

// The Library tab groups models from one pack unless this is false (absent means on).
export const LIBRARY_GROUP_PACKS_SETTING = "resources.library.groupPacks";

export const FOLDER_ROOTS_SETTING = "resources.folderBrowser.roots";

export const FOLDER_ACTIVE_ROOT_SETTING = "resources.folderBrowser.activeRootId";

// Last browsed folder per effect role (e.g. "ir-cab", "nam-amp") so an IR Cab
// picker reopens where IR Cab browsing left off, independent of NAM browsing.
export const FOLDER_LAST_LOCATIONS_SETTING = "resources.folderBrowser.lastLocationByContext";

export const DEFAULT_RESOURCE_CONTEXT_KEY = "default";

/// A slot after a node's first browses as a context of its own, "<role>#<index>", so its
/// next/prev and remembered folder are its own. Where it has none yet it starts from the role's.
const SLOT_CONTEXT_SEPARATOR = "#";

export function withResourceSlotContext(roleContextKey: string, resourceIndex: number): string {
  return resourceIndex > 0 ? `${roleContextKey}${SLOT_CONTEXT_SEPARATOR}${resourceIndex}` : roleContextKey;
}

export function resourceRoleContextKey(contextKey: string): string {
  return contextKey.split(SLOT_CONTEXT_SEPARATOR)[0];
}

export const FOLDER_VIRTUAL_GAP = 2;

export const FOLDER_VIRTUAL_OVERSCAN = 6;

export const FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT = 36;

/** A file row: the 36 px picture slot plus its 6 px insets (resource-browser.css). */
export const FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT = 48;
