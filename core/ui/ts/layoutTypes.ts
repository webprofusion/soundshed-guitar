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
   * Optional theme override for the visualisation container. When set, the corresponding
   * theme class (e.g. "theme-dark") is applied to the container div, scoping CSS colour
   * variables so text labels and controls render with that theme's palette regardless of
   * the global app theme. Useful when the layout background colour differs from the app
   * theme (e.g. a light-coloured backdrop that needs dark text).
   */
  containerTheme?: 'light' | 'dark' | 'classic';
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
  // Dimensions — snap to grid, clamped within limits
  layout.dimensions = {
    width: snapToGrid(
      Math.max(LAYOUT_DIMENSION_LIMITS.minWidth, Math.min(LAYOUT_DIMENSION_LIMITS.maxWidth, layout.dimensions.width))
    ),
    height: snapToGrid(
      Math.max(LAYOUT_DIMENSION_LIMITS.minHeight, Math.min(LAYOUT_DIMENSION_LIMITS.maxHeight, layout.dimensions.height))
    ),
  };

  // Controls
  for (const control of layout.controls ?? []) {
    control.position = {
      x: snapToGrid(control.position.x),
      y: snapToGrid(control.position.y),
    };
    if (control.size) {
      control.size = {
        width: snapToGrid(control.size.width),
        height: snapToGrid(control.size.height),
      };
    }
  }

  // Overlays
  for (const overlay of layout.overlays ?? []) {
    overlay.position = {
      x: snapToGrid(overlay.position.x),
      y: snapToGrid(overlay.position.y),
    };
    overlay.size = {
      width: snapToGrid(overlay.size.width),
      height: snapToGrid(overlay.size.height),
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

  return layout;
}

/** Generate a unique ID for layout elements */
export function generateLayoutId(): string {
  return `layout-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
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
