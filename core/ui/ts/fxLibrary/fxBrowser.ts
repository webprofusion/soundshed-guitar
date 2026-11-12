/**
 * The FX library's browser: a rail of tabs (Favourites, Recent, then each category), a search
 * field and the effects in the open tab. The docked library (fxSelector.ts) and the chooser
 * a + in the chain opens (signalPath/addEffectDropdown.ts) are each one of these, mounted in
 * their own markup.
 *
 * Every effect can be dragged into the chain: a press announces "fx-pointer-drag-start" and
 * the signal path, which owns the graph, decides what the drop means. While the host has a
 * pick (replacing a node, or adding at a +), a click or Enter on an effect chooses it too.
 * The star on each effect marks it a favourite.
 */

import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon } from "../iconAssets.js";
import { getCustomLayout } from "../layoutRenderer.js";
import { escapeHtml, safeImageDataUrl } from "../utils.js";
import {
  CATEGORY_METADATA,
  fxItemIdentity,
  fxPayloadIdentity,
  getFxLibraryItems,
  getOrderedFxCategories,
  isSameFxItem,
  type FxItemIdentity,
  type FxLibraryItem,
  type FxPointerDragPayload,
} from "./catalog.js";
import {
  FX_MARKS_CHANGED_EVENT,
  fxItemKey,
  getFxFavoriteKeys,
  getFxRecentKeys,
  recordFxRecent,
  toggleFxFavorite,
} from "./favorites.js";

export const FX_FAVORITES_TAB = "favorites";
export const FX_RECENT_TAB = "recent";

/** A click on an effect chooses it, for a node being replaced or a + being added at. */
export type FxPick = {
  /** The effect in the node being replaced: marked "Current". */
  current?: FxItemIdentity;
  onPick: (payload: FxPointerDragPayload) => void;
};

export type FxBrowserOptions = {
  categories: HTMLElement;
  list: HTMLElement;
  search: HTMLInputElement | null;
  /** The tab open at first; it falls back to the first category with something in it. */
  tab?: string;
  excludeTypes?: string[];
  /** The host's pick, if it has one now; without one, effects are only dragged. */
  getPick: () => FxPick | null;
  /** Rendered with no categories at all: the catalog has not arrived yet. */
  onEmptyCatalog?: () => void;
  /** The open tab changed, by a click on the rail. */
  onTabChange?: (tab: string) => void;
};

const STAR_SVG = `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"></polygon></svg>`;
const CLOCK_SVG = `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"></circle><polyline points="12 7 12 12 15 14"></polyline></svg>`;

const MARK_TABS: Record<string, { name: string; icon: string; color: string; empty: string }> = {
  [FX_FAVORITES_TAB]: {
    name: "Favourites",
    icon: STAR_SVG,
    color: "var(--color-accent)",
    empty: "Star an effect to keep it here.",
  },
  [FX_RECENT_TAB]: {
    name: "Recent",
    icon: CLOCK_SVG,
    color: "var(--text-secondary)",
    empty: "Effects you add to the chain show up here.",
  },
};

function categoryColor(categoryId: string): string {
  return CATEGORY_METADATA[categoryId]?.color || "#808080";
}

function encodeDatasetJson(value: unknown): string {
  return encodeURIComponent(JSON.stringify(value ?? {}));
}

/** The items a list of keys names, in the list's order; keys for deleted items are skipped. */
function itemsForKeys(keys: string[], items: FxLibraryItem[]): FxLibraryItem[] {
  const byKey = new Map(items.map((item) => [fxItemKey(fxItemIdentity(item)), item]));
  return keys.map((key) => byKey.get(key)).filter((item): item is FxLibraryItem => Boolean(item));
}

function itemThumbnail(effect: FxLibraryItem): string {
  const layout = effect.blendId ? (getCustomLayout(effect.type, effect.blendId) ?? getCustomLayout(effect.type)) : getCustomLayout(effect.type);
  const url = safeImageDataUrl(layout?.thumbnailDataUrl ?? effect.thumbnailDataUrl);
  return url ? `<img src="${escapeHtml(url)}" alt="" aria-hidden="true" class="fx-item-thumb" />` : getFxEffectIcon(effect.type);
}

