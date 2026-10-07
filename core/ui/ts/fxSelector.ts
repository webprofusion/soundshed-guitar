/**
 * FX Library Selector Panel
 *
 * Provides a categorized browser for effects that can be dragged
 * into the signal path. Opened to replace a node (double-click it in the chain), a click
 * on an effect also puts it in that node's place; see signalPath/replaceChooser.ts.
 */

import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { uiState, setPresetDirty } from "./state.js";
import { postMessage } from "./bridge.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon } from "./iconAssets.js";
import { BLEND_CATEGORIES, CATEGORIES, CATEGORY_ORDER, DEFAULT_BLEND_CATEGORY } from "./generated/effectPresentation.js";
import { getCompositeEffectEntries } from "./compositeEffects.js";
import { getCustomEffectLibrary } from "./customEffects.js";
import { getCustomLayout } from "./layoutRenderer.js";
import type { ResourceRef } from "./types.js";
import { escapeHtml } from "./utils.js";

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

/** Which library entry: its effect type, and the blend or custom effect it carries. */
export type FxItemIdentity = { effectType: string; blendId?: string; customEffectId?: string };

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
let activeCategory = "amp"; // Currently selected category tab
let searchFilter = "";
let dragDelegationBound = false;
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
  if (!isCollapsed && options?.focusSearch) {
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
    searchFilter = "";
    if (fxSearchInput) {
      fxSearchInput.value = "";
    }
  }

  activeCategory = categoryId;
  if (options?.expand) {
    replaceTarget = options.replace ?? null;
    renderReplaceMode();
  }
  renderCategories();
  renderEffectsList();

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

