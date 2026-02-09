/**
 * Layout Renderer
 *
 * Renders effect parameter panels using custom layouts.
 */

import { uiState } from "./state.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import type {
  EffectLayout,
  LayoutControl,
  LayoutTextLabel,
  LayoutBackground,
} from "./layoutTypes.js";
import { layoutLookupKey } from "./layoutTypes.js";
import type { GraphNode } from "./types.js";

/**
 * Check if a custom layout exists for an effect type (and optionally a specific blend).
 * When blendId is provided, checks for a per-blend layout first.
 */
export function hasCustomLayout(effectType: string, blendId?: string): boolean {
  if (!uiState.layoutLibrary) return false;
  const key = layoutLookupKey(effectType, blendId);
  const defaultId = uiState.layoutLibrary.defaults[key];
  if (!defaultId) return false;
  const entries = uiState.layoutLibrary.byEffectType[key];
  return entries?.some((e) => e.layoutId === defaultId) ?? false;
}

/**
 * Get the custom layout for an effect type (and optionally a specific blend).
 * When blendId is provided, looks up the per-blend layout.
 */
export function getCustomLayout(effectType: string, blendId?: string): EffectLayout | null {
  if (!uiState.layoutLibrary) return null;
  const key = layoutLookupKey(effectType, blendId);
  const defaultId = uiState.layoutLibrary.defaults[key];
  if (!defaultId) return null;
  const entries = uiState.layoutLibrary.byEffectType[key];
  const entry = entries?.find((e) => e.layoutId === defaultId);
  return entry?.layout ?? null;
}

/**
 * Get a layout image URL by imageId
 */
function getLayoutImageUrl(imageId: string): string | null {
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === imageId);
  if (image) {
    // Prefer data URL (base64) for WebView access
    if (image.dataUrl) {
      return image.dataUrl;
    }
    if (image.fileName) {
      return `layout-images/${image.fileName}`;
    }
  }
  return null;
}

/**
 * Render a custom layout for a node's parameters panel
 */
export function renderCustomLayout(
  node: GraphNode,
  layout: EffectLayout,
  paramDefs: ParameterDef[]
): string {
  const backgrounds = renderBackgrounds(layout.backgrounds);
  const controls = renderControls(node, layout.controls, paramDefs);
  const labels = renderTextLabels(layout.textLabels);

  return `
    <div 
      class="custom-layout-container" 
      style="
        position: relative;
        width: ${layout.dimensions.width}px;
        height: ${layout.dimensions.height}px;
        overflow: hidden;
        border-radius: 8px;
        background: var(--bg-dark-secondary);
        margin: 0;
        padding: 0;
        box-sizing: border-box;
        flex-shrink: 0;
      "
    >
      ${backgrounds}
      <div class="custom-layout-controls" style="position: absolute; inset: 0; z-index: 2; margin: 0; padding: 0;">
        ${controls}
      </div>
      <div class="custom-layout-labels" style="position: absolute; inset: 0; z-index: 3; pointer-events: none; margin: 0; padding: 0;">
        ${labels}
      </div>
    </div>
  `;
}

/**
 * Render background layers
 */
function renderBackgrounds(backgrounds: LayoutBackground[]): string {
  return backgrounds
    .sort((a, b) => a.layerIndex - b.layerIndex)
    .map((bg) => {
      let style = `
        position: absolute;
        inset: 0;
        z-index: ${bg.layerIndex};
      `;

      if (bg.type === "color") {
        style += `background-color: ${bg.value};`;
      } else if (bg.type === "gradient") {
        style += `background: ${bg.value};`;
      } else if (bg.type === "image") {
        const url = getLayoutImageUrl(bg.value);
        if (url) {
          // Determine background-size
          let bgSize: string = bg.size || "cover";
          if (bg.size === "custom" && bg.scale !== undefined) {
            bgSize = `${bg.scale * 100}%`;
          } else if (bg.size === "stretch") {
            bgSize = "100% 100%";
          }
          // Determine position (offset or center)
          const offsetX = bg.offsetX || 0;
          const offsetY = bg.offsetY || 0;
          const bgPosition = offsetX !== 0 || offsetY !== 0 
            ? `${offsetX}px ${offsetY}px` 
            : "center";
          // Tile mode uses repeat
          const bgRepeat = bg.size === "tile" ? "repeat" : "no-repeat";

          style += `
            background-image: url('${url}');
            background-size: ${bgSize};
            background-position: ${bgPosition};
            background-repeat: ${bgRepeat};
          `;
        }
      }

      if (bg.opacity !== undefined && bg.opacity < 1) {
        style += `opacity: ${bg.opacity};`;
      }

      return `<div class="custom-layout-bg" style="${style}"></div>`;
    })
    .join("");
}

/**
 * Render positioned controls
 */