/** One effect's card. Names and ids come from blends, custom effects and archives, so all of it is escaped. */
function renderFxItem(effect: FxLibraryItem, pick: FxPick | null, favorite: boolean): string {
  const resourceBadge = effect.requiresResource && !effect.customEffectId
    ? `<span class="fx-item-badge">${getBadgeIcon("resource", `Requires ${effect.resourceType}`)}</span>`
    : "";
  const blendBadge = effect.blendId
    ? `<span class="fx-item-badge">${getBadgeIcon("blend", "Custom blend")}</span>`
    : "";
  const compositeBadge = effect.compositeId
    ? `<span class="fx-item-badge" title="Composite channel strip">&#x1f4e6;</span>`
    : "";
  const customEffectBadge = effect.customEffectId
    ? `<span class="fx-item-badge" title="Saved custom effect">Custom</span>`
    : "";
  // With a pick, every effect is a button that chooses it.
  const isCurrent = Boolean(pick?.current && isSameFxItem(fxItemIdentity(effect), pick.current));
  const chooseAttrs = pick ? ` role="button" tabindex="0"${isCurrent ? ' aria-current="true"' : ""}` : "";
  const currentBadge = isCurrent ? `<span class="fx-item-current">Current</span>` : "";
  const favoriteLabel = favorite ? `Remove ${effect.displayName} from favourites` : `Add ${effect.displayName} to favourites`;

  return `
        <div class="fx-item${isCurrent ? " is-current" : ""}${favorite ? " is-favorite" : ""}"${chooseAttrs}
          data-effect-type="${escapeHtml(effect.type)}"
          data-blend-id="${escapeHtml(effect.blendId ?? "")}"
          data-blend-category="${escapeHtml(effect.blendCategory ?? "")}"
          data-composite-id="${escapeHtml(effect.compositeId ?? "")}"
          data-custom-effect-id="${escapeHtml(effect.customEffectId ?? "")}"
          data-custom-effect-resource-type="${escapeHtml(effect.moduleResourceType ?? "")}"
          data-custom-effect-resource-id="${escapeHtml(effect.moduleResourceId ?? "")}"
          data-custom-effect-default-params="${escapeHtml(effect.customEffectId ? encodeDatasetJson(effect.defaultParams ?? {}) : "")}"
          data-effect-category="${escapeHtml(effect.category)}"
          style="--category-color: ${escapeHtml(categoryColor(effect.category))}">
      <div class="fx-item-icon">${itemThumbnail(effect)}</div>
      <div class="fx-item-info">
        <div class="fx-item-name">${escapeHtml(effect.displayName)}</div>
        <div class="fx-item-type">${escapeHtml(CATEGORY_METADATA[effect.category]?.name ?? effect.category)}</div>
      </div>
      ${currentBadge}${resourceBadge}${blendBadge}${customEffectBadge}${compositeBadge}
      <button type="button" class="fx-item-fav" aria-pressed="${favorite}" aria-label="${escapeHtml(favoriteLabel)}" title="${favorite ? "Remove from favourites" : "Add to favourites"}">${STAR_SVG}</button>
    </div>
  `;
}

/** What an item carries into the chain, read back from its data attributes. */
export function readFxItemPayload(el: HTMLElement): FxPointerDragPayload | null {
  const effectType = el.dataset.effectType;
  if (!effectType) return null;
  const name = el.querySelector(".fx-item-name")?.textContent ?? undefined;

  let customEffect: FxPointerDragPayload["customEffect"];
  const customEffectId = el.dataset.customEffectId;
  if (customEffectId) {
    let defaultParams: Record<string, number> = {};
    try {
      defaultParams = JSON.parse(decodeURIComponent(el.dataset.customEffectDefaultParams ?? "%7B%7D")) as Record<string, number>;
    } catch {
      defaultParams = {};
    }
    customEffect = {
      customEffectId,
      name: name ?? "Custom Effect",
      category: el.dataset.effectCategory ?? "utility",
      moduleResourceType: el.dataset.customEffectResourceType ?? "",
      moduleResourceId: el.dataset.customEffectResourceId ?? "",
      defaultParams,
    };
  }

  return {
    effectType,
    blendId: el.dataset.blendId || undefined,
    blendName: name,
    blendCategory: el.dataset.blendCategory || undefined,
    customEffect,
  };
}

