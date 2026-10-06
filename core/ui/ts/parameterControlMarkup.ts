/**
 * Shared, render-only parameter control markup. The layout designer uses this
 * for previews without importing the live signal-path panel.
 */

import { formatParamValue } from "./layoutRenderer.js";
import { effectiveTaper } from "./paramTaper.js";
import type { ParameterDef } from "./presetV2.js";
import { escapeHtml } from "./utils.js";

export function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

export function isToggleParam(paramDef: { key: string; min?: number; max?: number; unit?: string }): boolean {
  return paramDef.unit === "toggle";
}

/**
 * Build default parameter controls using only definitions and default values.
 * The DOM structure matches the live panel; nodeId is used for knob attributes.
 */
export function buildDefaultParamControlsHtml(paramDefs: ParameterDef[], nodeId = "preview"): string {
  const renderOne = (p: ParameterDef): string => {
    const label = p.name || formatParamLabel(p.key);
    const value = p.default ?? 0;
    const min = p.min ?? 0;
    const max = p.max ?? 1;
    const unit = p.unit || "amount";
    const isToggle = isToggleParam(p);
    const isEnum = unit === "enum" && Array.isArray(p.labels) && p.labels.length > 0;
    const enumLabels = Array.isArray(p.labels) ? p.labels : [];
    const taper = effectiveTaper(p.taper, min, max);

    if (isToggle) {
      const checked = value >= 0.5;
      return `
        <div class="node-param-group">
          <span class="node-param-label">${label}</span>
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${escapeHtml(nodeId)}" data-param-key="${escapeHtml(p.key)}" ${checked ? "checked" : ""} disabled>
            <span class="toggle-slider"></span>
          </label>
          <span class="node-param-value">${checked ? "On" : "Off"}</span>
        </div>`;
    }

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div class="knob node-param-knob"
          data-node-id="${escapeHtml(nodeId)}"
          data-param-key="${escapeHtml(p.key)}"
          data-value="${value}"
          data-default="${value}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${escapeHtml(unit)}"
          ${p.step !== undefined ? `data-step="${p.step}"` : ""}
          ${isEnum ? `data-labels="${escapeHtml(enumLabels.join("|"))}"` : ""}
          ${taper === "log" ? `data-taper="log"` : ""}
        >
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${formatParamValue(value, unit, enumLabels, taper, min, p.step)}</span>
      </div>`;
  };

  const hasGroups = paramDefs.some((p) => typeof p.group === "string" && p.group.trim().length > 0);
  if (!hasGroups) {
    // Ungrouped controls sit in one untitled inset panel, as the live panel draws them.
    return paramDefs.length === 0 ? "" : `
    <div class="node-param-group-block node-param-group-block-untitled">
      <div class="node-param-group-items">
        ${paramDefs.map(renderOne).join("")}
      </div>
    </div>`;
  }

  const groupOrder: string[] = [];
  const groupMap = new Map<string, string[]>();
  paramDefs.forEach((p) => {
    const group = p.group?.trim() || "Other";
    if (!groupMap.has(group)) {
      groupMap.set(group, []);
      groupOrder.push(group);
    }
    groupMap.get(group)!.push(renderOne(p));
  });

  return groupOrder.map((group) => `
    <div class="node-param-group-block">
      <div class="node-param-group-title">${group}</div>
      <div class="node-param-group-items">
        ${groupMap.get(group)!.join("")}
      </div>
    </div>`).join("");
}
