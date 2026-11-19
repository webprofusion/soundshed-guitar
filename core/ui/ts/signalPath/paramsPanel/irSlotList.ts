/**
 * The IR Cabinet's IR list, between its IR A and IR B slots: the list one slot's next/prev
 * steps through (the library, the folder it was last picked from, or a Tone3000 result set),
 * shown as rows that drag onto either slot. Each row's A and B buttons do the same without a
 * drag, for the keyboard and for a touch that would rather scroll.
 *
 * A row loaded into the other slot hands that slot the list too, so IR B's arrows then walk
 * the folder IR A's list was showing. Shown or hidden for every cabinet at once; which slot's
 * list, the filter and the scroll position are kept per node across the panel's re-renders.
 */

import { renderIcon } from "../../iconAssets.js";
import { showNotification } from "../../notifications.js";
import { beginPointerDrag, type PointerDragTarget } from "../../pointerDrag.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import { resourceBrowserModal } from "../../resourceBrowser.js";
import type { NavigationListItem, NavigationView } from "../../resourceBrowser/navigationListing.js";
import type { GraphNode, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendNodeResourceUpdate } from "../commands.js";
import { getNodeResourceAtIndex } from "../nodeResources.js";
import { requestNodeParamsPanel } from "../render.js";
import { buildResourceNavKey, pendingResourceNavSelections } from "../resourceControls.js";
import { resolveResourceContextKey, resolveResourceNavigationCategoryHint } from "../resourceContext.js";
import { nodeParamsPanelElement } from "../state.js";

/** Rows drawn at once; a filter narrows a longer list. */
const MAX_ROWS = 300;

const SLOT_NAMES = ["IR A", "IR B"] as const;

const VIEW_LABELS: Record<NavigationView, string> = { library: "Library", folder: "Folder", tone3000: "Tone3000" };

interface NodeListState {
  sourceSlot: 0 | 1;
  query: string;
  scrollTop: number;
  searchFocused: boolean;
}

let listOpen = false;
const stateByNode = new Map<string, NodeListState>();

function nodeState(nodeId: string): NodeListState {
  let state = stateByNode.get(nodeId);
  if (!state) {
    state = { sourceSlot: 0, query: "", scrollTop: 0, searchFocused: false };
    stateByNode.set(nodeId, state);
  }
  return state;
}

export function isIrSlotListOpen(): boolean {
  return listOpen;
}

function navOptions(node: GraphNode, preset: Preset, slot: number): { categoryHint?: string; contextKey: string } {
  return {
    categoryHint: resolveResourceNavigationCategoryHint(node, preset, "ir"),
    contextKey: resolveResourceContextKey(node, "ir", slot),
  };
}

function loadedSlots(node: GraphNode, item: NavigationListItem): number[] {
  return [0, 1].filter((slot) => {
    const current = getNodeResourceAtIndex(node, slot);
    return Boolean((item.resourceId && item.resourceId === current.id) || (item.filePath && item.filePath === current.filePath));
  });
}

function filteredItems(items: NavigationListItem[], query: string): NavigationListItem[] {
  const needle = query.trim().toLowerCase();
  return needle
    ? items.filter((item) => `${item.label} ${item.detail ?? ""}`.toLowerCase().includes(needle))
    : items;
}

function rowHtml(node: GraphNode, item: NavigationListItem): string {
  const key = escapeHtml(item.key);
  // A tone whose IRs have not been fetched is a row that fetches them, not one to load.
  if (item.tone3000 && !item.tone3000.modelId) {
    return `<li class="ir-slot-list-row is-tone" data-item-key="${key}">
      <button type="button" class="ir-slot-list-tone-btn" data-item-key="${key}" title="Show this tone's IRs">
        <span class="ir-slot-list-row-name">${escapeHtml(item.label)}</span>
        <span class="ir-slot-list-row-detail">${escapeHtml(item.detail ?? "")}</span>
      </button>
    </li>`;
  }
  const loaded = loadedSlots(node, item);
  const badges = loaded.map((slot) => `<span class="ir-slot-list-row-loaded">${slot === 0 ? "A" : "B"}</span>`).join("");
  const assign = (slot: number) => {
    const title = `Load into ${SLOT_NAMES[slot]}`;
    return `<button type="button" class="ir-slot-list-assign" data-item-key="${key}" data-slot="${slot}" title="${escapeHtml(title)}" aria-label="${escapeHtml(`${title}: ${item.label}`)}">${slot === 0 ? "A" : "B"}</button>`;
  };
  return `<li class="ir-slot-list-row${loaded.length ? " is-loaded" : ""}" data-item-key="${key}" title="Drag onto IR A or IR B">
    <span class="ir-slot-list-row-text">
      <span class="ir-slot-list-row-name">${escapeHtml(item.label)}</span>
      ${item.detail ? `<span class="ir-slot-list-row-detail">${escapeHtml(item.detail)}</span>` : ""}
    </span>
    ${badges}
    ${assign(0)}${assign(1)}
  </li>`;
}

