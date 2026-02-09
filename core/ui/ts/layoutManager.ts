/**
 * Layout Manager Panel
 *
 * Lists all effect layouts from the layout library within the
 * Advanced → Effect Layouts sub-tab. Allows browsing, searching,
 * opening the layout designer, and deleting layouts.
 */

import { uiState } from "./state.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { layoutDesigner } from "./layoutDesigner.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import type { LayoutLibraryEntry, EffectLayout } from "./layoutTypes.js";

// ─────────────────────────────────────────────────────────────
// DOM References
// ─────────────────────────────────────────────────────────────

const layoutList = document.getElementById("layout-list");
const layoutSearchInput = document.getElementById("layout-search-input") as HTMLInputElement | null;

// ─────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────

let initialized = false;
let searchFilter = "";

// ─────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────

export function initLayoutManager(): void {
  if (initialized) return;
  initialized = true;

  layoutSearchInput?.addEventListener("input", () => {
    searchFilter = layoutSearchInput.value.toLowerCase();
    renderLayoutList();
  });

  renderLayoutList();
}

// ─────────────────────────────────────────────────────────────
// Rendering
// ─────────────────────────────────────────────────────────────

export function renderLayoutList(): void {
  if (!layoutList) return;

  const library = uiState.layoutLibrary;
  if (!library) {
    layoutList.innerHTML = `<div class="layout-empty">Layout library not loaded.</div>`;
    return;
  }

  // Flatten all entries from byEffectType
  const entries: { key: string; entry: LayoutLibraryEntry }[] = [];
  for (const [key, list] of Object.entries(library.byEffectType)) {
    for (const entry of list) {
      entries.push({ key, entry });
    }
  }

  // Filter
  const filtered = searchFilter
    ? entries.filter(
        (e) =>
          e.key.toLowerCase().includes(searchFilter) ||
          (e.entry.layout.name ?? "").toLowerCase().includes(searchFilter) ||
          (e.entry.layout.effectType ?? "").toLowerCase().includes(searchFilter) ||
          (e.entry.layout.author ?? "").toLowerCase().includes(searchFilter)
      )
    : entries;

  if (filtered.length === 0) {
    layoutList.innerHTML = `
      <div class="layout-empty">
        ${searchFilter ? "No layouts match your search." : "No effect layouts defined yet. Use the layout designer on an effect to create one."}
      </div>`;
    return;
  }

  layoutList.innerHTML = filtered
    .map(({ key, entry }) => {
      const layout = entry.layout;
      const typeInfo = EffectTypeRegistry.get(layout.effectType);
      const effectName = typeInfo?.displayName || layout.effectType;
      const layoutName = layout.name || "(Unnamed)";
      const isDefault = entry.isDefault;
      const blendLabel = layout.blendId ? ` — blend: ${layout.blendId}` : "";
      const controlCount = layout.controls.length;
      const dims = layout.dimensions;

      return `
      <div class="layout-list-item" data-layout-key="${escAttr(key)}" data-layout-id="${escAttr(entry.layoutId)}">
        <div class="layout-list-info">
          <span class="layout-list-name">${escHtml(layoutName)}</span>
          <span class="layout-list-meta">
            ${escHtml(effectName)}${escHtml(blendLabel)} · ${controlCount} controls · ${dims.width}×${dims.height}
            ${isDefault ? ' · <strong>Default</strong>' : ""}
          </span>
          ${layout.author ? `<span class="layout-list-author">by ${escHtml(layout.author)}</span>` : ""}
        </div>
        <div class="layout-list-actions">
          <button class="layout-edit-btn advanced-action-btn" data-layout-key="${escAttr(key)}" data-layout-id="${escAttr(entry.layoutId)}" data-effect-type="${escAttr(layout.effectType)}" data-blend-id="${escAttr(layout.blendId ?? "")}" title="Edit in designer">Edit</button>
          <button class="layout-delete-btn advanced-action-btn danger" data-layout-key="${escAttr(key)}" data-layout-id="${escAttr(entry.layoutId)}" title="Delete layout">Delete</button>
        </div>
      </div>`;
    })
    .join("");

  // Bind handlers
  layoutList.querySelectorAll(".layout-edit-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const el = btn as HTMLElement;
      const key = el.dataset.layoutKey ?? "";
      const layoutId = el.dataset.layoutId ?? "";
      const effectType = el.dataset.effectType ?? "";
      const blendId = el.dataset.blendId || undefined;
      openLayoutInDesigner(key, layoutId, effectType, blendId);
    });
  });

  layoutList.querySelectorAll(".layout-delete-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const el = btn as HTMLElement;
      const key = el.dataset.layoutKey ?? "";
      const layoutId = el.dataset.layoutId ?? "";
      confirmDeleteLayout(key, layoutId);
    });
  });
}

// ─────────────────────────────────────────────────────────────
// Actions
// ─────────────────────────────────────────────────────────────

function openLayoutInDesigner(
  key: string,
  layoutId: string,
  effectType: string,
  blendId?: string,
): void {
  const library = uiState.layoutLibrary;
  if (!library) return;

  const entries = library.byEffectType[key] ?? [];
  const entry = entries.find((e) => e.layoutId === layoutId);
  if (!entry) {
    showNotification("Layout not found", "error");
    return;
  }

  layoutDesigner.open(effectType, entry.layout, {
    blendId,
    blendName: blendId ? `blend ${blendId}` : undefined,
  });
}

function confirmDeleteLayout(key: string, layoutId: string): void {
  const library = uiState.layoutLibrary;
  if (!library) return;

  const entries = library.byEffectType[key] ?? [];
  const entry = entries.find((e) => e.layoutId === layoutId);
  if (!entry) return;

  const name = entry.layout.name || entry.layout.effectType;
  if (!confirm(`Delete layout "${name}"?`)) return;

  // Send delete request to engine
  postMessage({
    type: "deleteLayout",
    effectType: entry.layout.effectType,
    blendId: entry.layout.blendId ?? "",
    layoutId,
  });

  // Optimistically remove from local state
  library.byEffectType[key] = entries.filter((e) => e.layoutId !== layoutId);
  if (library.byEffectType[key].length === 0) {
    delete library.byEffectType[key];
  }

  appendLog(`Layout deleted: ${layoutId}`);
  showNotification(`Layout "${name}" deleted`, "success");
  renderLayoutList();
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

function escHtml(str: string): string {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function escAttr(str: string): string {
  return str.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
