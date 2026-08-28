import { getSignalPathPreset } from "../state.js";
import type {
  GraphNode,
} from "../types.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { getLibraryResource } from "../resourceLibrary.js";
import {
  effectVisualizationElement,
} from "./state.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { getCanonicalLibraryResourceId, getNodeCategory, handleNamIrFileDrop, inferResourceTypeFromFile, isNamOrCabIrNode, nodeAcceptsResourceType } from "./nodeTypes.js";

export let effectVisualizationDropCleanup: (() => void) | null = null;

export const EFFECT_VISUAL_BACKGROUNDS: Record<string, string> = {
  amp: "linear-gradient(145deg, rgba(44, 62, 94, 0.92) 0%, rgba(15, 20, 32, 0.96) 100%)",
  cab: "linear-gradient(145deg, rgba(62, 76, 96, 0.92) 0%, rgba(16, 20, 30, 0.96) 100%)",
  eq: "linear-gradient(145deg, rgba(56, 96, 132, 0.95) 0%, rgba(18, 24, 44, 0.95) 100%)",
  dynamics: "linear-gradient(145deg, rgba(132, 64, 64, 0.95) 0%, rgba(38, 18, 24, 0.95) 100%)",
  modulation: "linear-gradient(145deg, rgba(88, 64, 132, 0.95) 0%, rgba(26, 18, 44, 0.95) 100%)",
  delay: "linear-gradient(145deg, rgba(64, 132, 112, 0.95) 0%, rgba(18, 34, 38, 0.95) 100%)",
  reverb: "linear-gradient(145deg, rgba(64, 92, 132, 0.95) 0%, rgba(18, 24, 38, 0.95) 100%)",
  channel: "linear-gradient(145deg, rgba(148, 108, 48, 0.95) 0%, rgba(38, 28, 12, 0.95) 100%)",
  utility: "linear-gradient(145deg, rgba(86, 86, 96, 0.95) 0%, rgba(26, 26, 30, 0.95) 100%)",
};

export const EFFECT_VISUAL_EQUIPMENT_IMAGES: Record<string, string> = {
  amp: "../images/equipment/amps/full-rig-1.jpg",
  cab: "../images/equipment/cabs/cab-02.png",
  delay: "../images/equipment/fx/studio-rack-delay.png",
  reverb: "../images/equipment/fx/studio-rack-reverb.png",
};

export const EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE: Record<string, string> = {
  [EffectGuids.kPluginHost]:"../images/equipment/fx/studio-rack-multifx.png",
  [EffectGuids.kDelayDigital]: "../images/equipment/fx/studio-rack-delay.png",
  [EffectGuids.kDelayDoubler]: "../images/equipment/fx/studio-rack-delay.png",
  [EffectGuids.kFxNam]: "../images/equipment/pedals/colourful-pedal2.png",
  fx_nam: "../images/equipment/pedals/colourful-pedal2.png",
  [EffectGuids.kWasmHost]: "../images/equipment/pedals/colourful-pedal2.png",
  wasm_host: "../images/equipment/pedals/colourful-pedal2.png",
  
};

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
