import { uiState, getActivePresetForRender } from "../state.js";
import type {
  GraphNode,
  Preset,
  StoredEffectPreset,
} from "../types.js";
import { postMessage } from "../bridge.js";
import { escapeHtml } from "../utils.js";
import { showNotification } from "../notifications.js";
import { showConfirm } from "../dialogs.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import {
  nodeParamsPanelElement,
} from "./state.js";
import { sendSignalPathNodeParamUpdate } from "./commands.js";
import { requestNodeParamsRefresh } from "./render.js";
/**
 * Storage key for a node's user presets. Resolved to the canonical effect type so
 * a node still carrying a legacy alias (e.g. "eq_graphic") shares one bucket with
 * nodes using the GUID, instead of quietly splitting the user's saved presets.
 */
export function effectPresetStorageKey(node: GraphNode): string {
  return EffectTypeRegistry.resolve(node.type) || node.type;
}

export function getUserEffectPresets(node: GraphNode): StoredEffectPreset[] {
  return uiState.effectPresets?.[effectPresetStorageKey(node)] ?? [];
}

/**
 * Apply a preset's parameters to a node.
 *
 * `order` comes from a factory preset's parameterOrder and matters where one
 * parameter constrains another — Graphic EQ's bandCount has to land before the
 * per-band values it bounds. Keys the effect no longer declares are skipped, so
 * a snapshot saved before a parameter was renamed cannot inject dead keys.
 */
export function applyEffectPresetParams(
  node: GraphNode,
  preset: Preset,
  parameters: Record<string, number>,
  order?: string[],
): void {
  const known = new Set((getNodeEffectInfo(node)?.parameters ?? []).map((def) => def.key));
  const explicitOrder = order ?? [];
  const ordered = [
    ...explicitOrder.filter((key) => key in parameters),
    ...Object.keys(parameters).filter((key) => !explicitOrder.includes(key)),
  ];

  for (const key of ordered) {
    const value = parameters[key];
    if (!known.has(key) || typeof value !== "number" || !Number.isFinite(value)) {
      continue;
    }
    node.params[key] = value;
    sendSignalPathNodeParamUpdate(node.id, key, value);
  }
  // Re-render so knobs, toggles and the EQ/spatial visualisations reflect the load.
  requestNodeParamsRefresh();
}

export let effectPresetsPopover: HTMLElement | null = null;

export let closeEffectPresetsPopover: (() => void) | null = null;

/** Closes the presets flyout if it is open. Safe to call at any time. */
export function closeEffectPresetsFlyout(): void {
  closeEffectPresetsPopover?.();
}

/** Re-render in place; called when the backend re-broadcasts the preset store. */
export function refreshEffectPresetsFlyout(): void {
  effectPresetsPopover?.dispatchEvent(new CustomEvent("effect-presets-refresh"));
}

/** Option values encode which list an entry came from: "factory:id" / "user:id". */
export function parseEffectPresetOptionValue(value: string): { kind: string; id: string } | null {
  const separator = value.indexOf(":");
  if (separator <= 0) return null;
  return { kind: value.slice(0, separator), id: value.slice(separator + 1) };
}

