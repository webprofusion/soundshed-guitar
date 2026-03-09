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
import { showConfirm } from "./dialogs.js";
import { arrayBufferToBase64 } from "./utils.js";
import { renderCustomLayoutPreviewLayers, type LayoutResourceControlDef } from "./layoutRenderer.js";
import { buildDefaultParamControlsHtml } from "./signalPath.js";
import type { GraphNode } from "./types.js";
import {
  type EffectLayout,
  type LayoutControl,
  type LayoutTextLabel,
  type LayoutBackground,
  type LayoutImageRef,
  type LayoutRectangleOverlay,
  type LayoutLibraryEntry,
  type LabelPosition,
  type KnobStylePreset,
  LAYOUT_GRID_SIZE,
  DEFAULT_LAYOUT_DIMENSIONS,
  LAYOUT_DIMENSION_LIMITS,
  snapToGrid,
  sanitizeLayout,
  generateLayoutId,
  generateLabelId,
  generateOverlayId,
  createEmptyLayout,
  layoutLookupKey,
} from "./layoutTypes.js";

type SelectedElement =
  | { type: "control"; paramKey: string }
  | { type: "label"; id: string }
  | { type: "overlay"; id: string }
  | { type: "background"; layerIndex: number }
  | null;

interface DragState {
  active: boolean;
  element: HTMLElement | null;
  startX: number;
  startY: number;
  elementStartX: number;
  elementStartY: number;
  elementStartWidth: number;
  elementStartHeight: number;
  type: "control" | "label" | "overlay" | null;
  mode: "move" | "resize";
  resizeHandle: "top-left" | "top-right" | "bottom-left" | "bottom-right" | null;
  id: string;
}

interface LayoutResourceCandidate {
  controlKey: string;
  displayName: string;
  resourceType: string;
  resourceIndex: number;
  exposedResourceId?: string;
  allowBrowseFile?: boolean;
}

interface CopiedTextLabelPayload {
  text: string;
  position: { x: number; y: number };
  fontSize: number;
  fontWeight?: "normal" | "bold";
  fontFamily?: string;
  color?: string;
  textAlign?: "left" | "center" | "right";
}

export class LayoutDesignerModal {
  private static readonly LIBRARY_CHANGED_EVENT = "layout-library-changed";
  private initialized = false;
  private effectType = "";
  private blendId = "";
  private layout: EffectLayout | null = null;
  private isFactoryLayout = false;
  private isNewLayout = false;  // true for layouts never persisted to disk yet
  private paramDefs: ParameterDef[] = [];
  private resourceCandidates: LayoutResourceCandidate[] = [];

  // Selection and drag state
  private selectedElement: SelectedElement = null;
  private dragState: DragState = {
    active: false,
    element: null,
    startX: 0,
    startY: 0,
    elementStartX: 0,
    elementStartY: 0,
    elementStartWidth: 0,
    elementStartHeight: 0,
    type: null,
    mode: "move",
    resizeHandle: null,
    id: "",
  };

  // UI options
  private gridVisible = true;
  private previewMode = false;
  private zoom = 1;

  // Undo/redo history
  private undoStack: string[] = [];
  private redoStack: string[] = [];
  private static readonly MAX_UNDO = 50;
  private copiedTextLabel: CopiedTextLabelPayload | null = null;
  /** Tracks whether undo was pushed for current sidebar edit session */
  private sidebarUndoPushed = false;
  /** Timer for nudge undo debouncing */
  private nudgeUndoTimer: ReturnType<typeof setTimeout> | null = null;

  // DOM references
  private modal: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private saveBtn: HTMLButtonElement | null = null;
  private deleteBtn: HTMLButtonElement | null = null;
  private exportBtn: HTMLButtonElement | null = null;
  private importBtn: HTMLButtonElement | null = null;
  private importFileInput: HTMLInputElement | null = null;
  private canvas: HTMLElement | null = null;
  private canvasWrapper: HTMLElement | null = null;
  private grid: HTMLElement | null = null;
  private controlsLayer: HTMLElement | null = null;
  private sidebar: HTMLElement | null = null;
  private sidebarContent: HTMLElement | null = null;
  private titleEl: HTMLElement | null = null;

  // Toolbar buttons
  private gridToggleBtn: HTMLButtonElement | null = null;
  private previewToggleBtn: HTMLButtonElement | null = null;
  private addLabelBtn: HTMLButtonElement | null = null;
  private addControlBtn: HTMLButtonElement | null = null;
  private addRectBtn: HTMLButtonElement | null = null;
  private addBgBtn: HTMLButtonElement | null = null;
  private addColorBgBtn: HTMLButtonElement | null = null;
  private useDefaultControlsCheckbox: HTMLInputElement | null = null;
  private resetLayoutBtn: HTMLButtonElement | null = null;
  private undoBtn: HTMLButtonElement | null = null;
  private redoBtn: HTMLButtonElement | null = null;
  private zoomInBtn: HTMLButtonElement | null = null;
  private zoomOutBtn: HTMLButtonElement | null = null;
  private zoomResetBtn: HTMLButtonElement | null = null;
  private zoomLabel: HTMLElement | null = null;

  // Dimension inputs
  private widthInput: HTMLInputElement | null = null;
  private heightInput: HTMLInputElement | null = null;

  // Callbacks
  private onSaveCallback?: (layout: EffectLayout) => void;
  private onCloseCallback?: (didSave: boolean) => void;

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
    this.deleteBtn = document.getElementById("layout-designer-delete") as HTMLButtonElement;
    this.exportBtn = document.getElementById("layout-designer-export") as HTMLButtonElement;
    this.importBtn = document.getElementById("layout-designer-import") as HTMLButtonElement;
    this.importFileInput = document.getElementById("layout-designer-import-file") as HTMLInputElement;
    this.canvas = document.getElementById("layout-designer-canvas");
    this.canvasWrapper = document.getElementById("layout-designer-canvas-wrapper");
    this.grid = document.getElementById("layout-designer-grid");
    this.controlsLayer = document.getElementById("layout-designer-controls");
    this.sidebar = document.getElementById("layout-designer-sidebar");
    this.sidebarContent = document.getElementById("layout-designer-sidebar-content");

    // Toolbar buttons
    this.gridToggleBtn = document.getElementById("layout-designer-grid-toggle") as HTMLButtonElement;
    this.previewToggleBtn = document.getElementById("layout-designer-preview-toggle") as HTMLButtonElement;
    this.addLabelBtn = document.getElementById("layout-designer-add-label") as HTMLButtonElement;
    this.addControlBtn = document.getElementById("layout-designer-add-control") as HTMLButtonElement;
    this.addRectBtn = document.getElementById("layout-designer-add-rect") as HTMLButtonElement;
    this.addBgBtn = document.getElementById("layout-designer-add-bg") as HTMLButtonElement;
    this.addColorBgBtn = document.getElementById("layout-designer-add-color-bg") as HTMLButtonElement;
    this.useDefaultControlsCheckbox = document.getElementById("layout-designer-use-default-controls") as HTMLInputElement;
    this.resetLayoutBtn = document.getElementById("layout-designer-reset") as HTMLButtonElement;
    this.undoBtn = document.getElementById("layout-designer-undo") as HTMLButtonElement;
    this.redoBtn = document.getElementById("layout-designer-redo") as HTMLButtonElement;
    this.zoomInBtn = document.getElementById("layout-designer-zoom-in") as HTMLButtonElement;
    this.zoomOutBtn = document.getElementById("layout-designer-zoom-out") as HTMLButtonElement;
    this.zoomResetBtn = document.getElementById("layout-designer-zoom-reset") as HTMLButtonElement;
    this.zoomLabel = document.getElementById("layout-designer-zoom-label");

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
    this.saveBtn?.addEventListener("click", () => { void this.save(); });
    this.deleteBtn?.addEventListener("click", () => { void this.confirmDeleteCurrentLayout(); });
    this.exportBtn?.addEventListener("click", () => this.exportLayout());
    this.importBtn?.addEventListener("click", () => this.importFileInput?.click());
    this.importFileInput?.addEventListener("change", () => this.handleImportFileSelected());

    // Modal backdrop click
    this.modal?.addEventListener("mousedown", (e) => {
      if (e.target === this.modal) {
        this.close();
      }
    });

    // Toolbar buttons
    this.gridToggleBtn?.addEventListener("click", () => this.toggleGrid());
    this.previewToggleBtn?.addEventListener("click", () => this.togglePreview());
    this.addLabelBtn?.addEventListener("click", () => this.addTextLabel());
    this.addControlBtn?.addEventListener("click", () => this.showAddControlMenu());
    this.addRectBtn?.addEventListener("click", () => this.addRectangleOverlay());
    this.addBgBtn?.addEventListener("click", () => this.showAddBackgroundMenu());
    this.addColorBgBtn?.addEventListener("click", () => this.addColorBackground());
    this.useDefaultControlsCheckbox?.addEventListener("change", () => this.toggleUseDefaultControls());
    this.resetLayoutBtn?.addEventListener("click", () => this.resetLayout());
    this.undoBtn?.addEventListener("click", () => this.undo());
    this.redoBtn?.addEventListener("click", () => this.redo());
    this.zoomInBtn?.addEventListener("click", () => this.setZoom(this.zoom + 0.25));
    this.zoomOutBtn?.addEventListener("click", () => this.setZoom(this.zoom - 0.25));
    this.zoomResetBtn?.addEventListener("click", () => this.setZoom(1));

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

