/**
 * Effect Layout Picker
 *
 * Floating popover launched from the effect shell header. Lets the user switch the
 * selected effect between the standard auto-generated controls and any custom
 * layout available for that effect type, and decide how widely that choice sticks:
 * for the effect type, for models matching a make/model keyword, or for one preset.
 *
 * The popover is appended to <body> with fixed positioning because the effect
 * shell clips its own overflow.
 *
 * It opens from the right half of the header's layout split toggle; the left half
 * switches straight to the standard controls without it.
 */

import { escapeHtml } from "./utils.js";
import { showNotification } from "./notifications.js";
import {
  STANDARD_LAYOUT_ID,
  getAvailableLayoutEntries,
  getLayoutPreferenceRulesForKeys,
  layoutLookupKeysFor,
  removeLayoutPreference,
  resolveLayoutSelection,
  setLayoutPreference,
  suggestLayoutKeywords,
  type LayoutPreferenceRule,
  type LayoutPreferenceScope,
} from "./layoutPreferences.js";
import type { LayoutLibraryEntry } from "./layoutTypes.js";

export interface LayoutPickerContext {
  effectType: string;
  blendId?: string;
  /** Human label for the effect, shown in the popover header. */
  nodeLabel: string;
  /** Lower-cased make/model text this node would be matched against. */
  matchText: string;
  presetId: string | null;
  presetName: string;
  /** Called after any change so the caller can re-render the params panel. */
  onApplied: () => void;
  /**
   * Opens the layout designer: `null` designs a brand-new layout for this effect
   * type, a layout id edits that layout. Omit to hide the design actions.
   */
  onDesignLayout?: (layoutId: string | null) => void;
}

/** Pencil glyph for the per-layout edit buttons. */
const EDIT_ICON_SVG = `
  <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
    <path d="M4 20h4l10-10-4-4L4 16v4z" stroke="currentColor" stroke-width="1.6" fill="none" stroke-linejoin="round" />
    <path d="M14 6l4 4" stroke="currentColor" stroke-width="1.6" fill="none" />
  </svg>
`;

/** The popover's two sections: choosing a layout, and managing saved rules. */
type LayoutPickerTab = "layout" | "rules";

let openPopover: HTMLElement | null = null;
let closeCurrentPopover: (() => void) | null = null;

/** Closes the layout picker if it is open. Safe to call at any time. */
export function closeLayoutPicker(): void {
  closeCurrentPopover?.();
}

/** True when at least one custom layout exists for this effect (picker is useful). */
export function hasSelectableLayouts(effectType: string, blendId?: string): boolean {
  return getAvailableLayoutEntries(effectType, blendId).length > 0;
}

function scopeLabel(rule: LayoutPreferenceRule): string {
  switch (rule.scope) {
    case "preset":
      return `Preset: ${rule.presetName || rule.presetId || "(unknown)"}`;
    case "keyword":
      return `Matching “${rule.keyword}”`;
    default:
      return "This effect type";
  }
}

function layoutNameFor(layoutId: string, effectType: string, blendId?: string): string {
  if (layoutId === STANDARD_LAYOUT_ID) return "Standard controls";
  const entry = getAvailableLayoutEntries(effectType, blendId).find((e) => e.layoutId === layoutId);
  if (!entry) return "(missing layout)";
  return entry.layout.name || "(Unnamed layout)";
}

function entryMetaText(entry: LayoutLibraryEntry): string {
  return `${entry.layout.controls.length} controls · ${entry.layout.dimensions.width}×${entry.layout.dimensions.height}`
    + (entry.layout.author ? ` · by ${entry.layout.author}` : "");
}

/** Thumbnail + name/meta pair shared by the radio options, the dropdown trigger and its items. */
function renderChoiceBodyHtml(thumb: string, name: string, meta: string, badgesHtml: string): string {
  return `
    <span class="layout-picker-option-thumb">${thumb ? `<img src="${escapeHtml(thumb)}" alt="" />` : ""}</span>
    <span class="layout-picker-option-text">
      <span class="layout-picker-option-name">${escapeHtml(name)}${badgesHtml}</span>
      <span class="layout-picker-option-meta">${escapeHtml(meta)}</span>
    </span>
  `;
}

