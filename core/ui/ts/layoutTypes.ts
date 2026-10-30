/**
 * Effect Layout Designer Types
 *
 * Defines data structures for custom effect parameter layouts including
 * background images, control positioning, and text labels.
 */

import type { LayoutImageStrip, LayoutKnobImage } from "./layoutKnobImages.js";
import type { LayoutImageRendition } from "./layoutRenditions.js";
import type { LayoutScaling } from "./layoutScaling.js";

export type { LayoutImageRendition, LayoutImageStrip, LayoutKnobImage, LayoutScaling };

/** A toggle drawn from images: an off and an on picture, or a strip of frames, which wins. */
export interface LayoutToggleImage {
  onImageId?: string;
  offImageId?: string;
  strip?: LayoutImageStrip;
  /** The off picture's height over its width, so the toggle takes the picture's shape. */
  aspect?: number;
}

/** The switch every toggle in a layout is drawn from unless it has its own: images and a size. */
export interface LayoutSwitchImage extends LayoutToggleImage {
  /** The toggle's drawn size, px; unset: 36 wide and the picture's shape. */
  width?: number;
  height?: number;
}

/** The light of a layout's generated control shadows; every field is optional (CONTROL_SHADOW_DEFAULTS). */
export interface LayoutControlShadow {
  /** Where the light comes from, degrees clockwise from the top: 315 is top left. */
  angle?: number;
  /** How high the light is above the panel, 15 to 80 degrees: lower casts longer shadows. */
  elevation?: number;
  /** Blur, 0 (hard edged) to 1. */
  softness?: number;
  /** Darkness, 0 to 1. */
  opacity?: number;
}

/** Image reference for layout backgrounds and knobs */
export interface LayoutImageRef {
  /** Unique ID for the image in the layout library */
  imageId: string;
  /** Original filename for display */
  fileName?: string;
  /**
   * Base64 data URL. The engine no longer sends one; it is set only for an image imported in
   * this session (until the library reloads) and by older layouts' embedded data.
   */
  dataUrl?: string;
  /** Where the WebView loads the image from: `layout-images/id/<id>?v=<version>`. */
  url?: string;
  /** Pixel size the file declares, when the engine could read it. */
  width?: number;
  height?: number;
  /** File size in bytes. */
  bytes?: number;
  /** The file's version (its write time), which the URLs carry; a string, as it is past 2^53. */
  version?: string;
  /** Smaller copies of a large image, made for this version (layoutRenditions.ts). */
  renditions?: LayoutImageRendition[];
  /** The category the designer files it under (layoutImageCategories.ts); "general" is Misc. */
  type?: "background" | "knob" | "switch" | "general";
  /** Shipped with the app: it cannot be refiled under another category. */
  factory?: boolean;
  /** The licence it was shared under (layoutImageCredits.ts) and the credit it asks for. */
  license?: string;
  credit?: string;
}

/** Background layer configuration */
export interface LayoutBackground {
  /** Layer index (0 = bottom, 1 = top) */
  layerIndex: number;
  /** Background type */
  type: "color" | "gradient" | "image";
  /** CSS color/gradient value, or imageId for image type */
  value: string;
  /** Opacity (0-1), default 1 */
  opacity?: number;
  /** Background size mode */
  size?: "cover" | "contain" | "stretch" | "tile" | "custom";
  /** Custom scale (1.0 = 100%, only used when size is "custom") */
  scale?: number;
  /** Position offset from top-left (pixels) */
  offsetX?: number;
  offsetY?: number;
}

/** Knob visual style preset */
export type KnobStylePreset = "default" | "pedal" | "amp" | "minimal" | "custom";

/** Label position relative to control */
export type LabelPosition = "top" | "bottom" | "left" | "right" | "none";

