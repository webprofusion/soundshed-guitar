import { getSignalPathPreset } from "../state.js";
import type {
  GraphNode,
} from "../types.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { CATEGORIES, EFFECTS, LEGACY_EFFECT_EQUIPMENT_IMAGES } from "../generated/effectPresentation.js";
import { getLibraryResource } from "../resourceLibrary.js";
import {
  effectVisualizationElement,
} from "./state.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { getCanonicalLibraryResourceId, getNodeCategory, handleNamIrFileDrop, inferResourceTypeFromFile, isNamOrCabIrNode, nodeAcceptsResourceType } from "./nodeTypes.js";

export let effectVisualizationDropCleanup: (() => void) | null = null;

// Backgrounds and stock artwork come from core/ui/data/effect-presentation.json, which
// Soundshed Guitar Nano reads too. Its images are relative to core/ui; the web UI has always
// addressed them one level up.
const WEB_IMAGE_PREFIX = "../";

/** The effect view's background for each category: the table's two colours as a 145deg gradient. */
export const EFFECT_VISUAL_BACKGROUNDS: Record<string, string> = Object.fromEntries(
  Object.entries(CATEGORIES).flatMap(([id, category]) => category.visualBackground
    ? [[id, `linear-gradient(145deg, ${category.visualBackground[0]} 0%, ${category.visualBackground[1]} 100%)`]]
    : []),
);

/** Stock artwork by category. */
export const EFFECT_VISUAL_EQUIPMENT_IMAGES: Record<string, string> = Object.fromEntries(
  Object.entries(CATEGORIES).flatMap(([id, category]) => category.equipmentImage
    ? [[id, `${WEB_IMAGE_PREFIX}${category.equipmentImage}`]]
    : []),
);

/** Stock artwork by effect type, and by the legacy ids presets may still carry. */
export const EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE: Record<string, string> = Object.fromEntries([
  ...Object.entries(EFFECTS).flatMap(([type, effect]) => effect.equipmentImage
    ? [[type, `${WEB_IMAGE_PREFIX}${effect.equipmentImage}`]]
    : []),
  ...Object.entries(LEGACY_EFFECT_EQUIPMENT_IMAGES).map(([id, image]) => [id, `${WEB_IMAGE_PREFIX}${image}`]),
]);

export /**
 * Artwork the capture author supplied for the loaded model (Tone3000 tones ship
 * a photo of the real gear). Only http(s)/data URLs are accepted so a stray
 * metadata value can never turn into a local file or script URL.
 */
function normalizeResourceArtworkUrl(value: string | undefined): string {
  const trimmed = (value ?? "").trim();
  if (!trimmed) {
    return "";
  }
  return trimmed.startsWith("https://") || trimmed.startsWith("http://") || trimmed.startsWith("data:image/")
    ? trimmed
    : "";
}

export function getNodeResourceArtworkImage(node: GraphNode): string {
  const typeInfo = getNodeEffectInfo(node);
  const candidates: Array<{ resourceType: string; resourceIndex: number }> = [];

  (typeInfo?.exposedResources ?? []).forEach((exposedResource, exposedResourceIndex) => {
    candidates.push({
      resourceType: exposedResource.resourceType,
      resourceIndex: exposedResource.resourceIndex ?? exposedResourceIndex,
    });
  });

  if (typeInfo?.resourceType) {
    const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
      ? (node as unknown as { resources?: unknown[] }).resources ?? []
      : [];
    for (let index = 0; index < Math.max(1, resources.length); index += 1) {
      candidates.push({ resourceType: typeInfo.resourceType, resourceIndex: index });
    }
  }

  for (const candidate of candidates) {
    if (candidate.resourceType !== "nam" && candidate.resourceType !== "ir") {
      continue;
    }
    const current = getNodeResourceAtIndex(node, candidate.resourceIndex);
    if (!current.id) {
      continue;
    }
    const resource = getLibraryResource(
      candidate.resourceType,
      getCanonicalLibraryResourceId(candidate.resourceType, current.id),
    );
    const artwork = normalizeResourceArtworkUrl(resource?.metadata?.imageUrl);
    if (artwork) {
      return artwork;
    }
  }

  return "";
}

