/**
 * The Library tab's pack grouping: models that came together (a Tone3000 tone, or the factory
 * archive) are drawn as one collapsible group, so a 37-model
 * pack is one row until it is opened. On by default; the toolbar's pack button turns it off,
 * and the choice is kept as an app setting.
 *
 * A group of one is drawn as a plain row. The group holding the loaded model starts open, as
 * does every group while a search or the Favourites filter narrows the list.
 */

import { getAppSetting, updateAppSetting } from "../appSettingsStore.js";
import type { LibraryResource } from "../types.js";
import { groupCountLabel, renderGroupHeader, sharedCreator } from "./groupHeader.js";
import { getResourceCreator } from "./helpers.js";
import { isThumbnailCandidate, libraryThumbnailAttr, libraryThumbnailSlot } from "./libraryThumbnails.js";
import { LIBRARY_GROUP_PACKS_SETTING } from "./settings.js";

export interface LibraryPack {
  key: string;
  title: string;
}

/**
 * The pack a resource came from: its Tone3000 tone or its factory archive. A file the user
 * brought in has none, whatever folder it is in: one folder often holds every capture they
 * own, which would fold the whole local library behind one header.
 */
export function libraryPackOf(resource: LibraryResource): LibraryPack | null {
  const metadata = resource.metadata ?? {};
  const toneId = (metadata.toneId || metadata.groupId || "").trim();
  if (toneId) {
    const title = (metadata.toneTitle || metadata.groupName || "").trim();
    return { key: `tone:${toneId}`, title: title || `Tone3000 pack ${toneId}` };
  }
  const archive = (metadata.factoryArchiveKey || "").trim();
  if (archive) {
    return { key: `archive:${archive}`, title: archive.replace(/[-_]+/g, " ") };
  }
  return null;
}

/** A model's name with its pack's title taken off the front, where the name repeats it. */
export function nameWithinPack(name: string, pack: LibraryPack): string {
  const lower = name.toLowerCase();
  const prefix = pack.title.toLowerCase();
  if (!prefix || !lower.startsWith(prefix)) {
    return name;
  }
  const rest = name.slice(pack.title.length).replace(/^[\s\-–_|:·,]+/u, "").trim();
  return rest || name;
}

export interface LibraryEntry {
  /** Absent for a model drawn on its own row. */
  pack: LibraryPack | null;
  items: LibraryResource[];
}

/**
 * Groups resources by pack, keeping each group's members in the order given. Entries holding
 * a favourite come first, then by title (the pack's, or the model's for a lone row).
 */
export function groupLibraryByPack(
  resources: LibraryResource[],
  isFavorite: (resourceId: string) => boolean,
): LibraryEntry[] {
  const byKey = new Map<string, LibraryEntry>();
  const entries: LibraryEntry[] = [];
  resources.forEach((resource) => {
    const pack = libraryPackOf(resource);
    const existing = pack ? byKey.get(pack.key) : undefined;
    if (existing) {
      existing.items.push(resource);
      return;
    }
    const entry: LibraryEntry = { pack, items: [resource] };
    entries.push(entry);
    if (pack) {
      byKey.set(pack.key, entry);
    }
  });

  // A pack of one is just a model.
  entries.forEach((entry) => {
    if (entry.items.length < 2) {
      entry.pack = null;
    }
  });

  const label = (entry: LibraryEntry): string => entry.pack?.title ?? (entry.items[0].name || entry.items[0].id);
  return entries.sort((a, b) => {
    const aFav = a.items.some((item) => isFavorite(item.id));
    const bFav = b.items.some((item) => isFavorite(item.id));
    if (aFav !== bFav) {
      return aFav ? -1 : 1;
    }
    return label(a).localeCompare(label(b));
  });
}

export function isPackGroupingEnabled(): boolean {
  return getAppSetting(LIBRARY_GROUP_PACKS_SETTING) !== false;
}

