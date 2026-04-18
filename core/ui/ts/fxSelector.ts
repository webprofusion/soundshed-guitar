/**
 * FX Library Selector Panel
 * 
 * Provides a categorized browser for effects that can be dragged
 * into the signal path.
 */

import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { uiState, setPresetDirty } from "./state.js";
import { postMessage } from "./bridge.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon } from "./iconAssets.js";
import { getCompositeEffectEntries } from "./compositeEffects.js";
import { getCustomEffectLibrary } from "./customEffects.js";
import { getCustomLayout } from "./layoutRenderer.js";
import type { ResourceRef } from "./types.js";

// DOM Elements
const fxSelectorPanel = document.getElementById("fx-selector-panel");
const fxSelectorCategories = document.getElementById("fx-selector-categories");
const fxSelectorEffectsList = document.getElementById("fx-selector-effects-list");
const fxSearchInput = document.getElementById("fx-search-input") as HTMLInputElement | null;
const fxSelectorToggle = document.getElementById("fx-selector-toggle") as HTMLButtonElement | null;
const fxSelectorHeader = document.querySelector(".fx-selector-header") as HTMLElement | null;
const signalPathBar = document.getElementById("signal-path-bar");
const floatingAddFxButton = document.getElementById("signal-path-floating-add-fx") as HTMLButtonElement | null;

// State
let activeCategory = "amp"; // Currently selected category tab
let searchFilter = "";

function syncFxSelectorCollapsedState(options?: { focusSearch?: boolean }): void {
  const isCollapsed = fxSelectorPanel?.classList.contains("collapsed") ?? false;
  fxSelectorToggle?.setAttribute("aria-expanded", String(!isCollapsed));
  if (fxSelectorToggle) {
    fxSelectorToggle.title = isCollapsed ? "Expand FX Library" : "Collapse FX Library";
  }
  signalPathBar?.classList.toggle("fx-library-collapsed", isCollapsed);
  floatingAddFxButton?.setAttribute("aria-hidden", String(!isCollapsed));

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
  fxSelectorPanel.classList.toggle("collapsed", collapsed);
  syncFxSelectorCollapsedState(options);
}

export function expandFxSelector(options?: { focusSearch?: boolean }): void {
  setFxSelectorCollapsed(false, options);
}

export function focusFxSelectorCategory(
  categoryId: string,
  options?: { expand?: boolean; focusSearch?: boolean; clearSearch?: boolean },
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
  renderCategories();
  renderEffectsList();

  if (options?.expand) {
    expandFxSelector({ focusSearch: options.focusSearch });
  }
}

