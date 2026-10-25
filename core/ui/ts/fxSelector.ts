/**
 * FX Library Selector Panel
 *
 * The docked FX library: an FxBrowser (fxLibrary/fxBrowser.ts) whose effects are dragged
 * into the signal path. Opened to replace a node (double-click it in the chain), a click
 * on an effect also puts it in that node's place; see signalPath/replaceChooser.ts.
 */

import { setPresetDirty } from "./state.js";
import { postMessage } from "./bridge.js";
import { getCatalogEffects, isSameFxItem, type FxItemIdentity, type FxPointerDragPayload } from "./fxLibrary/catalog.js";
import { FxBrowser } from "./fxLibrary/fxBrowser.js";
import type { ResourceRef } from "./types.js";

export {
  CATEGORY_METADATA,
  getCatalogEffects,
  getFxLibraryItems,
  getOrderedFxCategories,
  isSameFxItem,
  type FxItemIdentity,
  type FxLibraryItem,
  type FxPointerDragPayload,
} from "./fxLibrary/catalog.js";

// DOM Elements
const fxSelectorPanel = document.getElementById("fx-selector-panel");
const fxSelectorCategories = document.getElementById("fx-selector-categories");
const fxSelectorEffectsList = document.getElementById("fx-selector-effects-list");
const fxSearchInput = document.getElementById("fx-search-input") as HTMLInputElement | null;
const fxSelectorClose = document.getElementById("fx-selector-close") as HTMLButtonElement | null;
const fxSelectorTitle = document.getElementById("fx-selector-title");
const fxSelectorHint = document.getElementById("fx-selector-hint");
const signalPathBar = document.getElementById("signal-path-bar");
const libraryHint = fxSelectorHint?.textContent ?? "";

/**
 * The node the library was opened to replace. While there is one, a click on an effect
 * chooses it for that node; dragging works as it always does.
 */
export type FxReplaceTarget = {
  presetId: string;
  nodeId: string;
  /** What the title calls the node. */
  name: string;
  /** The effect in the node now. It is marked, and choosing it closes without a change. */
  current: FxItemIdentity;
  onChoose: (payload: FxPointerDragPayload) => void;
};

// State
let browser: FxBrowser | null = null;
let effectCatalogRequested = false;
let replaceTarget: FxReplaceTarget | null = null;

function ensureEffectCatalogHydrated(reason: string): void {
  const hasHydratedCatalog = getCatalogEffects().some((effect) => !!effect.category);
  if (hasHydratedCatalog) {
    effectCatalogRequested = false;
    return;
  }
  if (effectCatalogRequested) {
    return;
  }
  effectCatalogRequested = true;
  console.info(`[fxSelector] requesting effect catalog (${reason})`);
  postMessage({ type: "getEffectCatalog" });
}

function syncFxSelectorCollapsedState(options?: { focusSearch?: boolean }): void {
  const isCollapsed = fxSelectorPanel?.classList.contains("collapsed") ?? false;
  signalPathBar?.classList.toggle("fx-library-collapsed", isCollapsed);
  // Not on a touch screen, as with the effect dropdown: focusing the field brings up the
  // on-screen keyboard, which covers the library and scrolls the page to keep the field in view.
  if (!isCollapsed && options?.focusSearch && !window.matchMedia?.("(pointer: coarse)").matches) {
    fxSearchInput?.focus();
  }
}

export function isFxSelectorCollapsed(): boolean {
  return fxSelectorPanel?.classList.contains("collapsed") ?? false;
}

export function setFxSelectorCollapsed(collapsed: boolean, options?: { focusSearch?: boolean }): void {
  if (!fxSelectorPanel) {
    return;
  }
  if (collapsed) {
    // The panel stays in the DOM, hidden: a search field keeping focus would take typing.
    if (fxSelectorPanel.contains(document.activeElement)) {
      (document.activeElement as HTMLElement).blur();
    }
    if (replaceTarget) {
      setFxSelectorReplaceTarget(null);
    }
  }
  fxSelectorPanel.classList.toggle("collapsed", collapsed);
  syncFxSelectorCollapsedState(options);
}

/** Opens the library to add effects; opened to replace a node, it stops replacing it. */
export function expandFxSelector(options?: { focusSearch?: boolean }): void {
  if (replaceTarget) {
    setFxSelectorReplaceTarget(null);
  }
  setFxSelectorCollapsed(false, options);
}

