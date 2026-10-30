/**
 * Layout Renderer
 *
 * Renders effect parameter panels using custom layouts.
 */

import { uiState } from "./state.js";
import { blendKnobDataAttributes, denormalizeBlendValue, type BlendParamDef } from "./blendUtils.js";
import { renderIcon } from "./iconAssets.js";
import { effectiveTaper, valueToTaperPosition } from "./paramTaper.js";
import { escapeHtml } from "./utils.js";
import { formatParamValue } from "./paramValueFormat.js";

// Shared with the signal-path renderer and the designer preview, which import it from here.
export { formatParamValue };
import { backgroundImageDrawnWidth, ensureLayoutImagesLoaded, knobFaceDrawnWidth, layoutLargestScale, resolveLayoutImageUrl } from "./layoutImages.js";
import { DEFAULT_KNOB_DIAMETER, imageKnob, imageToggle, imageToggleSize, withLayoutSwitchImage, knobDiameter, overlayImageStyle, sliderAttributes, stripSlider, styledKnobSizeStyle, withLayoutKnobImage, type ImageCssResolver } from "./layoutControlImages.js";
import { knobShadowSvg, toggleShadowSvg, usableControlShadow } from "./layoutShadows.js";
import { defaultControlsPanelClass, layoutDefaultControlsHtml } from "./layoutDefaultControls.js";
import { controlsAreaAttributes, usableControlsArea } from "./layoutControlsArea.js";
import { layoutCanGrow, scalingAttribute } from "./layoutScaling.js";
import { isLibraryPickerType, libraryPickerControlsHtml, libraryPickerNavigation, librarySlotHasBrowseButton } from "./resourceSlotMarkup.js";
import type {
  EffectLayout,
  LayoutControl,
  LayoutTextLabel,
  LayoutBackground,
  LayoutRectangleOverlay,
} from "./layoutTypes.js";
import { layoutLookupKey } from "./layoutTypes.js";
import type { GraphNode } from "./types.js";
import { isNodeBypassed } from "./graphNodes.js";