  /**
   * Open the layout designer.
   * @param effectType The effect type to design a layout for
   * @param existingLayout Optional existing layout to edit
   * @param options.blendId Optional blend definition ID (for per-blend layouts)
   * @param options.blendName Optional blend display name for the title
   * @param options.blendParamDefs Optional override parameter definitions (blend params + base params)
   */
  open(
    effectType: string,
    existingLayout?: EffectLayout,
    options?: { blendId?: string; blendName?: string; blendParamDefs?: ParameterDef[] },
  ): void {
    this.initialize();

    if (!this.modal) return;

    this.effectType = effectType;
    this.blendId = options?.blendId || "";
    const typeInfo = EffectTypeRegistry.get(effectType);

    // Use blend-specific params if provided, otherwise fall back to registry
    if (options?.blendParamDefs?.length) {
      this.paramDefs = options.blendParamDefs;
    } else {
      this.paramDefs = typeInfo?.parameters || [];
    }
    this.resourceCandidates = this.buildResourceCandidates(typeInfo);

    // Set title — include blend name when designing a per-blend layout
    if (this.titleEl) {
      const baseName = typeInfo?.displayName || effectType;
      const blendSuffix = options?.blendName ? ` — ${options.blendName}` : "";
      this.titleEl.textContent = `Layout Designer - ${baseName}${blendSuffix}`;
    }

    // Load or create layout
    if (existingLayout) {
      this.layout = JSON.parse(JSON.stringify(existingLayout)); // Deep clone
    } else {
      this.layout = this.createDefaultLayout(effectType);
    }

    // Ensure layoutId is always set at open time so images can reference it before save.
    if (this.layout && !this.layout.layoutId) {
      this.layout.layoutId = generateLayoutId();
    }

    // Determine whether this is a factory layout (read-only origin).
    // If so, fork a new layoutId so it saves as a brand-new user layout.
    const blendKey = (options?.blendId || "") ? layoutLookupKey(effectType, options?.blendId) : effectType;

    // Look up the library entry by layoutId. Also fall back to the base effectType key:
    // getCustomLayout() may return a non-blend layout as fallback for a blend context,
    // which would cause a key mismatch and incorrectly set isNewLayout = true.
    let libraryEntry = uiState.layoutLibrary?.byEffectType[blendKey]?.find(
      (e) => e.layoutId === this.layout?.layoutId
    );
    if (!libraryEntry && blendKey !== effectType) {
      libraryEntry = uiState.layoutLibrary?.byEffectType[effectType]?.find(
        (e) => e.layoutId === this.layout?.layoutId
      );
    }

    this.isFactoryLayout = libraryEntry?.isFactory === true;
    this.isNewLayout = !libraryEntry || this.isFactoryLayout;

    if (this.isFactoryLayout && this.layout) {
      // Before forking, check if a non-factory user layout already exists for this key.
      // If one does, redirect to editing it instead of creating yet another copy.
      const existingUserEntry =
        uiState.layoutLibrary?.byEffectType[blendKey]?.find(
          (e) => !e.isFactory && e.isDefault
        ) ??
        uiState.layoutLibrary?.byEffectType[blendKey]?.find((e) => !e.isFactory);

      if (existingUserEntry) {
        // Redirect: open the existing user copy instead of forking again
        this.layout = JSON.parse(JSON.stringify(existingUserEntry.layout));
        this.isFactoryLayout = false;
        this.isNewLayout = false;
      } else {
        // No user copy yet: fork a new one from this factory layout
        this.layout.layoutId = generateLayoutId();
      }
    }

    if (this.layout && !Array.isArray(this.layout.overlays)) {
      this.layout.overlays = [];
    }

    // Tag the layout with blendId so it persists across save/load
    if (this.layout && this.blendId) {
      this.layout.blendId = this.blendId;
    }

    // Reset state
    this.selectedElement = null;
    this.gridVisible = true;
    this.previewMode = false;
    this.zoom = 1;
    this.undoStack = [];
    this.redoStack = [];

    // Sync toolbar checkbox
    if (this.useDefaultControlsCheckbox) {
      this.useDefaultControlsCheckbox.checked = this.layout?.useDefaultControls === true;
    }
    // Sync Add Control button dim state
    if (this.addControlBtn) {
      const isDefaultControls = this.layout?.useDefaultControls === true;
      this.addControlBtn.style.opacity = isDefaultControls ? "0.4" : "";
      this.addControlBtn.title = isDefaultControls
        ? "Add Control (disabled \u2014 Default Controls mode)"
        : "Add Parameter Control";
    }

    // Update UI
    this.updateDimensionInputs();
    this.updateZoomUI();
    this.updateUndoRedoButtons();
    this.updateDeleteButtonVisibility();
    this.renderCanvas();
    this.renderSidebar();

    // Show modal
    this.modal.style.display = "flex";
  }

  close(didSave: boolean = false): void {
    if (this.modal) {
      this.modal.style.display = "none";
    }
    this.layout = null;
    this.effectType = "";
    this.blendId = "";
    this.selectedElement = null;
    this.onCloseCallback?.(didSave);
  }

  onSave(callback: (layout: EffectLayout) => void): void {
    this.onSaveCallback = callback;
  }

  onClose(callback: (didSave: boolean) => void): void {
    this.onCloseCallback = callback;
  }

  private persistLayoutLocally(layout: EffectLayout): void {
    if (!uiState.layoutLibrary) {
      uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
    }

    const blendId = this.blendId || layout.blendId || "";
    const lookupKey = layoutLookupKey(this.effectType || layout.effectType, blendId || undefined);
    if (!uiState.layoutLibrary.byEffectType[lookupKey]) {
      uiState.layoutLibrary.byEffectType[lookupKey] = [];
    }

    const list = uiState.layoutLibrary.byEffectType[lookupKey];
    const layoutId = layout.layoutId || generateLayoutId();
    layout.layoutId = layoutId;

    const clonedLayout = JSON.parse(JSON.stringify(layout)) as EffectLayout;
    const existingIndex = list.findIndex((entry) => entry.layoutId === layoutId);
    const newEntry: LayoutLibraryEntry = {
      layout: clonedLayout,
      isDefault: true,
      layoutId,
      filePath: existingIndex >= 0 ? list[existingIndex].filePath : undefined,
    };

    if (existingIndex >= 0) {
      list[existingIndex] = newEntry;
    } else {
      list.push(newEntry);
    }

    uiState.layoutLibrary.defaults[lookupKey] = layoutId;
    uiState.layoutLibrary.byEffectType[lookupKey] = list.map((entry) => ({
      ...entry,
      isDefault: entry.layoutId === layoutId,
    }));
    window.dispatchEvent(new CustomEvent(LayoutDesignerModal.LIBRARY_CHANGED_EVENT));
  }

  private updateDeleteButtonVisibility(): void {
    if (!this.deleteBtn) {
      return;
    }
    const canDelete = !!this.layout?.layoutId && !this.isFactoryLayout && !this.isNewLayout;
    this.deleteBtn.hidden = !canDelete;
    this.deleteBtn.disabled = !canDelete;
  }

  private findCurrentLibraryEntry(): { key: string; entries: LayoutLibraryEntry[]; entry: LayoutLibraryEntry } | null {
    const library = uiState.layoutLibrary;
    const layoutId = this.layout?.layoutId;
    if (!library || !layoutId || !this.layout) {
      return null;
    }

    const lookupKeys = Array.from(new Set([
      layoutLookupKey(this.effectType || this.layout.effectType, this.blendId || this.layout.blendId || undefined),
      layoutLookupKey(this.layout.effectType, this.layout.blendId || undefined),
      this.effectType || this.layout.effectType,
      this.layout.effectType,
    ].filter(Boolean)));

    for (const key of lookupKeys) {
      const entries = library.byEffectType[key] ?? [];
      const entry = entries.find((candidate) => candidate.layoutId === layoutId);
      if (entry) {
        return { key, entries, entry };
      }
    }

    return null;
  }

  private async confirmDeleteCurrentLayout(): Promise<void> {
    const current = this.findCurrentLibraryEntry();
    if (!current || current.entry.isFactory) {
      return;
    }

    const name = current.entry.layout.name || current.entry.layout.effectType;
    const confirmed = await showConfirm(`Delete layout "${name}"? This cannot be undone.`, "Delete Layout");
    if (!confirmed) {
      return;
    }

    this.deleteCurrentLayout(current.key, current.entries, current.entry);
  }

  private deleteCurrentLayout(key: string, entries: LayoutLibraryEntry[], entry: LayoutLibraryEntry): void {
    const library = uiState.layoutLibrary;
    if (!library) {
      return;
    }
    const layoutName = entry.layout.name || entry.layout.effectType;

    postMessage({
      type: "deleteLayout",
      effectType: entry.layout.effectType,
      blendId: entry.layout.blendId ?? "",
      layoutId: entry.layoutId,
    });

    library.byEffectType[key] = entries.filter((candidate) => candidate.layoutId !== entry.layoutId);
    if (library.defaults[key] === entry.layoutId) {
      const nextDefault =
        library.byEffectType[key]?.find((candidate) => !candidate.isFactory)?.layoutId ??
        library.byEffectType[key]?.[0]?.layoutId;
      if (nextDefault) {
        library.defaults[key] = nextDefault;
        library.byEffectType[key] = library.byEffectType[key].map((candidate) => ({
          ...candidate,
          isDefault: candidate.layoutId === nextDefault,
        }));
      } else {
        delete library.defaults[key];
      }
    }

    if ((library.byEffectType[key] ?? []).length === 0) {
      delete library.byEffectType[key];
    }

    window.dispatchEvent(new CustomEvent(LayoutDesignerModal.LIBRARY_CHANGED_EVENT));
    showNotification(`Layout "${layoutName}" deleted`, "success");
    this.close(false);
  }

  /** Collect all image IDs referenced by the current layout. */
  private collectReferencedImageIds(): string[] {
    if (!this.layout) return [];
    const ids = new Set<string>();
    for (const bg of this.layout.backgrounds) {
      if (bg.type === "image" && bg.value) ids.add(bg.value);
    }
    for (const control of this.layout.controls) {
      if (control.style?.knobImageId) ids.add(control.style.knobImageId);
    }
    return Array.from(ids);
  }

  private async save(): Promise<void> {
    if (!this.layout) return;

    this.layout.modifiedAt = new Date().toISOString();

    // Ensure blendId is stored in the layout itself
    if (this.blendId) {
      this.layout.blendId = this.blendId;
    }

    // Capture a thumbnail of the current design state before persisting
    try {
      const thumbnail = await this.captureLayoutThumbnail();
      if (thumbnail) this.layout.thumbnailDataUrl = thumbnail;
    } catch { /* thumbnail failure must not block save */ }

    const isNewLayout = this.isNewLayout;
    const referencedImageIds = isNewLayout ? this.collectReferencedImageIds() : [];

    // Send to plugin for persistence (include blendId for per-blend file naming)
    postMessage({
      type: "saveEffectLayout",
      effectType: this.effectType,
      blendId: this.blendId || undefined,
      layoutId: this.layout.layoutId,
      layout: this.layout,
      isNewLayout,
      referencedImageIds,
    });

    // After first save the layout is no longer new/factory
    this.isNewLayout = false;
    this.isFactoryLayout = false;
    this.updateDeleteButtonVisibility();

    this.persistLayoutLocally(this.layout);

    if (this.onSaveCallback) {
      this.onSaveCallback(this.layout);
    }

    showNotification("Layout saved", "success");
    this.close(true);
  }

