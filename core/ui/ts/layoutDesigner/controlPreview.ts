/**
 * The designer draws its controls with the runtime renderer, so a control is laid out as the
 * params panel shows it: the same markup, sizes, images, label and value text. Edit mode draws
 * each control on its own inside a draggable placeholder; preview mode draws the layout whole.
 * Knobs are bound as the panel binds them (turned to their value, the value in the panel's
 * format) but never send anything.
 */

import { GenericKnob } from "../knob.js";
import { controlsAreaAttributes, fitDefaultControls, usableControlsArea } from "../layoutControlsArea.js";
import { defaultControlsPanelClass, layoutDefaultControlsHtml } from "../layoutDefaultControls.js";
import { defaultControlsColorAttrs, imageResolverFor, renderCustomLayoutPreviewLayers } from "../layoutRenderer.js";
import type { LayoutResourceControlDef } from "../layoutRenderer.js";
import type { EffectLayout, LayoutControl } from "../layoutTypes.js";
import { parseParamTaper } from "../paramTaper.js";
import { knobValueFormatter } from "../paramValueFormat.js";
import { buildDefaultParamControlsHtml } from "../parameterControlMarkup.js";
import type { ParameterDef } from "../presetV2.js";
import type { GraphNode } from "../types.js";
import { escapeHtml } from "../utils.js";
import type { LayoutResourceCandidate } from "./types.js";

type PreviewParamDefs = Parameters<typeof renderCustomLayoutPreviewLayers>[2];

/** A stand-in node for the effect type, every parameter at its default. */
export function designerPreviewNode(effectType: string, paramDefs: PreviewParamDefs): GraphNode {
  return {
    id: "layout-designer-preview",
    type: effectType,
    params: Object.fromEntries(paramDefs.map((paramDef) => [paramDef.key, paramDef.default ?? 0])),
  } as unknown as GraphNode;
}

/** The effect's resource slots as the renderer takes them, each showing its own name. */
export function designerResourceDefs(candidates: LayoutResourceCandidate[]): LayoutResourceControlDef[] {
  return candidates.map((candidate) => ({
    resourceControlKey: candidate.controlKey,
    displayName: candidate.displayName,
    resourceType: candidate.resourceType,
    resourceIndex: candidate.resourceIndex,
    exposedResourceId: candidate.exposedResourceId,
    allowBrowseFile: candidate.allowBrowseFile,
    currentDisplayName: candidate.displayName,
    currentFilePath: "",
    isMissing: false,
  }));
}

/**
 * One control as the runtime draws it, placed at the top left of its container rather than at
 * its layout position (drawn there first, so a width that depends on it is the runtime's); null
 * when the runtime would not draw it (its parameter or resource slot is not on this effect).
 */
export function controlPreviewElement(
  control: LayoutControl,
  layout: EffectLayout,
  node: GraphNode,
  paramDefs: PreviewParamDefs,
  resources: LayoutResourceControlDef[],
): HTMLElement | null {
  const single: EffectLayout = { ...layout, controls: [control], textLabels: [] };
  const template = document.createElement("template");
  template.innerHTML = renderCustomLayoutPreviewLayers(node, single, paramDefs, resources);
  const element = template.content.querySelector<HTMLElement>(".custom-layout-control");
  if (!element) return null;
  element.style.position = "relative";
  element.style.removeProperty("left");
  element.style.removeProperty("top");
  bindPreviewKnobs(element);
  return element;
}

/** Turns each knob under `root` to its value and writes its value text, as the panel's binding does. */
export function bindPreviewKnobs(root: HTMLElement): void {
  root.querySelectorAll<HTMLElement>(".node-param-knob").forEach((knob) => {
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    new GenericKnob({
      knobElement: knob,
      paramId: `layout-designer_${knob.dataset.paramKey ?? "param"}`,
      minValue: min,
      maxValue: max,
      defaultValue: parseFloat(knob.dataset.default || knob.dataset.value || "0"),
      displayFormat: knobValueFormatter(knob),
      valueDisplay: knob.parentElement?.querySelector<HTMLElement>(".node-param-value") ?? null,
      labelElement: knob.parentElement?.querySelector<HTMLElement>(".node-param-label, .custom-control-label") ?? null,
      stepValue: knob.dataset.step ? parseFloat(knob.dataset.step) : undefined,
      taper: parseParamTaper(knob.dataset.taper),
      sendParameter: false,
    });
    // Out of the tab order: the designer's arrow keys move the selection, not a knob.
    knob.tabIndex = -1;
  });
}

/**
 * The real default controls for a useDefaultControls layout, as the designer canvas shows them:
 * at the configured offset and scale, so they can be dialled in, and with the Main / Advanced
 * tab split of the signal-path panel. `viewportWidth` is the canvas area's width, for the
 * panel's fit-to-width scale.
 */
