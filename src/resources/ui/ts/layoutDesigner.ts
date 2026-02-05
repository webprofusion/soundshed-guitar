/**
 * Layout Designer Modal
 *
 * Visual editor for creating custom effect parameter layouts with
 * drag-and-drop control positioning, custom backgrounds, and text labels.
 */

import { postMessage } from "./bridge.js";
import { uiState } from "./state.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import { showNotification } from "./notifications.js";
import {
  type EffectLayout,
  type LayoutControl,
  type LayoutTextLabel,
  type LayoutBackground,
  type LabelPosition,
  type KnobStylePreset,
  LAYOUT_GRID_SIZE,
  DEFAULT_LAYOUT_DIMENSIONS,
  LAYOUT_DIMENSION_LIMITS,
  snapToGrid,
  generateLayoutId,
  generateLabelId,
  createEmptyLayout,
} from "./layoutTypes.js";

type SelectedElement =
  | { type: "control"; paramKey: string }
  | { type: "label"; id: string }
  | { type: "background"; layerIndex: number }
  | null;

interface DragState {
  active: boolean;
  element: HTMLElement | null;
  startX: number;
  startY: number;
  elementStartX: number;
  elementStartY: number;
  type: "control" | "label" | null;
  id: string;
}

export class LayoutDesignerModal {
  private initialized = false;
  private effectType = "";
  private layout: EffectLayout | null = null;
  private paramDefs: ParameterDef[] = [];

  // Selection and drag state
  private selectedElement: SelectedElement = null;
  private dragState: DragState = {
    active: false,
    element: null,
    startX: 0,
    startY: 0,
    elementStartX: 0,
    elementStartY: 0,
    type: null,
    id: "",
  };

  // UI options
  private gridVisible = true;
  private previewMode = false;
  private zoom = 1;

  // DOM references
  private modal: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private saveBtn: HTMLButtonElement | null = null;
  private exportBtn: HTMLButtonElement | null = null;
  private canvas: HTMLElement | null = null;
  private grid: HTMLElement | null = null;
  private controlsLayer: HTMLElement | null = null;
  private sidebar: HTMLElement | null = null;
  private sidebarContent: HTMLElement | null = null;
  private titleEl: HTMLElement | null = null;

  // Toolbar buttons
  private gridToggleBtn: HTMLButtonElement | null = null;
  private previewToggleBtn: HTMLButtonElement | null = null;
  private addLabelBtn: HTMLButtonElement | null = null;
  private addBgBtn: HTMLButtonElement | null = null;
  private resetLayoutBtn: HTMLButtonElement | null = null;

  // Dimension inputs
  private widthInput: HTMLInputElement | null = null;
  private heightInput: HTMLInputElement | null = null;

  // Callbacks
  private onSaveCallback?: (layout: EffectLayout) => void;

  initialize(): void {
    if (this.initialized) return;
    this.initialized = true;

    this.modal = document.getElementById("layout-designer-modal");
    if (!this.modal) {
      console.warn("LayoutDesignerModal: modal element not found");
      return;
    }

    // Get DOM references
    this.titleEl = document.getElementById("layout-designer-title");
    this.closeBtn = document.getElementById("layout-designer-close") as HTMLButtonElement;
    this.cancelBtn = document.getElementById("layout-designer-cancel") as HTMLButtonElement;
    this.saveBtn = document.getElementById("layout-designer-save") as HTMLButtonElement;
    this.exportBtn = document.getElementById("layout-designer-export") as HTMLButtonElement;
    this.canvas = document.getElementById("layout-designer-canvas");
    this.grid = document.getElementById("layout-designer-grid");
    this.controlsLayer = document.getElementById("layout-designer-controls");
    this.sidebar = document.getElementById("layout-designer-sidebar");
    this.sidebarContent = document.getElementById("layout-designer-sidebar-content");

    // Toolbar buttons
    this.gridToggleBtn = document.getElementById("layout-designer-grid-toggle") as HTMLButtonElement;
    this.previewToggleBtn = document.getElementById("layout-designer-preview-toggle") as HTMLButtonElement;
    this.addLabelBtn = document.getElementById("layout-designer-add-label") as HTMLButtonElement;
    this.addBgBtn = document.getElementById("layout-designer-add-bg") as HTMLButtonElement;
    this.resetLayoutBtn = document.getElementById("layout-designer-reset") as HTMLButtonElement;

    console.log("[LayoutDesigner] initialize - addBgBtn found:", !!this.addBgBtn);

    // Dimension inputs
    this.widthInput = document.getElementById("layout-designer-width") as HTMLInputElement;
    this.heightInput = document.getElementById("layout-designer-height") as HTMLInputElement;

    this.bindEvents();
  }