  /** Render a compact thumbnail of the current layout onto an offscreen canvas and return a JPEG data URL. */
  private async captureLayoutThumbnail(): Promise<string | null> {
    const layout = this.layout;
    if (!layout) return null;

    const THUMB_W = 280;
    const layoutW = layout.dimensions.width;
    const layoutH = layout.dimensions.height;
    const scale = Math.min(THUMB_W / layoutW, 1);
    const scaledW = Math.ceil(layoutW * scale);
    const scaledH = Math.ceil(layoutH * scale);

    const canvasEl = document.createElement("canvas");
    canvasEl.width = scaledW;
    canvasEl.height = scaledH;
    const ctx = canvasEl.getContext("2d");
    if (!ctx) return null;

    // Base fill for transparent/unset areas
    ctx.fillStyle = "#1c1c1c";
    ctx.fillRect(0, 0, scaledW, scaledH);

    // Backgrounds
    const sortedBgs = [...layout.backgrounds].sort((a, b) => a.layerIndex - b.layerIndex);
    for (const bg of sortedBgs) {
      ctx.globalAlpha = bg.opacity ?? 1;
      if (bg.type === "color") {
        ctx.fillStyle = bg.value;
        ctx.fillRect(0, 0, scaledW, scaledH);
      } else if (bg.type === "image") {
        const imageRef = uiState.layoutLibrary?.images.find((img) => img.imageId === bg.value);
        if (imageRef?.dataUrl) {
          try {
            const imgEl = await LayoutDesignerModal.loadThumbnailImage(imageRef.dataUrl);
            ctx.drawImage(imgEl, 0, 0, scaledW, scaledH);
          } catch { /* ignore failed image */ }
        }
      }
      ctx.globalAlpha = 1;
    }

    // Rectangle overlays
    for (const overlay of layout.overlays ?? []) {
      const ox = overlay.position.x * scale;
      const oy = overlay.position.y * scale;
      const ow = overlay.size.width * scale;
      const oh = overlay.size.height * scale;
      const style = overlay.style ?? {};
      if (style.backgroundColor) {
        ctx.globalAlpha = style.backgroundOpacity ?? 1;
        ctx.fillStyle = style.backgroundColor;
        ctx.fillRect(ox, oy, ow, oh);
        ctx.globalAlpha = 1;
      }
      if (style.borderColor && (style.borderWidth ?? 0) > 0) {
        ctx.strokeStyle = style.borderColor;
        ctx.lineWidth = Math.max(0.5, (style.borderWidth ?? 1) * scale);
        ctx.strokeRect(ox, oy, ow, oh);
      }
    }

    // Control indicator dots
    if (!layout.useDefaultControls) {
      ctx.fillStyle = "rgba(90, 159, 212, 0.55)";
      const r = Math.max(4, 7 * scale);
      for (const control of layout.controls) {
        const cx = control.position.x * scale + r;
        const cy = control.position.y * scale + r;
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Text labels
    for (const label of layout.textLabels) {
      const fontSize = Math.max(5, Math.round((label.fontSize || 11) * scale));
      ctx.font = `${label.fontWeight ?? "normal"} ${fontSize}px ${label.fontFamily ?? "sans-serif"}`;
      ctx.fillStyle = label.color ?? "#dddddd";
      ctx.globalAlpha = 0.9;
      ctx.textAlign = (label.textAlign as CanvasTextAlign) ?? "left";
      ctx.fillText(label.text, label.position.x * scale, label.position.y * scale + fontSize);
      ctx.globalAlpha = 1;
    }

    return canvasEl.toDataURL("image/jpeg", 0.82);
  }

  private static loadThumbnailImage(src: string): Promise<HTMLImageElement> {
    return new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = reject;
      img.src = src;
    });
  }

  private async exportLayout(): Promise<void> {
    if (!this.layout) return;

    const zipLib = window.JSZip;
    if (!zipLib) {
      showNotification("Export failed: archive library not available", "error");
      return;
    }

    const zip = new zipLib();

    // Collect all image IDs referenced by this layout
    const referencedImageIds = new Set<string>();
    for (const bg of this.layout.backgrounds) {
      if (bg.type === "image" && bg.value) {
        referencedImageIds.add(bg.value);
      }
    }
    for (const control of this.layout.controls) {
      if (control.style?.knobImageId) {
        referencedImageIds.add(control.style.knobImageId);
      }
    }

    // Add referenced images to zip
    const imagesFolder = zip.folder("images");
    const imageManifest: Array<{ imageId: string; fileName: string; type?: string }> = [];

    if (imagesFolder) {
      const images = uiState.layoutLibrary?.images ?? [];
      for (const imageId of referencedImageIds) {
        const img = images.find((i) => i.imageId === imageId);
        if (!img?.dataUrl) continue;

        // Extract base64 data from data URL (data:image/png;base64,...)
        const match = img.dataUrl.match(/^data:image\/([^;]+);base64,(.+)$/);
        if (!match) continue;

        const ext = match[1] === "jpeg" ? "jpg" : match[1];
        const base64Data = match[2];
        const fileName = img.fileName || `${imageId}.${ext}`;

        imagesFolder.file(fileName, base64Data, { base64: true });
        imageManifest.push({
          imageId: img.imageId,
          fileName,
          type: img.type,
        });
      }
    }

    // Build layout JSON for export (includes image manifest for reimport).
    // When useDefaultControls is true the controls array is redundant — strip it
    // to keep the exported file clean.
    const exportLayout = this.layout.useDefaultControls
      ? { ...this.layout, controls: [] }
      : this.layout;

    const exportData = {
      formatVersion: 1,
      createdAt: new Date().toISOString(),
      layout: exportLayout,
      images: imageManifest,
    };

    zip.file("layout.json", JSON.stringify(exportData, null, 2));

    const blob = await zip.generateAsync({ type: "blob" });
    const buffer = await blob.arrayBuffer();
    const data = arrayBufferToBase64(buffer);

    const safeName = this.effectType.replace(/[^a-zA-Z0-9_-]/g, "_");
    const blendSuffix = this.blendId ? `--${this.blendId.replace(/[^a-zA-Z0-9_-]/g, "_")}` : "";
    postMessage({
      type: "exportEffectLayout",
      fileName: `${safeName}${blendSuffix}.sgfxlayout.zip`,
      data,
    });
  }

  private async handleImportFileSelected(): Promise<void> {
    const file = this.importFileInput?.files?.[0];
    if (!file) return;

    // Reset file input so the same file can be re-selected
    if (this.importFileInput) this.importFileInput.value = "";

    const zipLib = window.JSZip;
    if (!zipLib) {
      showNotification("Import failed: archive library not available", "error");
      return;
    }

    try {
      const buffer = await file.arrayBuffer();
      const zip = await zipLib.loadAsync(buffer);

      const layoutEntry = zip.file("layout.json");
      if (!layoutEntry) {
        showNotification("Import failed: archive is missing layout.json", "error");
        return;
      }

      const layoutText = await layoutEntry.async("text");
      const archive = JSON.parse(layoutText) as {
        formatVersion?: number;
        layout?: EffectLayout;
        images?: Array<{ imageId: string; fileName: string; type?: string }>;
      };

      if (!archive.layout) {
        showNotification("Import failed: archive has no layout data", "error");
        return;
      }

      // Extract images from zip and build data URLs
      const imageManifest = archive.images ?? [];
      const importedImages: Array<{ imageId: string; fileName: string; dataUrl: string; rawBase64: string; type?: string }> = [];

      for (const imgRef of imageManifest) {
        const imgEntry = zip.file(`images/${imgRef.fileName}`);
        if (!imgEntry) continue;

        const imgBuffer = await imgEntry.async("arraybuffer");
        const imgBase64 = arrayBufferToBase64(imgBuffer);

        // Determine MIME type from extension
        const ext = imgRef.fileName.split(".").pop()?.toLowerCase() ?? "png";
        const mimeMap: Record<string, string> = {
          png: "image/png",
          jpg: "image/jpeg",
          jpeg: "image/jpeg",
          gif: "image/gif",
          webp: "image/webp",
          svg: "image/svg+xml",
        };
        const mime = mimeMap[ext] ?? "image/png";
        const dataUrl = `data:${mime};base64,${imgBase64}`;

        importedImages.push({
          imageId: imgRef.imageId,
          fileName: imgRef.fileName,
          dataUrl,
          rawBase64: imgBase64,
          type: imgRef.type,
        });
      }

      // Register images in the layout library
      if (importedImages.length > 0) {
        if (!uiState.layoutLibrary) {
          uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
        }
        for (const img of importedImages) {
          // Replace existing or add new
          const existingIdx = uiState.layoutLibrary.images.findIndex((i) => i.imageId === img.imageId);
          const imageRef = {
            imageId: img.imageId,
            fileName: img.fileName,
            dataUrl: img.dataUrl,
            type: img.type as "background" | "knob" | "general" | undefined,
          };
          if (existingIdx >= 0) {
            uiState.layoutLibrary.images[existingIdx] = imageRef;
          } else {
            uiState.layoutLibrary.images.push(imageRef);
          }

          // Send image to C++ for persistent storage
          postMessage({
            type: "saveLayoutImage",
            imageId: img.imageId,
            fileName: img.fileName,
            data: img.rawBase64,
            layoutId: archive.layout.layoutId ?? "",
          });
        }
      }

      // Load the imported layout into the designer
      this.pushUndoState();
      this.layout = sanitizeLayout(archive.layout);
      // Update effect type if it matches or override with current
      if (this.effectType && archive.layout.effectType !== this.effectType) {
        this.layout.effectType = this.effectType;
      }
      this.layout.modifiedAt = new Date().toISOString();

      this.updateDimensionInputs();
      this.renderCanvas();
      this.selectElement(null);
      showNotification("Layout imported successfully", "success");
    } catch (err) {
      console.error("[LayoutDesigner] Import failed:", err);
      showNotification(`Import failed: ${err instanceof Error ? err.message : "unknown error"}`, "error");
    }
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

    this.pushUndoState();
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
      return;
    }