function renderEntryChoiceHtml(entry: LayoutLibraryEntry): string {
  return renderChoiceBodyHtml(
    entry.layout.thumbnailDataUrl ?? "",
    entry.layout.name || "(Unnamed layout)",
    entryMetaText(entry),
    entry.isFactory ? `<span class="layout-picker-badge">Factory</span>` : "",
  );
}

/** Factory layouts are read-only: opening one in the designer forks an editable copy. */
function editTitleFor(entry: LayoutLibraryEntry): string {
  const name = entry.layout.name || "(Unnamed layout)";
  return entry.isFactory
    ? `Edit a copy of “${name}” in the layout designer`
    : `Edit “${name}” in the layout designer`;
}

function renderEditButtonHtml(entry: LayoutLibraryEntry, extraClass = ""): string {
  const title = escapeHtml(editTitleFor(entry));
  return `<button type="button" class="layout-picker-option-edit${extraClass ? ` ${extraClass}` : ""}" data-layout-id="${escapeHtml(entry.layoutId)}" title="${title}" aria-label="${title}">${EDIT_ICON_SVG}</button>`;
}

function renderDropdownItemHtml(entry: LayoutLibraryEntry, isSelected: boolean, canDesign: boolean): string {
  return `
    <div class="layout-picker-dropdown-row">
      <button
        type="button"
        class="layout-picker-dropdown-item${isSelected ? " is-selected" : ""}"
        role="option"
        aria-selected="${isSelected ? "true" : "false"}"
        data-layout-id="${escapeHtml(entry.layoutId)}"
      >${renderEntryChoiceHtml(entry)}</button>
      ${canDesign ? renderEditButtonHtml(entry) : ""}
    </div>
  `;
}

function renderHeaderHtml(context: LayoutPickerContext): string {
  return `
    <div class="layout-picker-header">
      <div class="layout-picker-title">Effect layout</div>
      <div class="layout-picker-subtitle">${escapeHtml(context.nodeLabel)}</div>
      <button type="button" class="layout-picker-close" aria-label="Close">×</button>
    </div>
  `;
}