  private bindEvents(): void {
    // Close buttons
    this.closeBtn?.addEventListener("click", () => this.close());
    this.cancelBtn?.addEventListener("click", () => this.close());
    this.saveBtn?.addEventListener("click", () => this.save());
    this.exportBtn?.addEventListener("click", () => this.exportLayout());

    // Modal backdrop click
    this.modal?.addEventListener("click", (e) => {
      if (e.target === this.modal) {
        this.close();
      }
    });

    // Toolbar buttons
    this.gridToggleBtn?.addEventListener("click", () => this.toggleGrid());
    this.previewToggleBtn?.addEventListener("click", () => this.togglePreview());
    this.addLabelBtn?.addEventListener("click", () => this.addTextLabel());
    this.addBgBtn?.addEventListener("click", () => this.browseBackgroundImage());
    this.resetLayoutBtn?.addEventListener("click", () => this.resetLayout());

    // Dimension inputs
    this.widthInput?.addEventListener("change", () => this.updateDimensions());
    this.heightInput?.addEventListener("change", () => this.updateDimensions());

    // Canvas mouse events for drag
    this.canvas?.addEventListener("mousedown", (e) => this.onCanvasMouseDown(e));
    document.addEventListener("mousemove", (e) => this.onDocumentMouseMove(e));
    document.addEventListener("mouseup", () => this.onDocumentMouseUp());

    // Keyboard shortcuts
    document.addEventListener("keydown", (e) => this.onKeyDown(e));

    // Canvas click to deselect
    this.canvas?.addEventListener("click", (e) => {
      if (e.target === this.canvas || e.target === this.grid || e.target === this.controlsLayer) {
        this.selectElement(null);
      }
    });
  }

  open(effectType: string, existingLayout?: EffectLayout): void {
    this.initialize();

    if (!this.modal) return;

    this.effectType = effectType;
    const typeInfo = EffectTypeRegistry.get(effectType);
    this.paramDefs = typeInfo?.parameters || [];

    // Set title
    if (this.titleEl) {
      this.titleEl.textContent = `Layout Designer - ${typeInfo?.displayName || effectType}`;
    }

    // Load or create layout
    if (existingLayout) {
      this.layout = JSON.parse(JSON.stringify(existingLayout)); // Deep clone
    } else {
      this.layout = this.createDefaultLayout(effectType);
    }

    // Reset state
    this.selectedElement = null;
    this.gridVisible = true;
    this.previewMode = false;
    this.zoom = 1;

    // Update UI
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();

    // Show modal
    this.modal.style.display = "flex";
  }

  close(): void {
    if (this.modal) {
      this.modal.style.display = "none";
    }
    this.layout = null;
    this.effectType = "";
    this.selectedElement = null;
  }

  onSave(callback: (layout: EffectLayout) => void): void {
    this.onSaveCallback = callback;
  }

  private save(): void {
    if (!this.layout) return;

    this.layout.modifiedAt = new Date().toISOString();

    // Send to plugin for persistence
    postMessage({
      type: "saveEffectLayout",
      effectType: this.effectType,
      layout: this.layout,
    });

    if (this.onSaveCallback) {
      this.onSaveCallback(this.layout);
    }

    showNotification("Layout saved", "success");
    this.close();
  }

  private exportLayout(): void {
    if (!this.layout) return;

    // Request export via plugin (will handle zip creation and file dialog)
    postMessage({
      type: "exportEffectLayout",
      effectType: this.effectType,
      layout: this.layout,
    });
  }

  private createDefaultLayout(effectType: string): EffectLayout {
    const layout = createEmptyLayout(effectType);

    // Auto-populate controls from param definitions
    const controlsPerRow = 4;
    const controlSpacing = 80;
    const startX = 40;
    const startY = 60;

    this.paramDefs.forEach((param, index) => {
      const row = Math.floor(index / controlsPerRow);
      const col = index % controlsPerRow;

      layout.controls.push({
        paramKey: param.key,
        type: param.unit === "toggle" ? "toggle" : "knob",
        position: {
          x: snapToGrid(startX + col * controlSpacing),
          y: snapToGrid(startY + row * controlSpacing),
        },
        style: {
          labelPosition: "top",
          showValue: true,
          valuePosition: "bottom",
          knobStyle: "default",
        },
      });
    });

    // Adjust dimensions to fit controls
    const rows = Math.ceil(this.paramDefs.length / controlsPerRow);
    layout.dimensions.height = Math.max(
      DEFAULT_LAYOUT_DIMENSIONS.height,
      snapToGrid(startY + rows * controlSpacing + 40)
    );

    return layout;
  }

  private resetLayout(): void {
    if (!this.effectType) return;

    this.layout = this.createDefaultLayout(this.effectType);
    this.selectedElement = null;
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    showNotification("Layout reset to default", "info");
  }

  private toggleGrid(): void {
    this.gridVisible = !this.gridVisible;
    this.grid?.classList.toggle("grid-visible", this.gridVisible);
    this.gridToggleBtn?.classList.toggle("active", this.gridVisible);
  }

  private togglePreview(): void {
    this.previewMode = !this.previewMode;
    this.canvas?.classList.toggle("preview-mode", this.previewMode);
    this.previewToggleBtn?.classList.toggle("active", this.previewMode);
    
    if (this.previewMode) {
      this.selectElement(null);
    }
  }

