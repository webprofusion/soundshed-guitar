import { uiState } from "./state.js";
import { postMessage } from "./bridge.js";
import { tone3000AuthenticatedFetch } from "./tone3000.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { arrayBufferToBase64 } from "./utils.js";
import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import { buildTone3000ModelsUrl, extractTone3000Models } from "./tone3000Api.js";

interface JSZipObject {
  name: string;
  dir: boolean;
  async(type: "arraybuffer"): Promise<ArrayBuffer>;
}

export interface Tone3000ImportResult {
  importedResourceIds: string[];
  importedNamIds: string[];
}

export function getTone3000ImageUrl(tone: Tone3000Tone): string | null {
  const candidates = [
    Array.isArray(tone.images) ? tone.images[0] : undefined,
    tone.equipment_image_url,
    tone.equipment_image,
    tone.gear_image_url,
    tone.image_url,
    tone.thumbnail_url,
  ];

  for (const candidate of candidates) {
    const value = typeof candidate === "string" ? candidate.trim() : "";
    if (!value) {
      continue;
    }
    if (value.startsWith("http://") || value.startsWith("https://") || value.startsWith("data:")) {
      return value;
    }
  }

  return null;
}

/// Artwork is not part of the model download, so anything imported before the
/// image URL was recorded has no `imageUrl` in its metadata. Every tone listing
/// the API returns already carries the image, so fill it in for matching library
/// resources as the user browses rather than re-fetching tones later.
export function backfillTone3000ResourceImages(tones: Tone3000Tone[]): void {
  const imageUrlByToneId = new Map<string, string>();
  tones.forEach((tone) => {
    const imageUrl = getTone3000ImageUrl(tone);
    if (imageUrl) {
      imageUrlByToneId.set(String(tone.id), imageUrl);
    }
  });
  if (!imageUrlByToneId.size) {
    return;
  }

  for (const resourceType of ["nam", "ir"] as const) {
    (uiState.resourceLibrary[resourceType] ?? []).forEach((resource) => {
      const metadata = resource.metadata;
      if (!metadata || metadata.provider !== "tone3000" || metadata.imageUrl) {
        return;
      }

      const imageUrl = imageUrlByToneId.get(metadata.toneId ?? "");
      if (!imageUrl) {
        return;
      }

      const updated = { ...metadata, imageUrl };
      resource.metadata = updated;
      postMessage({
        type: "updateLibraryResource",
        resourceType,
        resourceId: resource.id,
        name: resource.name,
        category: resource.category,
        description: resource.description,
        metadata: updated,
      });
    });
  }
}

export async function fetchTone3000Models(
  tone: Tone3000Tone,
  architecture?: Tone3000Architecture,
): Promise<Tone3000Model[]> {
  const response = await tone3000AuthenticatedFetch(buildTone3000ModelsUrl(tone.id, 1, 100, architecture));
  if (!response.ok) {
    throw new Error(`Model fetch failed: ${response.status}`);
  }

  const data = await response.json();
  return extractTone3000Models(data);
}

export async function importTone3000Models(
  tone: Tone3000Tone,
  models: Tone3000Model[],
  onProgress?: (completed: number, total: number, currentName?: string) => void,
): Promise<Tone3000ImportResult> {
  const resourceType = (tone.platform ?? "nam").toLowerCase() === "ir" ? "ir" : "nam";
  const gearFolder = sanitizeFilename(tone.gear ?? "other");
  const toneLabel = tone.title ?? tone.name ?? "tone";
  const toneFolder = sanitizeFilename(toneLabel);
  const subfolder = `${gearFolder}/${toneFolder}`;

  const importedResourceIds: string[] = [];
  const importedNamIds: string[] = [];

  let completed = 0;
  const total = models.length;

  for (const model of models) {
    const modelResponse = await tone3000AuthenticatedFetch(model.model_url);

    if (!modelResponse.ok) {
      throw new Error(`Model download failed: ${modelResponse.status}`);
    }

    const buffer = await modelResponse.arrayBuffer();
    const contentType = modelResponse.headers.get("content-type") ?? "";
    const fileNameHint = sanitizeFilename(model.name ?? toneLabel ?? "model");

    if (contentType.includes("zip") || model.model_url.toLowerCase().endsWith(".zip")) {
      const zipped = await importZipBuffer(buffer, {
        tone,
        model,
        subfolder,
        fallbackNameHint: fileNameHint,
        resourceType,
      });
      importedResourceIds.push(...zipped.importedResourceIds);
      importedNamIds.push(...zipped.importedNamIds);
    } else {
      const data = arrayBufferToBase64(buffer);
      const extension = resourceType === "ir" ? ".wav" : ".nam";
      const fileName = `${fileNameHint}${extension}`;
      const resourceId = `tone3000:${model.id}`;

      importRemoteResource({
        tone,
        model,
        resourceType,
        resourceId,
        fileName,
        subfolder,
        data,
      });

      importedResourceIds.push(resourceId);
      if (resourceType === "nam") {
        importedNamIds.push(resourceId);
      }
    }

    completed += 1;
    onProgress?.(completed, total, model.name ?? model.id);
  }

  return {
    importedResourceIds,
    importedNamIds,
  };
}

