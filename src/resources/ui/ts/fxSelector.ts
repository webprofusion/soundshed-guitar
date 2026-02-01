/**
 * FX Library Selector Panel
 * 
 * Provides a categorized browser for effects that can be dragged
 * into the signal path.
 */

import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { uiState, setPresetDirty } from "./state.js";
import { postMessage } from "./bridge.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon } from "./iconAssets.js";

// DOM Elements
const fxSelectorPanel = document.getElementById("fx-selector-panel");
const fxSelectorCategories = document.getElementById("fx-selector-categories");
const fxSelectorEffectsList = document.getElementById("fx-selector-effects-list");
const fxSearchInput = document.getElementById("fx-search-input") as HTMLInputElement | null;
const fxSelectorToggle = document.getElementById("fx-selector-toggle") as HTMLButtonElement | null;
const fxSelectorHeader = document.querySelector(".fx-selector-header") as HTMLElement | null;

// State
let activeCategory = "dynamics"; // Currently selected category tab
let searchFilter = "";

// Category definitions
interface FxCategory {
  id: string;
  name: string;
  color: string;
}

const FX_CATEGORIES: FxCategory[] = [
  { id: "dynamics", name: "Dynamics", color: "#e04848" },
  { id: "amp", name: "Amplifiers", color: "#e07848" },
  { id: "cab", name: "Cabinets", color: "#a86830" },
  { id: "eq", name: "Equalizers", color: "#48a8e0" },
  { id: "modulation", name: "Modulation", color: "#9048e0" },
  { id: "delay", name: "Delay", color: "#48e0a8" },
  { id: "reverb", name: "Reverb", color: "#4878e0" },
  { id: "utility", name: "Utility", color: "#808080" },
];

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

  // Toggle collapse/expand
  fxSelectorToggle?.addEventListener("click", () => {
    fxSelectorPanel.classList.toggle("collapsed");
    const isCollapsed = fxSelectorPanel.classList.contains("collapsed");
    fxSelectorToggle.setAttribute("aria-expanded", String(!isCollapsed));
    fxSelectorToggle.title = isCollapsed ? "Expand FX Library" : "Collapse FX Library";
  });

  fxSelectorHeader?.addEventListener("click", (event) => {
    const target = event.target as HTMLElement;
    if (target.closest(".fx-selector-toggle")) {
      return;
    }
    fxSelectorPanel.classList.toggle("collapsed");
    const isCollapsed = fxSelectorPanel.classList.contains("collapsed");
    fxSelectorToggle?.setAttribute("aria-expanded", String(!isCollapsed));
    if (fxSelectorToggle) {
      fxSelectorToggle.title = isCollapsed ? "Expand FX Library" : "Collapse FX Library";
    }
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
 */
function renderCategories(): void {
  if (!fxSelectorCategories) return;

  const allEffects = EffectTypeRegistry.getAll();
  const blendItems = getBlendFxItems();
  
  const categoriesHtml = FX_CATEGORIES.map((category) => {
    const effects = allEffects.filter((e) => e.category === category.id);
    const blends = blendItems.filter((b) => b.category === category.id);
    const totalCount = effects.length + blends.length;
    const activeClass = activeCategory === category.id ? "active" : "";

    return `
      <div class="fx-category ${activeClass}" 
           data-category="${category.id}"
           style="--category-color: ${category.color}">
        ${getFxCategoryIcon(category.id)}
        <span class="fx-category-name">${category.name}</span>
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

  const allEffects = EffectTypeRegistry.getAll();
  const activeColorCategory = FX_CATEGORIES.find((c) => c.id === activeCategory);
  const categoryColor = activeColorCategory?.color || "#808080";
  
  // Get effects for active category
  let effects = allEffects.filter((e) => e.category === activeCategory);
  let blends = getBlendFxItems().filter((b) => b.category === activeCategory);
  
  // Apply search filter across all categories if searching
  if (searchFilter) {
    effects = allEffects.filter((e) => 
      e.displayName.toLowerCase().includes(searchFilter) ||
      e.type.toLowerCase().includes(searchFilter) ||
      e.category.toLowerCase().includes(searchFilter)
    );
    blends = getBlendFxItems().filter((b) =>
      b.displayName.toLowerCase().includes(searchFilter) ||
      b.type.toLowerCase().includes(searchFilter) ||
      b.category.toLowerCase().includes(searchFilter)
    );
  }

  if (effects.length === 0 && blends.length === 0) {
    fxSelectorEffectsList.innerHTML = `
      <div style="padding: 20px; text-align: center; color: #6a6a80; font-size: 12px;">
        ${searchFilter ? "No effects match your search" : "No effects in this category"}
      </div>
    `;
    return;
  }

  const effectsHtml = effects.map((effect) => {
    const color = searchFilter 
      ? FX_CATEGORIES.find((c) => c.id === effect.category)?.color || "#808080"
      : categoryColor;
    return renderFxItem(effect, color);
  }).join("");

  const blendsHtml = blends.map((blend) => {
    const color = searchFilter
      ? FX_CATEGORIES.find((c) => c.id === blend.category)?.color || "#808080"
      : categoryColor;
    return renderFxItem(blend, color, blend.blendId, blend.blendCategory);
  }).join("");

  fxSelectorEffectsList.innerHTML = effectsHtml + blendsHtml;

  // Bind drag handlers to FX items
  bindFxItemDragHandlers();
}

/**
 * Render a single FX item card.
 */
function renderFxItem(effect: EffectTypeInfo, categoryColor: string, blendId?: string, blendCategory?: string): string {
  const resourceBadge = effect.requiresResource 
    ? `<span class="fx-item-badge">${getBadgeIcon("resource", `Requires ${effect.resourceType}`)}</span>` 
    : "";
  const blendBadge = blendId
    ? `<span class="fx-item-badge">${getBadgeIcon("blend", "Custom blend")}</span>`
    : "";

  return `
        <div class="fx-item" 
          data-effect-type="${effect.type}" 
          data-blend-id="${blendId ?? ""}"
          data-blend-category="${blendCategory ?? ""}"
          data-effect-category="${effect.category}"
         draggable="true"
         style="--category-color: ${categoryColor}">
      <div class="fx-item-icon">${getFxEffectIcon(effect.type)}</div>
      <div class="fx-item-info">
        <div class="fx-item-name">${effect.displayName}</div>
        <div class="fx-item-type">${effect.type}</div>
      </div>
      ${resourceBadge}${blendBadge}
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
      if (effectType && e.dataTransfer) {
        e.dataTransfer.setData("application/x-fx-effect", effectType);
        if (blendId) {
          e.dataTransfer.setData("application/x-fx-blend", blendId);
          e.dataTransfer.setData("application/x-fx-blend-name", el.querySelector(".fx-item-name")?.textContent ?? "");
          e.dataTransfer.setData("application/x-fx-blend-category", el.dataset.blendCategory ?? "");
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
      type: "amp_nam_blend",
      displayName: blend.name || "Custom Blend",
      category: mappedCategory,
      requiresResource: true,
      resourceType: "nam",
      parameters: EffectTypeRegistry.get("amp_nam_blend")?.parameters ?? [],
      blendId: blend.id,
      blendCategory: blend.category,
    };
  });
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
  options?: { config?: Record<string, string>; label?: string; category?: string },
): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    insertAfter,
    config: options?.config,
    label: options?.label,
    category: options?.category,
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
  options?: { config?: Record<string, string>; label?: string; category?: string },
): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    edge,
    config: options?.config,
    label: options?.label,
    category: options?.category,
  });
  setPresetDirty(true);
}
