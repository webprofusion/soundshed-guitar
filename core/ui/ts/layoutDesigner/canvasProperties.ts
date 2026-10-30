/**
 * The properties panel for the canvas itself and for its background layers.
 *
 * Background layers are ordered, so most of what is here is reordering and
 * per-layer blend settings rather than a flat form.
 */

import type { LayoutBackground, LayoutScaling } from "../layoutTypes.js";
import { usableControlsArea } from "../layoutControlsArea.js";
import { bindKnobImageRows, renderKnobImageRows } from "./controlImageProperties.js";
import { getLayoutImageUrl } from "./images.js";
import { bindLayoutAssetsGroup, renderLayoutAssetsGroup } from "./layoutAssets.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";
import { bindShadowGroup, renderShadowGroup } from "./shadowProperties.js";
import { bindSwitchGroup, renderSwitchGroup } from "./switchProperties.js";
import { openLayoutSizePreview } from "./sizePreview.js";
import { colorInputValue } from "../layoutRenderer.js";
import { escapeHtml } from "../utils.js";

export class CanvasProperties {
  constructor(private readonly host: LayoutPropertiesHost) {}

  renderCanvasProperties(): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const containerTheme = layout.containerTheme ?? '';
    const isBackdrop = layout.useDefaultControls === true;
    const offsetX = layout.defaultControlsOffset?.x ?? 0;
    const offsetY = layout.defaultControlsOffset?.y ?? 0;
    const scaleX = layout.defaultControlsScale?.x ?? 1;
    const scaleY = layout.defaultControlsScale?.y ?? 1;
    const area = usableControlsArea(layout.defaultControlsArea);
    const scaling = layout.scaling ?? {};

