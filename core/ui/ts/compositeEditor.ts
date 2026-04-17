/**
 * Composite Effect Editor
 *
 * Provides UI for listing, creating, editing, and saving composite effect
 * definitions within the Advanced tab of the library panel.
 *
 * The composite's inner signal graph is edited via the main signal path
 * area (reusing the same rendering, drag-drop, knobs, and param editing).
 * This module manages: the definition list, metadata form, exposed params,
 * and the enter/exit of composite edit mode on the C++ side.
 */

import {
  getCompositeLibrary,
  getCompositeDefinition,
  deleteCompositeDefinition,
  createEmptyCompositeDefinition,
} from "./compositeEffects.js";
import type {
  CompositeEffectDefinition,
  ExposedParameter,
} from "./compositeTypes.js";
import { getNodeEffectInfo } from "./presetV2.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { appendLog } from "./logging.js";
import { showConfirm } from "./dialogs.js";
import {
  isCompositeEditMode,
  getCompositeEditDefinition,
} from "./state.js";

// ─────────────────────────────────────────────────────────────
// DOM References
// ─────────────────────────────────────────────────────────────

const compositeList = document.getElementById("composite-list");
const compositeEditor = document.getElementById("composite-editor");
const compositeEditorTitle = document.getElementById("composite-editor-title");
const compositeSearchInput = document.getElementById("composite-search-input") as HTMLInputElement | null;
const compositeNewBtn = document.getElementById("composite-new-btn");
const compositeSaveBtn = document.getElementById("composite-editor-save-btn");
const compositeSaveAsBtn = document.getElementById("composite-editor-save-as-btn");
const compositeCancelBtn = document.getElementById("composite-editor-cancel-btn");

// Editor form fields
const editName = document.getElementById("composite-edit-name") as HTMLInputElement | null;
const editCategory = document.getElementById("composite-edit-category") as HTMLSelectElement | null;
const editDescription = document.getElementById("composite-edit-description") as HTMLTextAreaElement | null;
const editAuthor = document.getElementById("composite-edit-author") as HTMLInputElement | null;
const editTags = document.getElementById("composite-edit-tags") as HTMLInputElement | null;

// Exposed params
const exposedParamsEl = document.getElementById("composite-exposed-params");
const addParamBtn = document.getElementById("composite-add-param-btn");

// ─────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────

let initialized = false;
let searchFilter = "";

/** The definition being edited — metadata is local; graph is kept in sync with C++. */
let editingDef: CompositeEffectDefinition | null = null;
/** Whether this is a new (unsaved) definition. */
let isNewDefinition = false;

// ─────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────

export function initCompositeEditor(): void {
  if (initialized) return;
  initialized = true;

  compositeSearchInput?.addEventListener("input", () => {
    searchFilter = compositeSearchInput.value.toLowerCase();
    renderCompositeList();
  });

  compositeNewBtn?.addEventListener("click", () => {
    startNewComposite();
  });

  compositeSaveBtn?.addEventListener("click", () => {
    saveCurrentComposite(false);
  });

  compositeSaveAsBtn?.addEventListener("click", () => {
    saveCurrentComposite(true);
  });

  compositeCancelBtn?.addEventListener("click", () => {
    cancelEditor();
  });

  addParamBtn?.addEventListener("click", () => {
    addExposedParam();
  });

  renderCompositeList();
}

// ─────────────────────────────────────────────────────────────
// List Rendering
// ─────────────────────────────────────────────────────────────

export function renderCompositeList(): void {
  if (!compositeList) return;

  const defs = getCompositeLibrary();
  const filtered = searchFilter
    ? defs.filter(
        (d) =>
          d.name.toLowerCase().includes(searchFilter) ||
          d.category.toLowerCase().includes(searchFilter) ||
          (d.description ?? "").toLowerCase().includes(searchFilter) ||
          (d.tags ?? []).some((t) => t.toLowerCase().includes(searchFilter))
      )
    : defs;

  if (filtered.length === 0) {
    compositeList.innerHTML = `
      <div class="composite-empty">
        ${searchFilter ? "No composites match your search." : "No composite effects defined yet. Click <strong>+ New Composite</strong> to create one."}
      </div>`;
    return;
  }

  compositeList.innerHTML = filtered
    .map(
      (def) => `
    <div class="composite-list-item" data-composite-id="${def.id}">
      <div class="composite-list-info">
        <span class="composite-list-name">${escHtml(def.name)}</span>
        <span class="composite-list-meta">${escHtml(def.category)} · ${def.innerGraph.nodes.filter((n) => n.type !== "input" && n.type !== "output").length} effects · ${def.exposedParams.length} params</span>
        ${def.description ? `<span class="composite-list-desc">${escHtml(def.description)}</span>` : ""}
      </div>
      <div class="composite-list-actions">
        <button class="composite-edit-btn advanced-action-btn" data-composite-id="${def.id}" title="Edit">Edit</button>
        <button class="composite-clone-btn advanced-action-btn" data-composite-id="${def.id}" title="Clone as new">Clone</button>
        <button class="composite-delete-btn advanced-action-btn danger" data-composite-id="${def.id}" title="Delete">Delete</button>
      </div>
    </div>`
    )
    .join("");

  // Bind handlers
  compositeList.querySelectorAll(".composite-edit-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = (btn as HTMLElement).dataset.compositeId;
      if (id) openEditor(id);
    });
  });

  compositeList.querySelectorAll(".composite-clone-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = (btn as HTMLElement).dataset.compositeId;
      if (id) cloneComposite(id);
    });
  });

  compositeList.querySelectorAll(".composite-delete-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = (btn as HTMLElement).dataset.compositeId;
      if (id) void confirmDeleteComposite(id);
    });
  });
}