function rowsHtml(node: GraphNode, items: NavigationListItem[], query: string): string {
  const shown = filteredItems(items, query);
  if (!shown.length) {
    return `<li class="ir-slot-list-empty">${items.length ? "No IRs match the filter." : "No IRs in this list."}</li>`;
  }
  const more = shown.length > MAX_ROWS
    ? `<li class="ir-slot-list-empty">Showing ${MAX_ROWS} of ${shown.length}. Filter to narrow the list.</li>`
    : "";
  return shown.slice(0, MAX_ROWS).map((item) => rowHtml(node, item)).join("") + more;
}

/**
 * The element drawn between the two slots: a button that shows the list, or the list. The
 * container takes `has-slot-list` while it is open (isIrSlotListOpen).
 */
export function buildIrSlotListHtml(node: GraphNode, preset: Preset): string {
  const nodeId = escapeHtml(node.id);
  if (!listOpen) {
    return `<div class="ir-slot-list" data-node-id="${nodeId}">
      <button type="button" class="ir-slot-list-toggle" aria-expanded="false" title="Show the IR list: drag an IR onto IR A or IR B" aria-label="Show IR list">${renderIcon("list", "ir-slot-list-toggle-icon")}</button>
    </div>`;
  }

  const state = nodeState(node.id);
  const listings = [0, 1].map((slot) => resourceBrowserModal.navigationLists.listing("ir", navOptions(node, preset, slot)));
  const listing = listings[state.sourceSlot];
  const sources = listings.map((sourceListing, slot) => {
    const active = slot === state.sourceSlot;
    return `<button type="button" role="tab" class="ir-slot-list-source${active ? " is-active" : ""}" data-source-slot="${slot}" aria-selected="${active}" title="The list ${SLOT_NAMES[slot]}'s arrows step through">${SLOT_NAMES[slot]} · ${VIEW_LABELS[sourceListing.view]}</button>`;
  }).join("");
  return `<div class="ir-slot-list is-open" data-node-id="${nodeId}">
    <div class="ir-slot-list-header">
      <div class="ir-slot-list-sources" role="tablist" aria-label="Whose list">${sources}</div>
      <button type="button" class="ir-slot-list-toggle is-close" aria-expanded="true" title="Hide the IR list" aria-label="Hide IR list">${renderIcon("close", "ir-slot-list-toggle-icon")}</button>
    </div>
    <div class="ir-slot-list-title" title="${escapeHtml(listing.title)}">${escapeHtml(listing.title)} <span class="ir-slot-list-count">${listing.items.length}</span></div>
    <input type="search" class="ir-slot-list-search" placeholder="Filter" aria-label="Filter the IR list" value="${escapeHtml(state.query)}" />
    <ul class="ir-slot-list-rows" aria-label="${escapeHtml(`${SLOT_NAMES[state.sourceSlot]}'s list`)}">${rowsHtml(node, listing.items, state.query)}</ul>
  </div>`;
}

interface SlotDropTarget extends PointerDragTarget {
  slot: number;
}

/** Loads a row into a slot: imports a Tone3000 model first, and hands the slot the row's list. */
async function assignToSlot(node: GraphNode, preset: Preset, sourceSlot: number, item: NavigationListItem, slot: number): Promise<void> {
  const sourceOptions = navOptions(node, preset, sourceSlot);
  let resourceId = item.resourceId ?? "";
  let filePath = item.filePath ?? "";
  if (!resourceId && !filePath && item.tone3000?.modelId) {
    const resolved = await resourceBrowserModal.navigationLists.resolveTone3000(sourceOptions, item.tone3000.toneId, item.tone3000.modelId);
    resourceId = resolved?.resourceId ?? "";
    filePath = resolved?.filePath ?? "";
  }
  if (!resourceId && !filePath) {
    return;
  }

  resourceBrowserModal.navigationLists.adopt("ir", sourceOptions, navOptions(node, preset, slot));
  const exposedResourceId = EffectTypeRegistry.get(node.type)?.exposedResources?.find((resource) => resource.resourceIndex === slot)?.resourceId;
  // As a next/prev step does: an arrow pressed before the engine answers steps from this.
  pendingResourceNavSelections.set(buildResourceNavKey(node.id, "ir", slot, exposedResourceId), { resourceId, filePath, at: performance.now() });
  sendNodeResourceUpdate(node.id, "ir", resourceId, filePath, slot, undefined, exposedResourceId);
  showNotification(SLOT_NAMES[slot], item.label);
}

function resolveSlotTarget(nodeId: string, event: PointerEvent): SlotDropTarget | null {
  const hit = document.elementFromPoint(event.clientX, event.clientY);
  // Side by side (irAbPanel.ts), anywhere on a slot's card lands on that slot.
  const element = hit?.closest<HTMLElement>(".ir-ab-slot")?.querySelector<HTMLElement>(".node-resource-selector[data-resource-index]")
    ?? hit?.closest<HTMLElement>(".cab-ir-resource-selectors .node-resource-selector[data-resource-index]");
  if (!element || element.dataset.nodeId !== nodeId) {
    return null;
  }
  const slot = Number.parseInt(element.dataset.resourceIndex ?? "", 10);
  return slot === 0 || slot === 1 ? { element, slot } : null;
}

export function bindIrSlotList(node: GraphNode, preset: Preset): void {
  const root = nodeParamsPanelElement?.querySelector<HTMLElement>(":is(.cab-ir-resource-selectors, .ir-ab-grid) > .ir-slot-list");
  if (!root) {
    return;
  }
  const state = nodeState(node.id);

  root.querySelectorAll<HTMLButtonElement>(".ir-slot-list-toggle").forEach((toggle) => {
    toggle.addEventListener("click", () => {
      listOpen = !listOpen;
      if (listOpen) {
        // Open on the slot that was last browsed from somewhere other than the library.
        const views = [0, 1].map((slot) => resourceBrowserModal.navigationLists.effectiveView("ir", navOptions(node, preset, slot)));
        state.sourceSlot = views[0] === "library" && views[1] !== "library" ? 1 : 0;
      }
      requestNodeParamsPanel(node, preset);
    });
  });
  if (!listOpen) {
    return;
  }

  const rows = root.querySelector<HTMLElement>(".ir-slot-list-rows");
  const search = root.querySelector<HTMLInputElement>(".ir-slot-list-search");
  const currentItems = (): NavigationListItem[] =>
    resourceBrowserModal.navigationLists.listing("ir", navOptions(node, preset, state.sourceSlot)).items;
  const findItem = (key: string | undefined): NavigationListItem | undefined =>
    key ? currentItems().find((item) => item.key === key) : undefined;
  const redrawRows = (): void => {
    if (rows) {
      rows.innerHTML = rowsHtml(node, currentItems(), state.query);
    }
  };

  root.querySelectorAll<HTMLButtonElement>(".ir-slot-list-source").forEach((button) => {
    button.addEventListener("click", () => {
      state.sourceSlot = button.dataset.sourceSlot === "1" ? 1 : 0;
      state.scrollTop = 0;
      requestNodeParamsPanel(node, preset);
    });
  });

  if (search) {
    search.addEventListener("input", () => {
      state.query = search.value;
      redrawRows();
    });
    search.addEventListener("focus", () => {
      state.searchFocused = true;
    });
    search.addEventListener("blur", () => {
      state.searchFocused = false;
    });
    // A re-render while typing (an engine broadcast) would otherwise drop the caret.
    if (state.searchFocused) {
      search.focus();
      search.setSelectionRange(search.value.length, search.value.length);
    }
  }

  if (!rows) {
    return;
  }
  rows.scrollTop = state.scrollTop;
  rows.addEventListener("scroll", () => {
    state.scrollTop = rows.scrollTop;
  });

  rows.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest<HTMLButtonElement>("button[data-item-key]") : null;
    const item = findItem(button?.dataset.itemKey);
    if (!button || !item || button.getAttribute("aria-busy") === "true") {
      return;
    }
    button.setAttribute("aria-busy", "true");
    const sourceSlot = state.sourceSlot;
    const work = button.classList.contains("ir-slot-list-tone-btn")
      ? resourceBrowserModal.navigationLists.loadTone(navOptions(node, preset, sourceSlot), item.tone3000?.toneId ?? "").then(redrawRows)
      : assignToSlot(node, preset, sourceSlot, item, button.dataset.slot === "1" ? 1 : 0);
    void work.finally(() => button.removeAttribute("aria-busy"));
  });

  rows.addEventListener("pointerdown", (event) => {
    const pressed = event.target instanceof Element ? event.target : null;
    if (!pressed || pressed.closest("button")) {
      return;
    }
    const row = pressed.closest<HTMLElement>(".ir-slot-list-row[data-item-key]:not(.is-tone)");
    const item = findItem(row?.dataset.itemKey);
    if (!row || !item) {
      return;
    }
    const sourceSlot = state.sourceSlot;
    beginPointerDrag<SlotDropTarget>(event, {
      source: row,
      sourceSelector: ".ir-slot-list-row",
      previewClass: "ir-slot-list-drag-preview",
      bodyClass: "ir-slot-list-dragging",
      resolveTarget: (moveEvent) => resolveSlotTarget(node.id, moveEvent),
      setTargetHighlight: (target, highlighted) => target.element.classList.toggle("drag-over", highlighted),
      // After the release: the load re-renders the panel, the row included (pointer-drag-click-after-rerender).
      onDrop: (target) => {
        if (target) {
          window.setTimeout(() => void assignToSlot(node, preset, sourceSlot, item, target.slot), 0);
        }
      },
    });
  });
}

/** Test hook: closed, and no node's filter, source or scroll remembered. */
export function resetIrSlotListForTests(): void {
  listOpen = false;
  stateByNode.clear();
}
