/**
 * Layout Renderer
 *
 * Renders effect parameter panels using custom layouts.
 */

import { uiState } from "./state.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import { renderIcon } from "./iconAssets.js";
import { escapeHtml } from "./utils.js";
import type {
  EffectLayout,
  LayoutControl,
  LayoutTextLabel,
  LayoutBackground,
  LayoutRectangleOverlay,
} from "./layoutTypes.js";
import { layoutLookupKey } from "./layoutTypes.js";
import type { GraphNode } from "./types.js";

export interface LayoutResourceControlDef {
  resourceControlKey: string;
  displayName: string;
  resourceType: string;
  resourceIndex: number;
  exposedResourceId?: string;
  allowBrowseFile?: boolean;
  currentDisplayName: string;
  currentFilePath?: string;
  isMissing?: boolean;
}

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
  paramDefs: ParameterDef[],
  resourceControls: LayoutResourceControlDef[] = []
): string {
  const w = Math.round(layout.dimensions.width);
  const h = Math.round(layout.dimensions.height);
  const backgrounds = renderBackgrounds(layout.backgrounds);
  const overlays = renderOverlays(node, layout.overlays ?? []);
  const controls = renderControls(node, layout.controls, paramDefs, resourceControls);
  const labels = renderTextLabels(layout.textLabels);
  const themeClass = layout.containerTheme ? ` theme-${layout.containerTheme}` : '';

  return `
    <div class="custom-layout-scale-outer" style="position: relative; width: 100%; overflow: hidden; height: ${h}px;">
      <div 
        class="custom-layout-container${themeClass}" 
        data-design-w="${w}"
        data-design-h="${h}"
        style="
          position: relative;
          width: ${w}px;
          height: ${h}px;
          overflow: hidden;
          border-radius: 8px;
          margin: 0;
          padding: 0;
          box-sizing: border-box;
          transform-origin: top left;
        "
      >
        ${backgrounds}
        ${overlays}
        <div class="custom-layout-controls" style="position: absolute; inset: 0; z-index: 2; margin: 0; padding: 0; pointer-events: none;">
          ${controls}
        </div>
        <div class="custom-layout-labels" style="position: absolute; inset: 0; z-index: 3; pointer-events: none; margin: 0; padding: 0;">
          ${labels}
        </div>
      </div>
    </div>
  `;
}

function renderOverlays(node: GraphNode, overlays: LayoutRectangleOverlay[]): string {
  const bypassed = isNodeBypassed(node);
  return overlays
    .map((overlay) => {
      const visibilityMode = overlay.style?.visibilityMode ?? "always";
      const toggleBypassOnClick = overlay.style?.toggleBypassOnClick === true;
      const isVisible =
        visibilityMode === "always"
        || (visibilityMode === "enabled" && !bypassed)
        || (visibilityMode === "bypassed" && bypassed);

      if (!isVisible && !toggleBypassOnClick) {
        return "";
      }

      const backgroundColor = overlay.style?.backgroundColor || "#000000";
      const backgroundOpacity = typeof overlay.style?.backgroundOpacity === "number"
        ? Math.max(0, Math.min(1, overlay.style.backgroundOpacity))
        : 0.25;
      const borderColor = overlay.style?.borderColor || "#ffffff";
      const borderWidth = Math.max(0, overlay.style?.borderWidth ?? 1);
      const borderRadius = Math.max(0, overlay.style?.borderRadius ?? 0);
      const fill = colorWithAlpha(backgroundColor, backgroundOpacity);

      return `
        <div
          class="custom-layout-overlay"
          style="
            position: absolute;
            left: ${Math.round(overlay.position.x)}px;
            top: ${Math.round(overlay.position.y)}px;
            width: ${Math.round(overlay.size.width)}px;
            height: ${Math.round(overlay.size.height)}px;
            background-color: ${isVisible ? fill : "transparent"};
            border: ${isVisible ? `${borderWidth}px solid ${borderColor}` : "0 solid transparent"};
            border-radius: ${borderRadius}px;
            z-index: ${toggleBypassOnClick ? 4 : 1};
            box-sizing: border-box;
            pointer-events: ${toggleBypassOnClick ? "auto" : "none"};
            cursor: ${toggleBypassOnClick ? "pointer" : "default"};
          "
          data-toggle-bypass="${toggleBypassOnClick ? "true" : "false"}"
        ></div>
      `;
    })
    .join("");
}

function isNodeBypassed(node: GraphNode): boolean {
  const anyNode = node as unknown as { bypassed?: unknown; enabled?: unknown };
  if (typeof anyNode.bypassed === "boolean") {
    return anyNode.bypassed;
  }
  if (typeof anyNode.enabled === "boolean") {
    return !anyNode.enabled;
  }
  return false;
}

