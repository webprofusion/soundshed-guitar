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
const fxSearchInput = document.getElementById("fx-search-input") as HTMLInputElement | null;
const fxLibraryToggle = document.getElementById("fx-library-toggle");
const fxSelectorCollapse = document.getElementById("fx-selector-collapse");
const fxSelectorClose = document.getElementById("fx-selector-close");

// State
let isPanelOpen = false;
let isCollapsed = false;
let expandedCategories = new Set<string>(["dynamics", "amp", "cab"]);
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
  console.log("[fxSelector] Toggle button:", fxLibraryToggle);
  
  if (!fxSelectorPanel) {
    console.warn("[fxSelector] Panel element not found");
    return;
  }

  // Toggle button
  if (fxLibraryToggle) {
    console.log("[fxSelector] Adding click handler to toggle button");
    fxLibraryToggle.addEventListener("click", () => {
      console.log("[fxSelector] Toggle button clicked");
      toggleFxSelectorPanel();
    });
  } else {
    console.warn("[fxSelector] Toggle button not found");
  }

  // Close button
  fxSelectorClose?.addEventListener("click", () => {
    toggleFxSelectorPanel(false);
  });

  // Collapse button
  fxSelectorCollapse?.addEventListener("click", () => {
    toggleCollapsed();
  });

  // Search input
  fxSearchInput?.addEventListener("input", (e) => {
    searchFilter = (e.target as HTMLInputElement).value.toLowerCase();
    renderFxCategories();
  });

  // Keyboard shortcut
  document.addEventListener("keydown", (e) => {
    // 'E' to toggle panel (when not in input)
    if (e.key === "e" || e.key === "E") {
      const target = e.target as HTMLElement;
      if (target.tagName !== "INPUT" && target.tagName !== "TEXTAREA") {
        e.preventDefault();
        toggleFxSelectorPanel();
      }
    }
    // Escape to close
    if (e.key === "Escape" && isPanelOpen) {
      toggleFxSelectorPanel(false);
    }
  });

  // Initial render
  renderFxCategories();
}

/**
 * Toggle the FX selector panel visibility.
 */
export function toggleFxSelectorPanel(visible?: boolean): void {
  isPanelOpen = visible ?? !isPanelOpen;
  fxSelectorPanel?.classList.toggle("open", isPanelOpen);
  fxLibraryToggle?.classList.toggle("active", isPanelOpen);
  
  if (isPanelOpen) {
    fxSearchInput?.focus();
  }
}

/**
 * Toggle collapsed state (show only category headers).
 */
function toggleCollapsed(): void {
  isCollapsed = !isCollapsed;
  fxSelectorPanel?.classList.toggle("collapsed", isCollapsed);
  
  if (fxSelectorCollapse) {
    fxSelectorCollapse.textContent = isCollapsed ? "□" : "─";
    fxSelectorCollapse.title = isCollapsed ? "Expand" : "Collapse";
  }
}

/**
 * Toggle a category's expanded/collapsed state.
 */
function toggleCategory(categoryId: string): void {
  if (expandedCategories.has(categoryId)) {
    expandedCategories.delete(categoryId);
  } else {
    expandedCategories.add(categoryId);
  }
  renderFxCategories();
}

/**
 * Render all FX categories and their effects.
 */
export function renderFxCategories(): void {
  if (!fxSelectorCategories) return;

  const allEffects = EffectTypeRegistry.getAll();
  
  const categoriesHtml = FX_CATEGORIES.map((category) => {
    const effects = allEffects.filter((e) => e.category === category.id);
    
    // Apply search filter
    const filteredEffects = searchFilter
      ? effects.filter((e) => 
          e.displayName.toLowerCase().includes(searchFilter) ||
          e.type.toLowerCase().includes(searchFilter) ||
          e.category.toLowerCase().includes(searchFilter)
        )
      : effects;

    // Skip empty categories when searching
    if (searchFilter && filteredEffects.length === 0) {
      return "";
    }

    const isExpanded = expandedCategories.has(category.id) || searchFilter.length > 0;
    const expandedClass = isExpanded ? "expanded" : "";
    const chevron = isExpanded ? "▼" : "▶";

    const effectsHtml = filteredEffects.map((effect) => renderFxItem(effect, category.color)).join("");

    return `
      <div class="fx-category ${expandedClass}" data-category="${category.id}">
        <div class="fx-category-header" style="--category-color: ${category.color}">
          <span class="fx-category-chevron">${chevron}</span>
          <span class="fx-category-icon">${category.icon}</span>
          <span class="fx-category-name">${category.name}</span>
          <span class="fx-category-count">${filteredEffects.length}</span>
        </div>
        <div class="fx-category-items">
          ${effectsHtml}
        </div>
      </div>
    `;
  }).join("");

  fxSelectorCategories.innerHTML = categoriesHtml;

  // Bind category header click handlers
  const categoryHeaders = fxSelectorCategories.querySelectorAll(".fx-category-header");
  categoryHeaders.forEach((header) => {
    header.addEventListener("click", () => {
      const categoryEl = header.closest(".fx-category") as HTMLElement;
      const categoryId = categoryEl?.dataset.category;
      if (categoryId) {
        toggleCategory(categoryId);
      }
    });
  });

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
  const fxItems = fxSelectorCategories?.querySelectorAll(".fx-item");
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
 * Send message to add a node at a specific position.
 */
export function sendAddNode(effectType: string, insertAfter: string): void {
  postMessage({
    type: "addNode",
    effectType,
    insertAfter,
  });
}