// ─────────────────────────────────────────────────────────────
// Editor Open/Close
// ─────────────────────────────────────────────────────────────

function startNewComposite(): void {
  editingDef = createEmptyCompositeDefinition();
  isNewDefinition = true;
  showEditor("New Composite Effect");
  // Tell C++ to enter composite edit mode with the new empty definition
  postMessage({ type: "enterCompositeEditMode", compositeId: editingDef.id, definition: editingDef });
}

function openEditor(id: string): void {
  const src = getCompositeDefinition(id);
  if (!src) {
    showNotification("Composite not found", "error");
    return;
  }
  // Deep clone for local metadata editing
  editingDef = JSON.parse(JSON.stringify(src));
  isNewDefinition = false;
  showEditor(`Edit: ${editingDef!.name}`);
  // Tell C++ to enter composite edit mode — C++ will clone the definition and
  // broadcast compositeEditState which sets up the signal path rendering
  postMessage({ type: "enterCompositeEditMode", compositeId: id });
}

function cloneComposite(id: string): void {
  const src = getCompositeDefinition(id);
  if (!src) return;
  editingDef = JSON.parse(JSON.stringify(src));
  editingDef!.id = `composite-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  editingDef!.name = `${src.name} (Copy)`;
  editingDef!.createdAt = new Date().toISOString();
  editingDef!.modifiedAt = new Date().toISOString();
  isNewDefinition = true;
  showEditor(`Clone: ${editingDef!.name}`);
  postMessage({ type: "enterCompositeEditMode", compositeId: editingDef!.id, definition: editingDef });
}

function showEditor(title: string): void {
  if (!compositeEditor || !editingDef) return;

  if (compositeEditorTitle) compositeEditorTitle.textContent = title;

  // Populate form
  if (editName) editName.value = editingDef.name;
  if (editCategory) editCategory.value = editingDef.category || "channel";
  if (editDescription) editDescription.value = editingDef.description || "";
  if (editAuthor) editAuthor.value = editingDef.author || "";
  if (editTags) editTags.value = (editingDef.tags ?? []).join(", ");

  // Show save-as only for existing defs
  if (compositeSaveAsBtn) {
    compositeSaveAsBtn.style.display = isNewDefinition ? "none" : "";
  }

  renderExposedParams();
  compositeEditor.style.display = "";
  compositeList?.parentElement?.querySelector(".advanced-toolbar")?.classList.add("hidden");
  if (compositeList) compositeList.style.display = "none";
}

function cancelEditor(): void {
  // Tell C++ to exit composite edit mode without saving
  postMessage({ type: "exitCompositeEditMode", save: false });
  closeEditorUI();
}

function closeEditorUI(): void {
  editingDef = null;
  isNewDefinition = false;
  if (compositeEditor) compositeEditor.style.display = "none";
  compositeList?.parentElement?.querySelector(".advanced-toolbar")?.classList.remove("hidden");
  if (compositeList) compositeList.style.display = "";
  renderCompositeList();
}

/**
 * Called when the C++ side confirms exit from composite edit mode.
 * Resets the editor UI.
 */
export function handleCompositeEditModeExited(): void {
  closeEditorUI();
}

/**
 * Called when C++ broadcasts compositeEditState. Keeps the local
 * exposed-params editor in sync with the live inner graph.
 */
export function handleCompositeEditStateUpdate(): void {
  const liveDef = getCompositeEditDefinition();
  if (!liveDef || !editingDef) return;
  // Keep the inner graph in sync (for exposed params node references)
  editingDef.innerGraph = JSON.parse(JSON.stringify(liveDef.innerGraph));
  renderExposedParams();
}

// ─────────────────────────────────────────────────────────────
// Save
// ─────────────────────────────────────────────────────────────

function saveCurrentComposite(saveAsNew: boolean): void {
  if (!editingDef) return;

  // Read form values back
  editingDef.name = editName?.value.trim() || "Untitled";
  editingDef.category = editCategory?.value || "channel";
  editingDef.description = editDescription?.value.trim() || "";
  editingDef.author = editAuthor?.value.trim() || "";
  editingDef.tags = (editTags?.value ?? "")
    .split(",")
    .map((t) => t.trim())
    .filter(Boolean);

  if (saveAsNew) {
    editingDef.id = `composite-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
    editingDef.createdAt = new Date().toISOString();
  }

  // Tell C++ to exit composite edit mode with save=true.
  // C++ will persist the current inner graph along with our metadata.
  postMessage({
    type: "exitCompositeEditMode",
    save: true,
    metadata: {
      id: editingDef.id,
      name: editingDef.name,
      category: editingDef.category,
      description: editingDef.description,
      author: editingDef.author,
      tags: editingDef.tags,
      exposedParams: editingDef.exposedParams,
    },
  });

  showNotification(`Composite "${editingDef.name}" saved`, "success");
  appendLog(`Composite saved: ${editingDef.id}`);
  closeEditorUI();
}

