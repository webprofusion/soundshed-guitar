/**
 * Turning an exported layout archive back into a layout, without trusting it.
 *
 * An archive is a file someone shared, so everything in it is the sender's to choose:
 * the image file names (the engine writes the images under them), the layout's id
 * (saving under an existing id overwrites that layout) and its shape. So the images
 * are given new names and ids of our own, the layout takes the identity of the one
 * open in the designer, and anything that is not the shape a layout has is dropped.
 */

import type { EffectLayout, LayoutBackground, LayoutControl, LayoutImageRef, LayoutRectangleOverlay, LayoutTextLabel } from "../layoutTypes.js";
import { DEFAULT_LAYOUT_DIMENSIONS, normalizeBackgroundLayers, renameLayoutImageIds, sanitizeLayout } from "../layoutTypes.js";
import { usableControlsArea } from "../layoutControlsArea.js";
import { cleanCredit, isLayoutImageLicense } from "../layoutImageCredits.js";
import { arrayBufferToBase64 } from "../utils.js";

/** Image types a layout takes, by extension, with the MIME type each is served as. */
export const LAYOUT_IMAGE_MIME_TYPES: Readonly<Record<string, string>> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
  svg: "image/svg+xml",
};

/** Largest image the engine stores for a layout; it refuses anything bigger. */
export const MAX_LAYOUT_IMAGE_BYTES = 64 * 1024 * 1024;

/** An image from the archive, renamed and ready to register and send to the engine. */
export interface ImportedLayoutImage {
  imageId: string;
  fileName: string;
  dataUrl: string;
  base64: string;
  type?: LayoutImageRef["type"];
  license?: string;
  credit?: string;
}

/** Who the imported layout becomes: the layout open in the designer. */
export interface LayoutImportTarget {
  layoutId: string;
  effectType: string;
  blendId?: string;
  name?: string;
  createdAt?: string;
}

export interface PreparedLayoutImport {
  layout: EffectLayout;
  images: ImportedLayoutImage[];
  /** Manifest entries left out: a name that is not a plain file name, an unknown type, a missing file, too large. */
  skippedImages: string[];
  /** Controls bound to something the open effect does not have. */
  unmatchedControls: number;
}

type Reader = (archivePath: string) => Promise<ArrayBuffer | null>;