// Category display metadata — id → { name, color }.
// This is the only UI-side definition needed; the actual category list
// is derived at render time from what the effect registry contains.
export const CATEGORY_METADATA: Record<string, { name: string; color: string }> = {
  amp:        { name: "Amplifiers",  color: "#e07848" },
  cab:        { name: "Cabinets",    color: "#a86830" },
  drive:      { name: "Drive",       color: "#e04848" },
  dynamics:   { name: "Dynamics",    color: "#e08030" },
  eq:         { name: "Equalizers",  color: "#48a8e0" },
  modulation: { name: "Modulation",  color: "#9048e0" },
  pitch:      { name: "Pitch",       color: "#c040e0" },
  delay:      { name: "Delay",       color: "#48e0a8" },
  reverb:     { name: "Reverb",      color: "#4878e0" },
  synth:      { name: "Synth",       color: "#7a8a02" },
  utility:    { name: "Utility",     color: "#808080" },
};

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
  return EffectTypeRegistry.getAll().filter((effect) => {
    if (effect.catalogHidden) return false;
    if (effect.type === EffectGuids.kAmpNam || effect.type === EffectGuids.kAmpNamBlend) return false;
    if (effect.type === EffectGuids.kWasmHost && !isFeatureEnabled(Features.CustomEffects)) return false;
    return !excludedTypes.has(effect.type);
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

  // Toggle collapse/expand
  fxSelectorToggle?.addEventListener("click", () => {
    setFxSelectorCollapsed(!isFxSelectorCollapsed());
  });

  fxSelectorHeader?.addEventListener("click", (event) => {
    const target = event.target as HTMLElement;
    if (target.closest(".fx-selector-toggle")) {
      return;
    }
    setFxSelectorCollapsed(!isFxSelectorCollapsed());
  });

  floatingAddFxButton?.addEventListener("click", () => {
    expandFxSelector({ focusSearch: true });
  });

  // Search input
  fxSearchInput?.addEventListener("input", (e) => {
    searchFilter = (e.target as HTMLInputElement).value.toLowerCase();
    renderEffectsList();
  });

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
           data-category="${categoryId}"
           style="--category-color: ${meta.color}">
        ${getFxCategoryIcon(categoryId)}
        <span class="fx-category-name">${meta.name}</span>
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
  bindFxItemDragHandlers();
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

  return `
        <div class="fx-item" 
          data-effect-type="${effect.type}" 
          data-blend-id="${effect.blendId ?? ""}"
          data-blend-category="${effect.blendCategory ?? ""}"
          data-composite-id="${effect.compositeId ?? ""}"
          data-custom-effect-id="${effect.customEffectId ?? ""}"
          data-custom-effect-resource-type="${effect.moduleResourceType ?? ""}"
          data-custom-effect-resource-id="${effect.moduleResourceId ?? ""}"
          data-custom-effect-default-params="${effect.customEffectId ? encodeDatasetJson(effect.defaultParams ?? {}) : ""}"
          data-effect-category="${effect.category}"
         draggable="true"
         style="--category-color: ${categoryColor}">
      <div class="fx-item-icon">${(() => { const thumb = effect.blendId ? (getCustomLayout(effect.type, effect.blendId) ?? getCustomLayout(effect.type)) : getCustomLayout(effect.type); const url = thumb?.thumbnailDataUrl ?? effect.thumbnailDataUrl; return url ? `<img src="${url.replace(/"/g, '&quot;')}" alt="" aria-hidden="true" class="fx-item-thumb" />` : getFxEffectIcon(effect.type); })()}</div>
      <div class="fx-item-info">
        <div class="fx-item-name">${effect.displayName}</div>
        <div class="fx-item-type">${effect.category}</div>
      </div>
      ${resourceBadge}${blendBadge}${customEffectBadge}${compositeBadge}
    </div>
  `;
}

/**
 * Bind drag event handlers to all FX items.
 */
function bindFxItemDragHandlers(): void {
  const fxItems = fxSelectorEffectsList?.querySelectorAll(".fx-item");
  if (!fxItems) return;

  fxItems.forEach((item) => {
    const el = item as HTMLElement;
    
    el.addEventListener("dragstart", (e: DragEvent) => {
      const effectType = el.dataset.effectType;
      const blendId = el.dataset.blendId;
      const compositeId = el.dataset.compositeId;
      const customEffectId = el.dataset.customEffectId;
      if (effectType && e.dataTransfer) {
        e.dataTransfer.setData("application/x-fx-effect", effectType);
        if (blendId) {
          e.dataTransfer.setData("application/x-fx-blend", blendId);
          e.dataTransfer.setData("application/x-fx-blend-name", el.querySelector(".fx-item-name")?.textContent ?? "");
          e.dataTransfer.setData("application/x-fx-blend-category", el.dataset.blendCategory ?? "");
        }
        if (customEffectId) {
          let defaultParams: Record<string, number> = {};
          try {
            defaultParams = JSON.parse(decodeURIComponent(el.dataset.customEffectDefaultParams ?? "%7B%7D")) as Record<string, number>;
          } catch {
            defaultParams = {};
          }
          e.dataTransfer.setData("application/x-fx-custom-effect", JSON.stringify({
            customEffectId,
            baseEffectType: effectType,
            name: el.querySelector(".fx-item-name")?.textContent ?? "",
            category: el.dataset.effectCategory ?? "utility",
            moduleResourceType: el.dataset.customEffectResourceType ?? "",
            moduleResourceId: el.dataset.customEffectResourceId ?? "",
            defaultParams,
          }));
        }
        if (compositeId) {
          e.dataTransfer.setData("application/x-fx-composite", compositeId);
          e.dataTransfer.setData("application/x-fx-composite-name", el.querySelector(".fx-item-name")?.textContent ?? "");
        }
        e.dataTransfer.effectAllowed = "copy";
        el.classList.add("dragging");
        
        // Notify signal path that we're dragging from FX library
        document.body.classList.add("fx-dragging");
      }
    });

    el.addEventListener("dragend", () => {
      el.classList.remove("dragging");
      document.body.classList.remove("fx-dragging");
    });
  });
}

type BlendFxItem = EffectTypeInfo & { blendId: string; blendCategory: string };

function getBlendFxItems(): BlendFxItem[] {
  const blends = uiState.blendLibrary ?? [];
  const categoryMap: Record<string, string> = {
    pedal: "utility",
    preamp: "amp",
    amp: "amp",
    "full-rig": "amp",
    cab: "cab",
  };

  return blends.map((blend) => {
    const mappedCategory = categoryMap[blend.category] ?? "amp";
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