async function confirmDeleteComposite(id: string): Promise<void> {
  const def = getCompositeDefinition(id);
  if (!def) return;
  const confirmed = await showConfirm(`Delete composite "${def.name}"? This cannot be undone.`, "Delete Composite");
  if (!confirmed) return;
  deleteCompositeDefinition(id);
  renderCompositeList();
}

// ─────────────────────────────────────────────────────────────
// Exposed Parameters
// ─────────────────────────────────────────────────────────────

function renderExposedParams(): void {
  if (!exposedParamsEl || !editingDef) return;

  const params = editingDef.exposedParams;
  if (params.length === 0) {
    exposedParamsEl.innerHTML = `<div class="composite-empty">No exposed parameters. Click <strong>+ Expose Parameter</strong> to add one.</div>`;
    return;
  }

  exposedParamsEl.innerHTML = params
    .map(
      (ep, idx) => `
    <div class="exposed-param-row" data-param-index="${idx}">
      <div class="exposed-param-fields">
        <input type="text" class="ep-param-id" value="${escAttr(ep.paramId)}" placeholder="Param ID" title="Parameter ID" />
        <input type="text" class="ep-display-name" value="${escAttr(ep.displayName)}" placeholder="Display Name" title="Display name" />
        <select class="ep-node-id" title="Target node">
          ${getInnerEffectNodeOptions(ep.nodeId)}
        </select>
        <select class="ep-node-param" title="Node parameter" data-current="${escAttr(ep.nodeParamKey)}">
          ${getNodeParamOptions(ep.nodeId, ep.nodeParamKey)}
        </select>
        <input type="number" class="ep-min" value="${ep.minValue ?? 0}" step="any" title="Min" placeholder="Min" />
        <input type="number" class="ep-max" value="${ep.maxValue ?? 1}" step="any" title="Max" placeholder="Max" />
        <input type="number" class="ep-default" value="${ep.defaultValue ?? 0}" step="any" title="Default" placeholder="Default" />
        <input type="text" class="ep-unit" value="${escAttr(ep.unit ?? "")}" placeholder="Unit" title="Unit" />
      </div>
      <button class="exposed-param-remove advanced-action-btn danger" data-param-index="${idx}" title="Remove">×</button>
    </div>`
    )
    .join("");

  // Bind change handlers
  exposedParamsEl.querySelectorAll(".exposed-param-row").forEach((row) => {
    const idx = parseInt((row as HTMLElement).dataset.paramIndex ?? "0", 10);
    const ep = editingDef!.exposedParams[idx];
    if (!ep) return;

    const paramIdInput = row.querySelector(".ep-param-id") as HTMLInputElement;
    const displayInput = row.querySelector(".ep-display-name") as HTMLInputElement;
    const nodeSelect = row.querySelector(".ep-node-id") as HTMLSelectElement;
    const paramSelect = row.querySelector(".ep-node-param") as HTMLSelectElement;
    const minInput = row.querySelector(".ep-min") as HTMLInputElement;
    const maxInput = row.querySelector(".ep-max") as HTMLInputElement;
    const defaultInput = row.querySelector(".ep-default") as HTMLInputElement;
    const unitInput = row.querySelector(".ep-unit") as HTMLInputElement;

    paramIdInput?.addEventListener("change", () => { ep.paramId = paramIdInput.value.trim(); });
    displayInput?.addEventListener("change", () => { ep.displayName = displayInput.value.trim(); });
    nodeSelect?.addEventListener("change", () => {
      ep.nodeId = nodeSelect.value;
      // Refresh param options for the new node
      if (paramSelect) {
        paramSelect.innerHTML = getNodeParamOptions(ep.nodeId, "");
        ep.nodeParamKey = paramSelect.value;
      }
    });
    paramSelect?.addEventListener("change", () => {
      ep.nodeParamKey = paramSelect.value;
      // Auto-fill range from param def
      const node = editingDef!.innerGraph.nodes.find((n) => n.id === ep.nodeId);
      if (node) {
        const pDef = getNodeEffectInfo(node)?.parameters.find((p) => p.key === ep.nodeParamKey);
        if (pDef) {
          if (minInput) { minInput.value = String(pDef.min); ep.minValue = pDef.min; }
          if (maxInput) { maxInput.value = String(pDef.max); ep.maxValue = pDef.max; }
          if (defaultInput) { defaultInput.value = String(pDef.default); ep.defaultValue = pDef.default; }
          if (unitInput) { unitInput.value = pDef.unit; ep.unit = pDef.unit; }
          if (displayInput && !displayInput.value) {
            displayInput.value = pDef.name;
            ep.displayName = pDef.name;
          }
        }
      }
    });
    minInput?.addEventListener("change", () => { ep.minValue = parseFloat(minInput.value) || 0; });
    maxInput?.addEventListener("change", () => { ep.maxValue = parseFloat(maxInput.value) || 1; });
    defaultInput?.addEventListener("change", () => { ep.defaultValue = parseFloat(defaultInput.value) || 0; });
    unitInput?.addEventListener("change", () => { ep.unit = unitInput.value.trim(); });
  });

  // Remove buttons
  exposedParamsEl.querySelectorAll(".exposed-param-remove").forEach((btn) => {
    btn.addEventListener("click", () => {
      const idx = parseInt((btn as HTMLElement).dataset.paramIndex ?? "0", 10);
      editingDef!.exposedParams.splice(idx, 1);
      renderExposedParams();
    });
  });
}