function renderControls(
  node: GraphNode,
  controls: LayoutControl[],
  paramDefs: ParameterDef[]
): string {
  return controls
    .map((control) => {
      const paramDef = paramDefs.find((p) => p.key === control.paramKey);
      if (!paramDef) {
        // Parameter doesn't exist for this effect, skip
        return "";
      }

      const { key, min, max, default: defaultValue, unit, step, labels } = paramDef;
      const rawValue = node.params[key];
      const value = typeof rawValue === "number" ? rawValue : defaultValue ?? 0;
      const displayValue = formatParamValue(value, unit, labels);
      const label = control.labelOverride || paramDef.name || key;

      const labelPosition = control.style?.labelPosition || "top";
      const showValue = control.style?.showValue !== false;
      const knobStyle = control.style?.knobStyle || "default";
      const hideLabel = control.style?.hideLabel === true;
      const labelColor = control.style?.labelColor || "var(--text-dark-secondary)";

      const isToggle = control.type === "toggle" || unit === "toggle";
      const isEnum = unit === "enum" && Array.isArray(labels);

      const controlStyle = `
        position: absolute;
        left: ${control.position.x}px;
        top: ${control.position.y}px;
        display: flex;
        flex-direction: column;
        align-items: center;
        ${control.size ? `width: ${control.size.width}px;` : ""}
      `;

      let controlHtml = "";

      if (!hideLabel && labelPosition === "top") {
        controlHtml += `<span class="custom-control-label" style="font-size: 10px; color: ${labelColor}; margin-bottom: 4px;">${escapeHtml(label)}</span>`;
      }

      if (isToggle) {
        const checked = value >= 0.5;
        controlHtml += `
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${node.id}" data-param-key="${key}" ${checked ? "checked" : ""}>
            <span class="toggle-slider"></span>
          </label>
        `;
      } else if (control.type === "slider") {
        // Slider
        const normalized = (value - (min ?? 0)) / ((max ?? 1) - (min ?? 0));
        controlHtml += `
          <input
            type="range"
            class="custom-layout-slider node-param-slider"
            data-node-id="${node.id}"
            data-param-key="${key}"
            data-value="${value}"
            data-default="${defaultValue ?? 0}"
            min="${min ?? 0}"
            max="${max ?? 1}"
            ${step !== undefined ? `step="${step}"` : `step="0.01"`}
            value="${value}"
            ${isEnum ? `data-labels="${labels?.join("|") ?? ""}"` : ""}
          >
        `;
      } else {
        // Knob
        const knobImageId = control.style?.knobImageId;
        const knobBg = knobImageId
          ? `background-image: url('${getLayoutImageUrl(knobImageId) || ""}'); background-size: contain;`
          : "";

        controlHtml += `
          <div 
            class="knob node-param-knob custom-knob-${knobStyle}" 
            data-node-id="${node.id}" 
            data-param-key="${key}"
            data-value="${value}"
            data-default="${defaultValue ?? 0}"
            data-min="${min ?? 0}"
            data-max="${max ?? 1}"
            data-unit="${unit || "amount"}"
            ${step !== undefined ? `data-step="${step}"` : ""}
            ${isEnum ? `data-labels="${labels?.join("|") ?? ""}"` : ""}
            style="${knobBg}"
          >
            <div class="knob-indicator"></div>
          </div>
        `;
      }

      if (!hideLabel && labelPosition === "bottom") {
        controlHtml += `<span class="custom-control-label" style="font-size: 10px; color: ${labelColor}; margin-top: 4px;">${escapeHtml(label)}</span>`;
      }

      if (showValue) {
        controlHtml += `<span class="node-param-value" style="font-size: 9px; color: var(--text-dark-muted); margin-top: 2px;">${displayValue}</span>`;
      }

      return `<div class="custom-layout-control" style="${controlStyle}">${controlHtml}</div>`;
    })
    .join("");
}

/**
 * Render text labels
 */
function renderTextLabels(labels: LayoutTextLabel[]): string {
  return labels
    .map((label) => {
      const style = `
        position: absolute;
        left: ${label.position.x}px;
        top: ${label.position.y}px;
        font-size: ${label.fontSize}px;
        font-weight: ${label.fontWeight || "normal"};
        ${label.fontFamily ? `font-family: ${label.fontFamily};` : ""}
        color: ${label.color || "var(--text-dark-primary)"};
        text-align: ${label.textAlign || "left"};
        pointer-events: none;
      `;

      return `<span class="custom-layout-text-label" style="${style}">${escapeHtml(label.text)}</span>`;
    })
    .join("");
}

/**
 * Format a parameter value for display
 */
function formatParamValue(value: number, unit?: string, labels?: string[]): string {
  if (unit === "toggle") {
    return value >= 0.5 ? "On" : "Off";
  }
  if (unit === "enum" && Array.isArray(labels)) {
    const index = Math.round(value);
    return labels[index] ?? `${index}`;
  }
  if (unit === "dB" || unit === "ms" || unit === "Hz") {
    return `${value.toFixed(1)}${unit}`;
  }
  return value.toFixed(2);
}

/**
 * Escape HTML entities
 */
function escapeHtml(str: string): string {
  return str
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}
