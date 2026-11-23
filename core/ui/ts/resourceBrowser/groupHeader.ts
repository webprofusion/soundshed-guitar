/**
 * The header of a group of models in the resource browser's local lists: a Library pack
 * (libraryPacks.ts) and a Folder tab tone (folderGroups.ts). Drawn as the Tone3000 tab draws a
 * tone (tone3000Tab.ts): the picture slot on the left where every row has it, so pictures line
 * up whether or not a row is in a group; the title over a meta line (creator, model count); and
 * the down chevron on the right, turned up while the group is open.
 */

import { escapeHtml } from "../utils.js";

export interface GroupHeaderParts {
  /** What a click on the header toggles (`data-pack-key`). */
  key: string;
  title: string;
  /** The meta line's text parts, drawn dot separated as a row's are. */
  meta: string[];
  open: boolean;
  /** ` data-thumb-for="…"`, so a picture arriving later finds the header (libraryThumbnails.ts). */
  thumbAttr: string;
  /** The picture slot's markup. */
  thumbSlot: string;
}

/** "3 models", "1 IR". */
export function groupCountLabel(count: number, resourceType: string | undefined): string {
  const noun = resourceType === "ir" ? "IR" : "model";
  return `${count} ${noun}${count === 1 ? "" : "s"}`;
}

/** The one creator every member names, else "" (a mixed group names nobody). */
export function sharedCreator(creators: string[]): string {
  const named = creators.map((creator) => creator.trim()).filter(Boolean);
  return named.length && named.every((creator) => creator === named[0]) ? named[0] : "";
}

export function renderGroupHeader(parts: GroupHeaderParts): string {
  const label = parts.open ? "Hide models" : "Show models";
  const meta = parts.meta
    .map((part) => part.trim())
    .filter(Boolean)
    .map((part) => `<span>${escapeHtml(part)}</span>`)
    .join("");
  return `
    <button class="resource-browser-pack-header" type="button" data-pack-key="${escapeHtml(parts.key)}"
            aria-expanded="${parts.open ? "true" : "false"}" title="${escapeHtml(`${parts.title} (${label.toLowerCase()})`)}"${parts.thumbAttr}>
      ${parts.thumbSlot}
      <span class="resource-browser-pack-info">
        <span class="resource-browser-pack-title">${escapeHtml(parts.title)}</span>
        ${meta ? `<span class="resource-browser-pack-meta">${meta}</span>` : ""}
      </span>
      <span class="resource-browser-pack-expand" aria-hidden="true">
        <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"></polyline></svg>
      </span>
    </button>`;
}
