/**
 * Blend effects handling for the signal path panel.
 *
 * Contains blend parameter specs, state derivation, indicator rendering,
 * and the blend-editor modal wiring extracted from signalPath.ts.
 */
import { uiState } from "./state.js";
import type {
  BlendModelMapping,
  BlendDefinition,
  BlendMode,
  BlendLibrary,
  GraphNode,
} from "./types.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { BlendEditorModal } from "./blendEditor.js";

// ---------------------------------------------------------------------------
// Blend parameter specs
// ---------------------------------------------------------------------------

export type BlendParamSpec = {
  id: string;
  label: string;
  min: number;
  max: number;
};

export const BLEND_PARAM_SPECS: BlendParamSpec[] = [
  { id: "gain", label: "Gain", min: 0, max: 10 },
  { id: "drive", label: "Drive", min: 0, max: 10 },
  { id: "contour", label: "Contour", min: 0, max: 10 },
  { id: "treble", label: "Treble", min: 0, max: 10 },
  { id: "middle", label: "Middle", min: 0, max: 10 },
  { id: "bass", label: "Bass", min: 0, max: 10 },
  { id: "presence", label: "Presence", min: 0, max: 10 },
  { id: "tone", label: "Tone", min: 0, max: 10 },
  { id: "level", label: "Level", min: 0, max: 10 },
  { id: "custom_a", label: "Custom A", min: 0, max: 10 },
  { id: "custom_b", label: "Custom B", min: 0, max: 10 },
  { id: "custom_c", label: "Custom C", min: 0, max: 10 },
];

function getBlendParamSpec(paramId: string): BlendParamSpec | null {
  if (!paramId) {
    return null;
  }
  return BLEND_PARAM_SPECS.find((spec) => spec.id === paramId) ?? null;
}

// ---------------------------------------------------------------------------
// Normalisation helpers
// ---------------------------------------------------------------------------

export function normalizeBlendValue(value: number, spec: BlendParamSpec | null): number {
  if (!spec) {
    return value;
  }
  if (value < 0) {
    return value / 10;
  }
  const clamped = Math.min(spec.max, Math.max(spec.min, value));
  const range = spec.max - spec.min;
  if (range <= 0) {
    return 0;
  }
  return (clamped - spec.min) / range;
}

export function denormalizeBlendValue(value: number, spec: BlendParamSpec | null): number {
  if (!spec) {
    return value;
  }
  if (value < 0) {
    return value * 10;
  }
  return spec.min + value * (spec.max - spec.min);
}

// ---------------------------------------------------------------------------
// Mapped-point helpers
// ---------------------------------------------------------------------------

export const BLEND_MAPPING_EPS = 1e-4;

type BlendMappedPoint = {
  normalized: number;
  display: number;
  isSelectable: boolean;
  isSelected: boolean;
};

function hasCloseValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): boolean {
  return values.some((existing) => Math.abs(existing - value) <= eps);
}

function addUniqueValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): void {
  if (!hasCloseValue(values, value, eps)) {
    values.push(value);
  }
}

function buildBlendMappedPointsForParam(
  paramId: string,
  blendState: BlendState,
  target: Record<string, number>,
): BlendMappedPoint[] {
  if (!paramId) {
    return [];
  }

  const normalizedValues: number[] = [];
  const selectableValues: number[] = [];
  const spec = getBlendParamSpec(paramId);

  blendState.mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue === "number") {
      addUniqueValue(normalizedValues, mappedValue);
    }
  });

  blendState.mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue !== "number") {
      return;
    }
    let matches = true;
    blendState.paramIds.forEach((activeParamId) => {
      if (activeParamId === paramId) {
        return;
      }
      const targetValue = target[activeParamId];
      const otherValue = params[activeParamId];
      if (typeof targetValue !== "number" || typeof otherValue !== "number") {
        matches = false;
        return;
      }
      if (Math.abs(otherValue - targetValue) > BLEND_MAPPING_EPS) {
        matches = false;
      }
    });
    if (matches) {
      addUniqueValue(selectableValues, mappedValue);
    }
  });

  const targetValue = target[paramId];
  return normalizedValues
    .slice()
    .sort((a, b) => a - b)
    .map((value) => ({
      normalized: value,
      display: denormalizeBlendValue(value, spec),
      isSelectable: hasCloseValue(selectableValues, value),
      isSelected: typeof targetValue === "number" && Math.abs(targetValue - value) <= BLEND_MAPPING_EPS,
    }));
}

function renderMappedPointElements(
  knob: HTMLElement,
  points: BlendMappedPoint[],
  min: number,
  max: number,
): void {
  let container = knob.querySelector(".knob-mapped-points") as HTMLElement | null;
  if (!container) {
    container = document.createElement("div");
    container.className = "knob-mapped-points";
    knob.prepend(container);
  }

  container.innerHTML = "";
  const range = max - min;
  const safeRange = range !== 0 ? range : 1;

  points.forEach((point) => {
    const angle = ((point.display - min) / safeRange) * 270 - 135;
    const el = document.createElement("span");
    el.className = "knob-mapped-point";
    if (point.isSelectable) {
      el.classList.add("is-selectable");
    }
    if (point.isSelected) {
      el.classList.add("is-selected");
    }
    el.style.setProperty("--mapped-angle", `${angle}deg`);
    container.appendChild(el);
  });

  knob.classList.toggle("has-mapped-points", points.length > 0);
}

