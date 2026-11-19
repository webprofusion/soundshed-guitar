import type { BlendDefinition, BlendLibrary, BlendMode, BlendModelMapping, ResourceLibrary } from "./types.js";
import type { ParameterDef } from "./presetV2.js";

/**
 * Blend definitions as the UI reads them. Lives here rather than in either editor because
 * blendEditor, signalPathBlend and the params panel all need it and some import each other
 * — this module is the leaf they share.
 *
 * The engine (MultiModelNAMAmpEffect, controller/internal/BlendSupport) decides which models
 * play; selectBlendMix() mirrors that rule so what the UI says is playing is what is heard.
 */

/**
 * Opens the blend editor on the blend in the event's `BlendEditorOpenDetail`. A document event,
 * so a module the editor's imports reach (the resource browser) can open it without importing it.
 */
export const BLEND_EDITOR_OPEN_EVENT = "blend-editor:open-definition";

/**
 * The blend in the event's `BlendEditorOpenDetail` has been saved from the editor opened on it,
 * which was started from `sourceNode`. The signal path offers the blend in that node's place
 * (signalPath/blendReplacement.ts).
 */
export const BLEND_SAVED_FROM_NODE_EVENT = "blend-editor:saved-from-node";

/** The NAM node a blend was started from (Create Blend in its model picker). */
export interface BlendSourceNode {
  presetId: string;
  nodeId: string;
}

export interface BlendEditorOpenDetail {
  blend: BlendDefinition;
  /** Set when the blend was made from a node: saving it offers the blend in that node's place. */
  sourceNode?: BlendSourceNode;
}

/**
 * A press on an equipment library group, with the `ResourceGroupDragStartDetail` to drag. The
 * library owns the rows but not the graph, so the signal path decides what dropping one means:
 * a blend of the group's models, in a node's place or on a connection. "fx-pointer-drag-start"
 * is the FX library's equivalent.
 */
export const RESOURCE_GROUP_DRAG_START_EVENT = "resource-group-pointer-drag-start";

/** The equipment library group being dragged onto the chain. */
export interface ResourceGroupDragPayload {
  groupId: string;
  title: string;
  category: string;
  modelIds: string[];
  modelMappings?: BlendModelMapping[];
}

export interface ResourceGroupDragStartDetail {
  source: HTMLElement;
  pointerEvent: PointerEvent;
  payload: ResourceGroupDragPayload;
}

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

export function getBlendParamSpec(paramId: string): BlendParamSpec | null {
  if (!paramId) {
    return null;
  }
  return BLEND_PARAM_SPECS.find((spec) => spec.id === paramId) ?? null;
}

// ---------------------------------------------------------------------------
// Normalisation: knobs show a parameter's own scale, the engine takes 0..1
// ---------------------------------------------------------------------------