export function isSameFxItem(a: FxItemIdentity, b: FxItemIdentity): boolean {
  return EffectTypeRegistry.resolve(a.effectType) === EffectTypeRegistry.resolve(b.effectType)
    && (a.blendId ?? "") === (b.blendId ?? "")
    && (a.customEffectId ?? "") === (b.customEffectId ?? "");
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

// Category display metadata — id → { name, color }, in the FX library's order. It comes from
// core/ui/data/effect-presentation.json, which Soundshed Guitar Nano reads too; the actual
// category list is derived at render time from what the effect registry contains.
export const CATEGORY_METADATA: Record<string, { name: string; color: string }> = Object.fromEntries(
  CATEGORY_ORDER.map((id) => [id, { name: CATEGORIES[id].name, color: CATEGORIES[id].color }]),
);

export type FxLibraryItem = EffectTypeInfo & {
  blendId?: string;
  blendCategory?: string;
  compositeId?: string;
  customEffectId?: string;
  moduleResourceType?: string;
  moduleResourceId?: string;
  defaultParams?: Record<string, number>;
  description?: string;
};

export type FxPointerDragPayload = {
  effectType: string;
  blendId?: string;
  blendName?: string;
  blendCategory?: string;
  customEffect?: {
    customEffectId: string;
    name: string;
    category: string;
    moduleResourceType: string;
    moduleResourceId: string;
    defaultParams: Record<string, number>;
  };
};

export interface SignalPathNodeOptions {
  config?: Record<string, string>;
  label?: string;
  category?: string;
  params?: Record<string, number>;
  resources?: ResourceRef[];
}

function encodeDatasetJson(value: unknown): string {
  return encodeURIComponent(JSON.stringify(value ?? {}));
}

export function getCatalogEffects(options?: { excludeTypes?: string[] }): EffectTypeInfo[] {
  const excludedTypes = new Set([EffectGuids.kMixer, ...(options?.excludeTypes ?? [])]);
  const experimentalEffectTypes = new Set<string>([EffectGuids.kTransposeStft, EffectGuids.kTransposeHybrid]);
  return EffectTypeRegistry.getAll().filter((effect) => {
    const resolvedType = EffectTypeRegistry.resolve(effect.type);
    if (effect.catalogHidden) return false;
    if (resolvedType === EffectGuids.kAmpNamBlend) return false;
    if (resolvedType === EffectGuids.kWasmHost && !isFeatureEnabled(Features.CustomEffects)) return false;
    if (experimentalEffectTypes.has(resolvedType) && !isFeatureEnabled(Features.ExperimentalEffects)) return false;
    return !excludedTypes.has(resolvedType);
  });
}

export function getOrderedFxCategories(items: Array<{ category: string }>): string[] {
  const categoriesWithContent = new Set(items.map((item) => item.category).filter(Boolean));
  const metadataOrder = Object.keys(CATEGORY_METADATA);
  return [
    ...metadataOrder.filter((id) => categoriesWithContent.has(id)),
    ...[...categoriesWithContent].filter((id) => !CATEGORY_METADATA[id]).sort(),
  ];
}

export function getFxLibraryItems(options?: { excludeTypes?: string[] }): FxLibraryItem[] {
  return [
    ...getCatalogEffects(options),
    ...getBlendFxItems(),
    ...getCustomEffectFxItems(),
    ...getCompositeFxItems(),
  ].filter((item) => !(options?.excludeTypes ?? []).includes(item.type));
}

/**
 * Initialize the FX selector panel and bind event handlers.
 */
export function initFxSelector(): void {
  console.log("[fxSelector] Initializing...");
  console.log("[fxSelector] Panel element:", fxSelectorPanel);
  
  if (!fxSelectorPanel) {
    console.warn("[fxSelector] Panel element not found");
    return;
  }

  syncFxSelectorCollapsedState();

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

  // Search input
  fxSearchInput?.addEventListener("input", (e) => {
    searchFilter = (e.target as HTMLInputElement).value.toLowerCase();
    renderEffectsList();
  });

  bindFxItemHandlers();

  ensureEffectCatalogHydrated("init");

  // Initial render
  renderCategories();
  renderEffectsList();
}

/**
 * Select a category and update the effects list.
 */
function selectCategory(categoryId: string): void {
  activeCategory = categoryId;
  renderCategories();
  renderEffectsList();
}

/**
 * Render the category tabs in the left panel.
 * Categories are derived from the effect registry; only categories that have
 * at least one effect (or blend/composite) are shown.
 */
function renderCategories(): void {
  if (!fxSelectorCategories) return;

  const allEffects = getCatalogEffects();
  const blendItems = getBlendFxItems();
  const customEffectItems = getCustomEffectFxItems();
  const compositeItems = getCompositeFxItems();
  const orderedCategories = getOrderedFxCategories([
    ...allEffects,
    ...blendItems,
    ...customEffectItems,
    ...compositeItems,
  ]);

  if (orderedCategories.length === 0) {
    ensureEffectCatalogHydrated("empty");
  } else if (!orderedCategories.includes(activeCategory)) {
    // The category that was open has nothing in it now; open the first one that has.
    activeCategory = orderedCategories[0];
  }

  const categoriesHtml = orderedCategories.map((categoryId) => {
    const meta = CATEGORY_METADATA[categoryId] ?? { name: categoryId, color: "#606060" };
    const effects = allEffects.filter((e) => e.category === categoryId);
    const blends = blendItems.filter((b) => b.category === categoryId);
    const customEffects = customEffectItems.filter((c) => c.category === categoryId);
    const composites = compositeItems.filter((c) => c.category === categoryId);
    const totalCount = effects.length + blends.length + customEffects.length + composites.length;
    const activeClass = activeCategory === categoryId ? "active" : "";

    return `
      <div class="fx-category ${activeClass}" 
           data-category="${escapeHtml(categoryId)}"
           style="--category-color: ${escapeHtml(meta.color)}">
        ${getFxCategoryIcon(categoryId)}
        <span class="fx-category-name">${escapeHtml(meta.name)}</span>
        <span class="fx-category-count">${totalCount}</span>
      </div>
    `;
  }).join("");

  fxSelectorCategories.innerHTML = categoriesHtml;

  // Bind category click handlers
  const categoryElements = fxSelectorCategories.querySelectorAll(".fx-category");
  categoryElements.forEach((element) => {
    element.addEventListener("click", () => {
      const categoryId = (element as HTMLElement).dataset.category;
      if (categoryId) {
        selectCategory(categoryId);
      }
    });
  });
}

/**
 * Render the effects list in the right panel for the active category.
 */
export function renderEffectsList(): void {
  if (!fxSelectorEffectsList) return;

  const allEffects = getCatalogEffects();
  const allCustomEffects = getCustomEffectFxItems();
  const activeColorCategory = CATEGORY_METADATA[activeCategory];
  const categoryColor = activeColorCategory?.color || "#808080";
  const matchesSearch = (item: FxLibraryItem): boolean => [
    item.displayName,
    item.type,
    item.category,
    item.description ?? "",
  ].join(" ").toLowerCase().includes(searchFilter);
  
  // Get effects for active category
  let effects = allEffects.filter((e) => e.category === activeCategory);
  let blends = getBlendFxItems().filter((b) => b.category === activeCategory);
  let customEffects = allCustomEffects.filter((c) => c.category === activeCategory);
  let composites = getCompositeFxItems().filter((c) => c.category === activeCategory);
  
  // Apply search filter across all categories if searching
  if (searchFilter) {
    effects = allEffects.filter((e) => matchesSearch(e));
    blends = getBlendFxItems().filter((b) => matchesSearch(b));
    customEffects = allCustomEffects.filter((c) => matchesSearch(c));
    composites = getCompositeFxItems().filter((c) => matchesSearch(c));
  }

  if (effects.length === 0 && blends.length === 0 && customEffects.length === 0 && composites.length === 0) {
    fxSelectorEffectsList.innerHTML = `
      <div style="padding: 20px; text-align: center; color: #6a6a80; font-size: 12px;">
        ${searchFilter ? "No effects match your search" : "No effects in this category"}
      </div>
    `;
    return;
  }

  const effectsHtml = effects.map((effect) => {
    const color = searchFilter 
      ? CATEGORY_METADATA[effect.category]?.color || "#808080"
      : categoryColor;
    return renderFxItem(effect, color);
  }).join("");

  const blendsHtml = blends.map((blend) => {
    const color = searchFilter
      ? CATEGORY_METADATA[blend.category]?.color || "#808080"
      : categoryColor;
    return renderFxItem(blend, color);
  }).join("");

  const customEffectsHtml = customEffects.map((customEffect) => {
    const color = searchFilter
      ? CATEGORY_METADATA[customEffect.category]?.color || "#808080"
      : categoryColor;
    return renderFxItem(customEffect, color);
  }).join("");

  const compositesHtml = composites.map((comp) => {
    const color = searchFilter
      ? CATEGORY_METADATA[comp.category]?.color || "#808080"
      : categoryColor;
    return renderFxItem(comp, color);
  }).join("");

  fxSelectorEffectsList.innerHTML = effectsHtml + blendsHtml + customEffectsHtml + compositesHtml;

  // Bind drag handlers to FX items
  bindFxItemHandlers();

}

/**
 * Render a single FX item card.
 */
function renderFxItem(effect: FxLibraryItem, categoryColor: string): string {
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
  // Opened to replace a node, every effect is a button that chooses it.
  const isCurrent = replaceTarget !== null
    && isSameFxItem({ effectType: effect.type, blendId: effect.blendId, customEffectId: effect.customEffectId }, replaceTarget.current);
  const chooseAttrs = replaceTarget ? ` role="button" tabindex="0"${isCurrent ? ' aria-current="true"' : ""}` : "";
  const currentBadge = isCurrent ? `<span class="fx-item-current">Current</span>` : "";

  // Names and ids here come from blends, custom effects and imported archives, and a blend's
  // name can be a Tone3000 title, so everything user-supplied is escaped.
  return `
        <div class="fx-item${isCurrent ? " is-current" : ""}"${chooseAttrs}
          data-effect-type="${escapeHtml(effect.type)}"
          data-blend-id="${escapeHtml(effect.blendId ?? "")}"
          data-blend-category="${escapeHtml(effect.blendCategory ?? "")}"
          data-composite-id="${escapeHtml(effect.compositeId ?? "")}"
          data-custom-effect-id="${escapeHtml(effect.customEffectId ?? "")}"
          data-custom-effect-resource-type="${escapeHtml(effect.moduleResourceType ?? "")}"
          data-custom-effect-resource-id="${escapeHtml(effect.moduleResourceId ?? "")}"
          data-custom-effect-default-params="${escapeHtml(effect.customEffectId ? encodeDatasetJson(effect.defaultParams ?? {}) : "")}"
          data-effect-category="${escapeHtml(effect.category)}"
          style="--category-color: ${escapeHtml(categoryColor)}">
      <div class="fx-item-icon">${(() => { const thumb = effect.blendId ? (getCustomLayout(effect.type, effect.blendId) ?? getCustomLayout(effect.type)) : getCustomLayout(effect.type); const url = thumb?.thumbnailDataUrl ?? effect.thumbnailDataUrl; return url ? `<img src="${escapeHtml(url)}" alt="" aria-hidden="true" class="fx-item-thumb" />` : getFxEffectIcon(effect.type); })()}</div>
      <div class="fx-item-info">
        <div class="fx-item-name">${escapeHtml(effect.displayName)}</div>
        <div class="fx-item-type">${escapeHtml(effect.category)}</div>
      </div>
      ${currentBadge}${resourceBadge}${blendBadge}${customEffectBadge}${compositeBadge}
    </div>
  `;
}

/** What an FX library item carries into the chain, read back from its data attributes. */
function readFxItemPayload(el: HTMLElement): FxPointerDragPayload | null {
  const effectType = el.dataset.effectType;
  if (!effectType) return null;

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
      name: el.querySelector(".fx-item-name")?.textContent ?? "Custom Effect",
      category: el.dataset.effectCategory ?? "utility",
      moduleResourceType: el.dataset.customEffectResourceType ?? "",
      moduleResourceId: el.dataset.customEffectResourceId ?? "",
      defaultParams,
    };
  }

  return {
    effectType,
    blendId: el.dataset.blendId || undefined,
    blendName: el.querySelector(".fx-item-name")?.textContent ?? undefined,
    blendCategory: el.dataset.blendCategory || undefined,
    customEffect,
  };
}