/** A positionable parameter control */
export interface LayoutControl {
  /** Parameter key this control binds to */
  paramKey: string;
  /** Binding kind (default: parameter for legacy layouts) */
  bindingType?: "parameter" | "resource";
  /** Control type */
  type: "knob" | "toggle" | "slider" | "dropdown";
  /** Resource type when bindingType is "resource" */
  resourceType?: string;
  /** Resource slot index when bindingType is "resource" */
  resourceIndex?: number;
  /** Exposed resource ID (for surfaced composite resources) */
  exposedResourceId?: string;
  /** Whether file browsing is allowed for this resource selector */
  allowBrowseFile?: boolean;
  /** Position in pixels (snapped to grid) */
  position: { x: number; y: number };
  /** Optional size override */
  size?: { width: number; height: number };
  /** Visual style */
  style?: {
    /** Label position relative to control */
    labelPosition?: LabelPosition;
    /** Whether to hide the built-in label */
    hideLabel?: boolean;
    /** Label text color (CSS); unset uses the layout's controlLabelColor */
    labelColor?: string;
    /** Whether to show value display */
    showValue?: boolean;
    /** Value text color (CSS); unset uses the layout's controlValueColor */
    valueColor?: string;
    /** Value display position */
    valuePosition?: LabelPosition;
    /** Knob style preset */
    knobStyle?: KnobStylePreset;
    /** Custom knob image ID (when knobStyle is "custom") */
    knobImageId?: string;
    /** Color override for control accent */
    accentColor?: string;
    /** Label text size (px); unset uses the layout's controlLabelFontSize, else 10. */
    labelFontSize?: number;
    /** Value text size (px); unset uses the layout's controlValueFontSize, else 9. */
    valueFontSize?: number;
    /**
     * Drawn size (px) of the control itself. Unset: the original sizes (38 px knob, 80 px slider,
     * 36 x 20 toggle). Not `size`, which older layouts already set to mean the wrapper's width.
     */
    knobDiameter?: number;
    sliderLength?: number;
    toggleWidth?: number;
    toggleHeight?: number;
    /** A knob drawn from images: a turning face or a filmstrip (layoutKnobImages.ts). Wins over knobStyle. */
    knobImage?: LayoutKnobImage;
    /** A toggle drawn as two pictures, one for off and one for on, or from a strip, which wins. */
    toggleImage?: LayoutToggleImage;
    /** A slider drawn with a track picture and a thumb picture, or from a strip of frames, which wins. */
    sliderImage?: { trackImageId?: string; thumbImageId?: string; strip?: LayoutImageStrip };
    /** False: this knob or toggle casts no shadow under the layout's controlShadow light. */
    castShadow?: boolean;
  };
  /** Custom label text (overrides param name) */
  labelOverride?: string;
}

/** A positioned text label */
export interface LayoutTextLabel {
  /** Unique ID within the layout */
  id: string;
  /** Text content */
  text: string;
  /** Position in pixels */
  position: { x: number; y: number };
  /** Font size in pixels */
  fontSize: number;
  /** Font weight */
  fontWeight?: "normal" | "bold";
  /** Font family (CSS) */
  fontFamily?: string;
  /** Text color (CSS) */
  color?: string;
  /** Text alignment */
  textAlign?: "left" | "center" | "right";
}

/** A draggable/resizable rectangular overlay drawn above backgrounds */
export interface LayoutRectangleOverlay {
  /** Unique ID within the layout */
  id: string;
  /** Position in pixels */
  position: { x: number; y: number };
  /** Size in pixels */
  size: { width: number; height: number };
  /** Visual appearance */
  style?: {
    /** Visibility mode for this overlay */
    visibilityMode?: "always" | "enabled" | "bypassed";
    /** Whether clicking this overlay toggles effect bypass in runtime view */
    toggleBypassOnClick?: boolean;
    /** Fill color (without alpha) */
    backgroundColor?: string;
    /** Fill opacity (0-1) */
    backgroundOpacity?: number;
    /** Border color */
    borderColor?: string;
    /** Border width (pixels) */
    borderWidth?: number;
    /** Corner radius (pixels) */
    borderRadius?: number;
    /**
     * An image drawn in the rectangle (a logo, a lit LED, a footswitch), shown and hidden with
     * it by visibilityMode. Older apps ignore it and draw the rectangle's own fill.
     */
    imageId?: string;
    /** How the image fills the rectangle; default "contain". */
    imageFit?: "contain" | "cover" | "stretch";
  };
}

