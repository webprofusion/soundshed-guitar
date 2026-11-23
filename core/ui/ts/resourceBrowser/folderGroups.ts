/**
 * The Folder tab's tones: files in one folder that a lookup ("Look up details", or a single
 * file's "Find on Tone3000") or their library entries say came from the same tone, drawn as one
 * collapsible group with the tone's picture, creator and how many of its models are here, as the
 * Library tab groups a pack (libraryPacks.ts) and the Tone3000 tab draws a tone.
 *
 * Groups are worked out on every draw from what is known about each file, so a file a later
 * lookup matches to a tone already shown joins that tone's group. A tone with one file here is
 * drawn as a plain row: its picture is on the row already. A group sits where its first file
 * would, so the list keeps its order.
 */

import { formatGearDescription } from "./helpers.js";
import { groupCountLabel, renderGroupHeader } from "./groupHeader.js";
import { libraryThumbnailAttr, libraryThumbnailSlot } from "./libraryThumbnails.js";
import type { FolderFileGroup, FolderListingFile, FolderVirtualEntry } from "./types.js";
import type { LibraryResource } from "../types.js";
import { escapeHtml } from "../utils.js";

export type FolderListItem =
  | { kind: "file"; file: FolderListingFile }
  | { kind: "group"; group: FolderFileGroup };

/**
 * The files in list order, those sharing a tone gathered into a group at the first one's place.
 * `detailsFor` gives a file's tone details (a lookup's, or its library entry's), if any.
 */
export function groupFolderFiles(
  files: FolderListingFile[],
  detailsFor: (file: FolderListingFile) => Record<string, string> | undefined,
): FolderListItem[] {
  const byTone = new Map<string, FolderFileGroup>();
  const items: Array<FolderListItem & { toneKey?: string }> = [];
  for (const file of files) {
    const details = detailsFor(file);
    const toneId = details?.toneId?.trim();
    if (!toneId) {
      items.push({ kind: "file", file });
      continue;
    }
    const key = `tone:${toneId}`;
    const existing = byTone.get(key);
    if (existing) {
      existing.files.push(file);
      // A member that knows more of the tone (its title, its creator) fills in what the first did not.
      for (const [field, value] of Object.entries(details ?? {})) {
        if (value && !existing.metadata[field]) {
          existing.metadata[field] = value;
        }
      }
      existing.title ||= (existing.metadata.toneTitle || existing.metadata.groupName || "").trim();
      continue;
    }
    const group: FolderFileGroup = {
      key,
      title: (details?.toneTitle || details?.groupName || "").trim(),
      metadata: { ...details },
      files: [file],
    };
    byTone.set(key, group);
    items.push({ kind: "group", group });
  }
  // A tone with one file here is just that file.
  return items.map((item) => (item.kind === "group" && item.group.files.length < 2 ? { kind: "file", file: item.group.files[0] } : item));
}

export interface FolderGroupOptions {
  /** The user's own open/closed choice for a group, when they made one. */
  toggled: Map<string, boolean>;
  /** A search is narrowing the list: every group opens, as the Library's packs do. */
  openAll: boolean;
  selectedPath: string;
}

/** The list's virtual entries: a group's header, then its files while it is open. */
export function folderVirtualEntries(items: FolderListItem[], options: FolderGroupOptions): FolderVirtualEntry[] {
  const entries: FolderVirtualEntry[] = [];
  for (const item of items) {
    if (item.kind === "file") {
      entries.push({ key: `file:${item.file.path}`, kind: "file", file: item.file });
      continue;
    }
    const { group } = item;
    const holdsSelected = Boolean(options.selectedPath) && group.files.some((file) => file.path === options.selectedPath);
    const open = options.toggled.get(group.key) ?? (holdsSelected || options.openAll);
    entries.push({ key: `group:${group.key}`, kind: "group", group, open, holdsSelected });
    if (open) {
      group.files.forEach((file) => entries.push({ key: `file:${file.path}`, kind: "file", file, groupMember: true }));
    }
  }
  return entries;
}

/** The files in the order the list draws them (a group's together), which next/prev follows. */
export function folderFilesInListOrder(items: FolderListItem[]): FolderListingFile[] {
  return items.flatMap((item) => (item.kind === "file" ? [item.file] : item.group.files));
}

/**
 * The tone's title from its models' library names when no title was saved: an import names a
 * model "<tone> - <model>", so it is what the names share up to their last " - ". "" when the
 * names are missing or share no such part.
 */
export function sharedTonePrefix(names: string[]): string {
  const known = names.map((name) => name.trim()).filter(Boolean);
  if (known.length < 2) {
    return "";
  }
  let prefix = known[0];
  for (const name of known.slice(1)) {
    let length = 0;
    while (length < prefix.length && length < name.length && prefix[length].toLowerCase() === name[length].toLowerCase()) {
      length += 1;
    }
    prefix = prefix.slice(0, length);
  }
  // Every name identical: the tone is the part before its own last separator.
  const cut = prefix.lastIndexOf(" - ");
  return cut > 0 ? prefix.slice(0, cut).trim() : "";
}

/**
 * A group's header: the tone's picture (a member's library thumbnail, else the tone's own), its
 * title, "gear · by creator · N models", and the chevron on the right.
 */
export function renderFolderGroupHeader(
  entry: Extract<FolderVirtualEntry, { kind: "group" }>,
  libraryEntryFor: (file: FolderListingFile) => LibraryResource | undefined,
): string {
  const { group, open, holdsSelected } = entry;
  const resourceType = group.files[0]?.resourceType ?? "nam";
  const members = group.files.map(libraryEntryFor);
  const pictured = members.find((resource) => Boolean(resource));
  // Who made it, as the tone names them: not "tone3000", the provider a bare entry falls back to.
  const creator = (group.metadata.creatorName || group.metadata.authorUsername || group.metadata.modeledBy || group.metadata.creator || "").trim();
  const title = group.title
    || sharedTonePrefix(members.map((resource) => resource?.name ?? ""))
    || formatGearDescription(group.metadata.gearMake, group.metadata.gearModel)
    || group.files[0]?.name
    || "";
  const header = renderGroupHeader({
    key: group.key,
    title,
    meta: [group.metadata.tone3000Category || group.metadata.gear || "", creator ? `by ${creator}` : "", groupCountLabel(group.files.length, resourceType)],
    open,
    thumbAttr: pictured ? libraryThumbnailAttr(resourceType, pictured) : "",
    thumbSlot: libraryThumbnailSlot(resourceType, pictured, group.metadata.imageUrl),
  });
  return `
    <div class="resource-browser-folder-entry resource-browser-folder-group-entry${open ? " is-open" : ""}${holdsSelected ? " holds-selected" : ""}"
         data-kind="group" data-group-key="${escapeHtml(group.key)}">
      ${header}
    </div>`;
}