export interface LayoutResourceControlDef {
  resourceControlKey: string;
  displayName: string;
  resourceType: string;
  resourceIndex: number;
  exposedResourceId?: string;
  navigationCategoryHint?: string;
  navigationContextKey?: string;
  allowBrowseFile?: boolean;
  currentResourceId?: string;
  /** Empty when nothing is loaded: the label then asks for a resource instead. */
  currentDisplayName: string;
  currentFilePath?: string;
  isMissing?: boolean;
  isOptional?: boolean;
  /**
   * The slot's controls as the params panel draws them (its .resource-controls row and what goes
   * under it), so a layout draws the very same; without it a model or IR slot is drawn by
   * libraryPickerControlsHtml.
   */
  controlsHtml?: string;
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
function getLayoutImageUrl(imageId: string, drawnWidth?: number): string | null {
  const url = resolveLayoutImageUrl(imageId, drawnWidth);
  if (url) {
    return url;
  }
  // The image list arrives on demand. If it has not been loaded yet, request it; the
  // layoutImagesLoaded handler re-renders so the background resolves on the next pass.
  ensureLayoutImagesLoaded();
  return null;
}

/**
 * Render a custom layout for a node's parameters panel
 */
export function renderCustomLayout(
  node: GraphNode,
  layout: EffectLayout,
  paramDefs: BlendParamDef[],
  resourceControls: LayoutResourceControlDef[] = []
): string {
  const w = Math.round(layout.dimensions.width);
  const h = Math.round(layout.dimensions.height);
  const backgrounds = renderBackgrounds(layout.backgrounds, layout);
  const overlays = renderOverlays(node, layout.overlays ?? [], imageResolverFor(layout));
  const controls = renderControls(node, layout, paramDefs, resourceControls);
  const labels = renderTextLabels(layout.textLabels);
  const themeClass = layout.containerTheme ? ` theme-${layout.containerTheme}` : '';

  return `
    <div class="custom-layout-scale-outer"${layoutCanGrow(layout.scaling) ? " data-layout-grows" : ""} style="position: relative; width: 100%; overflow: hidden; height: ${h}px;">
      <div 
        class="custom-layout-container${escapeHtml(themeClass)}" 
        data-design-w="${w}"
        data-design-h="${h}"${scalingAttribute(layout)}
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

/** A layout's own image at the width it is drawn, as a CSS url(), or null when it cannot be drawn. */
export function imageResolverFor(layout: EffectLayout): ImageCssResolver {
  return (imageId, drawnWidth) => {
    const url = getLayoutImageUrl(imageId, drawnWidth * layoutLargestScale(layout));
    const css = url ? layoutImageCssUrl(url) : "none";
    return css === "none" ? null : css;
  };
}

function renderOverlays(node: GraphNode, overlays: LayoutRectangleOverlay[], resolveImage: ImageCssResolver): string {
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

      const backgroundColor = safeCssValue(overlay.style?.backgroundColor, "#000000");
      const backgroundOpacity = typeof overlay.style?.backgroundOpacity === "number"
        ? Math.max(0, Math.min(1, overlay.style.backgroundOpacity))
        : 0.25;
      const borderColor = safeCssValue(overlay.style?.borderColor, "#ffffff");
      const borderWidth = Math.max(0, layoutNumber(overlay.style?.borderWidth, 1));
      const borderRadius = Math.max(0, layoutNumber(overlay.style?.borderRadius, 0));
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
            background-color: ${escapeHtml(isVisible ? fill : "transparent")};${escapeHtml(isVisible ? overlayImageStyle(overlay, resolveImage) : "")}
            border: ${escapeHtml(isVisible ? `${borderWidth}px solid ${borderColor}` : "0 solid transparent")};
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

/**
 * A colour, gradient or font value from a layout, or `fallback` when it could do
 * more than set its one property. Layouts are imported from files: a `;` would
 * start another declaration, and url() would fetch from anywhere.
 */
export function safeCssValue(value: unknown, fallback: string): string {
  if (typeof value !== "string") {
    return fallback;
  }
  const trimmed = value.trim();
  if (!trimmed || trimmed.length > 256 || !/^[\w\s#%.,()'"+\-/]+$/.test(trimmed)
    || /\b(url|image|image-set|expression)\s*\(/i.test(trimmed)) {
    return fallback;
  }
  return trimmed;
}

type LayoutTextColorDefaults = Pick<EffectLayout, "controlLabelColor" | "controlValueColor">;

/**
 * A control's label and value colours: its own, else the layout's default, else null
 * (the theme's colour).
 */
export function resolveControlTextColors(
  layout: LayoutTextColorDefaults,
  control?: LayoutControl,
): { label: string | null; value: string | null } {
  const pick = (own: unknown, fallback: unknown) => safeCssValue(own, "") || safeCssValue(fallback, "") || null;
  return {
    label: pick(control?.style?.labelColor, layout.controlLabelColor),
    value: pick(control?.style?.valueColor, layout.controlValueColor),
  };
}

/** A colour for an `<input type="color">`, which takes only #rrggbb. */
export function colorInputValue(color: string | null | undefined, fallback: string): string {
  return color && /^#[0-9a-f]{6}$/i.test(color) ? color : fallback;
}

/**
 * Classes and custom properties for the default-controls wrapper of a backdrop
 * layout, so the layout's default label and value colours reach those controls too.
 */
export function defaultControlsColorAttrs(layout: LayoutTextColorDefaults): { classes: string; style: string } {
  const { label, value } = resolveControlTextColors(layout);
  return {
    classes: `${label ? " has-layout-label-color" : ""}${value ? " has-layout-value-color" : ""}`,
    style: `${label ? ` --layout-label-color: ${label};` : ""}${value ? ` --layout-value-color: ${value};` : ""}`,
  };
}

/** A finite number from a layout (whose JSON may hold anything), else `fallback`. */
function layoutNumber(value: unknown, fallback: number): number {
  const number = typeof value === "number" ? value : Number(value);
  return Number.isFinite(number) ? number : fallback;
}

/** A text size (px) from a layout, clamped to something legible and sane, or `fallback`. */
function textSize(own: unknown, layoutDefault: unknown, fallback: number): number {
  const size = layoutNumber(own, layoutNumber(layoutDefault, fallback));
  return Math.max(6, Math.min(96, size));
}

/** url() for a layout's own image — data, blob or bundled file — or `none` for anything else. */
function layoutImageCssUrl(url: string): string {
  return /^(data:image\/|blob:|layout-images\/)/.test(url) && !/["\\\r\n]/.test(url) ? `url("${url}")` : "none";
}

/** Applies an alpha channel to a #rgb or #rrggbb colour, passing anything else through. */
export function colorWithAlpha(hexColor: string, alpha: number): string {
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
  paramDefs: BlendParamDef[],
  resourceControls: LayoutResourceControlDef[] = []
): string {
  const controls = renderControls(node, layout, paramDefs, resourceControls);
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
  const backgrounds = renderBackgrounds(layout.backgrounds, layout);
  const overlays = renderOverlays(node, layout.overlays ?? [], imageResolverFor(layout));
  const labels = renderTextLabels(layout.textLabels);
  const themeClass = layout.containerTheme ? ` theme-${layout.containerTheme}` : '';

  // Numbers, whatever a stored or shared layout holds: a string here went into the style as CSS.
  const offsetX = layoutNumber(layout.defaultControlsOffset?.x, 0);
  const offsetY = layoutNumber(layout.defaultControlsOffset?.y, 0);
  const scaleX = layoutNumber(layout.defaultControlsScale?.x, 1);
  const scaleY = layoutNumber(layout.defaultControlsScale?.y, 1);
  const area = usableControlsArea(layout.defaultControlsArea);
  const hasTransformOrOffset = offsetX !== 0 || offsetY !== 0 || scaleX !== 1 || scaleY !== 1 || area !== undefined;
  // Fix the controls wrapper to the design canvas width (or its area's) so flex-wrap break points
  // match the designer exactly. Uniform outer scaling (applied at runtime) handles fitting.
  const wrapperWidth = `width: ${area ? area.width / scaleX : w}px;`;
  const wrapperStyle = hasTransformOrOffset
    ? `position: absolute; left: ${Math.round(offsetX)}px; top: ${Math.round(offsetY)}px; transform: scale(${scaleX}, ${scaleY}); transform-origin: top left; z-index: 2; ${wrapperWidth}`
    : `position: relative; z-index: 2; ${wrapperWidth}`;
  const colors = defaultControlsColorAttrs(layout);

  return `
    <div class="custom-layout-scale-outer"${layoutCanGrow(layout.scaling) ? " data-layout-grows" : ""} style="position: relative; width: 100%; overflow: hidden; height: ${h}px;">
      <div
        class="custom-layout-container custom-layout-backdrop${escapeHtml(themeClass)}"
        data-design-w="${w}"
        data-design-h="${h}"${scalingAttribute(layout)}
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
        <div class="custom-layout-default-controls${escapeHtml(colors.classes + defaultControlsPanelClass(layout))}" style="${escapeHtml(wrapperStyle + colors.style)}"${area ? controlsAreaAttributes(area, scaleX, scaleY) : ""}>
          ${layoutDefaultControlsHtml(innerHtml, layout, imageResolverFor(layout))}
        </div>
      </div>
    </div>
  `;
}

/**
 * Render background layers
 */
function renderBackgrounds(backgrounds: LayoutBackground[], layout: EffectLayout): string {
  return backgrounds
    .sort((a, b) => a.layerIndex - b.layerIndex)
    .map((bg) => {
      let style = `
        position: absolute;
        inset: 0;
        z-index: ${layoutNumber(bg.layerIndex, 0)};
      `;

      if (bg.type === "color") {
        style += `background-color: ${safeCssValue(bg.value, "transparent")};`;
      } else if (bg.type === "gradient") {
        style += `background: ${safeCssValue(bg.value, "none")};`;
      } else if (bg.type === "image") {
        const url = getLayoutImageUrl(bg.value, backgroundImageDrawnWidth(bg, layout));
        if (url) {
          // Determine background-size
          let bgSize: string = safeCssValue(bg.size, "cover");
          if (bg.size === "custom" && bg.scale !== undefined) {
            bgSize = `${layoutNumber(bg.scale, 1) * 100}%`;
          } else if (bg.size === "stretch") {
            bgSize = "100% 100%";
          }
          // Match designer semantics exactly: origin is always top-left with pixel offsets.
          const offsetX = layoutNumber(bg.offsetX, 0);
          const offsetY = layoutNumber(bg.offsetY, 0);
          const bgPosition = `${offsetX}px ${offsetY}px`;
          // Tile mode uses repeat
          const bgRepeat = bg.size === "tile" ? "repeat" : "no-repeat";

          style += `
            background-image: ${layoutImageCssUrl(url)};
            background-size: ${bgSize};
            background-position: ${bgPosition};
            background-repeat: ${bgRepeat};
          `;
        }
      }

      const opacity = layoutNumber(bg.opacity, 1);
      if (opacity < 1) {
        style += `opacity: ${opacity};`;
      }

      return `<div class="custom-layout-bg" style="${escapeHtml(style)}"></div>`;
    })
    .join("");
}

/**
 * Render positioned controls
 */
function renderControls(
  node: GraphNode,
  layout: EffectLayout,
  paramDefs: BlendParamDef[],
  resourceControls: LayoutResourceControlDef[]
): string {
  const resourceByKey = new Map(resourceControls.map((resource) => [resource.resourceControlKey, resource]));
  const resolveImage = imageResolverFor(layout);
  // The layout's light, when its knobs and toggles cast shadows (layoutShadows.ts).
  const light = usableControlShadow(layout.controlShadow);

  return layout.controls
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
      // A blend node's mapped knob stores 0..1 but is drawn on its parameter's own scale, and
      // the params panel binds it by these attributes (see bindNodeParamControls).
      const blendBinding = paramDef?.blend;
      const blendAttrs = blendBinding ? blendKnobDataAttributes(blendBinding) : "";
      const value = typeof rawValue === "number"
        ? (blendBinding ? denormalizeBlendValue(rawValue, { min: blendBinding.specMin, max: blendBinding.specMax }) : rawValue)
        : defaultValue ?? 0;
      // A blend knob is drawn on its blend spec's scale, which has no taper of its own.
      const taper = blendBinding ? "linear" : effectiveTaper(paramDef?.taper, min ?? 0, max ?? 1);
      const displayValue = isResourceControl
        ? ""
        : formatParamValue(value, unit, labels, taper, min, step);
      const label = control.labelOverride
        || (isResourceControl ? resourceDef?.displayName : paramDef?.name)
        || control.paramKey;

      const labelPosition = control.style?.labelPosition || "top";
      const showValue = control.style?.showValue !== false;
      // A class-name suffix, and the JSON behind it is not held to its type.
      const knobStyle = /^[\w-]+$/.test(String(control.style?.knobStyle ?? "")) ? control.style!.knobStyle! : "default";
      const hideLabel = control.style?.hideLabel === true;
      const textColors = resolveControlTextColors(layout, control);
      const labelColor = textColors.label ?? "var(--text-dark-secondary)";
      const valueColor = textColors.value ?? "var(--text-tertiary)";
      // Unset, these give the sizes every layout was drawn with and add nothing to the style.
      const labelSize = textSize(control.style?.labelFontSize, layout.controlLabelFontSize, 10);
      const valueSize = textSize(control.style?.valueFontSize, layout.controlValueFontSize, 9);
      const fontFamily = layout.controlFontFamily ? safeCssValue(layout.controlFontFamily, "") : "";
      const fontStyle = fontFamily ? ` font-family: ${fontFamily};` : "";
      const labelMaxWidth = layoutNumber(layout.controlLabelMaxWidth, 0);
      const labelWidthStyle = labelMaxWidth > 0 ? ` max-width: ${Math.round(labelMaxWidth)}px;` : "";

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
        ${isResourceControl ? resourceMaxWidthStyle(layout, control) : ""}
      `;

      let controlHtml = "";
      const castsShadow = light !== null && control.style?.castShadow !== false;
      let shadowed = false;

      if (!hideLabel && labelPosition === "top") {
        controlHtml += `<span class="custom-control-label" style="font-size: ${labelSize}px; color: ${escapeHtml(labelColor)}; margin-bottom: 4px;${escapeHtml(fontStyle + labelWidthStyle)}">${escapeHtml(label)}</span>`;
      }

      if (isResourceControl && resourceDef) {
        controlHtml += `
          <div
            class="node-resource-selector custom-layout-resource-selector"
            data-node-id="${escapeHtml(node.id)}"
            data-resource-index="${resourceDef.resourceIndex}"
            data-resource-type="${escapeHtml(resourceDef.resourceType)}"
          >${resourceSlotControlsHtml(node, resourceDef)}</div>
        `;
      } else if (isToggle) {
        const checked = value >= 0.5;
        // Its own images, else the layout's switch.
        const toggleStyle = withLayoutSwitchImage(control.style, layout.switchImage);
        const toggleImage = imageToggle(toggleStyle, resolveImage);
        // An image toggle is drawn at its own size; the standard switch is 36 x 20.
        const toggleSize = toggleImage ? imageToggleSize(toggleStyle) : { width: 36, height: 20 };
        const shadow = castsShadow && light ? toggleShadowSvg(light, toggleSize.width, toggleSize.height) : "";
        shadowed = shadow !== "";
        controlHtml += `
          <label class="toggle-switch${toggleImage ? " custom-image-toggle" : ""}"${toggleImage ? ` style="${escapeHtml(toggleImage.style)}"` : ""}>
            ${shadow}<input class="node-param-toggle" type="checkbox" data-node-id="${escapeHtml(node.id)}" data-param-key="${escapeHtml(key)}" ${checked ? "checked" : ""}>
            ${toggleImage ? toggleImage.children : `<span class="toggle-slider"></span>`}
          </label>
        `;
      } else if (control.type === "slider" && taper === "log") {
        // A log-taper slider runs over its travel, 0..1; bindNodeParamControls maps it back
        // through data-min/data-max. Unstepped, so a drag is as smooth as a knob's.
        const position = valueToTaperPosition(value, min ?? 0, max ?? 1, taper);
        const strip = stripSlider(control.style, position, resolveImage);
        controlHtml += `${strip?.open ?? ""}
          <input
            type="range"
            class="custom-layout-slider node-param-slider"
            data-node-id="${escapeHtml(node.id)}"
            data-param-key="${escapeHtml(key)}"
            data-value="${value}"
            data-default="${defaultValue ?? 0}"
            data-min="${min ?? 0}"
            data-max="${max ?? 1}"
            data-taper="log"
            min="0"
            max="1"
            step="any"
            ${strip ? strip.attributes : sliderAttributes(control.style, resolveImage)}
            value="${position}"
          >${strip?.close ?? ""}
        `;
      } else if (control.type === "slider") {
        // A strip slider shows the frame for its value, the input laid over it unseen.
        const strip = stripSlider(control.style, (max ?? 1) > (min ?? 0) ? (value - (min ?? 0)) / ((max ?? 1) - (min ?? 0)) : 0, resolveImage);
        controlHtml += `${strip?.open ?? ""}
          <input
            type="range"
            class="custom-layout-slider node-param-slider"
            data-node-id="${escapeHtml(node.id)}"
            data-param-key="${escapeHtml(key)}"
            data-value="${value}"
            data-default="${defaultValue ?? 0}"
            min="${min ?? 0}"
            max="${max ?? 1}"
            ${step !== undefined ? `step="${step}"` : `step="0.01"`}
            value="${value}"
            ${isEnum ? `data-labels="${escapeHtml(labels?.join("|") ?? "")}"` : ""}
            ${blendAttrs}
            ${strip ? strip.attributes : sliderAttributes(control.style, resolveImage)}
          >${strip?.close ?? ""}
        `;
      } else {
        // Knob. A knobImage (a turning face or a filmstrip), its own or the layout's, wins; without
        // one it draws as it always did.
        const position = taper === "log" ? valueToTaperPosition(value, min ?? 0, max ?? 1, taper)
          : ((max ?? 1) > (min ?? 0) ? (value - (min ?? 0)) / ((max ?? 1) - (min ?? 0)) : 0);
        const imageKnobParts = imageKnob(withLayoutKnobImage(control.style, layout.knobImage), position, resolveImage);
        const knobImageId = control.style?.knobImageId;
        const knobImageUrl = !imageKnobParts && knobImageId ? getLayoutImageUrl(knobImageId, knobFaceDrawnWidth(layout)) : null;
        const useCustomKnobImage = knobStyle === "custom" && !!knobImageUrl;
        const sizeStyle = styledKnobSizeStyle(control.style);
        const knobBg = imageKnobParts ? imageKnobParts.style
          : useCustomKnobImage ? `background-image: none;${sizeStyle}` : sizeStyle.trim();
        const customKnobAttr = imageKnobParts ? imageKnobParts.attributes.trim()
          : useCustomKnobImage ? `data-custom-knob-image="true"` : "";
        // An image knob is its diameter across; a styled one is 38 px, zoomed to its diameter.
        const shadow = castsShadow && light
          ? knobShadowSvg(light, imageKnobParts ? knobDiameter(control.style) : DEFAULT_KNOB_DIAMETER) : "";
        shadowed = shadow !== "";

        controlHtml += `
          <div 
            class="knob node-param-knob custom-knob-${knobStyle}" 
            data-node-id="${escapeHtml(node.id)}" 
            data-param-key="${escapeHtml(key)}"
            data-value="${value}"
            data-default="${defaultValue ?? 0}"
            data-min="${min ?? 0}"
            data-max="${max ?? 1}"
            data-unit="${escapeHtml(unit || "amount")}"
            ${step !== undefined ? `data-step="${step}"` : ""}
            ${isEnum ? `data-labels="${escapeHtml(labels?.join("|") ?? "")}"` : ""}
            ${taper === "log" ? `data-taper="log"` : ""}
            ${blendAttrs}
            ${customKnobAttr}
            style="${escapeHtml(knobBg)}"
          >
            ${shadow}${imageKnobParts ? imageKnobParts.children : useCustomKnobImage ? `<div class="custom-knob-face" style="background-image: url('${escapeHtml(knobImageUrl)}');"></div>` : ""}
            <div class="knob-indicator"></div>
          </div>
        `;
      }

      if (!hideLabel && labelPosition === "bottom") {
        controlHtml += `<span class="custom-control-label" style="font-size: ${labelSize}px; color: ${escapeHtml(labelColor)}; margin-top: 4px;${escapeHtml(fontStyle + labelWidthStyle)}">${escapeHtml(label)}</span>`;
      }

      if (!isResourceControl && showValue) {
        controlHtml += `<span class="node-param-value" style="font-size: ${valueSize}px; color: ${escapeHtml(valueColor)}; margin-top: 2px;${escapeHtml(fontStyle)}">${displayValue}</span>`;
      }

      // A resource slot given a width fills it (custom-layouts.css), its name ending in an ellipsis.
      const sizedClass = `${isResourceControl && control.size ? " has-width" : ""}${shadowed ? " has-control-shadow" : ""}`;
      return `<div class="custom-layout-control${escapeHtml(sizedClass)}" style="${escapeHtml(controlStyle)}">${controlHtml}</div>`;
    })
    .join("");
}