/** Complete effect layout definition */
export interface EffectLayout {
  /** Effect type this layout applies to (e.g., "delay_digital") */
  effectType: string;
  /** GUID layout ID used for persistence (filename stem) */
  layoutId?: string;
  /** Blend definition ID for per-blend layouts (amp_nam_blend only) */
  blendId?: string;
  /** Layout schema version */
  version: number;
  /** Layout display name */
  name?: string;
  /** Layout author */
  author?: string;
  /** Canvas dimensions */
  dimensions: {
    width: number;
    height: number;
  };
  /** Background layers (up to 2) */
  backgrounds: LayoutBackground[];
  /** Positioned parameter controls */
  controls: LayoutControl[];
  /** Rectangle overlays drawn above backgrounds and below controls */
  overlays?: LayoutRectangleOverlay[];
  /** Additional text labels */
  textLabels: LayoutTextLabel[];
  /** Image assets referenced by this layout (for export) */
  imageAssets?: LayoutImageRef[];
  /**
   * When true, the layout provides only the visual backdrop (backgrounds, overlays, labels)
   * and the standard auto-generated parameter controls are rendered on top in their default
   * flow layout. Use this to apply a custom background/theme to an otherwise standard effect.
   */
  useDefaultControls?: boolean;
  /**
   * When useDefaultControls is true: pixel offset applied to the default controls wrapper
   * (left / top inside the backdrop container).
   */
  defaultControlsOffset?: { x: number; y: number };
  /**
   * When useDefaultControls is true: CSS transform scale applied to the default controls
   * wrapper (transform-origin: top left). Values < 1 shrink, > 1 enlarge.
   */
  defaultControlsScale?: { x: number; y: number };
  /**
   * When useDefaultControls is true: the box (canvas px, its top left at defaultControlsOffset)
   * the controls stay inside. They wrap at its width and shrink until they fit its height
   * (layoutControlsArea.ts). Unset: they wrap at the canvas width and may run past its foot.
   */
  defaultControlsArea?: { width: number; height: number };
  /** When useDefaultControls is true: only the Main tab's controls, with no tab bar (layoutDefaultControls.ts). */
  defaultControlsMainOnly?: boolean;
  /**
   * When useDefaultControls is true, false: the groups lose their titled panels, so the controls
   * sit straight on the background. Unset or true: the panels the params panel draws.
   */
  defaultControlsPanels?: boolean;
  /**
   * Optional theme override for the visualisation container. When set, the corresponding
   * theme class (e.g. "theme-dark") is applied to the container div, scoping CSS colour
   * variables so text labels and controls render with that theme's palette regardless of
   * the global app theme. Useful when the layout background colour differs from the app
   * theme (e.g. a light-coloured backdrop that needs dark text).
   */
  containerTheme?: 'light' | 'dark' | 'classic';
  /**
   * Default label and value text colours (CSS) for every control in the layout, the
   * default controls included. A control's own style.labelColor / style.valueColor wins.
   */
  controlLabelColor?: string;
  controlValueColor?: string;
  /**
   * Default label and value text size (px), font family (CSS) and label width cap (px) for the
   * layout's positioned controls; a control's own style.labelFontSize / valueFontSize wins.
   * Unset: 10 px labels, 9 px values, the theme's font, labels capped at 80 px.
   */
  controlLabelFontSize?: number;
  controlValueFontSize?: number;
  controlFontFamily?: string;
  controlLabelMaxWidth?: number;
  /**
   * The knob image every knob in the layout is drawn from unless it has its own: the default
   * controls' knobs (useDefaultControls), and each positioned knob with no knobImage of its own
   * and a knobStyle of "default" or none. Older apps ignore it and draw standard knobs.
   */
  knobImage?: LayoutKnobImage;
  /**
   * The light the layout's knobs and toggles cast shadows from (layoutShadows.ts): each stands as
   * tall as it is wide and casts its footprint away from the light. Unset: no shadows, as before.
   * Older apps ignore it.
   */
  controlShadow?: LayoutControlShadow;
  /**
   * The switch every toggle in the layout is drawn from unless it has images of its own: the
   * default controls' toggles (useDefaultControls) and each positioned toggle. Older apps ignore it
   * and draw standard toggles.
   */
  switchImage?: LayoutSwitchImage;
  /** How the layout fits the params panel (layoutScaling.ts). Unset: the original fit-width rule. */
  scaling?: LayoutScaling;
  /** The designer's snap step (px), also used when the layout is imported. Unset: LAYOUT_GRID_SIZE. */
  gridSize?: number;
  /**
   * Features an older app cannot draw, set on save with version 2 (layoutRequirements.ts). A
   * layout that uses none of them stays version 1 and has no list.
   */
  requires?: string[];
  /** Creation timestamp */
  createdAt?: string;
  /** Last modified timestamp */
  modifiedAt?: string;
  /** Base64 JPEG data URL of the layout thumbnail, captured when saving in the designer */
  thumbnailDataUrl?: string;
}

