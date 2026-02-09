/**
 * Effect Layout Designer Types
 *
 * Defines data structures for custom effect parameter layouts including
 * background images, control positioning, and text labels.
 */

/** Image reference for layout backgrounds and knobs */
export interface LayoutImageRef {
  /** Unique ID for the image in the layout library */
  imageId: string;
  /** Original filename for display */
  fileName?: string;
  /** Base64 data URL for WebView access (data:image/...) */
  dataUrl?: string;
  /** Image type/category */
  type?: "background" | "knob" | "general";
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
  /** Control type */
  type: "knob" | "toggle" | "slider" | "dropdown";
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
    /** Label text color (CSS) */
    labelColor?: string;
    /** Whether to show value display */
    showValue?: boolean;
    /** Value display position */
    valuePosition?: LabelPosition;
    /** Knob style preset */
    knobStyle?: KnobStylePreset;
    /** Custom knob image ID (when knobStyle is "custom") */
    knobImageId?: string;
    /** Color override for control accent */
    accentColor?: string;
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

/** Complete effect layout definition */
export interface EffectLayout {
  /** Effect type this layout applies to (e.g., "delay_digital") */
  effectType: string;
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
  /** Additional text labels */
  textLabels: LayoutTextLabel[];
  /** Image assets referenced by this layout (for export) */
  imageAssets?: LayoutImageRef[];
  /** Creation timestamp */
  createdAt?: string;
  /** Last modified timestamp */
  modifiedAt?: string;
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
  maxWidth: 800,
  minHeight: 150,
  maxHeight: 600,
};

/**
 * Build the layout library lookup key.
 * For blend effects this is "amp_nam_blend::{blendId}",
 * otherwise just the effectType string.
 */
export function layoutLookupKey(effectType: string, blendId?: string): string {
  if (blendId && effectType === "amp_nam_blend") {
    return `${effectType}::${blendId}`;
  }
  return effectType;
}

/** Snap a value to the grid */
export function snapToGrid(value: number, gridSize: number = LAYOUT_GRID_SIZE): number {
  return Math.round(value / gridSize) * gridSize;
}

/** Generate a unique ID for layout elements */
export function generateLayoutId(): string {
  return `layout-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

/** Generate a unique ID for text labels */
export function generateLabelId(): string {
  return `label-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

/** Create an empty layout for an effect type */
export function createEmptyLayout(effectType: string): EffectLayout {
  return {
    effectType,
    version: 1,
    dimensions: { ...DEFAULT_LAYOUT_DIMENSIONS },
    backgrounds: [],
    controls: [],
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