/**
 * Chooses an effect for the node the library was opened to replace, and closes it.
 * pointerDrag.ts swallows the click that ends a drag, so only a plain click or a key lands here.
 */
function chooseFxItem(el: HTMLElement): void {
  const target = replaceTarget;
  const payload = readFxItemPayload(el);
  if (!target || !payload) return;

  setFxSelectorCollapsed(true);
  const chosen = { effectType: payload.effectType, blendId: payload.blendId, customEffectId: payload.customEffect?.customEffectId };
  if (!isSameFxItem(chosen, target.current)) {
    target.onChoose(payload);
  }
}

/**
 * Announce a press on an FX library item so the signal path can drag it.
 *
 * Dragging is pointer-driven rather than HTML5 drag-and-drop: WebKitGTK never
 * delivers the `drop` event to our targets, so native dragging could not add
 * effects on Linux at all (issue #27). The signal path owns the graph, so it
 * owns what a drop means — this module only reports where the drag started and
 * which effect it carries. Clicks and keys choose an effect while replacing a node.
 */
function bindFxItemHandlers(): void {
  if (!fxSelectorEffectsList || dragDelegationBound) return;

  fxSelectorEffectsList.addEventListener("pointerdown", (event: PointerEvent) => {
    if (!event.isPrimary || event.button !== 0) return;

    const target = event.target as HTMLElement | null;
    const el = target?.closest(".fx-item") as HTMLElement | null;
    const payload = el ? readFxItemPayload(el) : null;
    if (!el || !payload) return;

    document.dispatchEvent(new CustomEvent("fx-pointer-drag-start", {
      detail: { source: el, pointerEvent: event, payload },
    }));
  });

  fxSelectorEffectsList.addEventListener("click", (event: MouseEvent) => {
    const el = (event.target as HTMLElement | null)?.closest<HTMLElement>(".fx-item");
    if (el) chooseFxItem(el);
  });

  fxSelectorEffectsList.addEventListener("keydown", (event: KeyboardEvent) => {
    const el = (event.target as HTMLElement | null)?.closest<HTMLElement>(".fx-item");
    if (!el || !replaceTarget || (event.key !== "Enter" && event.key !== " ")) return;
    event.preventDefault();
    chooseFxItem(el);
  });

  dragDelegationBound = true;
}