/**
 * A placed resource slot's controls: the params panel's own markup when it passed it; else a
 * model or IR slot drawn as the panel draws one (with arrows unless it is a composite's slot,
 * as there); else, for another kind outside the panel, its name and a browse button.
 */
function resourceSlotControlsHtml(node: GraphNode, def: LayoutResourceControlDef): string {
  if (typeof def.controlsHtml === "string") return def.controlsHtml;
  const labelState = { resourceType: def.resourceType, displayName: def.currentDisplayName, isMissing: def.isMissing, isOptional: def.isOptional };
  const currentResourceId = def.currentResourceId ?? "";
  const currentFilePath = def.currentFilePath ?? "";
  if (isLibraryPickerType(def.resourceType)) {
    const navOptions = { categoryHint: def.navigationCategoryHint, contextKey: def.navigationContextKey };
    return libraryPickerControlsHtml({
      nodeId: node.id,
      resourceType: def.resourceType,
      resourceIndex: def.resourceIndex,
      includeIndexAttr: true,
      exposedResourceId: def.exposedResourceId,
      labelState,
      hasCurrentSelection: Boolean(currentResourceId || currentFilePath),
      showBrowseButton: librarySlotHasBrowseButton(node.type),
      navigation: def.exposedResourceId ? null : libraryPickerNavigation(def.resourceType, currentResourceId, currentFilePath, navOptions),
      currentFilePath,
    });
  }
  const attrs = [
    `data-node-id="${escapeHtml(node.id)}"`,
    `data-resource-type="${escapeHtml(def.resourceType)}"`,
    `data-resource-index="${def.resourceIndex}"`,
    def.exposedResourceId ? `data-exposed-resource-id="${escapeHtml(def.exposedResourceId)}"` : "",
  ].filter(Boolean).join(" ");
  const name = def.currentDisplayName || "No resource selected";
  return `
    <div class="resource-controls">
      <div class="resource-picker-label" ${attrs} title="${escapeHtml(name)}">${escapeHtml(name)}</div>
      ${(def.allowBrowseFile ?? true) ? `<button class="resource-browse-btn" ${attrs} title="Browse for file...">${renderIcon(def.resourceType === "plugin" ? "plus" : "folder", "resource-browse-icon")}</button>` : ""}
    </div>
  `;
}

/**
 * A resource selector grows with the loaded resource's name; held to the room left before the
 * layout's right edge, the name ends in an ellipsis instead of pushing the arrows and browse
 * button out of the layout.
 */
function resourceMaxWidthStyle(layout: EffectLayout, control: LayoutControl): string {
  const room = Math.round(layoutNumber(layout.dimensions.width, 0) - layoutNumber(control.position.x, 0));
  return room > 0 ? `max-width: ${room}px;` : "";
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
        font-size: ${layoutNumber(label.fontSize, 12)}px;
        font-weight: ${safeCssValue(label.fontWeight, "normal")};
        ${label.fontFamily ? `font-family: ${safeCssValue(label.fontFamily, "inherit")};` : ""}
        color: ${safeCssValue(label.color, "var(--text-dark-primary)")};
        text-align: ${safeCssValue(label.textAlign, "left")};
        pointer-events: none;
      `;

      return `<span class="custom-layout-text-label" style="${escapeHtml(style)}">${escapeHtml(label.text)}</span>`;
    })
    .join("");
}