  private updateDimensions(): void {
    if (!this.layout || !this.widthInput || !this.heightInput) return;

    const width = Math.max(
      LAYOUT_DIMENSION_LIMITS.minWidth,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxWidth, parseInt(this.widthInput.value) || DEFAULT_LAYOUT_DIMENSIONS.width)
    );
    const height = Math.max(
      LAYOUT_DIMENSION_LIMITS.minHeight,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxHeight, parseInt(this.heightInput.value) || DEFAULT_LAYOUT_DIMENSIONS.height)
    );

    this.layout.dimensions = { width, height };
    this.updateDimensionInputs();
    this.updateCanvasSize();
  }

  private updateDimensionInputs(): void {
    if (!this.layout) return;
    if (this.widthInput) this.widthInput.value = String(this.layout.dimensions.width);
    if (this.heightInput) this.heightInput.value = String(this.layout.dimensions.height);
  }

  private updateCanvasSize(): void {
    if (!this.canvas || !this.layout) return;
    this.canvas.style.width = `${this.layout.dimensions.width * this.zoom}px`;
    this.canvas.style.height = `${this.layout.dimensions.height * this.zoom}px`;
  }

  private renderCanvas(): void {
    if (!this.canvas || !this.controlsLayer || !this.layout) return;

    this.updateCanvasSize();
    this.renderBackgrounds();
    this.renderControls();
    this.renderTextLabels();
  }

  private renderBackgrounds(): void {
    if (!this.canvas || !this.layout) return;

    // Remove existing background layers
    this.canvas.querySelectorAll(".layout-designer-background").forEach((el) => el.remove());

    // Render each background layer
    this.layout.backgrounds.forEach((bg) => {
      const isSelected = this.selectedElement?.type === "background" && this.selectedElement.layerIndex === bg.layerIndex;
      const layer = document.createElement("div");
      layer.className = `layout-designer-background layer-${bg.layerIndex}${isSelected ? " selected" : ""}`;
      layer.dataset.layerIndex = String(bg.layerIndex);

      if (bg.type === "color") {
        layer.style.backgroundColor = bg.value;
      } else if (bg.type === "gradient") {
        layer.style.background = bg.value;
      } else if (bg.type === "image") {
        // bg.value is imageId - resolve to actual URL
        const imageUrl = this.getImageUrl(bg.value);
        if (imageUrl) {
          layer.style.backgroundImage = `url(${imageUrl})`;
          // Apply size mode or custom scale
          if (bg.size === "custom" && bg.scale !== undefined) {
            layer.style.backgroundSize = `${bg.scale * 100}%`;
          } else {
            layer.style.backgroundSize = bg.size || "cover";
          }
          layer.style.backgroundRepeat = bg.size === "tile" ? "repeat" : "no-repeat";
          layer.style.backgroundPosition = `${bg.offsetX || 0}px ${bg.offsetY || 0}px`;
        }
      }

      if (bg.opacity !== undefined && bg.opacity < 1) {
        layer.style.opacity = String(bg.opacity);
      }

      // Make background clickable for selection
      layer.addEventListener("click", (e) => {
        e.stopPropagation();
        this.selectElement({ type: "background", layerIndex: bg.layerIndex });
      });

      if (this.canvas && this.grid) {
        this.canvas.insertBefore(layer, this.grid);
      }
    });
  }

  private getImageUrl(imageId: string): string | null {
    // Check layout library for image
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

  private renderControls(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Clear existing controls
    this.controlsLayer.querySelectorAll(".layout-control-placeholder").forEach((el) => el.remove());

    // Render each control
    this.layout.controls.forEach((control) => {
      const el = this.createControlElement(control);
      this.controlsLayer!.appendChild(el);
    });
  }

  private createControlElement(control: LayoutControl): HTMLElement {
    const paramDef = this.paramDefs.find((p) => p.key === control.paramKey);
    const label = control.labelOverride || paramDef?.name || control.paramKey;
    const isSelected =
      this.selectedElement?.type === "control" && this.selectedElement.paramKey === control.paramKey;

    const el = document.createElement("div");
    el.className = `layout-control-placeholder${isSelected ? " selected" : ""}`;
    el.dataset.paramKey = control.paramKey;
    el.style.left = `${control.position.x}px`;
    el.style.top = `${control.position.y}px`;

    if (control.size) {
      el.style.width = `${control.size.width}px`;
      el.style.height = `${control.size.height}px`;
    }

    const labelPos = control.style?.labelPosition || "top";
    const showValue = control.style?.showValue !== false;

    // Build control HTML
    let html = "";

    if (labelPos === "top") {
      html += `<span class="control-label">${label}</span>`;
    }

    if (control.type === "toggle") {
      html += `<div class="control-toggle"></div>`;
    } else {
      html += `<div class="control-knob" data-style="${control.style?.knobStyle || "default"}"></div>`;
    }

    if (labelPos === "bottom") {
      html += `<span class="control-label position-bottom">${label}</span>`;
    }

    if (showValue) {
      html += `<span class="control-value">0.00</span>`;
    }

    el.innerHTML = html;

    // Click to select
    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "control", paramKey: control.paramKey });
      }
    });

    return el;
  }

  private renderTextLabels(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Clear existing text labels
    this.controlsLayer.querySelectorAll(".layout-text-label").forEach((el) => el.remove());

    // Render each label
    this.layout.textLabels.forEach((label) => {
      const el = this.createTextLabelElement(label);
      this.controlsLayer!.appendChild(el);
    });
  }

  private createTextLabelElement(label: LayoutTextLabel): HTMLElement {
    const isSelected = this.selectedElement?.type === "label" && this.selectedElement.id === label.id;

    const el = document.createElement("div");
    el.className = `layout-text-label${isSelected ? " selected" : ""}`;
    el.dataset.labelId = label.id;
    el.style.left = `${label.position.x}px`;
    el.style.top = `${label.position.y}px`;
    el.style.fontSize = `${label.fontSize}px`;
    el.style.fontWeight = label.fontWeight || "normal";
    if (label.fontFamily) {
      el.style.fontFamily = label.fontFamily;
    }
    el.style.color = label.color || "var(--text-dark-primary)";
    el.style.textAlign = label.textAlign || "left";
    el.textContent = label.text;

    // Click to select
    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "label", id: label.id });
      }
    });

    // Double-click to edit
    el.addEventListener("dblclick", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.editTextLabel(label, el);
      }
    });

    return el;
  }

  private editTextLabel(label: LayoutTextLabel, el: HTMLElement): void {
    el.classList.add("editing");

    const input = document.createElement("input");
    input.type = "text";
    input.value = label.text;
    input.style.fontSize = `${label.fontSize}px`;
    input.style.color = label.color || "inherit";

    el.textContent = "";
    el.appendChild(input);
    input.focus();
    input.select();

    const finishEdit = () => {
      const newText = input.value.trim();
      if (newText) {
        label.text = newText;
      }
      el.classList.remove("editing");
      el.textContent = label.text;
      this.renderSidebar();
    };

    input.addEventListener("blur", finishEdit);
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        finishEdit();
      } else if (e.key === "Escape") {
        el.classList.remove("editing");
        el.textContent = label.text;
      }
    });
  }

  private selectElement(element: SelectedElement): void {
    this.selectedElement = element;
    this.renderCanvas();
    this.renderSidebar();
  }

  private renderSidebar(): void {
    if (!this.sidebarContent) return;

    if (!this.selectedElement) {
      this.sidebarContent.innerHTML = `
        <div class="layout-designer-sidebar-empty">
          Select a control or label to edit its properties
        </div>
      `;
      return;
    }

    if (this.selectedElement.type === "control") {
      this.renderControlProperties(this.selectedElement.paramKey);
    } else if (this.selectedElement.type === "label") {
      this.renderLabelProperties(this.selectedElement.id);
    } else if (this.selectedElement.type === "background") {
      this.renderBackgroundProperties(this.selectedElement.layerIndex);
    }
  }

  private renderBackgroundProperties(layerIndex: number): void {
    if (!this.sidebarContent || !this.layout) return;

    const bg = this.layout.backgrounds.find((b) => b.layerIndex === layerIndex);
    if (!bg) return;

    const isCustomScale = bg.size === "custom";

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Background Layer ${layerIndex + 1}</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Type</span>
          <span class="layout-property-input">${bg.type}</span>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Size & Position</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Size Mode</span>
          <div class="layout-property-input">
            <select id="prop-bg-size">
              <option value="cover" ${bg.size === "cover" || !bg.size ? "selected" : ""}>Cover</option>
              <option value="contain" ${bg.size === "contain" ? "selected" : ""}>Contain</option>
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
            <input type="range" id="prop-bg-scale" min="10" max="300" value="${(bg.scale || 1) * 100}" style="width: 80px;">
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

      <div class="layout-property-group">
        <div class="layout-property-group-title">Appearance</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Opacity</span>
          <div class="layout-property-input">
            <input type="range" id="prop-bg-opacity" min="0" max="100" value="${(bg.opacity ?? 1) * 100}" style="width: 80px;">
            <span id="prop-bg-opacity-value">${Math.round((bg.opacity ?? 1) * 100)}%</span>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-bg" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Remove Background</button>
      </div>
    `;

    this.bindBackgroundPropertyHandlers(bg);
  }

  private bindBackgroundPropertyHandlers(bg: LayoutBackground): void {
    const sizeSelect = document.getElementById("prop-bg-size") as HTMLSelectElement;
    const scaleInput = document.getElementById("prop-bg-scale") as HTMLInputElement;
    const scaleValue = document.getElementById("prop-bg-scale-value") as HTMLElement;
    const offsetXInput = document.getElementById("prop-bg-offset-x") as HTMLInputElement;
    const offsetYInput = document.getElementById("prop-bg-offset-y") as HTMLInputElement;
    const opacityInput = document.getElementById("prop-bg-opacity") as HTMLInputElement;
    const opacityValue = document.getElementById("prop-bg-opacity-value") as HTMLElement;
    const deleteBtn = document.getElementById("prop-delete-bg") as HTMLButtonElement;

    sizeSelect?.addEventListener("change", () => {
      bg.size = sizeSelect.value as "cover" | "contain" | "stretch" | "tile" | "custom";
      if (bg.size === "custom" && bg.scale === undefined) {
        bg.scale = 1;
      }
      this.renderCanvas();
      this.renderSidebar(); // Re-render to show/hide scale slider
    });

    scaleInput?.addEventListener("input", () => {
      bg.scale = parseInt(scaleInput.value) / 100;
      if (scaleValue) scaleValue.textContent = `${scaleInput.value}%`;
      this.renderCanvas();
    });

    offsetXInput?.addEventListener("change", () => {
      bg.offsetX = parseInt(offsetXInput.value) || 0;
      this.renderCanvas();
    });

    offsetYInput?.addEventListener("change", () => {
      bg.offsetY = parseInt(offsetYInput.value) || 0;
      this.renderCanvas();
    });

    opacityInput?.addEventListener("input", () => {
      bg.opacity = parseInt(opacityInput.value) / 100;
      if (opacityValue) opacityValue.textContent = `${opacityInput.value}%`;
      this.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!this.layout) return;
      this.layout.backgrounds = this.layout.backgrounds.filter((b) => b.layerIndex !== bg.layerIndex);
      this.selectElement(null);
      this.renderCanvas();
    });
  }

  private renderControlProperties(paramKey: string): void {
    if (!this.sidebarContent || !this.layout) return;

    const control = this.layout.controls.find((c) => c.paramKey === paramKey);
    if (!control) return;

    const paramDef = this.paramDefs.find((p) => p.key === paramKey);

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Control</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Parameter</span>
          <span class="layout-property-input">${paramDef?.name || paramKey}</span>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Type</span>
          <div class="layout-property-input">
            <select id="prop-control-type">
              <option value="knob" ${control.type === "knob" ? "selected" : ""}>Knob</option>
              <option value="toggle" ${control.type === "toggle" ? "selected" : ""}>Toggle</option>
              <option value="slider" ${control.type === "slider" ? "selected" : ""}>Slider</option>
            </select>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-pos-x" value="${control.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-pos-y" value="${control.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Label</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Hide Label</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-hide-label" ${control.style?.hideLabel ? "checked" : ""}>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Override</span>
          <div class="layout-property-input">
            <input type="text" id="prop-label-override" value="${control.labelOverride || ""}" placeholder="${paramDef?.name || paramKey}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Position</span>
          <div class="layout-property-input">
            <select id="prop-label-position">
              <option value="top" ${control.style?.labelPosition === "top" ? "selected" : ""}>Top</option>
              <option value="bottom" ${control.style?.labelPosition === "bottom" ? "selected" : ""}>Bottom</option>
              <option value="left" ${control.style?.labelPosition === "left" ? "selected" : ""}>Left</option>
              <option value="right" ${control.style?.labelPosition === "right" ? "selected" : ""}>Right</option>
              <option value="none" ${control.style?.labelPosition === "none" ? "selected" : ""}>Hidden</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-label-color" value="${control.style?.labelColor || "#ffffff"}">
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Style</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Knob Style</span>
          <div class="layout-property-input">
            <select id="prop-knob-style">
              <option value="default" ${control.style?.knobStyle === "default" ? "selected" : ""}>Default</option>
              <option value="pedal" ${control.style?.knobStyle === "pedal" ? "selected" : ""}>Pedal</option>
              <option value="amp" ${control.style?.knobStyle === "amp" ? "selected" : ""}>Amp</option>
              <option value="minimal" ${control.style?.knobStyle === "minimal" ? "selected" : ""}>Minimal</option>
              <option value="custom" ${control.style?.knobStyle === "custom" ? "selected" : ""}>Custom Image</option>
            </select>
          </div>
        </div>
        ${control.style?.knobStyle === "custom" ? `
        <div class="layout-image-preview">
          ${control.style?.knobImageId ? `<img src="${this.getImageUrl(control.style.knobImageId) || ""}" alt="Knob">` : `<span class="layout-image-preview-placeholder">No image</span>`}
        </div>
        <div class="layout-image-actions">
          <button id="prop-browse-knob-image">Browse...</button>
          ${control.style?.knobImageId ? `<button id="prop-clear-knob-image">Clear</button>` : ""}
        </div>
        ` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Show Value</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-show-value" ${control.style?.showValue !== false ? "checked" : ""}>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-control" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Remove from Layout</button>
      </div>
    `;

    // Bind property change handlers
    this.bindControlPropertyHandlers(control);
  }

  private bindControlPropertyHandlers(control: LayoutControl): void {
    const typeSelect = document.getElementById("prop-control-type") as HTMLSelectElement;
    const posXInput = document.getElementById("prop-pos-x") as HTMLInputElement;
    const posYInput = document.getElementById("prop-pos-y") as HTMLInputElement;
    const hideLabelCheck = document.getElementById("prop-hide-label") as HTMLInputElement;
    const labelOverrideInput = document.getElementById("prop-label-override") as HTMLInputElement;
    const labelPosSelect = document.getElementById("prop-label-position") as HTMLSelectElement;
    const labelColorInput = document.getElementById("prop-label-color") as HTMLInputElement;
    const knobStyleSelect = document.getElementById("prop-knob-style") as HTMLSelectElement;
    const showValueCheck = document.getElementById("prop-show-value") as HTMLInputElement;
    const deleteBtn = document.getElementById("prop-delete-control") as HTMLButtonElement;
    const browseKnobBtn = document.getElementById("prop-browse-knob-image") as HTMLButtonElement;
    const clearKnobBtn = document.getElementById("prop-clear-knob-image") as HTMLButtonElement;

    typeSelect?.addEventListener("change", () => {
      control.type = typeSelect.value as "knob" | "toggle" | "slider";
      this.renderCanvas();
    });

    posXInput?.addEventListener("change", () => {
      control.position.x = snapToGrid(parseInt(posXInput.value) || 0);
      posXInput.value = String(control.position.x);
      this.renderCanvas();
    });

    posYInput?.addEventListener("change", () => {
      control.position.y = snapToGrid(parseInt(posYInput.value) || 0);
      posYInput.value = String(control.position.y);
      this.renderCanvas();
    });

    hideLabelCheck?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.hideLabel = hideLabelCheck.checked;
      this.renderCanvas();
    });

    labelOverrideInput?.addEventListener("change", () => {
      control.labelOverride = labelOverrideInput.value.trim() || undefined;
      this.renderCanvas();
    });

    labelPosSelect?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.labelPosition = labelPosSelect.value as LabelPosition;
      this.renderCanvas();
    });

    labelColorInput?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.labelColor = labelColorInput.value;
      this.renderCanvas();
    });

    knobStyleSelect?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.knobStyle = knobStyleSelect.value as KnobStylePreset;
      this.renderCanvas();
      this.renderSidebar(); // Re-render to show/hide custom image picker
    });

    showValueCheck?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.showValue = showValueCheck.checked;
      this.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!this.layout) return;
      this.layout.controls = this.layout.controls.filter((c) => c.paramKey !== control.paramKey);
      this.selectElement(null);
      this.renderCanvas();
    });

    browseKnobBtn?.addEventListener("click", () => {
      this.browseKnobImage(control);
    });

    clearKnobBtn?.addEventListener("click", () => {
      if (control.style) {
        control.style.knobImageId = undefined;
        this.renderCanvas();
        this.renderSidebar();
      }
    });
  }

  private renderLabelProperties(labelId: string): void {
    if (!this.sidebarContent || !this.layout) return;

    const label = this.layout.textLabels.find((l) => l.id === labelId);
    if (!label) return;

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Text Label</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Text</span>
          <div class="layout-property-input">
            <input type="text" id="prop-label-text" value="${label.text}">
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-label-pos-x" value="${label.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-label-pos-y" value="${label.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Style</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Font</span>
          <div class="layout-property-input">
            <select id="prop-label-font">
              <option value="" ${!label.fontFamily ? "selected" : ""}>Default</option>
              <option value="Arial, sans-serif" ${label.fontFamily === "Arial, sans-serif" ? "selected" : ""}>Arial</option>
              <option value="'Helvetica Neue', Helvetica, sans-serif" ${label.fontFamily?.includes("Helvetica") ? "selected" : ""}>Helvetica</option>
              <option value="'Segoe UI', Tahoma, sans-serif" ${label.fontFamily?.includes("Segoe") ? "selected" : ""}>Segoe UI</option>
              <option value="Georgia, serif" ${label.fontFamily?.includes("Georgia") ? "selected" : ""}>Georgia</option>
              <option value="'Times New Roman', Times, serif" ${label.fontFamily?.includes("Times") ? "selected" : ""}>Times New Roman</option>
              <option value="'Courier New', Courier, monospace" ${label.fontFamily?.includes("Courier") ? "selected" : ""}>Courier New</option>
              <option value="Impact, sans-serif" ${label.fontFamily?.includes("Impact") ? "selected" : ""}>Impact</option>
              <option value="'Comic Sans MS', cursive" ${label.fontFamily?.includes("Comic") ? "selected" : ""}>Comic Sans</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Font Size</span>
          <div class="layout-property-input">
            <input type="number" id="prop-label-font-size" value="${label.fontSize}" min="8" max="48">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Weight</span>
          <div class="layout-property-input">
            <select id="prop-label-weight">
              <option value="normal" ${label.fontWeight !== "bold" ? "selected" : ""}>Normal</option>
              <option value="bold" ${label.fontWeight === "bold" ? "selected" : ""}>Bold</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-label-color" value="${label.color || "#ffffff"}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Align</span>
          <div class="layout-property-input">
            <select id="prop-label-align">
              <option value="left" ${label.textAlign !== "center" && label.textAlign !== "right" ? "selected" : ""}>Left</option>
              <option value="center" ${label.textAlign === "center" ? "selected" : ""}>Center</option>
              <option value="right" ${label.textAlign === "right" ? "selected" : ""}>Right</option>
            </select>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-label" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Delete Label</button>
      </div>
    `;

    // Bind property handlers
    this.bindLabelPropertyHandlers(label);
  }

  private bindLabelPropertyHandlers(label: LayoutTextLabel): void {
    const textInput = document.getElementById("prop-label-text") as HTMLInputElement;
    const posXInput = document.getElementById("prop-label-pos-x") as HTMLInputElement;
    const posYInput = document.getElementById("prop-label-pos-y") as HTMLInputElement;
    const fontSelect = document.getElementById("prop-label-font") as HTMLSelectElement;
    const fontSizeInput = document.getElementById("prop-label-font-size") as HTMLInputElement;
    const weightSelect = document.getElementById("prop-label-weight") as HTMLSelectElement;
    const colorInput = document.getElementById("prop-label-color") as HTMLInputElement;
    const alignSelect = document.getElementById("prop-label-align") as HTMLSelectElement;
    const deleteBtn = document.getElementById("prop-delete-label") as HTMLButtonElement;

    textInput?.addEventListener("change", () => {
      label.text = textInput.value || "Label";
      this.renderCanvas();
    });

    posXInput?.addEventListener("change", () => {
      label.position.x = snapToGrid(parseInt(posXInput.value) || 0);
      posXInput.value = String(label.position.x);
      this.renderCanvas();
    });

    posYInput?.addEventListener("change", () => {
      label.position.y = snapToGrid(parseInt(posYInput.value) || 0);
      posYInput.value = String(label.position.y);
      this.renderCanvas();
    });

    fontSelect?.addEventListener("change", () => {
      label.fontFamily = fontSelect.value || undefined;
      this.renderCanvas();
    });

    fontSizeInput?.addEventListener("change", () => {
      label.fontSize = Math.max(8, Math.min(48, parseInt(fontSizeInput.value) || 12));
      this.renderCanvas();
    });

    weightSelect?.addEventListener("change", () => {
      label.fontWeight = weightSelect.value as "normal" | "bold";
      this.renderCanvas();
    });

    colorInput?.addEventListener("change", () => {
      label.color = colorInput.value;
      this.renderCanvas();
    });

    alignSelect?.addEventListener("change", () => {
      label.textAlign = alignSelect.value as "left" | "center" | "right";
      this.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!this.layout) return;
      this.layout.textLabels = this.layout.textLabels.filter((l) => l.id !== label.id);
      this.selectElement(null);
      this.renderCanvas();
    });
  }

  private addTextLabel(): void {
    if (!this.layout) return;

    const newLabel: LayoutTextLabel = {
      id: generateLabelId(),
      text: "New Label",
      position: { x: snapToGrid(this.layout.dimensions.width / 2 - 40), y: snapToGrid(20) },
      fontSize: 14,
      fontWeight: "normal",
      color: "#ffffff",
      textAlign: "center",
    };

    this.layout.textLabels.push(newLabel);
    this.selectElement({ type: "label", id: newLabel.id });
    this.renderCanvas();
  }

  private browseBackgroundImage(): void {
    console.log("[LayoutDesigner] browseBackgroundImage called");
    postMessage({
      type: "browseLayoutImage",
      purpose: "background",
      layerIndex: this.layout?.backgrounds.length || 0,
    });
  }

  private browseKnobImage(control: LayoutControl): void {
    postMessage({
      type: "browseLayoutImage",
      purpose: "knob",
      paramKey: control.paramKey,
    });
  }

  /** Called when the plugin responds with a selected image */
  handleImageSelected(purpose: string, imageId: string, layerIndex?: number, paramKey?: string): void {
    console.log("[LayoutDesigner] handleImageSelected:", { purpose, imageId, layerIndex, paramKey });
    if (!this.layout) {
      console.warn("[LayoutDesigner] handleImageSelected: no layout!");
      return;
    }

    if (purpose === "background" && layerIndex !== undefined) {
      // Add or update background layer
      const existingIndex = this.layout.backgrounds.findIndex((bg) => bg.layerIndex === layerIndex);
      const newBg: LayoutBackground = {
        layerIndex,
        type: "image",
        value: imageId,
        opacity: 1,
        size: "cover",
      };

      if (existingIndex >= 0) {
        this.layout.backgrounds[existingIndex] = newBg;
      } else {
        this.layout.backgrounds.push(newBg);
      }
    } else if (purpose === "knob" && paramKey) {
      const control = this.layout.controls.find((c) => c.paramKey === paramKey);
      if (control) {
        if (!control.style) control.style = {};
        control.style.knobImageId = imageId;
      }
    }

    this.renderCanvas();
    this.renderSidebar();
  }

  // === Drag and Drop ===

  private onCanvasMouseDown(e: MouseEvent): void {
    if (this.previewMode) return;

    const target = e.target as HTMLElement;
    const placeholder = target.closest(".layout-control-placeholder") as HTMLElement;
    const textLabel = target.closest(".layout-text-label") as HTMLElement;

    if (placeholder) {
      const paramKey = placeholder.dataset.paramKey;
      if (paramKey) {
        this.startDrag(e, placeholder, "control", paramKey);
      }
    } else if (textLabel) {
      const labelId = textLabel.dataset.labelId;
      if (labelId) {
        this.startDrag(e, textLabel, "label", labelId);
      }
    }
  }

  private startDrag(e: MouseEvent, element: HTMLElement, type: "control" | "label", id: string): void {
    e.preventDefault();

    this.dragState = {
      active: true,
      element,
      startX: e.clientX,
      startY: e.clientY,
      elementStartX: parseInt(element.style.left) || 0,
      elementStartY: parseInt(element.style.top) || 0,
      type,
      id,
    };

    element.classList.add("dragging");
  }

  private onDocumentMouseMove(e: MouseEvent): void {
    if (!this.dragState.active || !this.dragState.element || !this.layout) return;

    const dx = e.clientX - this.dragState.startX;
    const dy = e.clientY - this.dragState.startY;

    const newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
    const newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);

    // Clamp to canvas bounds
    const clampedX = Math.max(0, Math.min(this.layout.dimensions.width - 60, newX));
    const clampedY = Math.max(0, Math.min(this.layout.dimensions.height - 60, newY));

    this.dragState.element.style.left = `${clampedX}px`;
    this.dragState.element.style.top = `${clampedY}px`;
  }

  private onDocumentMouseUp(): void {
    if (!this.dragState.active || !this.layout) return;

    const { element, type, id } = this.dragState;

    if (element) {
      element.classList.remove("dragging");

      const newX = parseInt(element.style.left) || 0;
      const newY = parseInt(element.style.top) || 0;

      // Update model
      if (type === "control") {
        const control = this.layout.controls.find((c) => c.paramKey === id);
        if (control) {
          control.position = { x: newX, y: newY };
        }
      } else if (type === "label") {
        const label = this.layout.textLabels.find((l) => l.id === id);
        if (label) {
          label.position = { x: newX, y: newY };
        }
      }

      // Update sidebar if selected
      if (
        (this.selectedElement?.type === "control" && this.selectedElement.paramKey === id) ||
        (this.selectedElement?.type === "label" && this.selectedElement.id === id)
      ) {
        this.renderSidebar();
      }
    }

    this.dragState = {
      active: false,
      element: null,
      startX: 0,
      startY: 0,
      elementStartX: 0,
      elementStartY: 0,
      type: null,
      id: "",
    };
  }

  // === Keyboard Shortcuts ===

  private onKeyDown(e: KeyboardEvent): void {
    if (!this.modal || this.modal.style.display === "none") return;
    if ((e.target as HTMLElement).tagName === "INPUT") return;

    if (e.key === "Escape") {
      if (this.selectedElement) {
        this.selectElement(null);
      } else {
        this.close();
      }
      return;
    }

    if (e.key === "Delete" || e.key === "Backspace") {
      this.deleteSelectedElement();
      return;
    }

    // Arrow keys for nudging
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.key)) {
      e.preventDefault();
      const nudge = e.shiftKey ? LAYOUT_GRID_SIZE : 1;
      this.nudgeSelectedElement(e.key, nudge);
    }

    // Grid toggle: G
    if (e.key === "g" || e.key === "G") {
      this.toggleGrid();
    }

    // Preview toggle: P
    if (e.key === "p" || e.key === "P") {
      this.togglePreview();
    }
  }

  private deleteSelectedElement(): void {
    if (!this.layout || !this.selectedElement) return;

    if (this.selectedElement.type === "control") {
      const selectedParamKey = this.selectedElement.paramKey;
      this.layout.controls = this.layout.controls.filter(
        (c) => c.paramKey !== selectedParamKey
      );
    } else if (this.selectedElement.type === "label") {
      this.layout.textLabels = this.layout.textLabels.filter(
        (l) => l.id !== (this.selectedElement as { type: "label"; id: string }).id
      );
    }

    this.selectElement(null);
    this.renderCanvas();
  }

  private nudgeSelectedElement(key: string, amount: number): void {
    if (!this.layout || !this.selectedElement) return;

    let position: { x: number; y: number } | undefined;

    if (this.selectedElement.type === "control") {
      const selectedParamKey = this.selectedElement.paramKey;
      const control = this.layout.controls.find((c) => c.paramKey === selectedParamKey);
      position = control?.position;
    } else if (this.selectedElement.type === "label") {
      const label = this.layout.textLabels.find(
        (l) => l.id === (this.selectedElement as { type: "label"; id: string }).id
      );
      position = label?.position;
    }

    if (!position) return;

    switch (key) {
      case "ArrowUp":
        position.y = Math.max(0, position.y - amount);
        break;
      case "ArrowDown":
        position.y = Math.min(this.layout.dimensions.height - 20, position.y + amount);
        break;
      case "ArrowLeft":
        position.x = Math.max(0, position.x - amount);
        break;
      case "ArrowRight":
        position.x = Math.min(this.layout.dimensions.width - 20, position.x + amount);
        break;
    }

    this.renderCanvas();
    this.renderSidebar();
  }
}

// Singleton instance
export const layoutDesigner = new LayoutDesignerModal();