type BlendFxItem = EffectTypeInfo & { blendId: string; blendCategory: string };

function getBlendFxItems(): BlendFxItem[] {
  const blends = uiState.blendLibrary ?? [];

  return blends.map((blend) => {
    const mappedCategory = BLEND_CATEGORIES[blend.category] ?? DEFAULT_BLEND_CATEGORY;
    return {
      type: EffectGuids.kAmpNamBlend,
      displayName: blend.name || "Custom Blend",
      category: mappedCategory,
      requiresResource: true,
      resourceType: "nam",
      parameters: EffectTypeRegistry.get(EffectGuids.kAmpNamBlend)?.parameters ?? [],
      blendId: blend.id,
      blendCategory: blend.category,
    };
  });
}

type CustomEffectFxItem = EffectTypeInfo & {
  customEffectId: string;
  moduleResourceType: string;
  moduleResourceId: string;
  defaultParams?: Record<string, number>;
};

function getCustomEffectFxItems(): CustomEffectFxItem[] {
  if (!isFeatureEnabled(Features.CustomEffects)) {
    return [];
  }
  const wasmInfo = EffectTypeRegistry.get(EffectGuids.kWasmHost);
  return getCustomEffectLibrary().map((entry) => ({
    type: EffectGuids.kWasmHost,
    displayName: entry.name || "Custom Effect",
    category: entry.category || "utility",
    description: entry.description ?? "",
    thumbnailDataUrl: entry.thumbnailDataUrl,
    requiresResource: false,
    parameters: wasmInfo?.parameters ?? [],
    customEffectId: entry.id,
    moduleResourceType: entry.moduleResourceType,
    moduleResourceId: entry.moduleResourceId,
    defaultParams: entry.defaultParams,
  }));
}

type CompositeFxItem = EffectTypeInfo & { compositeId: string; description: string };

function getCompositeFxItems(): CompositeFxItem[] {
  return getCompositeEffectEntries().map((entry) => ({
    type: entry.type,
    displayName: entry.displayName,
    category: entry.category,
    requiresResource: false,
    parameters: [],
    compositeId: entry.type.replace("composite:", ""),
    description: entry.description,
  }));
}

export function refreshFxSelector(): void {
  ensureEffectCatalogHydrated("refresh");
  renderCategories();
  renderEffectsList();
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
