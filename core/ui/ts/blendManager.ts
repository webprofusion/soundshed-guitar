import { uiState } from "./state.js";
import { openBlendEditorWithDefinition } from "./signalPath.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import type { BlendDefinition, Preset } from "./types.js";

const blendList = document.getElementById("blend-list");
const blendSearchInput = document.getElementById("blend-search-input") as HTMLInputElement | null;

let initialized = false;
let searchFilter = "";

export function initBlendManager(): void {
  if (initialized) return;
  initialized = true;

  blendSearchInput?.addEventListener("input", () => {
    searchFilter = blendSearchInput.value.toLowerCase();
    renderBlendList();
  });

  renderBlendList();
}

export function renderBlendList(): void {
  if (!blendList) return;

  const blends = uiState.blendLibrary ?? [];
  const filtered = searchFilter
    ? blends.filter((blend) => {
        const haystack = [
          blend.id,
          blend.name,
          blend.category,
          blend.toneGroupId ?? "",
          blend.toneGroupTitle ?? "",
          ...(blend.parameters ?? []),
        ]
          .join(" ")
          .toLowerCase();
        return haystack.includes(searchFilter);
      })
    : blends;

  if (!filtered.length) {
    blendList.innerHTML = `
      <div class="composite-empty">
        ${searchFilter ? "No blends match your search." : "No blends defined yet."}
      </div>`;
    return;
  }

  blendList.innerHTML = filtered
    .map((blend) => {
      const modelCount = blend.models?.length ?? 0;
      const paramCount = blend.parameters?.length ?? 0;
      const mode = blend.blendMode ?? "interpolate";
      return `
        <div class="composite-list-item" data-blend-id="${escAttr(blend.id)}">
          <div class="composite-list-info">
            <span class="composite-list-name">${escHtml(blend.name || blend.id)}</span>
            <span class="composite-list-meta">${escHtml(blend.category || "amp")} · ${modelCount} models · ${paramCount} params · ${escHtml(mode)}</span>
            ${blend.toneGroupTitle ? `<span class="composite-list-desc">${escHtml(blend.toneGroupTitle)}</span>` : ""}
          </div>
          <div class="composite-list-actions">
            <button class="blend-edit-btn advanced-action-btn" data-blend-id="${escAttr(blend.id)}" title="Edit">Edit</button>
            <button class="blend-delete-btn advanced-action-btn danger" data-blend-id="${escAttr(blend.id)}" title="Delete">Delete</button>
          </div>
        </div>
      `;
    })
    .join("");

  blendList.querySelectorAll(".blend-edit-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const blendId = (btn as HTMLElement).dataset.blendId ?? "";
      if (!blendId) return;
      const blend = (uiState.blendLibrary ?? []).find((entry) => entry.id === blendId);
      if (!blend) {
        showNotification("Blend not found");
        return;
      }
      const clone = JSON.parse(JSON.stringify(blend)) as BlendDefinition;
      openBlendEditorWithDefinition(clone);
    });
  });

  blendList.querySelectorAll(".blend-delete-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const blendId = (btn as HTMLElement).dataset.blendId ?? "";
      if (!blendId) return;
      void confirmDeleteBlend(blendId);
    });
  });
}

async function confirmDeleteBlend(blendId: string): Promise<void> {
  const blend = (uiState.blendLibrary ?? []).find((entry) => entry.id === blendId);
  if (!blend) {
    showNotification("Blend not found");
    return;
  }

  const usageCount = countBlendUsage(blendId);
  if (usageCount > 0) {
    showNotification("Delete blocked", `Blend is used by ${usageCount} preset(s).`);
    return;
  }

  const confirmed = await showConfirm(
    `Delete blend "${blend.name || blend.id}"? This cannot be undone.`,
    "Delete blend",
  );
  if (!confirmed) {
    return;
  }

  postMessage({
    type: "deleteBlendDefinition",
    blendId,
  });
}

function countBlendUsage(blendId: string): number {
  let count = 0;
  const seen = new Set<string>();

  const scanPreset = (preset?: Preset | null): void => {
    if (!preset || seen.has(preset.id)) {
      return;
    }
    seen.add(preset.id);
    const nodes = preset.graph?.nodes ?? [];
    const used = nodes.some((node) => {
      if (node.type !== "amp_nam_blend") {
        return false;
      }
      const nodeBlendId = (node.config?.blendId ?? "").trim();
      return nodeBlendId === blendId;
    });
    if (used) {
      count += 1;
    }
  };

  uiState.presets.forEach((preset) => scanPreset(preset));
  uiState.presetCache.forEach((preset) => scanPreset(preset));

  return count;
}

function escHtml(str: string): string {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\"/g, "&quot;");
}

function escAttr(str: string): string {
  return str.replace(/&/g, "&amp;").replace(/\"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