/** Layout library entry with metadata */
export interface LayoutLibraryEntry {
  /** The layout definition */
  layout: EffectLayout;
  /** Whether this is the default layout for this effect type */
  isDefault: boolean;
  /** User-assigned ID for per-instance references */
  layoutId: string;
  /** File path where this layout is stored */
  filePath?: string;
  /** True for layouts shipped with the application (read-only, cannot be deleted) */
  isFactory?: boolean;
}

/** Complete layout library state */
export interface LayoutLibrary {
  /** Layouts indexed by effect type */
  byEffectType: Record<string, LayoutLibraryEntry[]>;
  /** Default layout ID per effect type */
  defaults: Record<string, string>;
  /** All available layout images */
  images: LayoutImageRef[];
}

/** Grid snap configuration */
export const LAYOUT_GRID_SIZE = 8;

/** Default canvas dimensions */
export const DEFAULT_LAYOUT_DIMENSIONS = {
  width: 400,
  height: 280,
};

/** Minimum/maximum canvas dimensions */
export const LAYOUT_DIMENSION_LIMITS = {
  minWidth: 200,
  maxWidth: 4096,
  minHeight: 150,
  maxHeight: 4096,
};

/** The canvas size every layout fitted in before the limits were raised; larger needs a newer app. */
export const LEGACY_DIMENSION_LIMITS = { maxWidth: 1024, maxHeight: 768 };

/** A layout's snap step: its gridSize when that is a whole number from 1 to 64, else LAYOUT_GRID_SIZE. */
export function layoutGridSize(layout: Pick<EffectLayout, "gridSize"> | null | undefined): number {
  const size = layout?.gridSize;
  return typeof size === "number" && Number.isInteger(size) && size >= 1 && size <= 64 ? size : LAYOUT_GRID_SIZE;
}

/**
 * Build the layout library lookup key.
 * For blend effects this is "amp_nam_blend::{blendId}",
 * otherwise just the effectType string.
 */
export function layoutLookupKey(effectType: string, blendId?: string): string {
  if (blendId) {
    return `${effectType}::${blendId}`;
  }
  return effectType;
}

/** Snap a value to the grid */
export function snapToGrid(value: number, gridSize: number = LAYOUT_GRID_SIZE): number {
  return Math.round(value / gridSize) * gridSize;
}

/**
 * Sanitize all pixel coordinates in a layout to be integer-aligned.
 * Controls and overlays are snapped to the 8px grid; text labels and
 * defaultControlsOffset are rounded to the nearest integer.
 * Scale/opacity values are intentionally left untouched.
 * Call this after importing an external layout to prevent sub-pixel drift.
 */
