import type { BlendModelMapping, ResourceLibrary } from "./types.js";

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