export function focusFxSelectorCategory(
  categoryId: string,
  options?: { expand?: boolean; focusSearch?: boolean; clearSearch?: boolean; replace?: FxReplaceTarget },
): void {
  if (!categoryId) {
    return;
  }

  if (options?.clearSearch) {
    browser?.clearSearch();
  }

  if (options?.expand) {
    replaceTarget = options.replace ?? null;
    renderReplaceMode();
  }
  browser?.setTab(categoryId);

  if (options?.expand) {
    setFxSelectorCollapsed(false, { focusSearch: options.focusSearch });
  }
}

export function getFxSelectorReplaceTarget(): FxReplaceTarget | null {
  return replaceTarget;
}

/** Sets or clears the node the library replaces, leaving it open or closed as it is. */
export function setFxSelectorReplaceTarget(target: FxReplaceTarget | null): void {
  replaceTarget = target;
  renderReplaceMode();
  renderEffectsList();
}

function renderReplaceMode(): void {
  fxSelectorPanel?.classList.toggle("is-replacing", Boolean(replaceTarget));
  if (fxSelectorTitle) {
    fxSelectorTitle.textContent = replaceTarget ? `Replace ${replaceTarget.name}` : "FX Library";
  }
  if (fxSelectorHint) {
    fxSelectorHint.textContent = replaceTarget
      ? "Click an effect to use it instead, or drag one anywhere in the chain."
      : libraryHint;
  }
}

export interface SignalPathNodeOptions {
  config?: Record<string, string>;
  label?: string;
  category?: string;
  params?: Record<string, number>;
  resources?: ResourceRef[];
}

/** Chooses an effect for the node the library was opened to replace, and closes it. */
function chooseForReplaceTarget(payload: FxPointerDragPayload): void {
  const target = replaceTarget;
  if (!target) return;

  setFxSelectorCollapsed(true);
  const chosen = { effectType: payload.effectType, blendId: payload.blendId, customEffectId: payload.customEffect?.customEffectId };
  if (!isSameFxItem(chosen, target.current)) {
    target.onChoose(payload);
  }
}

/**
 * Initialize the FX selector panel and bind event handlers.
 */
export function initFxSelector(): void {
  if (!fxSelectorPanel || !fxSelectorCategories || !fxSelectorEffectsList) {
    console.warn("[fxSelector] Panel element not found");
    return;
  }

  syncFxSelectorCollapsedState();

  browser = new FxBrowser({
    categories: fxSelectorCategories,
    list: fxSelectorEffectsList,
    search: fxSearchInput,
    tab: "amp",
    getPick: () => replaceTarget ? { current: replaceTarget.current, onPick: chooseForReplaceTarget } : null,
    onEmptyCatalog: () => ensureEffectCatalogHydrated("empty"),
  });

  fxSelectorClose?.addEventListener("click", () => {
    setFxSelectorCollapsed(true);
  });

  // Escape from the library or the chain it serves. A dialog opened over them has its own.
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || event.defaultPrevented || isFxSelectorCollapsed()) {
      return;
    }
    const target = event.target as Node | null;
    if (target === document.body || (target && fxSelectorPanel.parentElement?.contains(target))) {
      setFxSelectorCollapsed(true);
    }
  });

  ensureEffectCatalogHydrated("init");
  browser.render();
}

/** Redraws the effects in the open tab. */
export function renderEffectsList(): void {
  browser?.renderList();
}

export function refreshFxSelector(): void {
  ensureEffectCatalogHydrated("refresh");
  browser?.render();
}

/**
 * Send message to add a signal path node at a specific position.
 */
export function sendAddSignalPathNode(
  effectType: string,
  insertAfter: string,
  options?: SignalPathNodeOptions,
): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    insertAfter,
    config: options?.config,
    label: options?.label,
    category: options?.category,
    params: options?.params,
    resources: options?.resources,
  });
  setPresetDirty(true);
}

export interface SignalPathEdgeRef {
  from: string;
  to: string;
  fromPort: number;
  toPort: number;
  gain?: number;
}

/**
 * Add a signal path node by splitting a specific edge.
 * This is required once the graph supports parallel paths (splitter/mixer),
 * because a node may have multiple outgoing edges.
 */
export function sendAddSignalPathNodeOnEdge(
  effectType: string,
  edge: SignalPathEdgeRef,
  options?: SignalPathNodeOptions,
): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    edge,
    config: options?.config,
    label: options?.label,
    category: options?.category,
    params: options?.params,
    resources: options?.resources,
  });
  setPresetDirty(true);
}