export function sanitizeLayout(layout: EffectLayout): EffectLayout {
  // The layout's own grid; one that never chose a grid snaps to 8 px as every layout did.
  const grid = layoutGridSize(layout);
  const snap = (value: number) => snapToGrid(value, grid);

  // Dimensions — snap to grid, clamped within limits
  layout.dimensions = {
    width: snap(
      Math.max(LAYOUT_DIMENSION_LIMITS.minWidth, Math.min(LAYOUT_DIMENSION_LIMITS.maxWidth, layout.dimensions.width))
    ),
    height: snap(
      Math.max(LAYOUT_DIMENSION_LIMITS.minHeight, Math.min(LAYOUT_DIMENSION_LIMITS.maxHeight, layout.dimensions.height))
    ),
  };

  // Controls
  for (const control of layout.controls ?? []) {
    control.position = {
      x: snap(control.position.x),
      y: snap(control.position.y),
    };
    if (control.size) {
      control.size = {
        width: snap(control.size.width),
        height: snap(control.size.height),
      };
    }
  }

  // Overlays
  for (const overlay of layout.overlays ?? []) {
    overlay.position = {
      x: snap(overlay.position.x),
      y: snap(overlay.position.y),
    };
    overlay.size = {
      width: snap(overlay.size.width),
      height: snap(overlay.size.height),
    };
  }

  // Text labels — round to integer (no grid constraint needed)
  for (const label of layout.textLabels ?? []) {
    label.position = {
      x: Math.round(label.position.x),
      y: Math.round(label.position.y),
    };
  }

  // defaultControlsOffset — round to integer so left/top px values are whole numbers
  if (layout.defaultControlsOffset) {
    layout.defaultControlsOffset = {
      x: Math.round(layout.defaultControlsOffset.x),
      y: Math.round(layout.defaultControlsOffset.y),
    };
  }
  if (layout.defaultControlsArea) {
    layout.defaultControlsArea = {
      width: Math.round(layout.defaultControlsArea.width),
      height: Math.round(layout.defaultControlsArea.height),
    };
  }

  return layout;
}

/** A layout has at most this many background layers, indexed 0 (bottom) and 1 (top). */
export const MAX_BACKGROUND_LAYERS = 2;

/**
 * The lowest layer index no background uses, or null when every layer is taken.
 * Not `backgrounds.length`: after the bottom layer is deleted that is the index the
 * remaining top layer already has, so a new image replaced it and a new colour shared
 * its index (and Remove then deleted both).
 */
export function nextFreeBackgroundLayer(backgrounds: LayoutBackground[]): number | null {
  for (let index = 0; index < MAX_BACKGROUND_LAYERS; index++) {
    if (!backgrounds.some((bg) => bg.layerIndex === index)) {
      return index;
    }
  }
  return null;
}

/**
 * Give each background a layer index of its own, keeping their order. Layouts saved
 * before nextFreeBackgroundLayer can hold two layers with the same index, which the
 * designer can neither select apart nor delete one at a time.
 */
export function normalizeBackgroundLayers(layout: EffectLayout): EffectLayout {
  const indexes = layout.backgrounds.map((bg) => bg.layerIndex);
  const valid = indexes.every((index, i) =>
    Number.isInteger(index) && index >= 0 && index < MAX_BACKGROUND_LAYERS && indexes.indexOf(index) === i);
  if (!valid) {
    layout.backgrounds = [...layout.backgrounds]
      .sort((a, b) => (Number(a.layerIndex) || 0) - (Number(b.layerIndex) || 0))
      .map((bg, index) => ({ ...bg, layerIndex: index }));
  }
  return layout;
}

/**
 * Where a layout refers to images: background layers, custom knob faces, image knobs (face, base,
 * glare), image toggles, image sliders and image overlays. `visit` gets each reference's id and a
 * setter for it. Export, import and the engine's copy on save (CollectLayoutImageIds) must agree.
 */