export function defaultControlsPreviewHtml(layout: EffectLayout, paramDefs: ParameterDef[], viewportWidth: number): string {
  // Mirror the main/advanced split from renderNodeParamsPanel
  let advancedDefs = paramDefs.filter((p) => Boolean(p.advanced));
  let mainDefs = paramDefs.filter((p) => !p.advanced);
  if (mainDefs.length === 0) {
    mainDefs = paramDefs;
    advancedDefs = [];
  }
  const hasAdvancedTab = advancedDefs.length > 0;

  const innerHtml = hasAdvancedTab ? `
    <div class="node-param-tabs" role="tablist" aria-label="Parameter Groups">
      <button class="node-param-tab is-active" data-tab="main" type="button" role="tab" aria-selected="true">Main</button>
      <button class="node-param-tab" data-tab="advanced" type="button" role="tab" aria-selected="false">Advanced</button>
    </div>
    <div class="node-param-tab-panels">
      <div class="node-param-tab-panel is-active" data-tab="main" role="tabpanel">
        <div class="params-controls">
          ${buildDefaultParamControlsHtml(mainDefs, "layout-designer-preview")}
        </div>
      </div>
      <div class="node-param-tab-panel" data-tab="advanced" role="tabpanel">
        <div class="params-controls">
          ${buildDefaultParamControlsHtml(advancedDefs, "layout-designer-preview")}
        </div>
      </div>
    </div>
  ` : `
    <div class="params-controls">
      ${buildDefaultParamControlsHtml(mainDefs, "layout-designer-preview")}
    </div>
  `;

  const offsetX = layout.defaultControlsOffset?.x ?? 0;
  const offsetY = layout.defaultControlsOffset?.y ?? 0;
  const scaleX = layout.defaultControlsScale?.x ?? 1;
  const scaleY = layout.defaultControlsScale?.y ?? 1;

  // Second-pass runtime parity: in the live panel, the custom layout sits inside a
  // padded section and may be auto-scaled down to fit narrower widths. Mirror that
  // fit behavior in the designer preview so default-controls wrapping/overflow is
  // closer to what users will see at runtime.
  const runtimeSectionHorizontalPadding = 36; // 18px left + 18px right in .default-effect-section-controls
  const runtimeAvailableWidth = Math.max(1, viewportWidth - runtimeSectionHorizontalPadding);
  const runtimeFitScale = Math.min(1, runtimeAvailableWidth / layout.dimensions.width);

  const effectiveOffsetX = Math.round(offsetX * runtimeFitScale);
  const effectiveOffsetY = Math.round(offsetY * runtimeFitScale);
  const effectiveScaleX = scaleX * runtimeFitScale;
  const effectiveScaleY = scaleY * runtimeFitScale;
  const layoutArea = usableControlsArea(layout.defaultControlsArea);
  const area = layoutArea && { width: layoutArea.width * runtimeFitScale, height: layoutArea.height * runtimeFitScale };
  const hasTransformOrOffset = effectiveOffsetX !== 0 || effectiveOffsetY !== 0 || effectiveScaleX !== 1 || effectiveScaleY !== 1 || area !== undefined;
  // Pin wrapper to canvas width (or its area's) so flex-wrap break points are identical in the runtime panel.
  const wrapperWidth = `width: ${layoutArea ? layoutArea.width / scaleX : layout.dimensions.width}px;`;
  const wrapperStyle = hasTransformOrOffset
    ? `position: absolute; left: ${effectiveOffsetX}px; top: ${effectiveOffsetY}px; transform: scale(${effectiveScaleX}, ${effectiveScaleY}); transform-origin: top left; z-index: 2; ${wrapperWidth}`
    : `position: relative; z-index: 2; ${wrapperWidth}`;

  // Use the same class structure as runtime (custom-layout-backdrop + theme) so that
  // .custom-layout-backdrop.theme-* CSS rules apply identically in designer and runtime.
  const themeClass = layout.containerTheme ? ` theme-${layout.containerTheme}` : '';
  const colors = defaultControlsColorAttrs(layout);

  // The area itself, outlined, so the box the controls fit into can be seen against the background.
  const outline = area
    ? `<div class="layout-default-controls-area" style="left: ${effectiveOffsetX}px; top: ${effectiveOffsetY}px; width: ${area.width}px; height: ${area.height}px;"></div>`
    : "";
  return `
    ${outline}
    <div class="layout-default-controls-preview custom-layout-backdrop${escapeHtml(themeClass)}${escapeHtml(colors.classes + defaultControlsPanelClass(layout))}" style="${escapeHtml(wrapperStyle + colors.style)} pointer-events: none;"${area ? controlsAreaAttributes(area, effectiveScaleX, effectiveScaleY) : ""}>
      ${layoutDefaultControlsHtml(innerHtml, layout, imageResolverFor(layout))}
    </div>
  `;
}

/** Draw the default controls preview into the canvas's controls layer, shrunk into their area. */
export function renderDefaultControlsPreview(layer: HTMLElement, layout: EffectLayout, paramDefs: ParameterDef[], viewportWidth: number): void {
  layer.innerHTML = defaultControlsPreviewHtml(layout, paramDefs, viewportWidth);
  layer.querySelectorAll<HTMLElement>("[data-area-w]").forEach(fitDefaultControls);
}
