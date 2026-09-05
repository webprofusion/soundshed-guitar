import type { BlendModelMapping, ResourceLibrary } from "./types.js";

/**
 * The blend-mappable amp parameters, in display order. Lives here rather than in
 * either editor because blendEditor and signalPathBlend both need it and import
 * each other — this module is the leaf they share.
 */
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

const PARAM_REGEX: Record<string, RegExp> = {
  gain: /(^|[^a-z0-9])g(?:ain)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  drive: /(^|[^a-z0-9])d(?:rive)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  treble: /(^|[^a-z0-9])t(?:reb(?:le)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  middle: /(^|[^a-z0-9])m(?:id(?:dle)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  bass: /(^|[^a-z0-9])b(?:ass)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  contour: /(^|[^a-z0-9])c(?:ontour)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
  presence: /(^|[^a-z0-9])p(?:res(?:ence)?)?\s*([-+]?\d{1,2})(?:\b|[^a-z0-9])/i,
};

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

export function buildBlendModelMappingsFromIds(modelIds: string[], library: ResourceLibrary): BlendModelMapping[] {
  const resources = library.nam ?? [];
  return modelIds.map((id) => {
    const match = resources.find((res) => res.id === id);
    const auto = inferBlendMappingFromName(match?.name ?? "", match?.category);
    return {
      id,
      parameterId: auto?.parameterId,
      parameterValue: auto?.parameterValue,
      parameters: auto?.parameters,
    };
  });
}