/** Favourites when there are any, else Recent when there is anything in it, else none. */
export function preferredMarkTab(excludeTypes?: string[]): string | null {
  const items = getFxLibraryItems({ excludeTypes });
  if (itemsForKeys(getFxFavoriteKeys(), items).length > 0) return FX_FAVORITES_TAB;
  if (itemsForKeys(getFxRecentKeys(), items).length > 0) return FX_RECENT_TAB;
  return null;
}

export class FxBrowser {
  private tab: string;
  private searchFilter = "";
  private readonly onMarksChanged = (): void => this.render();

  constructor(private readonly options: FxBrowserOptions) {
    this.tab = options.tab ?? "";
    options.search?.addEventListener("input", () => {
      this.searchFilter = (options.search?.value ?? "").toLowerCase();
      this.renderList();
    });
    options.search?.addEventListener("keydown", (event) => this.onSearchKey(event));
    options.categories.addEventListener("click", (event) => {
      const tab = (event.target as HTMLElement | null)?.closest<HTMLElement>(".fx-category")?.dataset.category;
      if (tab) {
        this.setTab(tab);
        options.onTabChange?.(tab);
      }
    });
    this.bindListHandlers();
    window.addEventListener(FX_MARKS_CHANGED_EVENT, this.onMarksChanged);
  }

  /** Stops following favourite changes; for a browser whose markup is going away. */
  destroy(): void {
    window.removeEventListener(FX_MARKS_CHANGED_EVENT, this.onMarksChanged);
  }

  getTab(): string {
    return this.tab;
  }

  setTab(tab: string): void {
    this.tab = tab;
    this.render();
  }

  clearSearch(): void {
    this.searchFilter = "";
    if (this.options.search) {
      this.options.search.value = "";
    }
  }

  render(): void {
    this.renderTabs();
    this.renderList();
  }

  private items(): FxLibraryItem[] {
    return getFxLibraryItems({ excludeTypes: this.options.excludeTypes });
  }

  private renderTabs(): void {
    const items = this.items();
    const categories = getOrderedFxCategories(items);
    if (categories.length === 0) {
      this.options.onEmptyCatalog?.();
    }
    const counts = new Map<string, number>();
    items.forEach((item) => counts.set(item.category, (counts.get(item.category) ?? 0) + 1));
    counts.set(FX_FAVORITES_TAB, itemsForKeys(getFxFavoriteKeys(), items).length);
    counts.set(FX_RECENT_TAB, itemsForKeys(getFxRecentKeys(), items).length);

    // The category that was open has nothing in it now; open the first one that has. Not
    // before the catalog arrives, or the tab asked for would be lost to that first render.
    if (categories.length > 0 && !MARK_TABS[this.tab] && !categories.includes(this.tab)) {
      this.tab = categories[0];
    }

    const tab = (id: string, name: string, icon: string, color: string): string => `
      <div class="fx-category${this.tab === id ? " active" : ""}${MARK_TABS[id] ? " fx-category-mark" : ""}"
           data-category="${escapeHtml(id)}"
           style="--category-color: ${escapeHtml(color)}">
        ${icon}
        <span class="fx-category-name">${escapeHtml(name)}</span>
        <span class="fx-category-count">${counts.get(id) ?? 0}</span>
      </div>
    `;
    this.options.categories.innerHTML = [
      ...Object.entries(MARK_TABS).map(([id, mark]) => tab(id, mark.name, `<span class="fx-category-glyph">${mark.icon}</span>`, mark.color)),
      `<div class="fx-category-divider" role="separator"></div>`,
      ...categories.map((id) => tab(id, CATEGORY_METADATA[id]?.name ?? id, getFxCategoryIcon(id), categoryColor(id))),
    ].join("");
  }

  /** What the list shows: the search's matches from every tab, or the open tab's effects. */
  private visibleItems(): FxLibraryItem[] {
    const items = this.items();
    if (this.searchFilter) {
      return items.filter((item) => [item.displayName, item.type, item.category, item.description ?? ""]
        .join(" ").toLowerCase().includes(this.searchFilter));
    }
    if (this.tab === FX_FAVORITES_TAB) return itemsForKeys(getFxFavoriteKeys(), items);
    if (this.tab === FX_RECENT_TAB) return itemsForKeys(getFxRecentKeys(), items);
    return items.filter((item) => item.category === this.tab);
  }