function renderPopoverInner(context: LayoutPickerContext, activeTab: LayoutPickerTab): string {
  const { effectType, blendId } = context;
  const entries = getAvailableLayoutEntries(effectType, blendId);
  const lookupKeys = layoutLookupKeysFor(effectType, blendId);
  const selection = resolveLayoutSelection({
    effectType,
    blendId,
    matchText: context.matchText,
    presetId: context.presetId,
  });
  const activeLayoutId = selection.layoutId ?? STANDARD_LAYOUT_ID;
  const rules = getLayoutPreferenceRulesForKeys(lookupKeys);
  const suggestions = suggestLayoutKeywords(context.matchText);

  const sourceNote = (() => {
    switch (selection.source) {
      case "preset":
        return "Currently set by a preset rule.";
      case "keyword":
        return `Currently set by the “${selection.rule?.keyword}” keyword rule.`;
      case "effectType":
        return "Currently set for this effect type.";
      case "libraryDefault":
        return "Currently using the layout library default.";
      default:
        return "No layout preference set — showing standard controls.";
    }
  })();

  const canDesign = typeof context.onDesignLayout === "function";

  // An effect type can accumulate many layouts, so they live behind one dropdown
  // rather than one radio each. The radio pair is "standard vs custom"; the dropdown
  // decides *which* custom layout, and carries its id as the custom radio's value.
  const customIsActive = activeLayoutId !== STANDARD_LAYOUT_ID
    && entries.some((entry) => entry.layoutId === activeLayoutId);
  const selectedEntry = (customIsActive
    ? entries.find((entry) => entry.layoutId === activeLayoutId)
    : entries[0]) ?? null;

  const standardOptionHtml = `
    <div class="layout-picker-option-row">
      <label class="layout-picker-option${customIsActive ? "" : " is-active"}">
        <input type="radio" name="layout-picker-layout" value="${escapeHtml(STANDARD_LAYOUT_ID)}"${customIsActive ? "" : " checked"} />
        ${renderChoiceBodyHtml("", "Standard controls", "Auto-generated knobs and switches", "")}
      </label>
    </div>
  `;

  const customOptionHtml = selectedEntry
    ? `
    <div class="layout-picker-option-row">
      <label class="layout-picker-option${customIsActive ? " is-active" : ""}">
        <input
          type="radio"
          name="layout-picker-layout"
          class="layout-picker-custom-radio"
          value="${escapeHtml(selectedEntry.layoutId)}"${customIsActive ? " checked" : ""}
        />
        ${renderChoiceBodyHtml("", "Custom layout", entries.length === 1 ? "1 layout available" : `${entries.length} layouts available`, "")}
      </label>
    </div>
    <div class="layout-picker-dropdown-row layout-picker-custom-row">
      <details class="layout-picker-dropdown">
        <summary class="layout-picker-dropdown-trigger" title="Choose a custom layout">
          <span class="layout-picker-dropdown-selected">${renderEntryChoiceHtml(selectedEntry)}</span>
          <span class="layout-picker-dropdown-caret" aria-hidden="true">▾</span>
        </summary>
        <div class="layout-picker-dropdown-list" role="listbox" aria-label="Custom layouts">
          ${entries.map((entry) => renderDropdownItemHtml(entry, entry.layoutId === selectedEntry.layoutId, canDesign)).join("")}
        </div>
      </details>
      ${canDesign ? renderEditButtonHtml(selectedEntry, "layout-picker-dropdown-edit") : ""}
    </div>
  `
    : `<div class="layout-picker-empty">No custom layouts for this effect yet.</div>`;

  const optionsHtml = standardOptionHtml + customOptionHtml;

  const presetScopeHtml = context.presetId
    ? `
      <label class="layout-picker-scope">
        <input type="radio" name="layout-picker-scope" value="preset" />
        <span>Only the preset “${escapeHtml(context.presetName || context.presetId)}”</span>
      </label>
    `
    : `
      <label class="layout-picker-scope is-disabled">
        <input type="radio" name="layout-picker-scope" value="preset" disabled />
        <span>Only this preset (save the preset first)</span>
      </label>
    `;

  const suggestionsHtml = suggestions.length
    ? `<div class="layout-picker-suggestions">${suggestions
      .map((keyword) => `<button type="button" class="layout-picker-suggestion" data-keyword="${escapeHtml(keyword)}">${escapeHtml(keyword)}</button>`)
      .join("")}</div>`
    : "";

  const rulesHtml = rules.length
    ? `
      <div class="layout-picker-note">Most specific rule wins: preset, then keyword, then effect type.</div>
      <ul class="layout-picker-rules">
        ${rules.map((rule) => `
          <li class="layout-picker-rule">
            <span class="layout-picker-rule-text">
              <strong>${escapeHtml(scopeLabel(rule))}</strong>
              <span>→ ${escapeHtml(layoutNameFor(rule.layoutId, effectType, blendId))}</span>
            </span>
            <button type="button" class="layout-picker-rule-remove" data-rule-id="${escapeHtml(rule.id)}" title="Delete rule" aria-label="Delete rule">×</button>
          </li>
        `).join("")}
      </ul>
    `
    : `<div class="layout-picker-empty">No saved rules for this effect yet.</div>`;

  const onLayoutTab = activeTab === "layout";

  return `
    ${renderHeaderHtml(context)}
    <div class="layout-picker-tabs" role="tablist" aria-label="Effect layout sections">
      <button
        type="button"
        class="layout-picker-tab${onLayoutTab ? " is-active" : ""}"
        role="tab"
        data-tab="layout"
        aria-selected="${onLayoutTab ? "true" : "false"}"
      >Layout</button>
      <button
        type="button"
        class="layout-picker-tab${onLayoutTab ? "" : " is-active"}"
        role="tab"
        data-tab="rules"
        aria-selected="${onLayoutTab ? "false" : "true"}"
      >Rules${rules.length ? ` (${rules.length})` : ""}</button>
    </div>
    <div class="layout-picker-body">
      <div class="layout-picker-tabpanel" data-tab="layout" role="tabpanel"${onLayoutTab ? "" : " hidden"}>
        <div class="layout-picker-note">${escapeHtml(sourceNote)}</div>
        <div class="layout-picker-section">
          <div class="layout-picker-section-title">Layout</div>
          <div class="layout-picker-options">${optionsHtml}</div>
        </div>
        <div class="layout-picker-section">
          <div class="layout-picker-section-title">Remember this for</div>
          <label class="layout-picker-scope">
            <input type="radio" name="layout-picker-scope" value="effectType" checked />
            <span>Every use of this effect</span>
          </label>
          ${presetScopeHtml}
          <label class="layout-picker-scope">
            <input type="radio" name="layout-picker-scope" value="keyword" />
            <span>Amps/FX matching a keyword</span>
          </label>
          <input
            class="layout-picker-keyword"
            type="text"
            placeholder="e.g. twin, ac30, rectifier"
            value="${escapeHtml(suggestions[0] ?? "")}"
            aria-label="Make or model keyword"
          />
          ${suggestionsHtml}
        </div>
      </div>
      <div class="layout-picker-tabpanel" data-tab="rules" role="tabpanel"${onLayoutTab ? " hidden" : ""}>
        ${rulesHtml}
      </div>
    </div>
    <div class="layout-picker-footer">
      <div class="layout-picker-footer-left">
        ${canDesign
      ? `<button type="button" class="layout-picker-btn layout-picker-new" title="Design a new layout for this effect type">New layout…</button>`
      : ""}
      </div>
      <button type="button" class="layout-picker-btn layout-picker-apply is-primary"${onLayoutTab ? "" : " hidden"}>Apply</button>
    </div>
  `;
}

