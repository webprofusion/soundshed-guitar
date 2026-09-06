/**
 * multiPresetMixer.ts — Multi-Rig (Composite Preset) UI panel.
 *
 * Handles the "Multi-Rig" tab in the preset library popover:
 *   - Listing saved composite presets, filtered by the library search box
 *   - Loading a composite preset (replaces active mixer slots)
 *   - Prompting to save or update the current mixer as a composite preset
 *   - Removing a composite preset, from a list card or the mixer toolbar
 */

import { uiState, setPresetDirty } from "./state.js";
import type { CompositePreset } from "./types.js";
import {
  saveCompositePreset,
  loadCompositePreset,
  getCompositePresetList,
  removeCompositePreset,
} from "./bridge.js";
import { escapeHtml } from "./utils.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { syncPresetLibraryFeatureVisibility, setSetlistPanelVisible } from "./presets.js";
import { presetSearchElement } from "./presets/dom.js";
import { renderSignalPathBar } from "./signalPath.js";
import { STANDARD_TAGS } from "./presetTags.js";
import {
  collectCompositePresetTags,
  compositePresetMatchesMixer,
  filterCompositePresets,
  resolveCompositeSlotNames,
} from "./multiPresetMixerSupport.js";

const multiRigSaveModal = document.getElementById("save-multi-rig-modal") as HTMLElement | null;
const multiRigNameInput = document.getElementById("multi-rig-name-input") as HTMLInputElement | null;
const multiRigDescriptionInput = document.getElementById("multi-rig-description-input") as HTMLTextAreaElement | null;