  renderList(): void {
    const list = this.options.list;
    const visible = this.visibleItems();
    if (visible.length === 0) {
      const message = this.searchFilter
        ? "No effects match your search"
        : MARK_TABS[this.tab]?.empty ?? "No effects in this category";
      list.innerHTML = `<div class="fx-selector-empty">${escapeHtml(message)}</div>`;
      return;
    }
    const pick = this.options.getPick();
    const favorites = new Set(getFxFavoriteKeys());
    list.innerHTML = visible
      .map((item) => renderFxItem(item, pick, favorites.has(fxItemKey(fxItemIdentity(item)))))
      .join("");
  }

  private choose(el: HTMLElement): void {
    const pick = this.options.getPick();
    const payload = readFxItemPayload(el);
    if (!pick || !payload) return;
    const identity = fxPayloadIdentity(payload);
    if (!pick.current || !isSameFxItem(identity, pick.current)) {
      recordFxRecent(identity);
    }
    pick.onPick(payload);
  }

  /** Enter in the search chooses the first match; Down moves into the list. */
  private onSearchKey(event: KeyboardEvent): void {
    const first = this.options.list.querySelector<HTMLElement>(".fx-item");
    if (!first || !this.options.getPick()) return;
    if (event.key === "Enter" && this.searchFilter) {
      event.preventDefault();
      this.choose(first);
    } else if (event.key === "ArrowDown") {
      event.preventDefault();
      first.focus();
    }
  }

  /** Up and Down step through the effects of a pick, from the search field's side. */
  private stepFocus(from: HTMLElement, step: 1 | -1): void {
    const items = Array.from(this.options.list.querySelectorAll<HTMLElement>(".fx-item"));
    const next = items[items.indexOf(from) + step];
    if (next) {
      next.focus();
    } else if (step < 0) {
      this.options.search?.focus();
    }
  }

  private bindListHandlers(): void {
    const list = this.options.list;

    // pointerDrag.ts swallows the click that ends a drag, so only a plain click chooses.
    // The signal path picks the item up after a touch rests on it (touchHoldMs): the
    // list scrolls, and on a tablet it hangs under the chain, so the drag to the chain
    // starts upwards, which without the rest would scroll the list instead.
    list.addEventListener("pointerdown", (event: PointerEvent) => {
      if (!event.isPrimary || event.button !== 0) return;
      const target = event.target as HTMLElement | null;
      if (target?.closest(".fx-item-fav")) return;
      const el = target?.closest<HTMLElement>(".fx-item") ?? null;
      const payload = el ? readFxItemPayload(el) : null;
      if (!el || !payload) return;

      document.dispatchEvent(new CustomEvent("fx-pointer-drag-start", {
        detail: { source: el, pointerEvent: event, payload },
      }));
    });

    list.addEventListener("click", (event: MouseEvent) => {
      const target = event.target as HTMLElement | null;
      const el = target?.closest<HTMLElement>(".fx-item");
      if (!el) return;
      const star = target?.closest(".fx-item-fav");
      const payload = star ? readFxItemPayload(el) : null;
      if (payload) {
        // Every library redraws, this list too, so a star pressed from the keyboard is refocused.
        const hadFocus = star === document.activeElement;
        const identity = fxPayloadIdentity(payload);
        toggleFxFavorite(identity);
        if (hadFocus) {
          const redrawn = Array.from(list.querySelectorAll<HTMLElement>(".fx-item"))
            .find((item) => { const p = readFxItemPayload(item); return p && isSameFxItem(fxPayloadIdentity(p), identity); });
          redrawn?.querySelector<HTMLElement>(".fx-item-fav")?.focus();
        }
        return;
      }
      this.choose(el);
    });

    list.addEventListener("keydown", (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      const el = target?.closest<HTMLElement>(".fx-item");
      if (!el || target?.closest(".fx-item-fav") || !this.options.getPick()) return;
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        this.choose(el);
      } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        this.stepFocus(el, event.key === "ArrowDown" ? 1 : -1);
      }
    });
  }
}
