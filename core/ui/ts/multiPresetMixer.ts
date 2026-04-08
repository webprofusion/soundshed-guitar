/**
 * multiPresetMixer.ts — Multi-Rig (Composite Preset) UI panel.
 *
 * Handles the "Multi-Rig" tab in the preset library popover:
 *   - Listing saved composite presets
 *   - Loading a composite preset (replaces active mixer slots)
 *   - Prompting to save the current mixer as a composite preset
 *   - Removing a composite preset
 */

import { uiState } from "./state.js";
import type { CompositePreset } from "./types.js";
import {
  saveCompositePreset,
  loadCompositePreset,
  getCompositePresetList,
  removeCompositePreset,
} from "./bridge.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";

// ── Rendering ─────────────────────────────────────────────────────────────────

export function renderCompositePresetList(): void {
  const container = document.getElementById("composite-preset-list");
  if (!container) return;

  const presets = uiState.compositePresets ?? [];

  if (presets.length === 0) {
    container.innerHTML = `<p class="composite-preset-empty">No Multi-Rig presets saved yet.<br>Switch to the <strong>Presets</strong> tab, click <strong>+ Mixer</strong> on two or more presets, then click <strong>Save Multi-Rig…</strong> in the mixer panel.</p>`;
    return;
  }

  container.innerHTML = presets
    .map((cp) => buildCompositePresetChip(cp))
    .join("");

  container.querySelectorAll<HTMLElement>(".composite-preset-chip").forEach((chip) => {
    const id = chip.dataset.id ?? "";

    chip.querySelector(".composite-preset-load-btn")?.addEventListener("click", () => {
      loadCompositePreset(id);
    });

    chip.querySelector(".composite-preset-remove-btn")?.addEventListener("click", async (e) => {
      e.stopPropagation();
      const confirmed = await showConfirm(`Remove Multi-Rig "${chip.dataset.name}"?`, "Remove Multi-Rig");
      if (!confirmed) return;
      removeCompositePreset(id);
    });
  });
}

function buildCompositePresetChip(cp: CompositePreset): string {
  const slotCount = cp.slots?.length ?? 0;
  const names = cp.slots?.map((s) => s.presetId).join(", ") ?? "";
  const desc = cp.description ? `<span class="composite-preset-desc">${escapeHtml(cp.description)}</span>` : "";
  return `
    <article class="composite-preset-chip" data-id="${escapeHtml(cp.id)}" data-name="${escapeHtml(cp.name)}">
      <div class="composite-preset-chip-header">
        <span class="composite-preset-name">${escapeHtml(cp.name)}</span>
        <span class="composite-preset-slot-count">${slotCount} preset${slotCount !== 1 ? "s" : ""}</span>
        <button type="button" class="composite-preset-load-btn primary-btn" title="Load Multi-Rig">Load</button>
        <button type="button" class="composite-preset-remove-btn icon-btn" title="Remove Multi-Rig">×</button>
      </div>
      ${desc}
      <div class="composite-preset-slots-summary">${escapeHtml(names)}</div>
    </article>`;
}

// ── Save modal ────────────────────────────────────────────────────────────────

/**
 * Show an inline save dialog in the Multi-Rig tab, or a simple prompt fallback.
 * Called by the "Save Multi-Rig…" button in views.ts via a custom event.
 */
export function handleSaveCompositePresetFlow(): void {
  const activeCount =
    uiState.mixer?.activePresetIds?.length ?? 0;
  if (activeCount < 2) {
    showNotification("Add at least 2 presets to the mixer before saving a Multi-Rig.", "warning");
    return;
  }

  const container = document.getElementById("composite-preset-list");
  if (!container) {
    // Fallback: use prompt
    const name = (prompt("Multi-Rig name:") ?? "").trim();
    if (name) saveCompositePreset(name);
    return;
  }

  // Show inline form at the top of the list
  const existingForm = container.querySelector(".composite-preset-save-form");
  if (existingForm) {
    (existingForm.querySelector("input") as HTMLInputElement | null)?.focus();
    return;
  }

  const formHtml = `
    <div class="composite-preset-save-form">
      <input type="text" id="composite-preset-name-input" placeholder="Multi-Rig name…" maxlength="80" />
      <input type="text" id="composite-preset-desc-input" placeholder="Description (optional)" maxlength="200" />
      <div class="composite-preset-save-actions">
        <button type="button" id="composite-preset-save-confirm" class="primary-btn">Save</button>
        <button type="button" id="composite-preset-save-cancel" class="secondary-btn">Cancel</button>
      </div>
    </div>`;
  container.insertAdjacentHTML("afterbegin", formHtml);

  const form = container.querySelector(".composite-preset-save-form")!;
  const nameInput = form.querySelector<HTMLInputElement>("#composite-preset-name-input")!;
  const descInput = form.querySelector<HTMLInputElement>("#composite-preset-desc-input")!;

  nameInput.focus();

  form.querySelector("#composite-preset-save-confirm")?.addEventListener("click", () => {
    const name = nameInput.value.trim();
    if (!name) { nameInput.classList.add("input-error"); return; }
    saveCompositePreset(name, descInput.value.trim());
    form.remove();
  });

  form.querySelector("#composite-preset-save-cancel")?.addEventListener("click", () => {
    form.remove();
  });

  nameInput.addEventListener("keydown", (e) => {
    if ((e as KeyboardEvent).key === "Enter") {
      const name = nameInput.value.trim();
      if (!name) { nameInput.classList.add("input-error"); return; }
      saveCompositePreset(name, descInput.value.trim());
      form.remove();
    } else if ((e as KeyboardEvent).key === "Escape") {
      form.remove();
    }
  });
}

// ── Message handlers ──────────────────────────────────────────────────────────

export function handleCompositePresetList(presets: CompositePreset[]): void {
  uiState.compositePresets = presets;
  renderCompositePresetList();
}

export function handleCompositePresetSaved(id: string, name: string): void {
  showNotification(`Multi-Rig "${name}" saved.`, "success");
  getCompositePresetList();
}

export function handleCompositePresetLoaded(id: string, name: string): void {
  showNotification(`Multi-Rig "${name}" loaded.`, "success");
}

// ── Tab switching ─────────────────────────────────────────────────────────────

export function initMultiRigTab(): void {
  const presetsTab = document.getElementById("preset-lib-tab-presets");
  const multiRigTab = document.getElementById("preset-lib-tab-multi-rig");
  const presetsPanel = document.getElementById("preset-library-presets-panel");
  const multiRigPanel = document.getElementById("preset-library-multi-rig-panel");

  if (!presetsTab || !multiRigTab || !presetsPanel || !multiRigPanel) return;

  presetsTab.addEventListener("click", () => {
    presetsTab.classList.add("active");
    multiRigTab.classList.remove("active");
    presetsPanel.hidden = false;
    multiRigPanel.hidden = true;
  });

  multiRigTab.addEventListener("click", () => {
    if (!isFeatureEnabled(Features.MultiRig)) {
      presetsTab.click();
      return;
    }

    multiRigTab.classList.add("active");
    presetsTab.classList.remove("active");
    presetsPanel.hidden = true;
    multiRigPanel.hidden = false;
    // Refresh list on open
    getCompositePresetList();
  });

  // "Save Multi-Rig…" button in the mixer panel fires a custom event
  document.addEventListener("mixerSaveMultiRig", () => {
    if (!isFeatureEnabled(Features.MultiRig)) {
      return;
    }

    // Switch to Multi-Rig tab so the save form is visible
    multiRigTab.click();
    handleSaveCompositePresetFlow();
  });
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function escapeHtml(str: string): string {
  return str
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}