    const backdropSections = isBackdrop ? `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Default Controls</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Main Tab Only</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-dc-main-only" ${layout.defaultControlsMainOnly === true ? "checked" : ""} title="Show only the Main tab's controls, with no tab bar and no Advanced tab">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Group Panels</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-dc-panels" ${layout.defaultControlsPanels !== false ? "checked" : ""} title="Draw each group's heading and panel; off, the controls sit straight on the background">
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Default Controls — Position</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Left (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-offset-x" value="${offsetX}" step="1">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Top (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-offset-y" value="${offsetY}" step="1">
          </div>
        </div>
        <div class="layout-property-hint">
          The box the controls stay inside: they wrap at its width and shrink to fit its height. Empty, they wrap at the canvas width and may run past its foot.
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Width (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-area-w" min="1" step="1" placeholder="Canvas" value="${area?.width ?? ""}" title="Width of the box the controls wrap within">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Height (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-area-h" min="1" step="1" placeholder="Any" value="${area?.height ?? ""}" title="Height the controls shrink to fit">
          </div>
        </div>
        <div class="layout-property-row">
          <button type="button" class="btn btn-secondary btn-sm" id="prop-dc-area-fill" title="Size the box to the canvas, with the same margin on the right and bottom as on the left and top">Fit to Canvas</button>
          ${area ? `<button type="button" class="btn btn-secondary btn-sm" id="prop-dc-area-clear" title="Let the controls wrap at the canvas width, at any height">Clear</button>` : ""}
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Default Controls — Scale</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Scale X</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-scale-x" value="${scaleX}" min="0.1" max="3" step="0.05">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Scale Y</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-scale-y" value="${scaleY}" min="0.1" max="3" step="0.05">
          </div>
        </div>
        <div class="layout-property-row">
          <button type="button" class="btn btn-secondary btn-sm" id="prop-dc-scale-reset">Reset to 1:1</button>
        </div>
      </div>
    ` : `
      <div class="layout-property-hint">
        Select a control, label, background, or rectangle to edit its properties.
      </div>
    `;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Container</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Theme</span>
          <div class="layout-property-input">
            <select id="prop-container-theme" title="Override CSS colour variables inside this container. Useful when the layout background differs from the global app theme.">
              <option value="" ${containerTheme === '' ? 'selected' : ''}>Inherit (app theme)</option>
              <option value="dark" ${containerTheme === 'dark' ? 'selected' : ''}>Dark</option>
              <option value="light" ${containerTheme === 'light' ? 'selected' : ''}>Light</option>
              <option value="classic" ${containerTheme === 'classic' ? 'selected' : ''}>Classic</option>
            </select>
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Control Text</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Label Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-layout-label-color" value="${escapeHtml(colorInputValue(layout.controlLabelColor, "#ffffff"))}" title="Label colour for every control that does not set its own">
            ${layout.controlLabelColor ? `<button type="button" class="btn btn-secondary btn-sm" id="prop-layout-label-color-reset" title="Use the theme's label colour">Theme</button>` : ""}
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Value Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-layout-value-color" value="${escapeHtml(colorInputValue(layout.controlValueColor, "#a0a0a0"))}" title="Value colour for every control that does not set its own">
            ${layout.controlValueColor ? `<button type="button" class="btn btn-secondary btn-sm" id="prop-layout-value-color-reset" title="Use the theme's value colour">Theme</button>` : ""}
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Label Size</span>
          <div class="layout-property-input">
            <input type="number" id="prop-layout-label-size" min="6" max="96" step="1" placeholder="10" value="${layout.controlLabelFontSize ?? ""}" title="Label text size (px) for every control that does not set its own">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Value Size</span>
          <div class="layout-property-input">
            <input type="number" id="prop-layout-value-size" min="6" max="96" step="1" placeholder="9" value="${layout.controlValueFontSize ?? ""}" title="Value text size (px) for every control that does not set its own">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Font</span>
          <div class="layout-property-input">
            <input type="text" id="prop-layout-font" placeholder="Theme font" value="${escapeHtml(layout.controlFontFamily ?? "")}" title="CSS font family for control labels and values">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Label Width</span>
          <div class="layout-property-input">
            <input type="number" id="prop-layout-label-width" min="16" max="2048" step="8" placeholder="80" value="${layout.controlLabelMaxWidth ?? ""}" title="Widest a control label may be (px) before it is cut short">
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Knobs</div>
        <div class="layout-property-hint">
          The image every knob is drawn from${isBackdrop ? ", the default controls' included" : ""}, unless a knob sets its own image or a knob style other than Default.
        </div>${renderKnobImageRows(layout.knobImage, (_purpose, selected) => this.host.renderImageOptionsHtml("knob", selected), "prop-layout-knob", "Standard")}
      </div>${renderSwitchGroup(layout, (purpose, selected) => this.host.renderImageOptionsHtml(purpose, selected), isBackdrop)}${renderShadowGroup(layout)}
      <div class="layout-property-group">
        <div class="layout-property-group-title">Fit to Panel</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Mode</span>
          <div class="layout-property-input">
            <select id="prop-scaling-mode" title="How the layout is sized in the effect panel">
              <option value="" ${!scaling.mode || scaling.mode === "fit-width" ? "selected" : ""}>Fit width</option>
              <option value="fit" ${scaling.mode === "fit" ? "selected" : ""}>Fit width and height</option>
              <option value="fixed" ${scaling.mode === "fixed" ? "selected" : ""}>Fixed size</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Max Scale</span>
          <div class="layout-property-input">
            <input type="number" id="prop-scaling-max" min="0.25" max="8" step="0.25" placeholder="1" value="${scaling.maxScale ?? ""}" title="Largest the layout is drawn, as a multiple of its own size. Above 1 it grows to fill a larger panel.">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Min Scale</span>
          <div class="layout-property-input">
            <input type="number" id="prop-scaling-min" min="0" max="8" step="0.05" placeholder="0" value="${scaling.minScale ?? ""}" title="Smallest the layout is drawn; below it the panel scrolls sideways">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Align</span>
          <div class="layout-property-input">
            <select id="prop-scaling-align">
              <option value="" ${scaling.align !== "start" ? "selected" : ""}>Centre</option>
              <option value="start" ${scaling.align === "start" ? "selected" : ""}>Left</option>
            </select>
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Grid</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Snap (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-grid-size" min="1" max="64" step="1" placeholder="8" value="${layout.gridSize ?? ""}" title="Snap step for dragging and placing. Hold Alt while dragging to place freely.">
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Preview</div>
        <div class="layout-property-row">
          <button type="button" class="btn btn-secondary btn-sm" id="prop-size-preview" title="See the layout as the effect panel draws it at different window sizes">Preview at panel size…</button>
        </div>
      </div>
      ${renderLayoutAssetsGroup(layout, (selected) => this.host.renderImageOptionsHtml("background", selected))}
      ${backdropSections}
    `;

    // Container theme selector
    const themeSelect = document.getElementById("prop-container-theme") as HTMLSelectElement | null;
    themeSelect?.addEventListener("change", () => {
      if (!layout) return;
      this.host.pushSidebarUndoOnce();
      const val = themeSelect.value as 'light' | 'dark' | 'classic' | '';
      layout.containerTheme = val === '' ? undefined : val;
      this.host.renderCanvas();
    });

    // Default control text colours; a control's own colour still wins
    const bindTextColor = (id: string, key: "controlLabelColor" | "controlValueColor") => {
      const input = document.getElementById(id) as HTMLInputElement | null;
      input?.addEventListener("change", () => {
        this.host.pushSidebarUndoOnce();
        layout[key] = input.value;
        this.host.renderCanvas();
        this.host.renderSidebar(); // Show the Theme button
      });
      document.getElementById(`${id}-reset`)?.addEventListener("click", () => {
        this.host.pushUndoState();
        delete layout[key];
        this.host.renderCanvas();
        this.host.renderSidebar();
      });
    };
    bindTextColor("prop-layout-label-color", "controlLabelColor");
    bindTextColor("prop-layout-value-color", "controlValueColor");

    // An emptied field removes the setting, so the layout falls back to the original default.
    const bindOptionalNumber = (id: string, apply: (value: number | undefined) => void, min: number, max: number) => {
      const input = document.getElementById(id) as HTMLInputElement | null;
      input?.addEventListener("change", () => {
        this.host.pushSidebarUndoOnce();
        const value = parseFloat(input.value);
        apply(input.value.trim() === "" || !Number.isFinite(value) ? undefined : Math.max(min, Math.min(max, value)));
        this.host.renderCanvas();
      });
    };
    const setOrDelete = <K extends "controlLabelFontSize" | "controlValueFontSize" | "controlLabelMaxWidth" | "gridSize">(key: K) =>
      (value: number | undefined) => {
        if (value === undefined) delete layout[key];
        else layout[key] = (key === "gridSize" ? Math.round(value) : value) as never;
      };
    bindOptionalNumber("prop-layout-label-size", setOrDelete("controlLabelFontSize"), 6, 96);
    bindOptionalNumber("prop-layout-value-size", setOrDelete("controlValueFontSize"), 6, 96);
    bindOptionalNumber("prop-layout-label-width", setOrDelete("controlLabelMaxWidth"), 16, 2048);
    bindOptionalNumber("prop-grid-size", setOrDelete("gridSize"), 1, 64);

    // The layout's knob image, which knobs without one of their own draw from.
    bindKnobImageRows("prop-layout-knob", (change, rerenderSidebar) => {
      this.host.pushSidebarUndoOnce();
      change(layout);
      this.host.renderCanvas();
      if (rerenderSidebar) this.host.renderSidebar();
    });
    bindSwitchGroup(layout, this.host);
    bindShadowGroup(layout, this.host);

    const fontInput = document.getElementById("prop-layout-font") as HTMLInputElement | null;
    fontInput?.addEventListener("change", () => {
      this.host.pushSidebarUndoOnce();
      const font = fontInput.value.trim();
      if (font) layout.controlFontFamily = font;
      else delete layout.controlFontFamily;
      this.host.renderCanvas();
    });

    // Fit to Panel: a scaling block only while it holds something, so an untouched layout keeps none.
    const updateScaling = (change: (scaling: LayoutScaling) => void) => {
      this.host.pushSidebarUndoOnce();
      const next = { ...(layout.scaling ?? {}) };
      change(next);
      for (const key of Object.keys(next) as (keyof typeof next)[]) {
        if (next[key] === undefined) delete next[key];
      }
      if (Object.keys(next).length) layout.scaling = next;
      else delete layout.scaling;
      this.host.renderCanvas();
    };
    const modeSelect = document.getElementById("prop-scaling-mode") as HTMLSelectElement | null;
    modeSelect?.addEventListener("change", () => updateScaling((next) => {
      next.mode = modeSelect.value === "fit" || modeSelect.value === "fixed" ? modeSelect.value : undefined;
    }));
    const alignSelect = document.getElementById("prop-scaling-align") as HTMLSelectElement | null;
    alignSelect?.addEventListener("change", () => updateScaling((next) => {
      next.align = alignSelect.value === "start" ? "start" : undefined;
    }));
    for (const [id, key, min] of [["prop-scaling-max", "maxScale", 0.25], ["prop-scaling-min", "minScale", 0]] as const) {
      const input = document.getElementById(id) as HTMLInputElement | null;
      input?.addEventListener("change", () => updateScaling((next) => {
        const value = parseFloat(input.value);
        next[key] = input.value.trim() === "" || !Number.isFinite(value) ? undefined : Math.max(min, Math.min(8, value));
      }));
    }

    document.getElementById("prop-size-preview")?.addEventListener("click", () => openLayoutSizePreview(layout, this.host.getParamDefs()));
    bindLayoutAssetsGroup(layout, this.host, sidebarContent);

    if (!isBackdrop) return;

    // Backdrop offset/scale bindings
    const bindNum = (id: string, apply: (v: number) => void) => {
      const el = document.getElementById(id) as HTMLInputElement | null;
      el?.addEventListener("change", () => {
        const v = parseFloat(el.value);
        if (!isNaN(v)) {
          this.host.pushSidebarUndoOnce();
          apply(v);
          this.host.renderCanvas();
        }
      });
    };

    bindNum("prop-dc-offset-x", (v) => {
      if (!layout) return;
      layout.defaultControlsOffset = { x: Math.round(v), y: layout.defaultControlsOffset?.y ?? 0 };
    });
    bindNum("prop-dc-offset-y", (v) => {
      if (!layout) return;
      layout.defaultControlsOffset = { x: layout.defaultControlsOffset?.x ?? 0, y: Math.round(v) };
    });
    bindNum("prop-dc-scale-x", (v) => {
      if (!layout) return;
      layout.defaultControlsScale = { x: Math.max(0.1, v), y: layout.defaultControlsScale?.y ?? 1 };
    });
    bindNum("prop-dc-scale-y", (v) => {
      if (!layout) return;
      layout.defaultControlsScale = { x: layout.defaultControlsScale?.x ?? 1, y: Math.max(0.1, v) };
    });

    // The area: either size alone takes the other from the canvas, out to its edge; emptying either clears it.
    const canvasRemaining = () => ({
      width: Math.max(1, layout.dimensions.width - (layout.defaultControlsOffset?.x ?? 0)),
      height: Math.max(1, layout.dimensions.height - (layout.defaultControlsOffset?.y ?? 0)),
    });
    for (const [id, key] of [["prop-dc-area-w", "width"], ["prop-dc-area-h", "height"]] as const) {
      const input = document.getElementById(id) as HTMLInputElement | null;
      input?.addEventListener("change", () => {
        this.host.pushSidebarUndoOnce();
        const value = parseFloat(input.value);
        if (input.value.trim() === "" || !Number.isFinite(value)) delete layout.defaultControlsArea;
        else layout.defaultControlsArea = { ...(usableControlsArea(layout.defaultControlsArea) ?? canvasRemaining()), [key]: Math.max(1, Math.round(value)) };
        this.host.renderCanvas();
        this.host.renderSidebar(); // The other size and the Clear button
      });
    }
    document.getElementById("prop-dc-area-fill")?.addEventListener("click", () => {
      this.host.pushUndoState();
      const x = layout.defaultControlsOffset?.x ?? 0;
      const y = layout.defaultControlsOffset?.y ?? 0;
      layout.defaultControlsArea = {
        width: Math.max(1, layout.dimensions.width - 2 * x),
        height: Math.max(1, layout.dimensions.height - 2 * y),
      };
      this.host.renderCanvas();
      this.host.renderSidebar();
    });
    document.getElementById("prop-dc-area-clear")?.addEventListener("click", () => {
      this.host.pushUndoState();
      delete layout.defaultControlsArea;
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    const bindCheck = (id: string, apply: (checked: boolean) => void) => {
      const el = document.getElementById(id) as HTMLInputElement | null;
      el?.addEventListener("change", () => {
        this.host.pushSidebarUndoOnce();
        apply(el.checked);
        this.host.renderCanvas();
      });
    };
    // Each is stored only when it differs from what the panel draws by default.
    bindCheck("prop-dc-main-only", (checked) => {
      if (checked) layout.defaultControlsMainOnly = true;
      else delete layout.defaultControlsMainOnly;
    });
    bindCheck("prop-dc-panels", (checked) => {
      if (checked) delete layout.defaultControlsPanels;
      else layout.defaultControlsPanels = false;
    });

    document.getElementById("prop-dc-scale-reset")?.addEventListener("click", () => {
      if (!layout) return;
      this.host.pushUndoState();
      layout.defaultControlsScale = { x: 1, y: 1 };
      this.host.renderCanvas();
      this.host.renderSidebar();
    });
  }

  renderBackgroundProperties(layerIndex: number): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const bg = layout.backgrounds.find((b) => b.layerIndex === layerIndex);
    if (!bg) return;

    const isCustomScale = bg.size === "custom";
    const isImage = bg.type === "image";

    // Type-specific value editor
    let valueEditor = "";
    if (bg.type === "color") {
      valueEditor = `
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-bg-color" value="${escapeHtml(bg.value || "#1a1a2e")}">
          </div>
        </div>
      `;
    } else if (bg.type === "gradient") {
      valueEditor = `
        <div class="layout-property-row">
          <span class="layout-property-label">Gradient</span>
          <div class="layout-property-input">
            <input type="text" id="prop-bg-gradient" value="${escapeHtml(bg.value)}" placeholder="linear-gradient(...)">
          </div>
        </div>
      `;
    }

    // Type selector
    const typeSelector = bg.type !== "image" ? `
      <div class="layout-property-row">
        <span class="layout-property-label">Type</span>
        <div class="layout-property-input">
          <select id="prop-bg-type">
            <option value="color" ${bg.type === "color" ? "selected" : ""}>Solid Color</option>
            <option value="gradient" ${bg.type === "gradient" ? "selected" : ""}>Gradient</option>
          </select>
        </div>
      </div>
    ` : `
      <div class="layout-property-row">
        <span class="layout-property-label">Type</span>
        <span class="layout-property-input">Image</span>
      </div>
    `;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Background Layer ${layerIndex + 1}</div>
        ${typeSelector}
        ${valueEditor}
      </div>

      ${isImage ? `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Image Source</div>
        <div class="layout-image-preview">
          ${bg.value ? `<img src="${escapeHtml(getLayoutImageUrl(bg.value) || "")}" alt="Background">` : `<span class="layout-image-preview-placeholder">No image</span>`}
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="prop-bg-image-select">
              ${this.host.renderImageOptionsHtml("background", bg.value)}
            </select>
          </div>
        </div>
        <div class="layout-image-actions">
          <button type="button" class="btn btn-secondary btn-sm" id="prop-browse-bg-image">Browse...</button>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Size & Position</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Size Mode</span>
          <div class="layout-property-input">
            <select id="prop-bg-size">
              <option value="cover" ${bg.size === "cover" ? "selected" : ""}>Cover</option>
              <option value="contain" ${bg.size === "contain" || !bg.size ? "selected" : ""}>Contain</option>
              <option value="stretch" ${bg.size === "stretch" ? "selected" : ""}>Stretch</option>
              <option value="tile" ${bg.size === "tile" ? "selected" : ""}>Tile</option>
              <option value="custom" ${bg.size === "custom" ? "selected" : ""}>Custom Scale</option>
            </select>
          </div>
        </div>
        ${isCustomScale ? `
        <div class="layout-property-row">
          <span class="layout-property-label">Scale</span>
          <div class="layout-property-input">
            <input type="range" id="prop-bg-scale" min="10" max="300" value="${(bg.scale || 1) * 100}">
            <span id="prop-bg-scale-value">${Math.round((bg.scale || 1) * 100)}%</span>
          </div>
        </div>
        ` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Offset X</span>
          <div class="layout-property-input">
            <input type="number" id="prop-bg-offset-x" value="${bg.offsetX || 0}" step="8">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Offset Y</span>
          <div class="layout-property-input">
            <input type="number" id="prop-bg-offset-y" value="${bg.offsetY || 0}" step="8">
          </div>
        </div>
      </div>
      ` : ""}

      <div class="layout-property-group">
        <div class="layout-property-group-title">Appearance</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Opacity</span>
          <div class="layout-property-input">
            <input type="range" id="prop-bg-opacity" min="0" max="100" value="${(bg.opacity ?? 1) * 100}">
            <span id="prop-bg-opacity-value">${Math.round((bg.opacity ?? 1) * 100)}%</span>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button type="button" class="btn btn-secondary btn-sm btn-block layout-danger-btn" id="prop-delete-bg">Remove Background</button>
      </div>
    `;

    this.bindBackgroundPropertyHandlers(bg);
  }

  private bindBackgroundPropertyHandlers(bg: LayoutBackground): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    // Push undo once on first input/change in this sidebar session
    sidebarContent?.addEventListener("input", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });
    sidebarContent?.addEventListener("change", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });

    const typeSelect = document.getElementById("prop-bg-type") as HTMLSelectElement;
    const colorInput = document.getElementById("prop-bg-color") as HTMLInputElement;
    const gradientInput = document.getElementById("prop-bg-gradient") as HTMLInputElement;
    const sizeSelect = document.getElementById("prop-bg-size") as HTMLSelectElement;
    const scaleInput = document.getElementById("prop-bg-scale") as HTMLInputElement;
    const scaleValue = document.getElementById("prop-bg-scale-value") as HTMLElement;
    const offsetXInput = document.getElementById("prop-bg-offset-x") as HTMLInputElement;
    const offsetYInput = document.getElementById("prop-bg-offset-y") as HTMLInputElement;
    const opacityInput = document.getElementById("prop-bg-opacity") as HTMLInputElement;
    const opacityValue = document.getElementById("prop-bg-opacity-value") as HTMLElement;
    const deleteBtn = document.getElementById("prop-delete-bg") as HTMLButtonElement;
    const bgImageSelect = document.getElementById("prop-bg-image-select") as HTMLSelectElement;
    const browseBgBtn = document.getElementById("prop-browse-bg-image") as HTMLButtonElement;

    typeSelect?.addEventListener("change", () => {
      const newType = typeSelect.value as "color" | "gradient";
      bg.type = newType;
      if (newType === "color") {
        bg.value = "#1a1a2e";
      } else if (newType === "gradient") {
        bg.value = "linear-gradient(180deg, #2a2a3a 0%, #1a1a2e 100%)";
      }
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    colorInput?.addEventListener("input", () => {
      bg.value = colorInput.value;
      this.host.renderCanvas();
    });

    gradientInput?.addEventListener("change", () => {
      bg.value = gradientInput.value;
      this.host.renderCanvas();
    });

    sizeSelect?.addEventListener("change", () => {
      bg.size = sizeSelect.value as "cover" | "contain" | "stretch" | "tile" | "custom";
      if (bg.size === "custom" && bg.scale === undefined) {
        bg.scale = 1;
      }
      this.host.renderCanvas();
      this.host.renderSidebar(); // Re-render to show/hide scale slider
    });

    scaleInput?.addEventListener("input", () => {
      bg.scale = parseInt(scaleInput.value) / 100;
      if (scaleValue) scaleValue.textContent = `${scaleInput.value}%`;
      this.host.renderCanvas();
    });

    offsetXInput?.addEventListener("change", () => {
      bg.offsetX = parseInt(offsetXInput.value) || 0;
      this.host.renderCanvas();
    });

    offsetYInput?.addEventListener("change", () => {
      bg.offsetY = parseInt(offsetYInput.value) || 0;
      this.host.renderCanvas();
    });

    opacityInput?.addEventListener("input", () => {
      bg.opacity = parseInt(opacityInput.value) / 100;
      if (opacityValue) opacityValue.textContent = `${opacityInput.value}%`;
      this.host.renderCanvas();
    });

    bgImageSelect?.addEventListener("change", () => {
      const imageId = bgImageSelect.value;
      if (!imageId) return;
      bg.type = "image";
      bg.value = imageId;
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    browseBgBtn?.addEventListener("click", () => {
      this.host.browseBackgroundImage(bg.layerIndex);
    });

    deleteBtn?.addEventListener("click", () => {
      if (!layout) return;
      this.host.pushUndoState();
      layout.backgrounds = layout.backgrounds.filter((b) => b.layerIndex !== bg.layerIndex);
      this.host.selectElement(null);
      this.host.renderCanvas();
    });
  }
}