export /** The stock artwork for this effect type/category, ignoring any loaded model. */
function getEffectVisualizationStockImage(node: GraphNode): string {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  const directTypeMatch = EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE[resolvedType]
    || EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE[node.type];
  if (directTypeMatch) {
    return directTypeMatch;
  }
  const category = getNodeCategory(node);
  return EFFECT_VISUAL_EQUIPMENT_IMAGES[category] || "";
}

export function getEffectVisualizationEquipmentImage(node: GraphNode): string {
  return getNodeResourceArtworkImage(node) || getEffectVisualizationStockImage(node);
}

/** Switch to the Play panel, where the visualization lives, if another panel is up. */
export function showVisualizerPanel(): void {
  const visualizerButton = document.querySelector<HTMLElement>('.icon-bar .icon-btn[data-panel="visualizer"]');
  if (visualizerButton && !visualizerButton.classList.contains("active")) {
    visualizerButton.click();
  }
}

export function updateEffectVisualization(node?: GraphNode): void {
  if (!effectVisualizationElement) {
    return;
  }

  // Remove previous file drop bindings on the visualization element
  if (effectVisualizationDropCleanup) {
    effectVisualizationDropCleanup();
    effectVisualizationDropCleanup = null;
  }

  if (!node) {
    effectVisualizationElement.classList.remove("has-selection");
    effectVisualizationElement.classList.remove("has-equipment-image");
    effectVisualizationElement.classList.remove("nam-ir-drop-target");
    effectVisualizationElement.style.removeProperty("--effect-visual-bg");
    effectVisualizationElement.dataset.effectType = "";
    effectVisualizationElement.dataset.effectCategory = "";
    return;
  }

  const category = getNodeCategory(node);
  const background = EFFECT_VISUAL_BACKGROUNDS[category] || EFFECT_VISUAL_BACKGROUNDS.utility;
  const hasEquipmentImage = Boolean(getEffectVisualizationEquipmentImage(node));

  effectVisualizationElement.classList.add("has-selection");
  effectVisualizationElement.classList.toggle("has-equipment-image", hasEquipmentImage);
  effectVisualizationElement.style.setProperty("--effect-visual-bg", background);
  effectVisualizationElement.dataset.effectType = node.type;
  effectVisualizationElement.dataset.effectCategory = category;

  // Bind file drop for NAM / cab-IR nodes on the effect visualization panel
  if (isNamOrCabIrNode(node)) {
    const nodeId = node.id;
    effectVisualizationElement.classList.add("nam-ir-drop-target");

    const onDragOver = (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
      effectVisualizationElement!.classList.add("drag-over");
    };

    const onDragLeave = (e: DragEvent) => {
      if (!effectVisualizationElement!.contains(e.relatedTarget as Node | null)) {
        effectVisualizationElement!.classList.remove("drag-over");
      }
    };

    const onDrop = (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      effectVisualizationElement!.classList.remove("drag-over");
      const files = Array.from(e.dataTransfer?.files ?? []);
      const file = files[0];
      if (!file) return;
      const resourceType = inferResourceTypeFromFile(file);
      const resolvedNode = getSignalPathPreset()?.graph?.nodes.find((n) => n.id === nodeId);
      if (resourceType && resolvedNode && nodeAcceptsResourceType(resolvedNode, resourceType)) {
        void handleNamIrFileDrop(file, nodeId);
      }
    };

    effectVisualizationElement.addEventListener("dragover", onDragOver);
    effectVisualizationElement.addEventListener("dragleave", onDragLeave);
    effectVisualizationElement.addEventListener("drop", onDrop);

    effectVisualizationDropCleanup = () => {
      effectVisualizationElement!.removeEventListener("dragover", onDragOver);
      effectVisualizationElement!.removeEventListener("dragleave", onDragLeave);
      effectVisualizationElement!.removeEventListener("drop", onDrop);
      effectVisualizationElement!.classList.remove("nam-ir-drop-target");
      effectVisualizationElement!.classList.remove("drag-over");
    };
  } else {
    effectVisualizationElement.classList.remove("nam-ir-drop-target");
  }
}