/**
 * What the Library tab keeps between draws: whether grouping is on, and the groups the user
 * opened or closed. It finds its toolbar button and binds its clicks on first use.
 */
export class LibraryPackGrouping {
  /** A group the user opened (true) or closed (false); otherwise the default rule applies. */
  private readonly toggled = new Map<string, boolean>();
  private bound = false;

  constructor(private readonly redraw: () => void) {}

  private bind(list: HTMLElement): void {
    if (this.bound) {
      return;
    }
    this.bound = true;
    document.getElementById("resource-browser-library-group")?.addEventListener("click", () => {
      updateAppSetting(LIBRARY_GROUP_PACKS_SETTING, !isPackGroupingEnabled());
      this.redraw();
    });
    list.addEventListener("click", (event) => {
      const header = (event.target as HTMLElement | null)?.closest<HTMLElement>(".resource-browser-pack-header");
      const key = header?.dataset.packKey;
      if (header && key) {
        this.toggled.set(key, header.getAttribute("aria-expanded") !== "true");
        this.redraw();
      }
    });
  }

  private syncButton(enabled: boolean): void {
    const button = document.getElementById("resource-browser-library-group");
    button?.classList.toggle("is-active", enabled);
    button?.setAttribute("aria-pressed", String(enabled));
  }

  /** The resources in the order the list draws them, which next/prev then follows. */
  order(resources: LibraryResource[], isFavorite: (resourceId: string) => boolean): LibraryResource[] {
    return isPackGroupingEnabled()
      ? groupLibraryByPack(resources, isFavorite).flatMap((entry) => entry.items)
      : resources;
  }

  /**
   * The list's markup. `renderRow` draws one model; it is given a shorter title for a model
   * inside its pack's group, where the pack's name is already in the header.
   */
  render(
    list: HTMLElement,
    resources: LibraryResource[],
    options: { selectedId: string; openAll: boolean; isFavorite: (resourceId: string) => boolean; resourceType?: string },
    renderRow: (resource: LibraryResource, titleInPack?: string) => string,
  ): string {
    this.bind(list);
    const enabled = isPackGroupingEnabled();
    this.syncButton(enabled);
    if (!enabled) {
      return resources.map((resource) => renderRow(resource)).join("");
    }

    return groupLibraryByPack(resources, options.isFavorite)
      .map((entry) => {
        const pack = entry.pack;
        if (!pack) {
          return renderRow(entry.items[0]);
        }
        const holdsSelected = entry.items.some((item) => item.id === options.selectedId);
        const open = this.toggled.get(pack.key) ?? (holdsSelected || options.openAll);
        const rows = open
          ? entry.items.map((item) => renderRow(item, nameWithinPack(item.name?.trim() || item.id, pack))).join("")
          : "";
        // A Tone3000 tone's group shows the tone's picture, as its models do (libraryThumbnails.ts).
        const pictured = options.resourceType ? entry.items.find((item) => isThumbnailCandidate(item)) : undefined;
        const thumbAttr = pictured ? libraryThumbnailAttr(options.resourceType!, pictured) : "";
        // Drawn as the Tone3000 tab draws a tone: picture, title over "by … · N models", and the
        // chevron on the right, so the pictures line up with the rows outside any group.
        const header = renderGroupHeader({
          key: pack.key,
          title: pack.title,
          meta: [
            (() => {
              const creator = sharedCreator(entry.items.map((item) => getResourceCreator(item)));
              return creator ? `by ${creator}` : "";
            })(),
            groupCountLabel(entry.items.length, options.resourceType),
          ],
          open,
          thumbAttr,
          thumbSlot: libraryThumbnailSlot(options.resourceType ?? "", pictured),
        });
        return `
          <div class="resource-browser-pack${open ? " is-open" : ""}${holdsSelected ? " holds-selected" : ""}">
            ${header}
            ${open ? `<div class="resource-browser-pack-items">${rows}</div>` : ""}
          </div>`;
      })
      .join("");
  }
}