// ---------------------------------------------------------------------------
// Blend state
// ---------------------------------------------------------------------------

export type BlendState = {
  blend: BlendDefinition | undefined;
  blendMode: BlendMode;
  mappings: BlendModelMapping[];
  paramIds: string[];
};

export function buildParameterMapFromLegacy(mapping: BlendModelMapping): Record<string, number> {
  if (mapping.parameters) {
    return mapping.parameters;
  }
  if (mapping.parameterId && typeof mapping.parameterValue === "number") {
    return { [mapping.parameterId]: mapping.parameterValue };
  }
  return {};
}

function resolveBlendActiveParams(blend: BlendDefinition | undefined, mappings: BlendModelMapping[]): string[] {
  const params = new Set<string>();
  if (blend?.parameters?.length) {
    blend.parameters.forEach((param) => params.add(param));
  }
  mappings.forEach((mapping) => {
    const map = buildParameterMapFromLegacy(mapping);
    Object.keys(map).forEach((param) => params.add(param));
  });
  if (!params.size) {
    params.add("gain");
  }
  return Array.from(params);
}

export type BlendParamRange = {
  min: number;
  max: number;
  defaultValue: number;
  spec: BlendParamSpec | null;
};

export function computeBlendParamRange(
  paramId: string,
  mappings: BlendModelMapping[],
  fallbackValue: number | undefined,
): BlendParamRange {
  const spec = getBlendParamSpec(paramId);
  const values: number[] = [];

  mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const raw = params[paramId];
    if (typeof raw === "number") {
      values.push(denormalizeBlendValue(raw, spec));
    }
  });

  const fallbackDisplay = typeof fallbackValue === "number"
    ? denormalizeBlendValue(fallbackValue, spec)
    : (spec ? spec.min : 0);

  if (!values.length) {
    return {
      min: spec ? spec.min : -1,
      max: spec ? spec.max : 1,
      defaultValue: fallbackDisplay,
      spec,
    };
  }

  const sorted = values.slice().sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const median = sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
  const min = sorted[0];
  const max = sorted[sorted.length - 1];
  const defaultValue = typeof fallbackValue === "number" ? fallbackDisplay : median;

  return {
    min: min === max ? min - 0.5 : min,
    max: min === max ? max + 0.5 : max,
    defaultValue,
    spec,
  };
}

export function getBlendState(node: GraphNode): BlendState | null {
  if (node.type !== "amp_nam_blend") {
    return null;
  }

  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (!blendId) {
    return null;
  }

  const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
  const mappings = blend?.modelMappings?.length
    ? blend.modelMappings
    : buildBlendModelMappingsFromIds(blend?.models ?? [], uiState.resourceLibrary);
  const paramIds = resolveBlendActiveParams(blend, mappings);
  const blendMode = (blend?.blendMode ?? "interpolate") as BlendMode;

  return {
    blend,
    blendMode,
    mappings,
    paramIds,
  };
}

// ---------------------------------------------------------------------------
// Blend param indicator rendering
// ---------------------------------------------------------------------------

export function updateBlendParamIndicators(
  panel: HTMLElement | null,
  node: GraphNode,
  blendState: BlendState,
): void {
  if (!panel) {
    return;
  }

  const target: Record<string, number> = {};
  blendState.paramIds.forEach((paramId) => {
    const value = node.params[paramId];
    if (typeof value === "number") {
      target[paramId] = value;
    }
  });

  const knobs = panel.querySelectorAll('.node-param-knob[data-blend-param="true"]');
  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const paramId = knob.dataset.paramKey ?? "";
    const min = knob.dataset.min ? parseFloat(knob.dataset.min) : 0;
    const max = knob.dataset.max ? parseFloat(knob.dataset.max) : 1;
    const points = buildBlendMappedPointsForParam(paramId, blendState, target);
    renderMappedPointElements(knob, points, min, max);
  });
}

// ---------------------------------------------------------------------------
// Blend library helpers
// ---------------------------------------------------------------------------

export function getBlendEntriesForCategory(categoryId: string): Array<{ id: string; name: string; category: string; originalCategory: string }> {
  const blends = uiState.blendLibrary ?? [];
  const mapCategory = (value: string): string => {
    switch (value) {
      case "cab":
        return "cab";
      case "pedal":
        return "utility";
      case "preamp":
      case "amp":
      case "full-rig":
      default:
        return "amp";
    }
  };

  return blends
    .map((blend) => ({
      id: blend.id,
      name: blend.name,
      category: mapCategory(blend.category),
      originalCategory: blend.category,
    }))
    .filter((blend) => blend.category === categoryId);
}

// ---------------------------------------------------------------------------
// Blend editor modal
// ---------------------------------------------------------------------------

const blendEditorModal = new BlendEditorModal({
  getBlendLibrary: () => uiState.blendLibrary ?? ([] as BlendLibrary),
  getResourceLibrary: () => uiState.resourceLibrary,
});

export function initializeBlendEditorModal(): void {
  blendEditorModal.initialize();
}

export function openBlendEditorWithDefinition(blend: BlendDefinition): void {
  blendEditorModal.openWithDefinition(blend);
}

export function bindBlendEditorControls(panel: HTMLElement | null, node: GraphNode): void {
  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (!blendId) {
    return;
  }

  const openButton = panel?.querySelector(".blend-open-btn") as HTMLButtonElement | null;
  openButton?.addEventListener("click", () => {
    blendEditorModal.open(node);
  });
}