export function createTone3000BlendDefinition(tone: Tone3000Tone, modelIds: string[]) {
  const id = typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const name = `${tone.title ?? tone.name ?? "Tone3000"} Blend`;
  const category = normalizeBlendCategory(tone.gear);
  const modelMappings = buildBlendModelMappingsFromIds(modelIds, uiState.resourceLibrary);

  return {
    id,
    name,
    category,
    models: modelMappings.map((mapping) => mapping.id),
    modelMappings,
    blendMode: "interpolate" as const,
  };
}

function importRemoteResource(options: {
  tone: Tone3000Tone;
  model: Tone3000Model;
  resourceType: "nam" | "ir";
  resourceId: string;
  fileName: string;
  subfolder: string;
  data: string;
  entryName?: string;
}): void {
  const { tone, model, resourceType, resourceId, fileName, subfolder, data, entryName } = options;

  postMessage({
    type: "importRemoteResource",
    provider: "tone3000",
    resourceType,
    resourceId,
    name: `${tone.title} - ${entryName ?? model.name}`,
    description: tone.description ?? "",
    category: tone.gear ?? "",
    subfolder,
    fileName,
    metadata: {
      provider: "tone3000",
      tone3000Category: tone.gear ?? "",
      toneId: String(tone.id),
      toneTitle: tone.title ?? "",
      groupId: String(tone.id),
      groupName: tone.title ?? tone.name ?? "",
      gear: tone.gear ?? "",
      platform: tone.platform ?? "",
      modelId: String(model.id),
      modelName: model.name ?? "",
      modelUrl: model.model_url,
      imageUrl: getTone3000ImageUrl(tone) ?? "",
      architectureVersion: model.architecture_version ?? "",
      ...(entryName ? { entryName } : {}),
      sourceUrl: `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`,
      creatorId: tone.user?.id != null ? String(tone.user.id) : "",
      creatorName: tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "",
      authorUsername: tone.user?.username ?? "",
    },
    data,
  });
}

async function importZipBuffer(
  buffer: ArrayBuffer,
  options: {
    tone: Tone3000Tone;
    model: Tone3000Model;
    subfolder: string;
    fallbackNameHint: string;
    resourceType: "nam" | "ir";
  },
): Promise<Tone3000ImportResult> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("JSZip not loaded");
  }

  const zip = await zipLib.loadAsync(buffer);
  const entries = Object.values(zip.files) as JSZipObject[];
  const importedResourceIds: string[] = [];
  const importedNamIds: string[] = [];

  for (const entry of entries) {
    if (entry.dir) {
      continue;
    }

    const lowerName = entry.name.toLowerCase();
    const isNam = lowerName.endsWith(".nam") || lowerName.endsWith(".json");
    const isIr = lowerName.endsWith(".wav") || lowerName.endsWith(".ir");

    if ((options.resourceType === "nam" && !isNam) || (options.resourceType === "ir" && !isIr)) {
      continue;
    }

    const fileBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(fileBuffer);
    const fileName = sanitizeFilename(entry.name.split("/").pop() ?? options.fallbackNameHint);
    const resourceId = `tone3000:${options.model.id}:${sanitizeFilename(entry.name)}`;

    importRemoteResource({
      tone: options.tone,
      model: options.model,
      resourceType: options.resourceType,
      resourceId,
      fileName,
      subfolder: options.subfolder,
      data,
      entryName: entry.name,
    });

    importedResourceIds.push(resourceId);
    if (options.resourceType === "nam") {
      importedNamIds.push(resourceId);
    }
  }

  if (!importedResourceIds.length) {
    throw new Error("No supported files found in archive");
  }

  return {
    importedResourceIds,
    importedNamIds,
  };
}

function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}

function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_.]+/gi, "-");
}