    this.renderCanvas();
  }

  // === Undo / Redo ===

  /** Snapshot the current layout state onto the undo stack. Call before any mutation. */
  private pushUndoState(): void {
    if (!this.layout) return;
    this.undoStack.push(JSON.stringify(this.layout));
    if (this.undoStack.length > LayoutDesignerModal.MAX_UNDO) {
      this.undoStack.shift();
    }
    this.redoStack = [];
    this.updateUndoRedoButtons();
  }

  private undo(): void {
    if (!this.undoStack.length || !this.layout) return;
    this.redoStack.push(JSON.stringify(this.layout));
    this.layout = JSON.parse(this.undoStack.pop()!) as EffectLayout;
    this.selectedElement = null;
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    this.updateUndoRedoButtons();
  }

  private redo(): void {
    if (!this.redoStack.length || !this.layout) return;
    this.undoStack.push(JSON.stringify(this.layout));
    this.layout = JSON.parse(this.redoStack.pop()!) as EffectLayout;
    this.selectedElement = null;
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    this.updateUndoRedoButtons();
  }

  private updateUndoRedoButtons(): void {
    if (this.undoBtn) {
      this.undoBtn.disabled = this.undoStack.length === 0;
      this.undoBtn.title = this.undoStack.length ? `Undo (Ctrl+Z) — ${this.undoStack.length} step${this.undoStack.length > 1 ? "s" : ""}` : "Undo (Ctrl+Z)";
    }
    if (this.redoBtn) {
      this.redoBtn.disabled = this.redoStack.length === 0;
      this.redoBtn.title = this.redoStack.length ? `Redo (Ctrl+Y) — ${this.redoStack.length} step${this.redoStack.length > 1 ? "s" : ""}` : "Redo (Ctrl+Y)";
    }
  }

  /** Push undo state once per sidebar editing session (first property change after selection). */
  private pushSidebarUndoOnce(): void {
    if (!this.sidebarUndoPushed) {
      this.sidebarUndoPushed = true;
      this.pushUndoState();
    }
  }

  // === Zoom ===

  private setZoom(level: number): void {
    this.zoom = Math.max(0.25, Math.min(3, level));
    this.updateZoomUI();
    this.updateCanvasSize();
  }

  private updateZoomUI(): void {
    if (this.zoomLabel) {
      this.zoomLabel.textContent = `${Math.round(this.zoom * 100)}%`;
    }
    if (this.zoomInBtn) this.zoomInBtn.disabled = this.zoom >= 3;
    if (this.zoomOutBtn) this.zoomOutBtn.disabled = this.zoom <= 0.25;
  }

  private updateDimensions(): void {
    if (!this.layout || !this.widthInput || !this.heightInput) return;

    this.pushUndoState();
    const width = snapToGrid(Math.max(
      LAYOUT_DIMENSION_LIMITS.minWidth,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxWidth, parseInt(this.widthInput.value) || DEFAULT_LAYOUT_DIMENSIONS.width)
    ));
    const height = snapToGrid(Math.max(
      LAYOUT_DIMENSION_LIMITS.minHeight,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxHeight, parseInt(this.heightInput.value) || DEFAULT_LAYOUT_DIMENSIONS.height)
    ));

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
    // Canvas element stays at design dimensions; zoom is applied via CSS transform
    this.canvas.style.width = `${this.layout.dimensions.width}px`;
    this.canvas.style.height = `${this.layout.dimensions.height}px`;
    this.canvas.style.transform = `scale(${this.zoom})`;
    this.canvas.style.transformOrigin = "top left";
    // Wrapper takes the zoomed dimensions so parent container scrolls correctly
    if (this.canvasWrapper) {
      this.canvasWrapper.style.width = `${Math.round(this.layout.dimensions.width * this.zoom)}px`;
      this.canvasWrapper.style.height = `${Math.round(this.layout.dimensions.height * this.zoom)}px`;
    }
  }

  private renderCanvas(): void {
    if (!this.canvas || !this.controlsLayer || !this.layout) return;

    this.updateCanvasSize();

    // Mirror the runtime container theme so preview colours match the live experience
    this.canvas.classList.remove('theme-light', 'theme-dark', 'theme-classic');
    if (this.layout.containerTheme) {
      this.canvas.classList.add(`theme-${this.layout.containerTheme}`);
    }

    this.renderBackgrounds();
    this.renderRectangleOverlays();

    if (this.previewMode) {
      this.renderRuntimePreview();
      return;
    }

    this.renderControls();
    this.renderTextLabels();
  }


  private renderRuntimePreview(): void {
    if (!this.controlsLayer || !this.layout) return;

    if (this.layout.useDefaultControls) {
      this.renderDefaultControlsPreview();
      return;
    }

    const previewNode = {
      id: "layout-designer-preview",
      type: this.effectType,
      params: Object.fromEntries(this.paramDefs.map((paramDef) => [paramDef.key, paramDef.default ?? 0])),
    } as unknown as GraphNode;

    const resourceControls: LayoutResourceControlDef[] = this.resourceCandidates.map((candidate) => ({
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

    this.controlsLayer.innerHTML = `
      <div class="layout-runtime-preview" style="position: absolute; inset: 0; pointer-events: none;">
        ${renderCustomLayoutPreviewLayers(previewNode, this.layout, this.paramDefs, resourceControls)}
      </div>
    `;
  }

  /**
   * Render the real default controls preview inside the designer canvas for useDefaultControls layouts.
   * Applies the configured offset and scale so the user can visually dial them in.
   * Mirrors the Main / Advanced tab split used by the runtime signal-path panel.
   */
  private renderDefaultControlsPreview(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Mirror the main/advanced split from renderNodeParamsPanel
    let advancedDefs = this.paramDefs.filter((p) => Boolean(p.advanced));
    let mainDefs = this.paramDefs.filter((p) => !p.advanced);
    if (mainDefs.length === 0) {
      mainDefs = this.paramDefs;
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

    const offsetX = this.layout.defaultControlsOffset?.x ?? 0;
    const offsetY = this.layout.defaultControlsOffset?.y ?? 0;
    const scaleX = this.layout.defaultControlsScale?.x ?? 1;
    const scaleY = this.layout.defaultControlsScale?.y ?? 1;
    const hasTransformOrOffset = offsetX !== 0 || offsetY !== 0 || scaleX !== 1 || scaleY !== 1;
    // Pin wrapper to canvas width so flex-wrap break points are identical in the runtime panel.
    const wrapperWidth = `width: ${this.layout.dimensions.width}px;`;
    const wrapperStyle = hasTransformOrOffset
      ? `position: absolute; left: ${offsetX}px; top: ${offsetY}px; transform: scale(${scaleX}, ${scaleY}); transform-origin: top left; ${wrapperWidth}`
      : `position: relative; ${wrapperWidth}`;

    this.controlsLayer.innerHTML = `
      <div class="layout-default-controls-preview" style="${wrapperStyle} pointer-events: none;">
        ${innerHtml}
      </div>
    `;
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
          } else if (bg.size === "stretch") {
            layer.style.backgroundSize = "100% 100%";
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

  private renderRectangleOverlays(): void {
    if (!this.canvas || !this.layout) return;

    this.canvas.querySelectorAll(".layout-rectangle-overlay").forEach((el) => el.remove());

    const overlays = this.layout.overlays ?? [];
    overlays.forEach((overlay) => {
      const el = this.createRectangleOverlayElement(overlay);
      if (this.canvas) {
        this.canvas.appendChild(el);
      }
    });
  }

  private createRectangleOverlayElement(overlay: LayoutRectangleOverlay): HTMLElement {
    const isSelected = this.selectedElement?.type === "overlay" && this.selectedElement.id === overlay.id;
    const backgroundColor = overlay.style?.backgroundColor || "#000000";
    const backgroundOpacity = typeof overlay.style?.backgroundOpacity === "number"
      ? Math.max(0, Math.min(1, overlay.style.backgroundOpacity))
      : 0.25;
    const borderColor = overlay.style?.borderColor || "#ffffff";
    const borderWidth = Math.max(0, overlay.style?.borderWidth ?? 1);
    const borderRadius = Math.max(0, overlay.style?.borderRadius ?? 0);
    const toggleBypassOnClick = overlay.style?.toggleBypassOnClick === true;
    const fill = this.colorWithAlpha(backgroundColor, backgroundOpacity);

    const el = document.createElement("div");
    el.className = `layout-rectangle-overlay${isSelected ? " selected" : ""}`;
    el.dataset.overlayId = overlay.id;
    el.style.left = `${overlay.position.x}px`;
    el.style.top = `${overlay.position.y}px`;
    el.style.width = `${overlay.size.width}px`;
    el.style.height = `${overlay.size.height}px`;
    el.style.backgroundColor = fill;
    el.style.border = `${borderWidth}px solid ${borderColor}`;
    el.style.borderRadius = `${borderRadius}px`;

    if (toggleBypassOnClick && !this.previewMode) {
      const badgeEl = document.createElement("span");
      badgeEl.className = "layout-overlay-power-badge";
      badgeEl.textContent = "PWR";
      el.appendChild(badgeEl);
    }

    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "overlay", id: overlay.id });
      }
    });

    if (isSelected && !this.previewMode) {
      (["top-left", "top-right", "bottom-left", "bottom-right"] as const).forEach((handle) => {
        const handleEl = document.createElement("div");
        handleEl.className = `layout-resize-handle ${handle}`;
        handleEl.dataset.overlayHandle = handle;
        el.appendChild(handleEl);
      });
    }

    return el;
  }

  private colorWithAlpha(hexColor: string, alpha: number): string {
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

  private getReusableLayoutImages(purpose: "background" | "knob"): LayoutImageRef[] {
    const images = uiState.layoutLibrary?.images ?? [];
    const available = images.filter((img) => Boolean(img.imageId) && (Boolean(img.dataUrl) || Boolean(img.fileName)));
    const preferredType = purpose === "background" ? "background" : "knob";
    const matching = available.filter((img) => img.type === preferredType);
    return [...matching].sort((a, b) => {
      const aName = (a.fileName || a.imageId).toLowerCase();
      const bName = (b.fileName || b.imageId).toLowerCase();
      return aName.localeCompare(bName);
    });
  }

  private renderImageOptionsHtml(purpose: "background" | "knob", selectedImageId?: string): string {
    const images = this.getReusableLayoutImages(purpose);
    const options = [
      `<option value="">Select existing image...</option>`,
      ...images.map((img) => {
        const selected = selectedImageId && img.imageId === selectedImageId ? "selected" : "";
        const typeLabel = img.type ? ` [${img.type}]` : "";
        const name = img.fileName || img.imageId;
        return `<option value="${img.imageId}" ${selected}>${name}${typeLabel}</option>`;
      }),
    ];
    return options.join("");
  }

  private renderControls(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Clear existing controls
    this.controlsLayer.querySelectorAll(".layout-control-placeholder, .layout-runtime-preview, .layout-default-controls-placeholder, .layout-default-controls-preview").forEach((el) => el.remove());

    // When useDefaultControls is on, render a live preview of the real controls at the configured position/scale
    if (this.layout.useDefaultControls) {
      this.renderDefaultControlsPreview();
      return;
    }

    // Render each control
    this.layout.controls.forEach((control) => {
      const el = this.createControlElement(control);
      this.controlsLayer!.appendChild(el);
    });
  }

  private toggleUseDefaultControls(): void {
    if (!this.layout || !this.useDefaultControlsCheckbox) return;
    this.pushUndoState();
    this.layout.useDefaultControls = this.useDefaultControlsCheckbox.checked;
    // Dim the "Add Control" button to signal it has no effect in this mode
    if (this.addControlBtn) {
      this.addControlBtn.style.opacity = this.layout.useDefaultControls ? "0.4" : "";
      this.addControlBtn.title = this.layout.useDefaultControls
        ? "Add Control (disabled — Default Controls mode)"
        : "Add Parameter Control";
    }
    this.selectedElement = null;
    this.renderCanvas();
    this.renderSidebar();
  }

  private createControlElement(control: LayoutControl): HTMLElement {
    const isResourceControl = this.isResourceControl(control);
    const paramDef = isResourceControl ? undefined : this.paramDefs.find((p) => p.key === control.paramKey);
    const resourceDef = isResourceControl ? this.resourceCandidates.find((candidate) => candidate.controlKey === control.paramKey) : undefined;
    const label = control.labelOverride || resourceDef?.displayName || paramDef?.name || control.paramKey;
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
    const hideLabel = control.style?.hideLabel === true || labelPos === "none";
    const showValue = control.style?.showValue !== false;

    // Build control HTML
    let html = "";

    if (!hideLabel && labelPos === "top") {
      html += `<span class="control-label">${label}</span>`;
    }

    if (isResourceControl || control.type === "dropdown") {
      html += `<div class="control-dropdown">${label}</div>`;
    } else if (control.type === "toggle") {
      html += `<div class="control-toggle"></div>`;
    } else if (control.type === "slider") {
      html += `<div class="control-slider"><div class="control-slider-track"><div class="control-slider-thumb"></div></div></div>`;
    } else {
      html += `<div class="control-knob" data-style="${control.style?.knobStyle || "default"}"></div>`;
    }

    if (!hideLabel && labelPos === "bottom") {
      html += `<span class="control-label position-bottom">${label}</span>`;
    }

    if (!isResourceControl && showValue) {
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
    this.controlsLayer.querySelectorAll(".layout-text-label, .layout-runtime-preview").forEach((el) => el.remove());

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
    this.sidebarUndoPushed = false;

    if (!this.selectedElement) {
      this.renderCanvasProperties();
      return;
    }

    if (this.selectedElement.type === "control") {
      this.renderControlProperties(this.selectedElement.paramKey);
    } else if (this.selectedElement.type === "label") {
      this.renderLabelProperties(this.selectedElement.id);
    } else if (this.selectedElement.type === "overlay") {
      this.renderOverlayProperties(this.selectedElement.id);
    } else if (this.selectedElement.type === "background") {
      this.renderBackgroundProperties(this.selectedElement.layerIndex);
    }
  }

  private renderCanvasProperties(): void {
    if (!this.sidebarContent || !this.layout) return;

    const containerTheme = this.layout.containerTheme ?? '';
    const isBackdrop = this.layout.useDefaultControls === true;
    const offsetX = this.layout.defaultControlsOffset?.x ?? 0;
    const offsetY = this.layout.defaultControlsOffset?.y ?? 0;
    const scaleX = this.layout.defaultControlsScale?.x ?? 1;
    const scaleY = this.layout.defaultControlsScale?.y ?? 1;

    const backdropSections = isBackdrop ? `
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
          <button id="prop-dc-scale-reset" style="font-size: 11px;">Reset to 1:1</button>
        </div>
      </div>
    ` : `
      <div class="layout-designer-sidebar-empty" style="font-size:11px; padding: 6px 0 0;">
        Select a control, label, background, or rectangle to edit its properties.
      </div>
    `;

    this.sidebarContent.innerHTML = `
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
      ${backdropSections}
    `;

    // Container theme selector
    const themeSelect = document.getElementById("prop-container-theme") as HTMLSelectElement | null;
    themeSelect?.addEventListener("change", () => {
      if (!this.layout) return;
      this.pushSidebarUndoOnce();
      const val = themeSelect.value as 'light' | 'dark' | 'classic' | '';
      this.layout.containerTheme = val === '' ? undefined : val;
      this.renderCanvas();
    });

    if (!isBackdrop) return;

    // Backdrop offset/scale bindings
    const bindNum = (id: string, apply: (v: number) => void) => {
      const el = document.getElementById(id) as HTMLInputElement | null;
      el?.addEventListener("change", () => {
        const v = parseFloat(el.value);
        if (!isNaN(v)) {
          this.pushSidebarUndoOnce();
          apply(v);
          this.renderCanvas();
        }
      });
    };

    bindNum("prop-dc-offset-x", (v) => {
      if (!this.layout) return;
      this.layout.defaultControlsOffset = { x: Math.round(v), y: this.layout.defaultControlsOffset?.y ?? 0 };
    });
    bindNum("prop-dc-offset-y", (v) => {
      if (!this.layout) return;
      this.layout.defaultControlsOffset = { x: this.layout.defaultControlsOffset?.x ?? 0, y: Math.round(v) };
    });
    bindNum("prop-dc-scale-x", (v) => {
      if (!this.layout) return;
      this.layout.defaultControlsScale = { x: Math.max(0.1, v), y: this.layout.defaultControlsScale?.y ?? 1 };
    });
    bindNum("prop-dc-scale-y", (v) => {
      if (!this.layout) return;
      this.layout.defaultControlsScale = { x: this.layout.defaultControlsScale?.x ?? 1, y: Math.max(0.1, v) };
    });

    document.getElementById("prop-dc-scale-reset")?.addEventListener("click", () => {
      if (!this.layout) return;
      this.pushUndoState();
      this.layout.defaultControlsScale = { x: 1, y: 1 };
      this.renderCanvas();
      this.renderSidebar();
    });
  }

  private renderBackgroundProperties(layerIndex: number): void {
    if (!this.sidebarContent || !this.layout) return;

    const bg = this.layout.backgrounds.find((b) => b.layerIndex === layerIndex);
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
            <input type="color" id="prop-bg-color" value="${bg.value || "#1a1a2e"}">
          </div>
        </div>
      `;
    } else if (bg.type === "gradient") {
      valueEditor = `
        <div class="layout-property-row">
          <span class="layout-property-label">Gradient</span>
          <div class="layout-property-input">
            <input type="text" id="prop-bg-gradient" value="${bg.value}" placeholder="linear-gradient(...)">
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

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Background Layer ${layerIndex + 1}</div>
        ${typeSelector}
        ${valueEditor}
      </div>

      ${isImage ? `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Image Source</div>
        <div class="layout-image-preview">
          ${bg.value ? `<img src="${this.getImageUrl(bg.value) || ""}" alt="Background">` : `<span class="layout-image-preview-placeholder">No image</span>`}
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="prop-bg-image-select">
              ${this.renderImageOptionsHtml("background", bg.value)}
            </select>
          </div>
        </div>
        <div class="layout-image-actions">
          <button id="prop-browse-bg-image">Browse...</button>
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
      ` : ""}

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
    // Push undo once on first input/change in this sidebar session
    this.sidebarContent?.addEventListener("input", () => this.pushSidebarUndoOnce(), { once: true, capture: true });
    this.sidebarContent?.addEventListener("change", () => this.pushSidebarUndoOnce(), { once: true, capture: true });

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
      this.renderCanvas();
      this.renderSidebar();
    });

    colorInput?.addEventListener("input", () => {
      bg.value = colorInput.value;
      this.renderCanvas();
    });

    gradientInput?.addEventListener("change", () => {
      bg.value = gradientInput.value;
      this.renderCanvas();
    });

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

    bgImageSelect?.addEventListener("change", () => {
      const imageId = bgImageSelect.value;
      if (!imageId) return;
      bg.type = "image";
      bg.value = imageId;
      this.renderCanvas();
      this.renderSidebar();
    });

    browseBgBtn?.addEventListener("click", () => {
      this.browseBackgroundImage(bg.layerIndex);
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

    const isResourceControl = this.isResourceControl(control);
    const paramDef = isResourceControl ? undefined : this.paramDefs.find((p) => p.key === paramKey);
    const resourceDef = isResourceControl
      ? this.resourceCandidates.find((candidate) => candidate.controlKey === paramKey)
      : undefined;

    const bindingLabel = isResourceControl
      ? `${resourceDef?.displayName || paramKey} (${resourceDef?.resourceType || "resource"})`
      : (paramDef?.name || paramKey);

    const typeOptions = isResourceControl
      ? `<option value="dropdown" selected>Dropdown</option>`
      : `
              <option value="knob" ${control.type === "knob" ? "selected" : ""}>Knob</option>
              <option value="toggle" ${control.type === "toggle" ? "selected" : ""}>Toggle</option>
              <option value="slider" ${control.type === "slider" ? "selected" : ""}>Slider</option>
            `;

    const styleSection = isResourceControl
      ? ""
      : `
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
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="prop-knob-image-select">
              ${this.renderImageOptionsHtml("knob", control.style?.knobImageId)}
            </select>
          </div>
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
      `;

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Control</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Binding</span>
          <span class="layout-property-input">${bindingLabel}</span>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Type</span>
          <div class="layout-property-input">
            <select id="prop-control-type">
              ${typeOptions}
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
            <input type="text" id="prop-label-override" value="${control.labelOverride || ""}" placeholder="${bindingLabel}">
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

      ${styleSection}

      <div class="layout-property-group">
        <button id="prop-delete-control" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Remove from Layout</button>
      </div>
    `;

    // Bind property change handlers
    this.bindControlPropertyHandlers(control);
  }

  private bindControlPropertyHandlers(control: LayoutControl): void {
    this.sidebarContent?.addEventListener("input", () => this.pushSidebarUndoOnce(), { once: true, capture: true });
    this.sidebarContent?.addEventListener("change", () => this.pushSidebarUndoOnce(), { once: true, capture: true });

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
    const knobImageSelect = document.getElementById("prop-knob-image-select") as HTMLSelectElement;
    const isResourceControl = this.isResourceControl(control);

    typeSelect?.addEventListener("change", () => {
      control.type = typeSelect.value as "knob" | "toggle" | "slider" | "dropdown";
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

    knobImageSelect?.addEventListener("change", () => {
      const imageId = knobImageSelect.value;
      if (!imageId) return;
      if (!control.style) control.style = {};
      control.style.knobStyle = "custom";
      control.style.knobImageId = imageId;
      this.renderCanvas();
      this.renderSidebar();
    });

    if (isResourceControl) {
      control.type = "dropdown";
    }
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
    this.sidebarContent?.addEventListener("input", () => this.pushSidebarUndoOnce(), { once: true, capture: true });
    this.sidebarContent?.addEventListener("change", () => this.pushSidebarUndoOnce(), { once: true, capture: true });

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

    this.pushUndoState();
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

  private addRectangleOverlay(): void {
    if (!this.layout) return;

    this.pushUndoState();
    const newOverlay: LayoutRectangleOverlay = {
      id: generateOverlayId(),
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 80),
        y: snapToGrid(this.layout.dimensions.height / 2 - 50),
      },
      size: {
        width: 160,
        height: 100,
      },
      style: {
        visibilityMode: "always",
        toggleBypassOnClick: false,
        backgroundColor: "#000000",
        backgroundOpacity: 0.25,
        borderColor: "#ffffff",
        borderWidth: 1,
        borderRadius: 0,
      },
    };

    if (!this.layout.overlays) {
      this.layout.overlays = [];
    }
    this.layout.overlays.push(newOverlay);
    this.selectElement({ type: "overlay", id: newOverlay.id });
    this.renderCanvas();
  }

  private renderOverlayProperties(overlayId: string): void {
    if (!this.sidebarContent || !this.layout) return;

    const overlay = (this.layout.overlays ?? []).find((item) => item.id === overlayId);
    if (!overlay) return;

    const style = overlay.style ?? {};
    const visibilityMode = style.visibilityMode ?? "always";
    const toggleBypassOnClick = style.toggleBypassOnClick === true;
    const backgroundColor = style.backgroundColor || "#000000";
    const backgroundOpacity = Math.round(((typeof style.backgroundOpacity === "number" ? style.backgroundOpacity : 0.25) * 100));
    const borderColor = style.borderColor || "#ffffff";
    const borderWidth = style.borderWidth ?? 1;
    const borderRadius = style.borderRadius ?? 0;

    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Rectangle Overlay</div>
        ${toggleBypassOnClick ? `<div class="layout-overlay-sidebar-badge">Power Indicator</div>` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Visible</span>
          <div class="layout-property-input">
            <select id="prop-overlay-visibility-mode">
              <option value="always" ${visibilityMode === "always" ? "selected" : ""}>Always</option>
              <option value="enabled" ${visibilityMode === "enabled" ? "selected" : ""}>When Enabled</option>
              <option value="bypassed" ${visibilityMode === "bypassed" ? "selected" : ""}>When Bypassed</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">On Click</span>
          <div class="layout-property-input">
            <label style="display:flex;align-items:center;gap:6px;font-size:11px;color:var(--text-dark-secondary);">
              <input type="checkbox" id="prop-overlay-toggle-bypass" ${toggleBypassOnClick ? "checked" : ""}>
              Toggle Bypass
            </label>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-overlay-x" value="${overlay.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-overlay-y" value="${overlay.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Size</div>
        <div class="layout-position-inputs">
          <label>W <input type="number" id="prop-overlay-width" value="${overlay.size.width}" min="16" step="8"></label>
          <label>H <input type="number" id="prop-overlay-height" value="${overlay.size.height}" min="16" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Appearance</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Fill</span>
          <div class="layout-property-input">
            <input type="color" id="prop-overlay-bg-color" value="${backgroundColor}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Fill Opacity</span>
          <div class="layout-property-input">
            <input type="range" id="prop-overlay-bg-opacity" min="0" max="100" value="${backgroundOpacity}" style="width: 80px;">
            <span id="prop-overlay-bg-opacity-value">${backgroundOpacity}%</span>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Border</span>
          <div class="layout-property-input">
            <input type="color" id="prop-overlay-border-color" value="${borderColor}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Border W</span>
          <div class="layout-property-input">
            <input type="number" id="prop-overlay-border-width" value="${borderWidth}" min="0" max="24" step="1">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Radius</span>
          <div class="layout-property-input">
            <input type="number" id="prop-overlay-border-radius" value="${borderRadius}" min="0" max="64" step="1">
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-overlay" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Delete Rectangle</button>
      </div>
    `;

    this.bindOverlayPropertyHandlers(overlay);
  }

  private bindOverlayPropertyHandlers(overlay: LayoutRectangleOverlay): void {
    this.sidebarContent?.addEventListener("input", () => this.pushSidebarUndoOnce(), { once: true, capture: true });
    this.sidebarContent?.addEventListener("change", () => this.pushSidebarUndoOnce(), { once: true, capture: true });

    const xInput = document.getElementById("prop-overlay-x") as HTMLInputElement;
    const yInput = document.getElementById("prop-overlay-y") as HTMLInputElement;
    const visibilityModeSelect = document.getElementById("prop-overlay-visibility-mode") as HTMLSelectElement;
    const toggleBypassCheck = document.getElementById("prop-overlay-toggle-bypass") as HTMLInputElement;
    const widthInput = document.getElementById("prop-overlay-width") as HTMLInputElement;
    const heightInput = document.getElementById("prop-overlay-height") as HTMLInputElement;
    const bgColorInput = document.getElementById("prop-overlay-bg-color") as HTMLInputElement;
    const bgOpacityInput = document.getElementById("prop-overlay-bg-opacity") as HTMLInputElement;
    const bgOpacityValue = document.getElementById("prop-overlay-bg-opacity-value") as HTMLElement;
    const borderColorInput = document.getElementById("prop-overlay-border-color") as HTMLInputElement;
    const borderWidthInput = document.getElementById("prop-overlay-border-width") as HTMLInputElement;
    const borderRadiusInput = document.getElementById("prop-overlay-border-radius") as HTMLInputElement;
    const deleteBtn = document.getElementById("prop-delete-overlay") as HTMLButtonElement;

    xInput?.addEventListener("change", () => {
      overlay.position.x = snapToGrid(parseInt(xInput.value) || 0);
      xInput.value = String(overlay.position.x);
      this.renderCanvas();
    });

    visibilityModeSelect?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.visibilityMode = visibilityModeSelect.value as "always" | "enabled" | "bypassed";
      this.renderCanvas();
    });

    toggleBypassCheck?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.toggleBypassOnClick = toggleBypassCheck.checked;
      this.renderCanvas();
    });

    yInput?.addEventListener("change", () => {
      overlay.position.y = snapToGrid(parseInt(yInput.value) || 0);
      yInput.value = String(overlay.position.y);
      this.renderCanvas();
    });

    widthInput?.addEventListener("change", () => {
      overlay.size.width = Math.max(16, snapToGrid(parseInt(widthInput.value) || 16));
      widthInput.value = String(overlay.size.width);
      this.renderCanvas();
    });

    heightInput?.addEventListener("change", () => {
      overlay.size.height = Math.max(16, snapToGrid(parseInt(heightInput.value) || 16));
      heightInput.value = String(overlay.size.height);
      this.renderCanvas();
    });

    bgColorInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.backgroundColor = bgColorInput.value;
      this.renderCanvas();
    });

    bgOpacityInput?.addEventListener("input", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.backgroundOpacity = (parseInt(bgOpacityInput.value) || 0) / 100;
      if (bgOpacityValue) bgOpacityValue.textContent = `${bgOpacityInput.value}%`;
      this.renderCanvas();
    });

    borderColorInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderColor = borderColorInput.value;
      this.renderCanvas();
    });

    borderWidthInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderWidth = Math.max(0, parseInt(borderWidthInput.value) || 0);
      this.renderCanvas();
    });

    borderRadiusInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderRadius = Math.max(0, parseInt(borderRadiusInput.value) || 0);
      this.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!this.layout?.overlays) return;
      this.layout.overlays = this.layout.overlays.filter((item) => item.id !== overlay.id);
      this.selectElement(null);
      this.renderCanvas();
    });
  }

  private showAddBackgroundMenu(): void {
    if (!this.layout || !this.sidebarContent) return;

    if (this.layout.backgrounds.length >= 2) {
      showNotification("Maximum 2 background layers", "warning");
      return;
    }

    const targetLayerIndex = this.layout.backgrounds.length;
    const images = this.getReusableLayoutImages("background");

    this.selectedElement = null;
    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Add Background Layer ${targetLayerIndex + 1}</div>
        <div style="font-size: 11px; color: var(--text-dark-muted); margin-bottom: 8px;">Choose an existing image resource or browse for a new file</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="layout-add-bg-image-select">
              ${this.renderImageOptionsHtml("background")}
            </select>
          </div>
        </div>
        ${images.length > 0 ? `
        <div style="font-size: 10px; color: var(--text-dark-muted); margin-top: 6px;">
          Found ${images.length} reusable image${images.length === 1 ? "" : "s"}
        </div>
        ` : ""}
        <div class="layout-image-actions" style="margin-top: 8px;">
          <button id="layout-add-bg-browse">Browse...</button>
        </div>
      </div>
    `;

    const existingSelect = document.getElementById("layout-add-bg-image-select") as HTMLSelectElement;
    const browseBtn = document.getElementById("layout-add-bg-browse") as HTMLButtonElement;

    existingSelect?.addEventListener("change", () => {
      const imageId = existingSelect.value;
      if (!imageId) return;
      this.applyImageBackground(targetLayerIndex, imageId);
    });

    browseBtn?.addEventListener("click", () => {
      this.browseBackgroundImage(targetLayerIndex);
    });
  }

  private applyImageBackground(layerIndex: number, imageId: string): void {
    if (!this.layout) return;

    this.pushUndoState();
    const existingIndex = this.layout.backgrounds.findIndex((bg) => bg.layerIndex === layerIndex);
    const newBg: LayoutBackground = {
      layerIndex,
      type: "image",
      value: imageId,
      opacity: 1,
      size: "contain",
    };

    if (existingIndex >= 0) {
      this.layout.backgrounds[existingIndex] = newBg;
    } else {
      this.layout.backgrounds.push(newBg);
    }

    this.selectElement({ type: "background", layerIndex });
    this.renderCanvas();
    this.renderSidebar();
  }

  private browseBackgroundImage(layerIndex?: number): void {
    console.log("[LayoutDesigner] browseBackgroundImage called");
    postMessage({
      type: "browseLayoutImage",
      purpose: "background",
      layerIndex: layerIndex ?? (this.layout?.backgrounds.length || 0),
      layoutId: this.layout?.layoutId ?? "",
    });
  }

  private addColorBackground(): void {
    if (!this.layout) return;

    if (this.layout.backgrounds.length >= 2) {
      showNotification("Maximum 2 background layers", "warning");
      return;
    }

    const layerIndex = this.layout.backgrounds.length;
    this.pushUndoState();
    const newBg: LayoutBackground = {
      layerIndex,
      type: "color",
      value: "#1a1a2e",
      opacity: 1,
    };

    this.layout.backgrounds.push(newBg);
    this.selectElement({ type: "background", layerIndex });
    this.renderCanvas();
    this.renderSidebar();
  }

  private showAddControlMenu(): void {
    if (!this.layout || !this.sidebarContent) return;

    // Find params not already in layout
    const usedKeys = new Set(this.layout.controls.map((c) => c.paramKey));
    const availableParams = this.paramDefs.filter((p) => !usedKeys.has(p.key));
    const availableResources = this.resourceCandidates.filter((resource) => !usedKeys.has(resource.controlKey));

    if (availableParams.length === 0 && availableResources.length === 0) {
      showNotification("All controls are already in the layout", "info");
      return;
    }

    // Show available params in sidebar
    this.selectedElement = null;
    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Add Parameter Control</div>
        <div style="font-size: 11px; color: var(--text-dark-muted); margin-bottom: 8px;">Click a parameter or resource selector to add it to the layout</div>
        ${availableParams
          .map(
            (p) => `
          <button
            class="layout-add-control-btn"
            data-param-key="${p.key}"
            style="
              display: block;
              width: 100%;
              padding: 6px 10px;
              margin-bottom: 4px;
              background: rgba(255, 255, 255, 0.05);
              border: 1px solid rgba(255, 255, 255, 0.1);
              border-radius: 4px;
              color: var(--text-dark-primary);
              font-size: 12px;
              cursor: pointer;
              text-align: left;
            "
          >
            ${p.name || p.key} <span style="opacity: 0.5; font-size: 10px;">${p.unit || ""}</span>
          </button>
        `
          )
          .join("")}
        ${availableResources.length > 0 ? `
          <div style="margin: 10px 0 6px; font-size: 11px; color: var(--text-dark-muted);">Resource Selectors</div>
          ${availableResources
            .map(
              (resource) => `
            <button
              class="layout-add-control-btn"
              data-resource-key="${resource.controlKey}"
              style="
                display: block;
                width: 100%;
                padding: 6px 10px;
                margin-bottom: 4px;
                background: rgba(255, 255, 255, 0.05);
                border: 1px solid rgba(255, 255, 255, 0.1);
                border-radius: 4px;
                color: var(--text-dark-primary);
                font-size: 12px;
                cursor: pointer;
                text-align: left;
              "
            >
              ${resource.displayName} <span style="opacity: 0.5; font-size: 10px;">${resource.resourceType}</span>
            </button>
          `,
            )
            .join("")}
        ` : ""}
      </div>
    `;

    // Bind click handlers
    this.sidebarContent.querySelectorAll(".layout-add-control-btn").forEach((btn) => {
      btn.addEventListener("click", () => {
        const paramKey = (btn as HTMLElement).dataset.paramKey;
        const resourceKey = (btn as HTMLElement).dataset.resourceKey;
        if (paramKey) {
          this.addControlForParam(paramKey);
        } else if (resourceKey) {
          this.addControlForResource(resourceKey);
        }
      });
    });
  }

  private addControlForParam(paramKey: string): void {
    if (!this.layout) return;

    const paramDef = this.paramDefs.find((p) => p.key === paramKey);
    if (!paramDef) return;

    this.pushUndoState();
    // Place in center of canvas
    const newControl: LayoutControl = {
      paramKey,
      bindingType: "parameter",
      type: paramDef.unit === "toggle" ? "toggle" : "knob",
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 24),
        y: snapToGrid(this.layout.dimensions.height / 2 - 24),
      },
      style: {
        labelPosition: "top",
        showValue: true,
        valuePosition: "bottom",
        knobStyle: "default",
      },
    };

    this.layout.controls.push(newControl);
    this.selectElement({ type: "control", paramKey });
    this.renderCanvas();
  }

  private addControlForResource(resourceKey: string): void {
    if (!this.layout) return;

    const resource = this.resourceCandidates.find((candidate) => candidate.controlKey === resourceKey);
    if (!resource) return;

    this.pushUndoState();

    const newControl: LayoutControl = {
      paramKey: resource.controlKey,
      bindingType: "resource",
      type: "dropdown",
      resourceType: resource.resourceType,
      resourceIndex: resource.resourceIndex,
      exposedResourceId: resource.exposedResourceId,
      allowBrowseFile: resource.allowBrowseFile,
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 90),
        y: snapToGrid(this.layout.dimensions.height / 2 - 18),
      },
      size: {
        width: 180,
        height: 36,
      },
      style: {
        labelPosition: "top",
        showValue: false,
        valuePosition: "bottom",
      },
      labelOverride: resource.displayName,
    };

    this.layout.controls.push(newControl);
    this.selectElement({ type: "control", paramKey: resource.controlKey });
    this.renderCanvas();
  }

  private isResourceControl(control: LayoutControl): boolean {
    return control.bindingType === "resource" || control.paramKey.startsWith("__resource__:");
  }

  private buildResourceCandidates(typeInfo?: { requiresResource?: boolean; resourceType?: string; exposedResources?: Array<{
    resourceId: string;
    displayName: string;
    resourceType: string;
    resourceIndex?: number;
    allowBrowseFile?: boolean;
  }> }): LayoutResourceCandidate[] {
    const candidates: LayoutResourceCandidate[] = [];

    const exposed = typeInfo?.exposedResources ?? [];
    if (exposed.length > 0) {
      exposed.forEach((resource, index) => {
        const resourceIndex = typeof resource.resourceIndex === "number" ? resource.resourceIndex : index;
        candidates.push({
          controlKey: `__resource__:${resource.resourceId}:${resourceIndex}`,
          displayName: resource.displayName || resource.resourceId,
          resourceType: resource.resourceType,
          resourceIndex,
          exposedResourceId: resource.resourceId,
          allowBrowseFile: resource.allowBrowseFile ?? true,
        });
      });
      return candidates;
    }

    if (typeInfo?.requiresResource && typeInfo.resourceType) {
      candidates.push({
        controlKey: `__resource__:primary:0`,
        displayName: typeInfo.resourceType === "nam" ? "Model" : typeInfo.resourceType === "ir" ? "IR" : "Resource",
        resourceType: typeInfo.resourceType,
        resourceIndex: 0,
        allowBrowseFile: true,
      });
    }

    return candidates;
  }

  private browseKnobImage(control: LayoutControl): void {
    postMessage({
      type: "browseLayoutImage",
      purpose: "knob",
      paramKey: control.paramKey,
      layoutId: this.layout?.layoutId ?? "",
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
      this.applyImageBackground(layerIndex, imageId);
    } else if (purpose === "knob" && paramKey) {
      this.pushUndoState();
      const control = this.layout.controls.find((c) => c.paramKey === paramKey);
      if (control) {
        if (!control.style) control.style = {};
        control.style.knobImageId = imageId;
      }

      this.renderCanvas();
      this.renderSidebar();
      return;
    }

    this.renderCanvas();
    this.renderSidebar();
  }

  // === Drag and Drop ===

  private onCanvasMouseDown(e: MouseEvent): void {
    if (this.previewMode) return;

    const target = e.target as HTMLElement;
    const resizeHandle = target.closest(".layout-resize-handle") as HTMLElement;
    const rectangle = target.closest(".layout-rectangle-overlay") as HTMLElement;
    const placeholder = target.closest(".layout-control-placeholder") as HTMLElement;
    const textLabel = target.closest(".layout-text-label") as HTMLElement;

    if (resizeHandle && rectangle) {
      const overlayId = rectangle.dataset.overlayId;
      const handle = resizeHandle.dataset.overlayHandle as "top-left" | "top-right" | "bottom-left" | "bottom-right" | undefined;
      if (overlayId && handle) {
        this.startDrag(e, rectangle, "overlay", overlayId, "resize", handle);
      }
      return;
    }

    if (rectangle) {
      const overlayId = rectangle.dataset.overlayId;
      if (overlayId) {
        this.startDrag(e, rectangle, "overlay", overlayId, "move");
      }
      return;
    }

    if (placeholder) {
      const paramKey = placeholder.dataset.paramKey;
      if (paramKey) {
        this.startDrag(e, placeholder, "control", paramKey, "move");
      }
    } else if (textLabel) {
      const labelId = textLabel.dataset.labelId;
      if (labelId) {
        this.startDrag(e, textLabel, "label", labelId, "move");
      }
    }
  }

  private startDrag(
    e: MouseEvent,
    element: HTMLElement,
    type: "control" | "label" | "overlay",
    id: string,
    mode: "move" | "resize",
    resizeHandle: "top-left" | "top-right" | "bottom-left" | "bottom-right" | null = null,
  ): void {
    e.preventDefault();

    this.pushUndoState();
    this.dragState = {
      active: true,
      element,
      startX: e.clientX,
      startY: e.clientY,
      elementStartX: parseInt(element.style.left) || 0,
      elementStartY: parseInt(element.style.top) || 0,
      elementStartWidth: parseInt(element.style.width) || 0,
      elementStartHeight: parseInt(element.style.height) || 0,
      type,
      mode,
      resizeHandle,
      id,
    };

    element.classList.add("dragging");
  }

  private onDocumentMouseMove(e: MouseEvent): void {
    if (!this.dragState.active || !this.dragState.element || !this.layout) return;

    const dx = e.clientX - this.dragState.startX;
    const dy = e.clientY - this.dragState.startY;

    if (this.dragState.type === "overlay" && this.dragState.mode === "resize") {
      const minSize = 16;
      let newX = this.dragState.elementStartX;
      let newY = this.dragState.elementStartY;
      let newWidth = this.dragState.elementStartWidth;
      let newHeight = this.dragState.elementStartHeight;

      switch (this.dragState.resizeHandle) {
        case "top-left":
          newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
          newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth - dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight - dy / this.zoom);
          break;
        case "top-right":
          newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth + dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight - dy / this.zoom);
          break;
        case "bottom-left":
          newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth - dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight + dy / this.zoom);
          break;
        case "bottom-right":
        default:
          newWidth = snapToGrid(this.dragState.elementStartWidth + dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight + dy / this.zoom);
          break;
      }

      if (newWidth < minSize) {
        if (this.dragState.resizeHandle === "top-left" || this.dragState.resizeHandle === "bottom-left") {
          newX += newWidth - minSize;
        }
        newWidth = minSize;
      }
      if (newHeight < minSize) {
        if (this.dragState.resizeHandle === "top-left" || this.dragState.resizeHandle === "top-right") {
          newY += newHeight - minSize;
        }
        newHeight = minSize;
      }

      newX = Math.max(0, Math.min(this.layout.dimensions.width - minSize, newX));
      newY = Math.max(0, Math.min(this.layout.dimensions.height - minSize, newY));
      newWidth = Math.min(newWidth, this.layout.dimensions.width - newX);
      newHeight = Math.min(newHeight, this.layout.dimensions.height - newY);

      this.dragState.element.style.left = `${newX}px`;
      this.dragState.element.style.top = `${newY}px`;
      this.dragState.element.style.width = `${Math.max(minSize, newWidth)}px`;
      this.dragState.element.style.height = `${Math.max(minSize, newHeight)}px`;
      return;
    }

    const newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
    const newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
    const dragWidth = this.dragState.type === "overlay"
      ? (parseInt(this.dragState.element.style.width) || this.dragState.elementStartWidth || 60)
      : 60;
    const dragHeight = this.dragState.type === "overlay"
      ? (parseInt(this.dragState.element.style.height) || this.dragState.elementStartHeight || 60)
      : 60;

    // Clamp to canvas bounds
    const clampedX = Math.max(0, Math.min(this.layout.dimensions.width - dragWidth, newX));
    const clampedY = Math.max(0, Math.min(this.layout.dimensions.height - dragHeight, newY));

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
      } else if (type === "overlay") {
        const overlay = (this.layout.overlays ?? []).find((item) => item.id === id);
        if (overlay) {
          overlay.position = { x: newX, y: newY };
          if (this.dragState.mode === "resize") {
            overlay.size = {
              width: Math.max(16, parseInt(element.style.width) || overlay.size.width),
              height: Math.max(16, parseInt(element.style.height) || overlay.size.height),
            };
          }
        }
      }

      // Update sidebar if selected
      if (
        (this.selectedElement?.type === "control" && this.selectedElement.paramKey === id) ||
        (this.selectedElement?.type === "label" && this.selectedElement.id === id) ||
        (this.selectedElement?.type === "overlay" && this.selectedElement.id === id)
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
      elementStartWidth: 0,
      elementStartHeight: 0,
      type: null,
      mode: "move",
      resizeHandle: null,
      id: "",
    };
  }

  // === Keyboard Shortcuts ===

  private onKeyDown(e: KeyboardEvent): void {
    if (!this.modal || this.modal.style.display === "none") return;
    if (this.isEditableKeyboardTarget(e.target)) return;

    // Undo/Redo: Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z
    if ((e.ctrlKey || e.metaKey) && !e.altKey) {
      if (e.key === "c" || e.key === "C") {
        if (this.copySelectedTextLabel()) {
          e.preventDefault();
        }
        return;
      }
      if (e.key === "v" || e.key === "V") {
        if (this.pasteCopiedTextLabel()) {
          e.preventDefault();
        }
        return;
      }
      if (e.key === "z" || e.key === "Z") {
        e.preventDefault();
        if (e.shiftKey) {
          this.redo();
        } else {
          this.undo();
        }
        return;
      }
      if (e.key === "y" || e.key === "Y") {
        e.preventDefault();
        this.redo();
        return;
      }
    }

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

    // Zoom: + / - / 0
    if (e.key === "+" || e.key === "=") {
      this.setZoom(this.zoom + 0.25);
    }
    if (e.key === "-" || e.key === "_") {
      this.setZoom(this.zoom - 0.25);
    }
    if (e.key === "0") {
      this.setZoom(1);
    }
  }

  private isEditableKeyboardTarget(target: EventTarget | null): boolean {
    if (!(target instanceof HTMLElement)) return false;
    if (target.isContentEditable) return true;

    const tagName = target.tagName;
    return tagName === "INPUT" || tagName === "TEXTAREA" || tagName === "SELECT";
  }

  private copySelectedTextLabel(): boolean {
    if (!this.layout || this.selectedElement?.type !== "label") {
      return false;
    }

    const selectedLabelId = this.selectedElement.id;
    const label = this.layout.textLabels.find((item) => item.id === selectedLabelId);
    if (!label) {
      return false;
    }

    this.copiedTextLabel = {
      text: label.text,
      position: { ...label.position },
      fontSize: label.fontSize,
      fontWeight: label.fontWeight,
      fontFamily: label.fontFamily,
      color: label.color,
      textAlign: label.textAlign,
    };
    return true;
  }

  private pasteCopiedTextLabel(): boolean {
    if (!this.layout || !this.copiedTextLabel) {
      return false;
    }

    const selectedLabel = this.selectedElement?.type === "label" ? this.selectedElement : null;
    const sourcePosition = selectedLabel
      ? this.layout.textLabels.find((item) => item.id === selectedLabel.id)?.position
      : undefined;
    const pasteOffset = LAYOUT_GRID_SIZE * 2;
    const basePosition = sourcePosition ?? this.copiedTextLabel.position;
    const nextX = Math.max(0, Math.min(
      this.layout.dimensions.width,
      snapToGrid(basePosition.x + pasteOffset),
    ));
    const nextY = Math.max(0, Math.min(
      this.layout.dimensions.height,
      snapToGrid(basePosition.y + pasteOffset),
    ));

    const newLabel: LayoutTextLabel = {
      id: generateLabelId(),
      text: this.copiedTextLabel.text,
      position: { x: nextX, y: nextY },
      fontSize: this.copiedTextLabel.fontSize,
      fontWeight: this.copiedTextLabel.fontWeight,
      fontFamily: this.copiedTextLabel.fontFamily,
      color: this.copiedTextLabel.color,
      textAlign: this.copiedTextLabel.textAlign,
    };

    this.pushUndoState();
    this.layout.textLabels.push(newLabel);
    this.selectElement({ type: "label", id: newLabel.id });
    this.renderCanvas();
    return true;
  }

  private deleteSelectedElement(): void {
    if (!this.layout || !this.selectedElement) return;

    this.pushUndoState();
    if (this.selectedElement.type === "control") {
      const selectedParamKey = this.selectedElement.paramKey;
      this.layout.controls = this.layout.controls.filter(
        (c) => c.paramKey !== selectedParamKey
      );
    } else if (this.selectedElement.type === "label") {
      this.layout.textLabels = this.layout.textLabels.filter(
        (l) => l.id !== (this.selectedElement as { type: "label"; id: string }).id
      );
    } else if (this.selectedElement.type === "overlay") {
      const selectedOverlayId = this.selectedElement.id;
      this.layout.overlays = (this.layout.overlays ?? []).filter(
        (item) => item.id !== selectedOverlayId
      );
    } else if (this.selectedElement.type === "background") {
      const selectedLayerIndex = this.selectedElement.layerIndex;
      this.layout.backgrounds = this.layout.backgrounds.filter(
        (b) => b.layerIndex !== selectedLayerIndex
      );
    }

    this.selectElement(null);
    this.renderCanvas();
  }

  private nudgeSelectedElement(key: string, amount: number): void {
    if (!this.layout || !this.selectedElement) return;

    // Debounced undo: push once at start of a nudge sequence, not on every arrow press
    if (!this.nudgeUndoTimer) {
      this.pushUndoState();
    } else {
      clearTimeout(this.nudgeUndoTimer);
    }
    this.nudgeUndoTimer = setTimeout(() => { this.nudgeUndoTimer = null; }, 500);

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
    } else if (this.selectedElement.type === "overlay") {
      const selectedOverlayId = this.selectedElement.id;
      const overlay = (this.layout.overlays ?? []).find((item) => item.id === selectedOverlayId);
      position = overlay?.position;
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