export function openEffectPresetsFlyout(anchor: HTMLElement, nodeId: string): void {
  // Re-clicking the same anchor closes, matching the layout picker.
  if (effectPresetsPopover) {
    const sameAnchor = effectPresetsPopover.dataset.anchorId === nodeId;
    closeEffectPresetsFlyout();
    if (sameAnchor) return;
  }

  /** Resolve the node fresh each render: a preset switch can invalidate it. */
  const resolveTarget = (): { node: GraphNode; preset: Preset } | null => {
    const preset = getActivePresetForRender() ?? undefined;
    const node = preset?.graph?.nodes.find((candidate) => candidate.id === nodeId);
    return preset && node ? { node, preset } : null;
  };

  if (!resolveTarget()) return;

  const popover = document.createElement("div");
  popover.className = "effect-presets-popover";
  popover.dataset.anchorId = nodeId;
  popover.setAttribute("role", "dialog");
  popover.setAttribute("aria-label", "Effect presets");
  document.body.appendChild(popover);
  effectPresetsPopover = popover;

  const position = (): void => {
    const rect = anchor.getBoundingClientRect();
    const margin = 8;
    let left = rect.right - popover.offsetWidth;
    left = Math.max(margin, Math.min(left, window.innerWidth - popover.offsetWidth - margin));
    let top = rect.bottom + 6;
    if (top + popover.offsetHeight > window.innerHeight - margin) {
      top = Math.max(margin, rect.top - popover.offsetHeight - 6);
    }
    popover.style.left = `${Math.round(left)}px`;
    popover.style.top = `${Math.round(top)}px`;
  };

  const onDocumentPointerDown = (event: PointerEvent) => {
    const target = event.target as Node | null;
    if (!target) return;
    if (popover.contains(target) || anchor.contains(target)) return;
    close();
  };
  const onKeyDown = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.stopPropagation();
      close();
    }
  };
  const onReposition = () => position();

  function close(): void {
    document.removeEventListener("pointerdown", onDocumentPointerDown, true);
    document.removeEventListener("keydown", onKeyDown, true);
    window.removeEventListener("resize", onReposition);
    window.removeEventListener("scroll", onReposition, true);
    popover.remove();
    if (effectPresetsPopover === popover) {
      effectPresetsPopover = null;
      closeEffectPresetsPopover = null;
    }
    anchor.setAttribute("aria-expanded", "false");
  }

  function render(): void {
    const target = resolveTarget();
    if (!target) {
      close();
      return;
    }
    const { node } = target;
    const factoryPresets = getNodeEffectInfo(node)?.presets ?? [];
    const userPresets = getUserEffectPresets(node);

    const options = (entries: { id: string; name: string }[], kind: string): string =>
      entries
        .map((entry) => `<option value="${escapeHtml(kind)}:${escapeHtml(entry.id)}">${escapeHtml(entry.name)}</option>`)
        .join("");

    // With nothing to choose from, the flyout collapses to just the save row —
    // an empty dropdown and a permanently disabled Delete are only clutter.
    const hasAnyPresets = factoryPresets.length > 0 || userPresets.length > 0;

    popover.innerHTML = `
      ${hasAnyPresets ? `
      <select class="effect-presets-picker" aria-label="Load a preset for this effect">
        <option value="">Select a preset…</option>
        ${factoryPresets.length ? `<optgroup label="Factory">${options(factoryPresets, "factory")}</optgroup>` : ""}
        ${userPresets.length ? `<optgroup label="My presets">${options(userPresets, "user")}</optgroup>` : ""}
      </select>` : ""}
      <div class="effect-presets-popover-row">
        <input class="effect-presets-popover-name" type="text" placeholder="Save current as…" aria-label="Name for the saved effect preset" />
        <button class="effect-presets-popover-save" type="button">Save</button>
      </div>
      ${userPresets.length ? `<button class="effect-presets-popover-delete" type="button" disabled>Delete selected</button>` : ""}
    `;
    bind();
    position();
  }

  function bind(): void {
    const picker = popover.querySelector<HTMLSelectElement>(".effect-presets-picker");
    const nameInput = popover.querySelector<HTMLInputElement>(".effect-presets-popover-name");
    const saveBtn = popover.querySelector<HTMLButtonElement>(".effect-presets-popover-save");
    const deleteBtn = popover.querySelector<HTMLButtonElement>(".effect-presets-popover-delete");
    if (!nameInput || !saveBtn) return;

    picker?.addEventListener("change", () => {
      const selection = parseEffectPresetOptionValue(picker.value);
      // Only the user's own presets can be deleted; factory ones are read-only.
      if (deleteBtn) deleteBtn.disabled = selection?.kind !== "user";

      const target = resolveTarget();
      if (!target || !selection) return;

      if (selection.kind === "factory") {
        const entry = (getNodeEffectInfo(target.node)?.presets ?? []).find((c) => c.id === selection.id);
        if (entry) applyEffectPresetParams(target.node, target.preset, entry.parameters, entry.parameterOrder);
      } else {
        const entry = getUserEffectPresets(target.node).find((c) => c.id === selection.id);
        if (entry) applyEffectPresetParams(target.node, target.preset, entry.parameters);
      }
    });

    const save = async (): Promise<void> => {
      const target = resolveTarget();
      if (!target) return;
      const name = nameInput.value.trim();
      if (!name) {
        nameInput.focus();
        showNotification("Name required", "Enter a name for this effect preset.");
        return;
      }
      if (getUserEffectPresets(target.node).some((c) => c.name === name)) {
        const confirmed = await showConfirm(`Replace the saved settings named "${name}"?`, "Overwrite preset");
        if (!confirmed) return;
      }
      // The backend owns the store and re-broadcasts, which re-renders this flyout.
      postMessage({
        type: "saveEffectPreset",
        effectType: effectPresetStorageKey(target.node),
        name,
        parameters: { ...target.node.params },
      });
      nameInput.value = "";
      showNotification("Effect preset saved", name);
    };

    saveBtn.addEventListener("click", () => void save());
    nameInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        void save();
      }
    });

    deleteBtn?.addEventListener("click", () => {
      const selection = parseEffectPresetOptionValue(picker?.value ?? "");
      const target = resolveTarget();
      if (!target || selection?.kind !== "user") return;
      const entry = getUserEffectPresets(target.node).find((c) => c.id === selection.id);
      if (!entry) return;
      void (async () => {
        const confirmed = await showConfirm(`Delete the saved settings named "${entry.name}"?`, "Delete preset");
        if (!confirmed) return;
        postMessage({
          type: "deleteEffectPreset",
          effectType: effectPresetStorageKey(target.node),
          presetId: entry.id,
        });
        showNotification("Effect preset deleted", entry.name);
      })();
    });
  }

  popover.addEventListener("effect-presets-refresh", () => render());

  closeEffectPresetsPopover = close;
  anchor.setAttribute("aria-expanded", "true");
  document.addEventListener("pointerdown", onDocumentPointerDown, true);
  document.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("resize", onReposition);
  window.addEventListener("scroll", onReposition, true);

  render();
}

export function bindEffectPresetsButton(node: GraphNode): void {
  const button = nodeParamsPanelElement?.querySelector<HTMLButtonElement>("[data-effect-presets-open]");
  if (!button) return;
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    openEffectPresetsFlyout(button, node.id);
  });
}