export function normalizeBlendValue(value: number, spec: Pick<BlendParamSpec, "min" | "max"> | null): number {
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

export function denormalizeBlendValue(value: number, spec: Pick<BlendParamSpec, "min" | "max"> | null): number {
  if (!spec) {
    return value;
  }
  if (value < 0) {
    return value * 10;
  }
  return spec.min + value * (spec.max - spec.min);
}

/**
 * A blend node's mapped knob. It is shown on its parameter's own scale (specMin..specMax)
 * and stored, and sent to the engine, normalised to 0..1.
 */
export type BlendKnobBinding = {
  specMin: number;
  specMax: number;
  blendMode: BlendMode;
};

/** A parameter as the params panel lays it out; `blend` is set on a blend node's mapped knobs. */
/** `blendAsKnob`: an A/B blend ("blend" unit) drawn as a knob rather than the A–B slider. */
export type BlendParamDef = ParameterDef & { blend?: BlendKnobBinding; blendAsKnob?: boolean };

/** The attributes that make a knob or slider a blend knob (see bindNodeParamControls). */
export function blendKnobDataAttributes(binding: BlendKnobBinding): string {
  return `data-blend-param="true" data-blend-spec-min="${binding.specMin}" data-blend-spec-max="${binding.specMax}" data-blend-mode="${binding.blendMode}"`;
}

// ---------------------------------------------------------------------------
// Mappings
// ---------------------------------------------------------------------------

export function buildParameterMapFromLegacy(mapping: BlendModelMapping): Record<string, number> {
  if (mapping.parameters) {
    return mapping.parameters;
  }
  if (mapping.parameterId && typeof mapping.parameterValue === "number") {
    return { [mapping.parameterId]: mapping.parameterValue };
  }
  return {};
}

/** Every model a definition names, each once: its mappings' first, then any only in `models`. */
export function collectBlendModelIds(blend: Pick<BlendDefinition, "models" | "modelMappings">): string[] {
  const ids: string[] = [];
  const add = (id: string | undefined): void => {
    if (id && !ids.includes(id)) {
      ids.push(id);
    }
  };
  (blend.modelMappings ?? []).forEach((mapping) => add(mapping.id));
  (blend.models ?? []).forEach((id) => add(id));
  return ids;
}

/**
 * The parameters a knob is shown for: those at least one model was captured at, in the
 * order the definition lists them. The engine ignores any other, so a knob for one would
 * do nothing. With none, the node is played with its Blend sweep instead.
 */
export function mappedBlendParamIds(blend: BlendDefinition | undefined, mappings: BlendModelMapping[]): string[] {
  const captured = new Set<string>();
  mappings.forEach((mapping) => {
    Object.entries(buildParameterMapFromLegacy(mapping)).forEach(([paramId, value]) => {
      if (typeof value === "number") {
        captured.add(paramId);
      }
    });
  });

  const ordered = (blend?.parameters ?? []).filter((paramId) => captured.has(paramId));
  captured.forEach((paramId) => {
    if (!ordered.includes(paramId)) {
      ordered.push(paramId);
    }
  });
  return ordered;
}

const PARAM_REGEX: Record<string, RegExp> = {
  gain: /(^|[^a-z0-9])g(?:ain)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  drive: /(^|[^a-z0-9])d(?:rive)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  treble: /(^|[^a-z0-9])t(?:reb(?:le)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  middle: /(^|[^a-z0-9])m(?:id(?:dle)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  bass: /(^|[^a-z0-9])b(?:ass)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  contour: /(^|[^a-z0-9])c(?:ontour)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  presence: /(^|[^a-z0-9])p(?:res(?:ence)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  // "T" is Treble on an amp and Tone on a pedal; the editor picks one by the models' gear.
  tone: /(^|[^a-z0-9])t(?:one)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  level: /(^|[^a-z0-9])l(?:evel|vl)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
};

/** The name with every parameter token ("G5", "Tone 3") taken out, for grouping a pack's voicings. */
export function stripParamTokensFromName(name: string): string {
  let stripped = ` ${name} `;
  Object.values(PARAM_REGEX).forEach((regex) => {
    const global = new RegExp(regex.source, "gi");
    stripped = stripped.replace(global, (_match, lead: string) => `${lead} `);
  });
  return stripped.replace(/\s+/g, " ").replace(/[\s\-_|·,]+$/u, "").trim();
}

export function inferParamValueFromName(name: string, parameterId: string): number | null {
  if (!name || !parameterId) {
    return null;
  }

  const regex = PARAM_REGEX[parameterId];
  if (!regex) {
    return null;
  }

  const match = name.toLowerCase().match(regex);
  if (!match) {
    return null;
  }

  const raw = Number.parseInt(match[2], 10);
  if (Number.isNaN(raw) || Math.abs(raw) > 10) {
    return null;
  }

  return raw / 10;
}

export function inferBlendMappingFromName(name: string, category?: string): Partial<BlendModelMapping> | null {
  if (!name) {
    return null;
  }

  const lower = name.toLowerCase();
  const isAmpLike = !category || ["amp", "preamp", "full-rig", "pedal"].includes(category.toLowerCase());
  if (isAmpLike) {
    const preferred = ["gain", "drive", "treble", "middle", "bass", "contour", "presence"];
    for (const param of preferred) {
      const value = inferParamValueFromName(lower, param);
      if (value !== null) {
        return {
          parameterId: param,
          parameterValue: value,
          parameters: { [param]: value },
        };
      }
    }
  }

  return null;
}

/** Mappings guessed from each model's name, such as "G5" for gain 5. */
export function buildBlendModelMappingsFromNames(
  models: Array<{ id: string; name?: string; category?: string }>,
): BlendModelMapping[] {
  return models.map(({ id, name, category }) => {
    const auto = inferBlendMappingFromName(name ?? "", category);
    return {
      id,
      parameterId: auto?.parameterId,
      parameterValue: auto?.parameterValue,
      parameters: auto?.parameters,
    };
  });
}

export function buildBlendModelMappingsFromIds(modelIds: string[], library: ResourceLibrary): BlendModelMapping[] {
  const resources = library.nam ?? [];
  return buildBlendModelMappingsFromNames(modelIds.map((id) => {
    const match = resources.find((res) => res.id === id);
    return { id, name: match?.name, category: match?.category };
  }));
}

/** An existing blend made from this tone group with exactly these models, if there is one. */
export function findBlendForToneGroup(library: BlendLibrary, groupId: string, modelIds: string[]): BlendDefinition | undefined {
  if (!groupId) {
    return undefined;
  }
  const wanted = [...new Set(modelIds)].sort();
  return library.find((blend) => {
    if (blend.toneGroupId !== groupId) {
      return false;
    }
    const ids = collectBlendModelIds(blend).sort();
    return ids.length === wanted.length && ids.every((id, index) => id === wanted[index]);
  });
}

// ---------------------------------------------------------------------------
// Which models play (mirrors MultiModelNAMAmpEffect)
// ---------------------------------------------------------------------------

/** The engine leaves out a model asked for less than this share of the mix. */
export const BLEND_MIN_AUDIBLE_WEIGHT = 0.02;

/** Indices into the mappings, and each one's share of the mix. */
export type BlendMix = {
  primary: number;
  primaryWeight: number;
  secondary: number | null;
  secondaryWeight: number;
};

function pruneMix(mix: BlendMix): BlendMix {
  if (mix.secondary === null) {
    return mix;
  }
  if (mix.secondaryWeight < BLEND_MIN_AUDIBLE_WEIGHT) {
    return { primary: mix.primary, primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }
  if (mix.primaryWeight < BLEND_MIN_AUDIBLE_WEIGHT) {
    return { primary: mix.secondary, primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }
  return mix;
}

/** Where each model sits on the Blend sweep, as BlendSupport.cpp places it. */
function sweepPosition(mapping: BlendModelMapping, index: number, count: number): number {
  if (typeof mapping.parameterValue === "number") {
    return mapping.parameterValue;
  }
  const captured = mapping.parameterId ? mapping.parameters?.[mapping.parameterId] : undefined;
  if (typeof captured === "number") {
    return captured;
  }
  return count > 1 ? index / (count - 1) : 0;
}

/**
 * The models the engine plays for these knob values. `target` holds the node's values for
 * the mapped parameters (normalised 0..1); `blend` is the Blend sweep, used when none of
 * them is set.
 */
export function selectBlendMix(
  mappings: BlendModelMapping[],
  target: Record<string, number>,
  blend: number,
  mode: BlendMode,
): BlendMix | null {
  if (!mappings.length) {
    return null;
  }
  if (mappings.length === 1) {
    return { primary: 0, primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }

  const maps = mappings.map(buildParameterMapFromLegacy);
  const mapped = new Set(maps.flatMap((params) => Object.keys(params)));
  const activeParams = Object.keys(target).filter((paramId) => mapped.has(paramId) && typeof target[paramId] === "number");

  if (activeParams.length) {
    const distances = maps.map((params) => {
      let distance = 0;
      let matched = false;
      activeParams.forEach((paramId) => {
        const value = params[paramId];
        if (typeof value !== "number") {
          distance += 4;
          return;
        }
        distance += (value - target[paramId]) ** 2;
        matched = true;
      });
      return matched ? distance : distance + 9;
    });

    const order = distances.map((_, index) => index).sort((a, b) => distances[a] - distances[b] || a - b);
    const [best, second] = order;
    if (mode === "snap") {
      return { primary: best, primaryWeight: 1, secondary: null, secondaryWeight: 0 };
    }
    const eps = 1e-6;
    const w1 = 1 / Math.max(distances[best], eps);
    const w2 = 1 / Math.max(distances[second], eps);
    const total = Math.max(w1 + w2, eps);
    return pruneMix({ primary: best, primaryWeight: w1 / total, secondary: second, secondaryWeight: w2 / total });
  }

  const positions = mappings.map((mapping, index) => sweepPosition(mapping, index, mappings.length));
  const order = positions.map((_, index) => index).sort((a, b) => positions[a] - positions[b] || a - b);
  const min = positions[order[0]];
  const max = positions[order[order.length - 1]];
  const at = min + Math.min(1, Math.max(0, blend)) * (max - min);

  if (at <= min) {
    return { primary: order[0], primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }
  if (at >= max) {
    return { primary: order[order.length - 1], primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }

  let upper = 1;
  while (upper < order.length && positions[order[upper]] < at) {
    upper += 1;
  }
  const lower = upper - 1;
  const lowerValue = positions[order[lower]];
  const upperValue = positions[order[upper]];

  if (mode === "snap") {
    const nearest = at - lowerValue <= upperValue - at ? order[lower] : order[upper];
    return { primary: nearest, primaryWeight: 1, secondary: null, secondaryWeight: 0 };
  }

  const t = Math.min(1, Math.max(0, (at - lowerValue) / Math.max(upperValue - lowerValue, 1e-9)));
  return pruneMix({ primary: order[lower], primaryWeight: 1 - t, secondary: order[upper], secondaryWeight: t });
}

export type BlendMatchSummary = {
  name: string;
  details: string;
};

/**
 * What `mappings` play for these knob values, in words: the model, and the mix when two
 * are heard. `nameOf` turns a model id into a display name.
 */
export function describeBlendMix(
  mappings: BlendModelMapping[],
  target: Record<string, number>,
  blend: number,
  mode: BlendMode,
  nameOf: (modelId: string) => string,
): BlendMatchSummary {
  const mix = selectBlendMix(mappings, target, blend, mode);
  if (!mix) {
    return { name: "—", details: "" };
  }

  const primaryName = nameOf(mappings[mix.primary].id) || "—";
  if (mix.secondary === null) {
    return { name: primaryName, details: "" };
  }

  const secondaryName = nameOf(mappings[mix.secondary].id);
  const p1 = Math.round(mix.primaryWeight * 100);
  return { name: primaryName, details: `Mix: ${primaryName} ${p1}% / ${secondaryName} ${100 - p1}%` };
}