function getMultiRigTagsPickerValue(): string[] {
  const picker = document.getElementById("multi-rig-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

function ensureMultiRigTagChips(tags: readonly string[]): void {
  const picker = document.getElementById("multi-rig-tags-picker");
  if (!picker) return;

  const existing = new Set(
    Array.from(picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip"))
      .map((button) => button.dataset.tag ?? "")
      .filter(Boolean),
  );

  let added = false;
  for (const tag of tags) {
    if (existing.has(tag)) continue;
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "preset-tag-chip";
    chip.dataset.tag = tag;
    chip.textContent = tag;
    picker.appendChild(chip);
    added = true;
  }

  if (added) {
    bindMultiRigTagPicker();
  }
}

function setMultiRigTagsPickerValue(tags: string[]): void {
  const picker = document.getElementById("multi-rig-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

function bindMultiRigTagPicker(): void {
  const picker = document.getElementById("multi-rig-tags-picker");
  if (!picker) return;
  picker.querySelectorAll<HTMLButtonElement>(".preset-tag-chip").forEach((btn) => {
    if (btn.dataset.bound === "true") return;
    btn.dataset.bound = "true";
    btn.addEventListener("click", () => btn.classList.toggle("active"));
  });
}

/** The composite preset the current mixer was loaded from / last saved as, if it still exists. */
function findActiveCompositePreset(): CompositePreset | undefined {
  const id = uiState.activeCompositePresetId;
  if (!id) return undefined;
  return (uiState.compositePresets ?? []).find((cp) => cp.id === id);
}

function isMultiRigPanelVisible(): boolean {
  const panel = document.getElementById("preset-library-multi-rig-panel");
  return !!panel && !panel.hidden;
}

function lookupPresetName(): (presetId: string) => string | undefined {
  const names = new Map<string, string>();
  for (const preset of uiState.presets) {
    names.set(preset.id, preset.name);
  }
  return (presetId) => uiState.presetCache.get(presetId)?.name ?? names.get(presetId);
}

function openSaveCompositePresetModal(): void {
  // Editing an already-saved Multi-Rig re-opens this same modal pre-filled with its
  // current name/description/tags, rather than prompting for a brand new one.
  const editing = findActiveCompositePreset();

  if (!multiRigSaveModal || !multiRigNameInput) {
    const name = (prompt("Multi-Rig name:", editing?.name ?? "") ?? "").trim();
    if (name) saveCompositePreset(name, editing?.description, editing?.tags, editing?.id);
    return;
  }

  ensureMultiRigTagChips(collectCompositePresetTags(uiState.compositePresets ?? []));
  multiRigNameInput.value = editing?.name ?? "";
  if (multiRigDescriptionInput) {
    multiRigDescriptionInput.value = editing?.description ?? "";
  }
  setMultiRigTagsPickerValue(editing?.tags ?? []);

  const titleEl = document.getElementById("save-multi-rig-modal-title");
  if (titleEl) titleEl.textContent = editing ? "Edit Multi-Rig Mix" : "Save Multi-Rig Mix";
  const confirmBtn = document.getElementById("save-multi-rig-confirm");
  if (confirmBtn) confirmBtn.textContent = editing ? "Update Mix" : "Save Mix";

  multiRigSaveModal.style.display = "flex";
  multiRigNameInput.focus();
  multiRigNameInput.select();
}

function closeSaveCompositePresetModal(): void {
  if (!multiRigSaveModal) return;
  multiRigSaveModal.style.display = "none";
}

function submitSaveCompositePresetModal(): void {
  if (!multiRigNameInput) return;
  const name = multiRigNameInput.value.trim();
  if (!name) {
    multiRigNameInput.classList.add("input-error");
    multiRigNameInput.focus();
    return;
  }
  multiRigNameInput.classList.remove("input-error");
  const editingId = findActiveCompositePreset()?.id;
  saveCompositePreset(name, multiRigDescriptionInput?.value.trim() ?? "", getMultiRigTagsPickerValue(), editingId);
  closeSaveCompositePresetModal();
}

// ── Rendering ─────────────────────────────────────────────────────────────────

export function renderCompositePresetList(): void {
  const container = document.getElementById("composite-preset-list");
  if (!container) return;

  const presets = uiState.compositePresets ?? [];

  if (presets.length === 0) {
    container.innerHTML = `<p class="composite-preset-empty">No Multi-Rig presets saved yet.<br>Switch to the <strong>Presets</strong> tab, click <strong>+ Mixer</strong> on two or more presets, then click <strong>Save</strong> in the mixer's <strong>Mix</strong> tab.</p>`;
    return;
  }

  const nameOf = lookupPresetName();
  const slotNamesOf = (cp: CompositePreset) => resolveCompositeSlotNames(cp, nameOf);
  const query = presetSearchElement?.value ?? "";
  const visible = filterCompositePresets(presets, query, slotNamesOf);

  if (visible.length === 0) {
    container.innerHTML = `<p class="composite-preset-empty">No Multi-Rig presets match &quot;${escapeHtml(query.trim())}&quot;.</p>`;
    return;
  }

  container.innerHTML = visible
    .map((cp) => buildCompositePresetChip(cp, slotNamesOf(cp)))
    .join("");

  container.querySelectorAll<HTMLElement>(".composite-preset-chip").forEach((chip) => {
    const id = chip.dataset.id ?? "";
    const preset = presets.find((cp) => cp.id === id);
    if (!preset) return;

    // The whole card is clickable to load, matching regular preset list items.
    chip.addEventListener("click", () => {
      void handleLoadCompositePresetFlow(preset);
    });
    chip.addEventListener("keydown", (e) => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        void handleLoadCompositePresetFlow(preset);
      } else if (e.key === "Delete") {
        e.preventDefault();
        void handleDeleteCompositePresetFlow(preset);
      }
    });

    chip.querySelector<HTMLButtonElement>(".composite-preset-delete")?.addEventListener("click", (e) => {
      e.stopPropagation();
      void handleDeleteCompositePresetFlow(preset);
    });
  });
}

function buildCompositePresetChip(cp: CompositePreset, slotNames: readonly string[]): string {
  const slotCount = cp.slots?.length ?? 0;
  const isActive = cp.id === uiState.activeCompositePresetId;
  const desc = cp.description ? `<p class="composite-preset-desc">${escapeHtml(cp.description)}</p>` : "";
  const slots = slotNames.length
    ? `<p class="composite-preset-slots" title="${escapeHtml(slotNames.join(", "))}">${slotNames.map((name) => `<span class="composite-preset-slot">${escapeHtml(name)}</span>`).join("")}</p>`
    : "";
  const tags = cp.tags?.length
    ? `<div class="composite-preset-tags">${cp.tags.map((tag) => `<span class="preset-category-badge">${escapeHtml(tag)}</span>`).join("")}</div>`
    : "";
  return `
    <article class="composite-preset-chip${isActive ? " active" : ""}" data-id="${escapeHtml(cp.id)}" data-name="${escapeHtml(cp.name)}" role="button" tabindex="0" title="Load Multi-Rig &quot;${escapeHtml(cp.name)}&quot;">
      <div class="composite-preset-chip-header">
        <span class="composite-preset-name">${escapeHtml(cp.name)}</span>
        <span class="composite-preset-slot-count">${slotCount} preset${slotCount !== 1 ? "s" : ""}</span>
        <button type="button" class="composite-preset-delete" title="Delete Multi-Rig &quot;${escapeHtml(cp.name)}&quot;" aria-label="Delete Multi-Rig ${escapeHtml(cp.name)}">×</button>
      </div>
      ${slots}
      ${desc}
      ${tags}
    </article>`;
}

// ── Load / save / delete flows ────────────────────────────────────────────────

/**
 * Loading a Multi-Rig replaces every mixer slot, so unsaved edits to whichever
 * preset is focused would be lost — ask first, the same as picking a preset.
 */
async function handleLoadCompositePresetFlow(preset: CompositePreset): Promise<void> {
  if (uiState.presetDirty) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) return;
    setPresetDirty(false);
  }
  loadCompositePreset(preset.id);
}

/**
 * Show an inline save dialog in the Multi-Rig tab, or a simple prompt fallback.
 * Called by the "Save" button in the mixer's Mix tab via a custom event.
 */
export function handleSaveCompositePresetFlow(): void {
  const activeCount =
    uiState.mixer?.activePresetIds?.length ?? 0;
  if (activeCount < 2) {
    showNotification("Add at least 2 presets to the mixer before saving a Multi-Rig.");
    return;
  }
  openSaveCompositePresetModal();
}

async function handleDeleteCompositePresetFlow(preset: CompositePreset): Promise<void> {
  const confirmed = await showConfirm(`Delete Multi-Rig "${preset.name}"? This cannot be undone.`, "Delete Multi-Rig");
  if (!confirmed) return;
  removeCompositePreset(preset.id);
  if (uiState.activeCompositePresetId === preset.id) {
    uiState.activeCompositePresetId = null;
    renderSignalPathBar(); // the mixer toolbar's Delete button goes back to disabled
  }
}

/**
 * "Delete" toolbar button in the mixer panel — removes the Multi-Rig preset the
 * current mixer was loaded from / last saved as. No-ops if nothing is currently linked.
 */
async function handleDeleteActiveCompositePresetFlow(): Promise<void> {
  const editing = findActiveCompositePreset();
  if (!editing) {
    showNotification("No Multi-Rig preset is loaded to delete.");
    return;
  }
  await handleDeleteCompositePresetFlow(editing);
}

/**
 * Keep `activeCompositePresetId` honest against the mixer the engine reports.
 * The UI clears it when the user adds or removes a slot here, but the mixer can
 * also change underneath us — a setlist step, a MIDI program change, a DAW
 * project restore — and after any of those a Save should create a new Multi-Rig
 * rather than overwrite the one that no longer describes the mix.
 */
export function reconcileActiveCompositePreset(): void {
  const active = findActiveCompositePreset();
  if (!active) return;
  const activePresetIds = uiState.mixer?.activePresetIds ?? [];
  if (!compositePresetMatchesMixer(active, activePresetIds)) {
    uiState.activeCompositePresetId = null;
    // Neither caller redraws the signal path, so the Mix tab would keep offering
    // Update (and an enabled Delete) for a Multi-Rig this mixer no longer is.
    renderSignalPathBar();
  }
}

// ── Message handlers ──────────────────────────────────────────────────────────

export function handleCompositePresetList(presets: CompositePreset[]): void {
  uiState.compositePresets = presets;
  ensureMultiRigTagChips(collectCompositePresetTags(presets));
  // A Multi-Rig deleted from another window must not leave the toolbar
  // offering to update it.
  if (uiState.activeCompositePresetId && !presets.some((cp) => cp.id === uiState.activeCompositePresetId)) {
    uiState.activeCompositePresetId = null;
    renderSignalPathBar();
  }
  renderCompositePresetList();
  syncPresetLibraryFeatureVisibility();
}

export function handleCompositePresetSaved(id: string, name: string): void {
  uiState.activeCompositePresetId = id;
  showNotification(`Multi-Rig "${name}" saved.`);
  getCompositePresetList();
  renderSignalPathBar(); // refresh mixer toolbar (Delete becomes available)
}

export function handleCompositePresetLoaded(id: string, name: string): void {
  uiState.activeCompositePresetId = id;
  showNotification(`Multi-Rig "${name}" loaded.`);
  renderCompositePresetList(); // highlight the loaded card
}

// ── Tab switching ─────────────────────────────────────────────────────────────

export function initMultiRigTab(): void {
  const presetsTab = document.getElementById("preset-lib-tab-presets");
  const multiRigTab = document.getElementById("preset-lib-tab-multi-rig");
  const presetsPanel = document.getElementById("preset-library-presets-panel");
  const multiRigPanel = document.getElementById("preset-library-multi-rig-panel");

  if (!presetsTab || !multiRigTab || !presetsPanel || !multiRigPanel) return;

  // Fetch composite presets up front (not just on first tab click / save) so
  // the Multi-Rig tab is populated as soon as the popover opens, for anyone who
  // already has saved Multi-Rig presets from a previous session.
  if (isFeatureEnabled(Features.MultiRig)) {
    getCompositePresetList();
  }

  // Seed the tags picker with the standard vocabulary shared across every
  // tag picker in the app; any additional tags already used on saved
  // Multi-Rigs get appended on top by ensureMultiRigTagChips() elsewhere.
  ensureMultiRigTagChips(STANDARD_TAGS);

  const selectTab = (multiRig: boolean): void => {
    presetsTab.classList.toggle("active", !multiRig);
    presetsTab.setAttribute("aria-selected", String(!multiRig));
    multiRigTab.classList.toggle("active", multiRig);
    multiRigTab.setAttribute("aria-selected", String(multiRig));
    presetsPanel.hidden = multiRig;
    multiRigPanel.hidden = !multiRig;
    // Setlists only make sense against the regular preset list.
    setSetlistPanelVisible(!multiRig);
  };

  presetsTab.addEventListener("click", () => selectTab(false));

  multiRigTab.addEventListener("click", () => {
    if (!isFeatureEnabled(Features.MultiRig)) {
      selectTab(false);
      return;
    }
    selectTab(true);
    // Refresh list on open
    getCompositePresetList();
  });

  // The library search box filters whichever tab is showing.
  presetSearchElement?.addEventListener("input", () => {
    if (isMultiRigPanelVisible()) {
      renderCompositePresetList();
    }
  });

  // "Save" toolbar button in the mixer panel fires a custom event. The save
  // form is a modal of its own, so there is no need to switch the (possibly
  // closed) library popover to the Multi-Rig tab first.
  document.addEventListener("mixerSaveMultiRig", () => {
    if (!isFeatureEnabled(Features.MultiRig)) {
      return;
    }
    handleSaveCompositePresetFlow();
  });

  // "Delete" toolbar button in the mixer panel fires a custom event
  document.addEventListener("mixerDeleteMultiRig", () => {
    if (!isFeatureEnabled(Features.MultiRig)) {
      return;
    }
    void handleDeleteActiveCompositePresetFlow();
  });

  document.getElementById("save-multi-rig-modal-close")?.addEventListener("click", closeSaveCompositePresetModal);
  document.getElementById("save-multi-rig-cancel")?.addEventListener("click", closeSaveCompositePresetModal);
  document.getElementById("save-multi-rig-confirm")?.addEventListener("click", submitSaveCompositePresetModal);
  bindMultiRigTagPicker();
  multiRigNameInput?.addEventListener("input", () => multiRigNameInput.classList.remove("input-error"));
  multiRigNameInput?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      submitSaveCompositePresetModal();
    } else if (event.key === "Escape") {
      closeSaveCompositePresetModal();
    }
  });
  multiRigDescriptionInput?.addEventListener("keydown", (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
      event.preventDefault();
      submitSaveCompositePresetModal();
    }
  });
  multiRigSaveModal?.addEventListener("mousedown", (event) => {
    if (event.target === multiRigSaveModal) {
      closeSaveCompositePresetModal();
    }
  });
}