function addExposedParam(): void {
  if (!editingDef) return;

  const effectNodes = editingDef.innerGraph.nodes.filter(
    (n) => n.type !== "input" && n.type !== "output"
  );

  const firstNode = effectNodes[0];
  const firstNodeType = firstNode ? getNodeEffectInfo(firstNode) : undefined;
  const firstParam = firstNodeType?.parameters[0];

  const newParam: ExposedParameter = {
    paramId: `param${editingDef.exposedParams.length + 1}`,
    displayName: firstParam?.name ?? "Parameter",
    nodeId: firstNode?.id ?? "",
    nodeParamKey: firstParam?.key ?? "",
    minValue: firstParam?.min ?? 0,
    maxValue: firstParam?.max ?? 1,
    defaultValue: firstParam?.default ?? 0,
    unit: firstParam?.unit ?? "",
  };

  editingDef.exposedParams.push(newParam);
  renderExposedParams();
}

function getInnerEffectNodeOptions(selectedNodeId: string): string {
  if (!editingDef) return "";
  return editingDef.innerGraph.nodes
    .filter((n) => n.type !== "input" && n.type !== "output")
    .map((n) => {
      const typeInfo = getNodeEffectInfo(n);
      const label = n.displayName || typeInfo?.displayName || n.type;
      return `<option value="${n.id}" ${n.id === selectedNodeId ? "selected" : ""}>${escHtml(label)} (${n.id})</option>`;
    })
    .join("");
}

function getNodeParamOptions(nodeId: string, selectedKey: string): string {
  if (!editingDef) return "";
  const node = editingDef.innerGraph.nodes.find((n) => n.id === nodeId);
  if (!node) return '<option value="">—</option>';

  const typeInfo = getNodeEffectInfo(node);
  const params = typeInfo?.parameters ?? [];

  if (params.length === 0) {
    // Fall back to node.params keys
    const keys = Object.keys(node.params ?? {});
    return keys
      .map((k) => `<option value="${k}" ${k === selectedKey ? "selected" : ""}>${k}</option>`)
      .join("");
  }

  return params
    .map(
      (p) =>
        `<option value="${p.key}" ${p.key === selectedKey ? "selected" : ""}>${escHtml(p.name)} (${p.key})</option>`
    )
    .join("");
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