const IMAGE_TYPES = new Set<LayoutImageRef["type"]>(["background", "knob", "switch", "general"]);

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function finite(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

/** A finite number, or nothing: for the optional numbers a layout may leave out. */
function optionalFinite(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === "string" ? value : undefined;
}

function optionalPoint(value: unknown, fallback: number): { x: number; y: number } | undefined {
  return isRecord(value) ? { x: finite(value.x, fallback), y: finite(value.y, fallback) } : undefined;
}

const CONTAINER_THEMES = new Set(["dark", "light", "classic"]);

function hasPosition(value: Record<string, unknown>): boolean {
  const position = value.position;
  return isRecord(position) && Number.isFinite(position.x) && Number.isFinite(position.y);
}

/** A file name with nothing in it that leads elsewhere, and a type layouts take. */
export function layoutImageExtension(fileName: unknown): string | null {
  if (typeof fileName !== "string" || !fileName || fileName === "." || fileName === "..") return null;
  // eslint-disable-next-line no-control-regex
  if (/[\\/:*?"<>|\u0000-\u001f]/.test(fileName)) return null;
  const dot = fileName.lastIndexOf(".");
  if (dot <= 0) return null;
  const extension = fileName.slice(dot + 1).toLowerCase();
  return LAYOUT_IMAGE_MIME_TYPES[extension] ? extension : null;
}

/** An image id of our own, from the archive's file name: plain characters, then a unique suffix. */
export function importedImageId(fileName: string, unique: string): string {
  const stem = fileName.slice(0, Math.max(0, fileName.lastIndexOf("."))) || fileName;
  const plain = stem.replace(/[^A-Za-z0-9_-]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 48);
  return `${plain || "image"}_${unique}`;
}

/**
 * The parts of an archive's layout that have a layout's shape, with defaults for what
 * is missing; null when it is not a layout at all.
 *
 * Every field the designer and the renderer put into markup or CSS is given its type here:
 * an archive can say anything, and a number field holding `1"><iframe …>` or
 * `1);background-image:url(…)` went straight into an attribute or a style. A theme that is
 * not one of ours, or a string where an object belongs, made the designer throw on every render.
 */
export function normalizeImportedLayout(raw: unknown): EffectLayout | null {
  if (!isRecord(raw)) return null;
  const dimensions = isRecord(raw.dimensions) ? raw.dimensions : {};

  const backgrounds = (Array.isArray(raw.backgrounds) ? raw.backgrounds : []).filter(
    (bg): bg is LayoutBackground =>
      isRecord(bg) && ["color", "gradient", "image"].includes(bg.type as string) && typeof bg.value === "string",
  ).map((bg) => ({
    ...bg,
    layerIndex: finite(bg.layerIndex, 0),
    opacity: optionalFinite(bg.opacity),
    scale: optionalFinite(bg.scale),
    offsetX: optionalFinite(bg.offsetX),
    offsetY: optionalFinite(bg.offsetY),
  }));

  const controls = (Array.isArray(raw.controls) ? raw.controls : []).filter(
    (control): control is LayoutControl =>
      isRecord(control) && typeof control.paramKey === "string" && control.paramKey !== "" && hasPosition(control),
  ).map((control) => {
    const size = control.size as unknown;
    const style = control.style as unknown;
    return {
      ...control,
      size: isRecord(size) && Number.isFinite(size.width) && Number.isFinite(size.height) ? control.size : undefined,
      style: isRecord(style)
        ? {
            ...(style as LayoutControl["style"]),
            labelColor: optionalString(style.labelColor),
            valueColor: optionalString(style.valueColor),
            accentColor: optionalString(style.accentColor),
            knobImageId: optionalString(style.knobImageId),
          }
        : undefined,
      labelOverride: optionalString(control.labelOverride),
    };
  });

  const overlays = (Array.isArray(raw.overlays) ? raw.overlays : []).filter(
    (overlay): overlay is LayoutRectangleOverlay =>
      isRecord(overlay) && typeof overlay.id === "string" && hasPosition(overlay) && isRecord(overlay.size)
      && Number.isFinite(overlay.size.width) && Number.isFinite(overlay.size.height),
  ).map((overlay) => {
    const style = overlay.style as unknown;
    return {
      ...overlay,
      style: isRecord(style)
        ? {
            ...(style as LayoutRectangleOverlay["style"]),
            backgroundColor: optionalString(style.backgroundColor),
            backgroundOpacity: optionalFinite(style.backgroundOpacity),
            borderColor: optionalString(style.borderColor),
            borderWidth: optionalFinite(style.borderWidth),
            borderRadius: optionalFinite(style.borderRadius),
          }
        : undefined,
    };
  });

  const textLabels = (Array.isArray(raw.textLabels) ? raw.textLabels : []).filter(
    (label): label is LayoutTextLabel =>
      isRecord(label) && typeof label.id === "string" && typeof label.text === "string" && hasPosition(label),
  ).map((label) => ({
    ...label,
    fontSize: finite(label.fontSize, 14),
    fontFamily: optionalString(label.fontFamily),
    color: optionalString(label.color),
  }));

  const layout: EffectLayout = {
    ...(raw as Partial<EffectLayout>),
    effectType: typeof raw.effectType === "string" ? raw.effectType : "",
    version: finite(raw.version, 1),
    name: typeof raw.name === "string" ? raw.name : undefined,
    author: optionalString(raw.author),
    containerTheme:
      typeof raw.containerTheme === "string" && CONTAINER_THEMES.has(raw.containerTheme)
        ? (raw.containerTheme as EffectLayout["containerTheme"])
        : undefined,
    controlLabelColor: optionalString(raw.controlLabelColor),
    controlValueColor: optionalString(raw.controlValueColor),
    useDefaultControls: raw.useDefaultControls === true ? true : undefined,
    defaultControlsOffset: optionalPoint(raw.defaultControlsOffset, 0),
    defaultControlsScale: optionalPoint(raw.defaultControlsScale, 1),
    defaultControlsArea: usableControlsArea(raw.defaultControlsArea),
    dimensions: {
      width: finite(dimensions.width, DEFAULT_LAYOUT_DIMENSIONS.width),
      height: finite(dimensions.height, DEFAULT_LAYOUT_DIMENSIONS.height),
    },
    backgrounds,
    controls,
    overlays,
    textLabels,
  };
  return sanitizeLayout(normalizeBackgroundLayers(layout));
}

/**
 * Prepare an archive's layout to replace the one open in the designer.
 *
 * `archive` is the parsed layout.json; `readFile` reads a path inside the zip, null when it
 * is not there. `knownControlKeys` are the parameter and resource keys the open effect has.
 */
export async function prepareLayoutImport(
  archive: unknown,
  readFile: Reader,
  target: LayoutImportTarget,
  knownControlKeys: ReadonlySet<string>,
): Promise<PreparedLayoutImport | { error: string }> {
  if (!isRecord(archive)) return { error: "the archive's layout.json is not a layout archive" };
  const layout = normalizeImportedLayout(archive.layout);
  if (!layout) return { error: "the archive has no layout data" };

  const manifest = Array.isArray(archive.images) ? archive.images : [];
  const images: ImportedLayoutImage[] = [];
  const skippedImages: string[] = [];
  const renamed = new Map<string, string>();
  const stamp = Date.now();

  for (const [index, entry] of manifest.entries()) {
    const fileName = isRecord(entry) ? entry.fileName : undefined;
    const oldId = isRecord(entry) && typeof entry.imageId === "string" ? entry.imageId : "";
    const extension = layoutImageExtension(fileName);
    const label = typeof fileName === "string" && fileName ? fileName : `image ${index + 1}`;
    if (!extension || !oldId) {
      skippedImages.push(label);
      continue;
    }

    const bytes = await readFile(`images/${fileName as string}`);
    if (!bytes || bytes.byteLength === 0 || bytes.byteLength > MAX_LAYOUT_IMAGE_BYTES) {
      skippedImages.push(label);
      continue;
    }

    const imageId = importedImageId(fileName as string, `${stamp}${index}`);
    const base64 = arrayBufferToBase64(bytes);
    const type = isRecord(entry) && IMAGE_TYPES.has(entry.type as LayoutImageRef["type"])
      ? entry.type as LayoutImageRef["type"]
      : undefined;
    // A licence the designer offers and a credit line, as the engine would keep them.
    const license = isRecord(entry) && isLayoutImageLicense(entry.license) ? entry.license : undefined;
    const credit = isRecord(entry) ? cleanCredit(entry.credit) || undefined : undefined;
    renamed.set(oldId, imageId);
    images.push({
      imageId,
      fileName: `${imageId}.${extension}`,
      dataUrl: `data:${LAYOUT_IMAGE_MIME_TYPES[extension]};base64,${base64}`,
      base64,
      type,
      license,
      credit,
    });
  }

  // Point the layout at the renamed images. An id the manifest did not carry is left as it
  // is: it may name an image already in this library (an archive exported on this machine).
  renameLayoutImageIds(layout, renamed);

  // The layout open in the designer, refilled: saving overwrites that layout and no other.
  layout.layoutId = target.layoutId;
  layout.effectType = target.effectType;
  if (target.blendId) {
    layout.blendId = target.blendId;
  } else {
    delete layout.blendId;
  }
  layout.name = layout.name?.trim() || target.name;
  layout.createdAt = target.createdAt ?? new Date().toISOString();
  layout.modifiedAt = new Date().toISOString();
  delete layout.thumbnailDataUrl;
  delete layout.imageAssets;

  const unmatchedControls = layout.useDefaultControls
    ? 0
    : layout.controls.filter((control) => !knownControlKeys.has(control.paramKey)).length;
  return { layout, images, skippedImages, unmatchedControls };
}
