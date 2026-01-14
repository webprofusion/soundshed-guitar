/**
 * FX Library Selector Panel
 * 
 * Provides a categorized browser for effects that can be dragged
 * into the signal path.
 */

import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { postMessage } from "./bridge.js";

// DOM Elements
const fxSelectorPanel = document.getElementById("fx-selector-panel");
const fxSelectorCategories = document.getElementById("fx-selector-categories");
const fxSelectorEffectsList = document.getElementById("fx-selector-effects-list");
const fxSearchInput = document.getElementById("fx-search-input") as HTMLInputElement | null;

// State
let activeCategory = "dynamics"; // Currently selected category tab
let searchFilter = "";

// Category definitions
interface FxCategory {
  id: string;
  name: string;
  icon: string;
  color: string;
}

const FX_CATEGORIES: FxCategory[] = [
  { id: "dynamics", name: "Dynamics", icon: "⚡", color: "#e04848" },
  { id: "amp", name: "Amplifiers", icon: "🎸", color: "#e07848" },
  { id: "cab", name: "Cabinets", icon: "🔊", color: "#a86830" },
  { id: "eq", name: "Equalizers", icon: "🎚️", color: "#48a8e0" },
  { id: "modulation", name: "Modulation", icon: "🌊", color: "#9048e0" },
  { id: "delay", name: "Delay", icon: "⏱️", color: "#48e0a8" },
  { id: "reverb", name: "Reverb", icon: "🏛️", color: "#4878e0" },
  { id: "utility", name: "Utility", icon: "🔧", color: "#808080" },
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
  
  const categoriesHtml = FX_CATEGORIES.map((category) => {
    const effects = allEffects.filter((e) => e.category === category.id);
    const activeClass = activeCategory === category.id ? "active" : "";

    return `
      <div class="fx-category ${activeClass}" 
           data-category="${category.id}"
           style="--category-color: ${category.color}">
        <span class="fx-category-icon">${category.icon}</span>
        <span class="fx-category-name">${category.name}</span>
        <span class="fx-category-count">${effects.length}</span>
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
  
  // Apply search filter across all categories if searching
  if (searchFilter) {
    effects = allEffects.filter((e) => 
      e.displayName.toLowerCase().includes(searchFilter) ||
      e.type.toLowerCase().includes(searchFilter) ||
      e.category.toLowerCase().includes(searchFilter)
    );
  }

  if (effects.length === 0) {
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

  fxSelectorEffectsList.innerHTML = effectsHtml;

  // Bind drag handlers to FX items
  bindFxItemDragHandlers();
}

/**
 * Render a single FX item card.
 */
function renderFxItem(effect: EffectTypeInfo, categoryColor: string): string {
  const resourceBadge = effect.requiresResource 
    ? `<span class="fx-item-badge" title="Requires ${effect.resourceType}">📁</span>` 
    : "";

  return `
    <div class="fx-item" 
         data-effect-type="${effect.type}" 
         draggable="true"
         style="--category-color: ${categoryColor}">
      <div class="fx-item-icon">${getEffectIcon(effect.type)}</div>
      <div class="fx-item-info">
        <div class="fx-item-name">${effect.displayName}</div>
        <div class="fx-item-type">${effect.type}</div>
      </div>
      ${resourceBadge}
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
      if (effectType && e.dataTransfer) {
        e.dataTransfer.setData("application/x-fx-effect", effectType);
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

/**
 * Get the icon for an effect type.
 */
function getEffectIcon(effectType: string): string {
  const icons: Record<string, string> = {
    // Dynamics
    "dynamics_gate": "🚪",
    "compressor_vca": "📊",
    "compressor_opto": "💡",
    "compressor_fet": "⚡",
    
    // Amps
    "amp_nam": "🎸",
    "amp_clean": "🎺",
    "amp_crunch": "🎷",
    
    // Cabs
    "cab_ir": "🔊",
    "cab_simple": "📻",
    
    // EQ
    "eq_parametric": "🎚️",
    "eq_graphic": "📊",
    "eq_tilt": "↗️",
    
    // Modulation
    "chorus_analog": "🌊",
    "chorus_digital": "🌈",
    "flanger": "〰️",
    "phaser": "🌀",
    "tremolo": "📳",
    "vibrato": "🎭",
    
    // Delay
    "delay_digital": "⏱️",
    "delay_tape": "📼",
    "delay_analog": "🔄",
    
    // Reverb
    "reverb_room": "🏠",
    "reverb_hall": "🏛️",
    "reverb_plate": "📀",
    "reverb_spring": "🌸",
    "reverb_shimmer": "✨",
    
    // Utility
    "gain": "📢",
    "splitter": "↗️",
    "mixer": "🎛️",
  };
  
  return icons[effectType] || "⚙️";
}

/**
 * Send message to add a signal path node at a specific position.
 */
export function sendAddSignalPathNode(effectType: string, insertAfter: string): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    insertAfter,
  });
}

export interface SignalPathEdgeRef {
  from: string;
  to: string;
  fromPort: number;
  toPort: number;
}

/**
 * Add a signal path node by splitting a specific edge.
 * This is required once the graph supports parallel paths (splitter/mixer),
 * because a node may have multiple outgoing edges.
 */
export function sendAddSignalPathNodeOnEdge(effectType: string, edge: SignalPathEdgeRef): void {
  postMessage({
    type: "addSignalPathNode",
    effectType,
    edge,
  });
}
