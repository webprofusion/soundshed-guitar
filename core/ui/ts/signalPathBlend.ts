/**
 * Blend effects handling for the signal path panel.
 *
 * Contains blend parameter specs, state derivation, indicator rendering,
 * and the blend-editor modal wiring extracted from signalPath.ts.
 */
import { uiState } from "./state.js";
import { EffectGuids } from "./effectGuids.js";
import type {
  BlendModelMapping,
  BlendDefinition,
  BlendMode,
  BlendLibrary,
  GraphNode,
  LibraryResource,
} from "./types.js";
import { BLEND_PARAM_SPECS, buildBlendModelMappingsFromIds, type BlendParamSpec } from "./blendUtils.js";
import { BlendEditorModal } from "./blendEditor.js";
import { escapeHtml, findResourceById } from "./utils.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";

// ---------------------------------------------------------------------------
// Blend parameter specs
// ---------------------------------------------------------------------------

export { BLEND_PARAM_SPECS, type BlendParamSpec } from "./blendUtils.js";

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
  if (node.type !== EffectGuids.kAmpNamBlend) {
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
  // Per-node override takes precedence over the blend definition's mode.
  const configBlendMode = node.config?.blendMode as BlendMode | undefined;
  const blendMode = (configBlendMode ?? blend?.blendMode ?? "interpolate") as BlendMode;

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
// Blend match summary (live view)
// ---------------------------------------------------------------------------

export type BlendMatchSummary = {
  name: string;
  details: string;
};

/**
 * Resolve a model id to a display name from the resource library.
 */
function resolveModelName(modelId: string): string {
  const resources = uiState.resourceLibrary?.nam ?? [];
  const resource = findResourceById<LibraryResource>(resources, modelId);
  return resource?.name?.trim() || modelId;
}

/**
 * Compute the matched model summary for the live view, mirroring the
 * Test view's logic in blendEditor.updateMatchedModel. Reads normalised
 * param values from the node and matches them against the blend's mappings.
 */
export function computeBlendMatchSummary(
  node: GraphNode,
  blendState: BlendState,
): BlendMatchSummary {
  const target: Record<string, number> = {};
  blendState.paramIds.forEach((paramId) => {
    const value = node.params[paramId];
    if (typeof value === "number") {
      target[paramId] = value;
    }
  });

  const mappings = blendState.mappings;
  if (!Object.keys(target).length || !mappings.length) {
    return { name: "—", details: "" };
  }

  let bestIndex = -1;
  let secondIndex = -1;
  let bestDistance = Number.POSITIVE_INFINITY;
  let secondDistance = Number.POSITIVE_INFINITY;

  mappings.forEach((mapping, index) => {
    const params = buildParameterMapFromLegacy(mapping);
    let distance = 0;
    let matched = false;
    blendState.paramIds.forEach((paramId) => {
      if (!(paramId in target)) {
        return;
      }
      const mappedValue = params[paramId];
      if (typeof mappedValue !== "number") {
        distance += 4;
        return;
      }
      const delta = mappedValue - target[paramId];
      distance += delta * delta;
      matched = true;
    });
    if (!matched) {
      distance += 9;
    }

    if (distance < bestDistance) {
      secondDistance = bestDistance;
      secondIndex = bestIndex;
      bestDistance = distance;
      bestIndex = index;
    } else if (distance < secondDistance) {
      secondDistance = distance;
      secondIndex = index;
    }
  });

  if (bestIndex < 0) {
    return { name: "—", details: "" };
  }

  const bestMapping = mappings[bestIndex];
  const bestName = resolveModelName(bestMapping.id);
  const hasSecond = secondIndex >= 0 && secondIndex !== bestIndex;

  if (blendState.blendMode === "interpolate" && hasSecond) {
    const secondMapping = mappings[secondIndex];
    const secondName = resolveModelName(secondMapping.id);
    const eps = 1e-6;
    const w1 = 1 / Math.max(bestDistance, eps);
    const w2 = 1 / Math.max(secondDistance, eps);
    const denom = Math.max(w1 + w2, eps);
    const p1 = Math.round((w1 / denom) * 100);
    const p2 = 100 - p1;
    return { name: bestName || "—", details: `Mix: ${bestName} ${p1}% / ${secondName} ${p2}%` };
  }

  return { name: bestName || "—", details: "" };
}

/**
 * Update the live-view blend match summary DOM elements in place.
 * Called after a blend param changes so the matched model label tracks the knob.
 */
export function updateBlendMatchSummary(
  panel: HTMLElement | null,
  node: GraphNode,
  blendState: BlendState,
): void {
  if (!panel) {
    return;
  }
  const nameEl = panel.querySelector(".blend-match-name") as HTMLElement | null;
  const detailsEl = panel.querySelector(".blend-match-details") as HTMLElement | null;
  if (!nameEl && !detailsEl) {
    return;
  }
  const summary = computeBlendMatchSummary(node, blendState);
  if (nameEl) {
    nameEl.textContent = summary.name;
  }
  if (detailsEl) {
    detailsEl.textContent = summary.details;
  }
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

// ---------------------------------------------------------------------------
// Blend live-view info rendering (match summary + blend mode override)
// ---------------------------------------------------------------------------

/**
 * Render the blend info block shown in the live effect panel: matched model
 * summary (same format as the Test view) plus a blend mode override control.
 * The summary text is updated in place by `updateBlendMatchSummary` as the
 * mapped param knobs are turned.
 */
export function renderBlendInfoHtml(node: GraphNode, blendState: BlendState): string {
  const summary = computeBlendMatchSummary(node, blendState);
  const blendName = escapeHtml(blendState.blend?.name ?? "");
  const modelCount = blendState.mappings.length;
  const blendMode = blendState.blendMode;
  const nodeId = escapeHtml(node.id);
  const editBlendButton = isFeatureEnabled(Features.BlendTools)
    ? `<button class="blend-open-btn" data-node-id="${node.id}" type="button">Edit Blend</button>`
    : "";

  return `
    <div class="node-resource-selector blend-info-block" data-node-id="${node.id}">
      <label>Blend</label>
      ${blendName ? `<div class="blend-info-name">${blendName}</div>` : ""}
      <div class="blend-match-summary">
        <span class="blend-match-name">${escapeHtml(summary.name)}</span>
        <span class="blend-match-details">${escapeHtml(summary.details)}</span>
      </div>
      <div class="blend-info-controls">
        <label class="blend-mode-label" for="blend-mode-select-${nodeId}">Blend Mode</label>
        <select id="blend-mode-select-${nodeId}" class="blend-mode-select" data-node-id="${node.id}">
          <option value="interpolate" ${blendMode === "interpolate" ? "selected" : ""}>Interpolate</option>
          <option value="snap" ${blendMode === "snap" ? "selected" : ""}>Snap</option>
        </select>
        ${editBlendButton}
      </div>
      <div class="resource-path-info">Models: ${modelCount}</div>
    </div>
  `;
}