/** Keeps the standard/custom rows highlighted in step with the checked radio. */
function updateActiveOptionHighlight(popover: HTMLElement): void {
  popover.querySelectorAll<HTMLLabelElement>(".layout-picker-option").forEach((label) => {
    const radio = label.querySelector<HTMLInputElement>('input[name="layout-picker-layout"]');
    label.classList.toggle("is-active", Boolean(radio?.checked));
  });
}

/**
 * Wires the custom-layout dropdown. Picking an item points the "Custom layout" radio
 * at that layout — the radio's value is the id Apply reads — then updates the trigger
 * and collapses the list.
 */
function bindCustomLayoutDropdown(popover: HTMLElement, context: LayoutPickerContext): void {
  popover.querySelectorAll<HTMLInputElement>('input[name="layout-picker-layout"]').forEach((radio) => {
    radio.addEventListener("change", () => updateActiveOptionHighlight(popover));
  });

  const details = popover.querySelector<HTMLDetailsElement>(".layout-picker-dropdown");
  const customRadio = popover.querySelector<HTMLInputElement>(".layout-picker-custom-radio");
  if (!details || !customRadio) return;

  const selectedDisplay = details.querySelector<HTMLElement>(".layout-picker-dropdown-selected");
  const triggerEdit = popover.querySelector<HTMLButtonElement>(".layout-picker-dropdown-edit");
  const items = Array.from(details.querySelectorAll<HTMLButtonElement>(".layout-picker-dropdown-item"));

  items.forEach((item) => {
    item.addEventListener("click", () => {
      const layoutId = item.dataset.layoutId;
      if (!layoutId) return;
      const entry = getAvailableLayoutEntries(context.effectType, context.blendId)
        .find((candidate) => candidate.layoutId === layoutId);
      if (!entry) return;

      customRadio.value = layoutId;
      customRadio.checked = true;

      if (selectedDisplay) {
        selectedDisplay.innerHTML = renderEntryChoiceHtml(entry);
      }
      if (triggerEdit) {
        triggerEdit.dataset.layoutId = layoutId;
        const title = editTitleFor(entry);
        triggerEdit.title = title;
        triggerEdit.setAttribute("aria-label", title);
      }
      items.forEach((other) => {
        const isSelected = other === item;
        other.classList.toggle("is-selected", isSelected);
        other.setAttribute("aria-selected", isSelected ? "true" : "false");
      });

      details.open = false;
      updateActiveOptionHighlight(popover);
    });
  });
}