export function visitLayoutImageRefs(
  layout: EffectLayout,
  visit: (id: string, set: (id: string) => void, where: string) => void,
): void {
  for (const bg of layout.backgrounds ?? []) {
    if (bg.type === "image" && bg.value) visit(bg.value, (id) => { bg.value = id; }, `Background ${Number(bg.layerIndex) + 1}`);
  }
  const visitField = <T extends object>(owner: T | undefined, key: keyof T, where: string) => {
    const id = owner?.[key];
    if (owner && typeof id === "string" && id) visit(id, (next) => { (owner as Record<keyof T, unknown>)[key] = next; }, where);
  };
  visitField(layout.knobImage, "imageId", `Layout knob: ${layout.knobImage?.mode === "filmstrip" ? "filmstrip" : "turning face"}`);
  visitField(layout.knobImage, "baseImageId", "Layout knob: base");
  visitField(layout.knobImage, "glareImageId", "Layout knob: glare");
  visitField(layout.switchImage, "offImageId", "Layout switch: off");
  visitField(layout.switchImage, "onImageId", "Layout switch: on");
  visitField(layout.switchImage?.strip, "imageId", "Layout switch: strip");
  for (const control of layout.controls ?? []) {
    const style = control.style;
    const name = control.labelOverride || control.paramKey;
    visitField(style, "knobImageId", `${name}: knob face`);
    visitField(style?.knobImage, "imageId", `${name}: ${style?.knobImage?.mode === "filmstrip" ? "filmstrip" : "turning face"}`);
    visitField(style?.knobImage, "baseImageId", `${name}: knob base`);
    visitField(style?.knobImage, "glareImageId", `${name}: knob glare`);
    visitField(style?.toggleImage, "onImageId", `${name}: on`);
    visitField(style?.toggleImage, "offImageId", `${name}: off`);
    visitField(style?.sliderImage, "trackImageId", `${name}: track`);
    visitField(style?.sliderImage, "thumbImageId", `${name}: thumb`);
    visitField(style?.toggleImage?.strip, "imageId", `${name}: switch strip`);
    visitField(style?.sliderImage?.strip, "imageId", `${name}: slider strip`);
  }
  for (const overlay of layout.overlays ?? []) {
    visitField(overlay.style, "imageId", "Rectangle image");
  }
}

/** Every image id `layout` draws on, each once, in the order they appear. */
export function layoutImageIds(layout: EffectLayout): string[] {
  const ids: string[] = [];
  visitLayoutImageRefs(layout, (id) => { if (!ids.includes(id)) ids.push(id); });
  return ids;
}

/** Points every image reference in `layout` that `renamed` holds at its new id. */
export function renameLayoutImageIds(layout: EffectLayout, renamed: ReadonlyMap<string, string>): void {
  visitLayoutImageRefs(layout, (id, set) => {
    const next = renamed.get(id);
    if (next) set(next);
  });
}

/** Generate a unique ID for layout elements */
export function generateLayoutId(): string {
  return `layout-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

/**
 * A copy of `layout` to build a new one from: its own id, so saving it never touches the
 * original, and a name that says it is a copy and is not one of `takenNames` ("Amp (copy)",
 * then "Amp (copy 2)"). Images are shared by id, so the copy draws the same ones.
 */
export function duplicateLayout(layout: EffectLayout, takenNames: string[] = []): EffectLayout {
  const copy = JSON.parse(JSON.stringify(layout)) as EffectLayout;
  copy.layoutId = generateLayoutId();
  const base = (layout.name ?? "").replace(/\s*\(copy(?: \d+)?\)$/, "").trim() || "Layout";
  const taken = new Set(takenNames.map((name) => name.trim()));
  let name = `${base} (copy)`;
  for (let n = 2; taken.has(name); n++) {
    name = `${base} (copy ${n})`;
  }
  copy.name = name;
  copy.createdAt = new Date().toISOString();
  delete copy.modifiedAt;
  return copy;
}

/** Generate a unique ID for text labels */
export function generateLabelId(): string {
  return `label-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

/** Generate a unique ID for rectangle overlays */
export function generateOverlayId(): string {
  return `overlay-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

/** Create an empty layout for an effect type */
export function createEmptyLayout(effectType: string): EffectLayout {
  return {
    effectType,
    version: 1,
    dimensions: { ...DEFAULT_LAYOUT_DIMENSIONS },
    backgrounds: [],
    controls: [],
    overlays: [],
    textLabels: [],
    createdAt: new Date().toISOString(),
    modifiedAt: new Date().toISOString(),
  };
}

/** Create a default layout library */
export function createEmptyLayoutLibrary(): LayoutLibrary {
  return {
    byEffectType: {},
    defaults: {},
    images: [],
  };
}