function colorWithAlpha(hexColor: string, alpha: number): string {
  const clampedAlpha = Math.max(0, Math.min(1, alpha));
  const normalized = hexColor.trim();
  const shortMatch = normalized.match(/^#([\da-fA-F]{3})$/);
  const longMatch = normalized.match(/^#([\da-fA-F]{6})$/);

  if (shortMatch) {
    const [r, g, b] = shortMatch[1].split("").map((ch) => parseInt(ch + ch, 16));
    return `rgba(${r}, ${g}, ${b}, ${clampedAlpha})`;
  }
  if (longMatch) {
    const hex = longMatch[1];
    const r = parseInt(hex.slice(0, 2), 16);
    const g = parseInt(hex.slice(2, 4), 16);
    const b = parseInt(hex.slice(4, 6), 16);
    return `rgba(${r}, ${g}, ${b}, ${clampedAlpha})`;
  }

  return normalized;
}

/**
 * Render only the controls/labels layers for preview contexts that already own the outer container.
 */
export function renderCustomLayoutPreviewLayers(
  node: GraphNode,
  layout: EffectLayout,
  paramDefs: ParameterDef[],
  resourceControls: LayoutResourceControlDef[] = []
): string {
  const controls = renderControls(node, layout.controls, paramDefs, resourceControls);
  const labels = renderTextLabels(layout.textLabels);

  return `
    <div class="custom-layout-controls" style="position: absolute; inset: 0; z-index: 2; margin: 0; padding: 0;">
      ${controls}
    </div>
    <div class="custom-layout-labels" style="position: absolute; inset: 0; z-index: 3; pointer-events: none; margin: 0; padding: 0;">
      ${labels}
    </div>
  `;
}

/**
 * Render the visual backdrop (backgrounds + overlays + labels) for layouts where
 * useDefaultControls is true. The default param controls are passed as `innerHtml`
 * and are rendered on top of the custom visual layers at normal document flow.
 * The container uses min-height so it expands to fit the default controls while
 * still honouring the layout's minimum canvas height.
 */
export function renderCustomLayoutBackdrop(
  node: GraphNode,
  layout: EffectLayout,
  innerHtml: string
): string {
  const w = Math.round(layout.dimensions.width);
  const h = Math.round(layout.dimensions.height);
  const backgrounds = renderBackgrounds(layout.backgrounds);
  const overlays = renderOverlays(node, layout.overlays ?? []);
  const labels = renderTextLabels(layout.textLabels);
  const themeClass = layout.containerTheme ? ` theme-${layout.containerTheme}` : '';

  const offsetX = layout.defaultControlsOffset?.x ?? 0;
  const offsetY = layout.defaultControlsOffset?.y ?? 0;
  const scaleX = layout.defaultControlsScale?.x ?? 1;
  const scaleY = layout.defaultControlsScale?.y ?? 1;
  const hasTransformOrOffset = offsetX !== 0 || offsetY !== 0 || scaleX !== 1 || scaleY !== 1;
  // Fix the controls wrapper to the design canvas width so flex-wrap break points match
  // the designer exactly. Uniform outer scaling (applied at runtime) handles fitting.
  const wrapperWidth = `width: ${w}px;`;
  const wrapperStyle = hasTransformOrOffset
    ? `position: absolute; left: ${Math.round(offsetX)}px; top: ${Math.round(offsetY)}px; transform: scale(${scaleX}, ${scaleY}); transform-origin: top left; z-index: 2; ${wrapperWidth}`
    : `position: relative; z-index: 2; ${wrapperWidth}`;

  return `
    <div class="custom-layout-scale-outer" style="position: relative; width: 100%; overflow: hidden; height: ${h}px;">
      <div
        class="custom-layout-container custom-layout-backdrop${themeClass}"
        data-design-w="${w}"
        data-design-h="${h}"
        style="
          position: relative;
          width: ${w}px;
          height: ${h}px;
          overflow: hidden;
          border-radius: 8px;
          margin: 0;
          padding: 0;
          box-sizing: border-box;
          transform-origin: top left;
        "
      >
        ${backgrounds}
        ${overlays}
        <div class="custom-layout-labels" style="position: absolute; inset: 0; z-index: 3; pointer-events: none; margin: 0; padding: 0;">
          ${labels}
        </div>
        <div class="custom-layout-default-controls" style="${wrapperStyle}">
          ${innerHtml}
        </div>
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
          // Match designer semantics exactly: origin is always top-left with pixel offsets.
          const offsetX = bg.offsetX || 0;
          const offsetY = bg.offsetY || 0;
          const bgPosition = `${offsetX}px ${offsetY}px`;
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
  paramDefs: ParameterDef[],
  resourceControls: LayoutResourceControlDef[]
): string {
  const resourceByKey = new Map(resourceControls.map((resource) => [resource.resourceControlKey, resource]));

  return controls
    .map((control) => {
      const isResourceControl =
        control.bindingType === "resource"
        || control.paramKey.startsWith("__resource__:");
      const resourceDef = isResourceControl ? resourceByKey.get(control.paramKey) : undefined;

      if (isResourceControl && !resourceDef) {
        return "";
      }

      const paramDef = paramDefs.find((p) => p.key === control.paramKey);
      if (!isResourceControl && !paramDef) {
        // Parameter doesn't exist for this effect, skip
        return "";
      }

      const key = paramDef?.key ?? "";
      const min = paramDef?.min;
      const max = paramDef?.max;
      const defaultValue = paramDef?.default;
      const unit = paramDef?.unit;
      const step = paramDef?.step;
      const labels = paramDef?.labels;
      const rawValue = key ? node.params[key] : undefined;
      const value = typeof rawValue === "number" ? rawValue : defaultValue ?? 0;
      const displayValue = isResourceControl
        ? ""
        : formatParamValue(value, unit, labels);
      const label = control.labelOverride
        || (isResourceControl ? resourceDef?.displayName : paramDef?.name)
        || control.paramKey;

      const labelPosition = control.style?.labelPosition || "top";
      const showValue = control.style?.showValue !== false;
      const knobStyle = control.style?.knobStyle || "default";
      const hideLabel = control.style?.hideLabel === true;
      const labelColor = control.style?.labelColor || "var(--text-dark-secondary)";

      const isToggle = !isResourceControl && (control.type === "toggle" || unit === "toggle");
      const isEnum = !isResourceControl && unit === "enum" && Array.isArray(labels);

      const controlStyle = `
        position: absolute;
        left: ${Math.round(control.position.x)}px;
        top: ${Math.round(control.position.y)}px;
        display: flex;
        flex-direction: column;
        align-items: center;
        ${control.size ? `width: ${Math.round(control.size.width)}px;` : ""}
      `;

      let controlHtml = "";

      if (!hideLabel && labelPosition === "top") {
        controlHtml += `<span class="custom-control-label" style="font-size: 10px; color: ${labelColor}; margin-bottom: 4px;">${escapeHtml(label)}</span>`;
      }

      if (isResourceControl) {
        const browseAccept = resourceDef?.resourceType === "nam"
          ? ".nam,.json"
          : resourceDef?.resourceType === "ir"
            ? ".wav"
            : "*";
        const missingClass = resourceDef?.isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
        const exposedResourceAttr = resourceDef?.exposedResourceId
          ? `data-exposed-resource-id="${escapeHtml(resourceDef.exposedResourceId)}"`
          : "";
        const resourceIndex = resourceDef?.resourceIndex ?? 0;

        controlHtml += `
          <div class="node-resource-selector custom-layout-resource-selector" data-node-id="${node.id}">
            <div class="resource-controls">
              <button
                class="resource-picker-btn"
                data-node-id="${node.id}"
                data-resource-type="${resourceDef?.resourceType ?? ""}"
                data-resource-index="${resourceIndex}"
                ${exposedResourceAttr}
              >Browse</button>
              <div
                class="${missingClass}"
                data-node-id="${node.id}"
                data-resource-type="${resourceDef?.resourceType ?? ""}"
                data-resource-index="${resourceIndex}"
                ${exposedResourceAttr}
                title="${escapeHtml(resourceDef?.currentDisplayName ?? "") }"
              >${escapeHtml(resourceDef?.currentDisplayName ?? "")}</div>
              ${(resourceDef?.allowBrowseFile ?? true) ? `
                <button
                  class="resource-browse-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceDef?.resourceType ?? ""}"
                  data-resource-index="${resourceIndex}"
                  ${exposedResourceAttr}
                  data-accept="${browseAccept}"
                  title="Browse for file..."
                >${renderIcon("folder", "resource-browse-icon")}</button>
              ` : ""}
            </div>
            ${resourceDef?.currentFilePath ? `<div class="resource-path-info" title="${escapeHtml(resourceDef.currentFilePath)}">${escapeHtml(resourceDef.currentFilePath)}</div>` : ""}
          </div>
        `;
      } else if (isToggle) {
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

      if (!isResourceControl && showValue) {
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
        left: ${Math.round(label.position.x)}px;
        top: ${Math.round(label.position.y)}px;
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
 * Format a parameter value for display.
 * Returns just the number for generic "amount" units to keep labels compact.
 * Exported so the signal-path renderer and designer preview share identical formatting.
 */
export function formatParamValue(value: number, unit?: string, labels?: string[]): string {
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