/**
 * Anchor rects are cached because applying a change re-renders the params panel,
 * which detaches the button we are anchored to — a detached element measures as
 * 0×0 at the origin and would fling the popover into the top-left corner.
 */
const lastAnchorRects = new WeakMap<HTMLElement, DOMRect>();

function positionPopover(popover: HTMLElement, anchor: HTMLElement): void {
  let rect = anchor.getBoundingClientRect();
  if (anchor.isConnected && (rect.width || rect.height)) {
    lastAnchorRects.set(anchor, rect);
  } else {
    rect = lastAnchorRects.get(anchor) ?? rect;
  }
  const width = popover.offsetWidth;
  const height = popover.offsetHeight;
  const margin = 8;

  let left = rect.right - width;
  left = Math.max(margin, Math.min(left, window.innerWidth - width - margin));

  let top = rect.bottom + 6;
  if (top + height > window.innerHeight - margin) {
    // Not enough room below — flip above the button, then clamp.
    top = Math.max(margin, rect.top - height - 6);
  }

  popover.style.left = `${Math.round(left)}px`;
  popover.style.top = `${Math.round(top)}px`;
}

/**
 * Opens the layout picker anchored to `anchor`. Re-opening while already open
 * toggles it closed, matching the other header popovers.
 */
export function openLayoutPicker(anchor: HTMLElement, context: LayoutPickerContext): void {
  if (openPopover) {
    const sameAnchor = openPopover.dataset.anchorId === anchor.dataset.nodeId;
    closeLayoutPicker();
    if (sameAnchor) return;
  }

  // Survives re-renders (e.g. after deleting a rule) so the user stays on their tab.
  let activeTab: LayoutPickerTab = "layout";

  const popover = document.createElement("div");
  popover.className = "layout-picker-popover";
  popover.dataset.anchorId = anchor.dataset.nodeId ?? "";
  popover.setAttribute("role", "dialog");
  popover.setAttribute("aria-label", "Effect layout");
  popover.innerHTML = renderPopoverInner(context, activeTab);
  document.body.appendChild(popover);
  openPopover = popover;

  positionPopover(popover, anchor);

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
  const onReposition = () => positionPopover(popover, anchor);

  function close(): void {
    document.removeEventListener("pointerdown", onDocumentPointerDown, true);
    document.removeEventListener("keydown", onKeyDown, true);
    window.removeEventListener("resize", onReposition);
    window.removeEventListener("scroll", onReposition, true);
    popover.remove();
    if (openPopover === popover) {
      openPopover = null;
      closeCurrentPopover = null;
    }
    anchor.setAttribute("aria-expanded", "false");
  }

  closeCurrentPopover = close;
  anchor.setAttribute("aria-expanded", "true");
  document.addEventListener("pointerdown", onDocumentPointerDown, true);
  document.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("resize", onReposition);
  window.addEventListener("scroll", onReposition, true);

  const rerender = () => {
    popover.innerHTML = renderPopoverInner(context, activeTab);
    bind();
    positionPopover(popover, anchor);
  };

  /** Tab switching is a pure show/hide so the pending layout selection survives it. */
  const showTab = (tab: LayoutPickerTab) => {
    activeTab = tab;
    popover.querySelectorAll<HTMLButtonElement>(".layout-picker-tab").forEach((btn) => {
      const isActive = btn.dataset.tab === tab;
      btn.classList.toggle("is-active", isActive);
      btn.setAttribute("aria-selected", isActive ? "true" : "false");
    });
    popover.querySelectorAll<HTMLElement>(".layout-picker-tabpanel").forEach((panel) => {
      panel.hidden = panel.dataset.tab !== tab;
    });
    // Apply commits the Layout tab's selection; it has nothing to do on Rules.
    const applyBtn = popover.querySelector<HTMLButtonElement>(".layout-picker-apply");
    if (applyBtn) applyBtn.hidden = tab !== "layout";
    positionPopover(popover, anchor);
  };

  function bind(): void {
    popover.querySelector<HTMLButtonElement>(".layout-picker-close")?.addEventListener("click", () => close());

    popover.querySelectorAll<HTMLButtonElement>(".layout-picker-tab").forEach((btn) => {
      btn.addEventListener("click", () => {
        const tab = btn.dataset.tab;
        if (tab === "layout" || tab === "rules") showTab(tab);
      });
    });

    bindCustomLayoutDropdown(popover, context);

    const keywordInput = popover.querySelector<HTMLInputElement>(".layout-picker-keyword");
    const scopeInputs = Array.from(popover.querySelectorAll<HTMLInputElement>('input[name="layout-picker-scope"]'));

    popover.querySelectorAll<HTMLButtonElement>(".layout-picker-suggestion").forEach((btn) => {
      btn.addEventListener("click", () => {
        if (keywordInput) {
          keywordInput.value = btn.dataset.keyword ?? "";
        }
        const keywordScope = scopeInputs.find((input) => input.value === "keyword");
        if (keywordScope) keywordScope.checked = true;
      });
    });

    keywordInput?.addEventListener("focus", () => {
      const keywordScope = scopeInputs.find((input) => input.value === "keyword");
      if (keywordScope) keywordScope.checked = true;
    });

    popover.querySelectorAll<HTMLButtonElement>(".layout-picker-rule-remove").forEach((btn) => {
      btn.addEventListener("click", () => {
        const ruleId = btn.dataset.ruleId;
        if (!ruleId) return;
        removeLayoutPreference(ruleId);
        context.onApplied();
        rerender();
      });
    });

    // Design actions close the popover first: the designer is a modal the popover
    // would otherwise sit over.
    popover.querySelector<HTMLButtonElement>(".layout-picker-new")?.addEventListener("click", () => {
      close();
      context.onDesignLayout?.(null);
    });

    popover.querySelectorAll<HTMLButtonElement>(".layout-picker-option-edit").forEach((btn) => {
      btn.addEventListener("click", (event) => {
        // Keep the click off the sibling label so editing does not change the selection.
        event.preventDefault();
        event.stopPropagation();
        const layoutId = btn.dataset.layoutId;
        if (!layoutId) return;
        close();
        context.onDesignLayout?.(layoutId);
      });
    });

    popover.querySelector<HTMLButtonElement>(".layout-picker-apply")?.addEventListener("click", () => {
      const layoutInput = popover.querySelector<HTMLInputElement>('input[name="layout-picker-layout"]:checked');
      if (!layoutInput) return;
      const layoutId = layoutInput.value;
      const scope = (scopeInputs.find((input) => input.checked)?.value ?? "effectType") as LayoutPreferenceScope;
      const keyword = (keywordInput?.value ?? "").trim().toLowerCase();

      if (scope === "keyword" && !keyword) {
        showNotification("Enter a make or model keyword to match");
        keywordInput?.focus();
        return;
      }
      if (scope === "preset" && !context.presetId) {
        showNotification("Save the preset before pinning a layout to it");
        return;
      }

      // Blend-specific layouts are keyed on the blend; everything else on the type.
      const lookupKey = layoutLookupKeysFor(context.effectType, context.blendId)[0];
      setLayoutPreference({
        lookupKey,
        scope,
        layoutId,
        keyword: scope === "keyword" ? keyword : undefined,
        presetId: scope === "preset" ? (context.presetId ?? undefined) : undefined,
        presetName: scope === "preset" ? context.presetName : undefined,
      });

      showNotification(`Layout set to ${layoutNameFor(layoutId, context.effectType, context.blendId)}`);
      context.onApplied();
      close();
    });
  }

  bind();
}
