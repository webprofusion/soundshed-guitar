import { uiState, clonePreset, getActivePresetForRender, getSignalPathPreset, setActivePresetDraft, setFocusedMixerPresetId, setPresetDirty, isCompositeEditMode } from "./state.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import type {
  Preset,
  GraphNode,
  GraphEdge,
  LibraryResource,
  ResourceRef,
  BlendModelMapping,
  BlendMode,
  CustomEffectLibraryEntry,
} from "./types.js";
import { postMessage, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled, removeActivePreset } from "./bridge.js";
import { escapeHtml, idAccentColor } from "./utils.js";
import { showNotification } from "./notifications.js";
import { EffectTypeRegistry, getNodeEffectInfo, type EffectTypeInfo } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon, renderIcon } from "./iconAssets.js";
import {
  CATEGORY_METADATA,
  focusFxSelectorCategory,
  getFxLibraryItems,
  getOrderedFxCategories,
  sendAddSignalPathNode,
  sendAddSignalPathNodeOnEdge,
  type FxLibraryItem,
  type SignalPathEdgeRef,
  type SignalPathNodeOptions,
} from "./fxSelector.js";
import { GenericKnob, enhanceRangeInput } from "./controls.js";
import { getUnsupportedPluginSelection, type PluginResourceSupportInfo } from "./pluginSupport.js";
import {
  EqCurveInteraction,
  buildEqBandConfigsFromParams,
  eqBandChangeToParams,
} from "./eqCurve.js";
import { resourceBrowserModal } from "./resourceBrowser.js";
import { findMatchingResourcePickerLabel } from "./resourcePickerLabel.js";
import { hasCustomLayout, getCustomLayout, renderCustomLayout, renderCustomLayoutBackdrop, formatParamValue, type LayoutResourceControlDef } from "./layoutRenderer.js";
import { layoutDesigner } from "./layoutDesigner.js";
import {
  type BlendParamSpec,
  type BlendParamRange,
  type BlendState,
  BLEND_PARAM_SPECS,
  BLEND_MAPPING_EPS,
  normalizeBlendValue,
  denormalizeBlendValue,
  buildParameterMapFromLegacy,
  computeBlendParamRange,
  getBlendState,
  updateBlendParamIndicators,
  getBlendEntriesForCategory,
  initializeBlendEditorModal,
  openBlendEditorWithDefinition,
  bindBlendEditorControls,
} from "./signalPathBlend.js";
import { getCustomEffectEntry, saveCurrentCustomEffect } from "./customEffects.js";
import { openCustomEffectDesigner } from "./customEffectDesigner.js";
import { createPresetScene, findPresetScene, normalizePresetScenes, removePresetScene, selectPresetScene } from "./presetScenes.js";
export { initializeBlendEditorModal, openBlendEditorWithDefinition } from "./signalPathBlend.js";

const signalPathNodesElement = document.getElementById("signal-path-nodes");
const nodeParamsPanelElement = document.getElementById("node-params-panel");
const signalPathToolbarSceneButton = document.getElementById("signal-path-toolbar-scene-btn") as HTMLButtonElement | null;

/** Whether the Mix tab is currently active in the multi-preset tab bar. */
let mixTabActive = false;
let signalPathEqInteraction: EqCurveInteraction | null = null;
/** Knob instances for the current node params panel, keyed by param key. */
const nodeParamKnobs = new Map<string, GenericKnob>();
const effectVisualizationElement = document.getElementById("effect-visualization");

// Drag-drop state
let draggedNodeId: string | null = null;
let dragOverNodeId: string | null = null;
let selectedNodeId: string | null = null;
let lastSelectedNodeType: string | null = null;
let lastSelectedNodeCategory: string | null = null;
let lastRenderedPresetId: string | null = null;
let overlayBypassClickCleanup: (() => void) | null = null;
let nodeDragStartPoint: { nodeId: string; x: number; y: number } | null = null;
let lastNodeDragPoint: { x: number; y: number } | null = null;
let nodeDragDropHandled = false;
type HostedPluginLoadFailure = {
  selectionKey: string;
  resourceIndex?: number;
  resource: PluginResourceSupportInfo;
  message: string;
};
const hostedPluginLoadFailures = new Map<string, HostedPluginLoadFailure>();

const NODE_BYPASS_DRAG_DISTANCE_PX = 36;
const NODE_BYPASS_DRAG_DIRECTION_RATIO = 1.2;

const EFFECT_VISUAL_BACKGROUNDS: Record<string, string> = {
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

const EFFECT_VISUAL_EQUIPMENT_IMAGES: Record<string, string> = {
  amp: "../images/equipment/amps/full-rig-1.jpg",
  cab: "../images/equipment/cabs/cab-02.png"
};

const EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE: Record<string, string> = {
  [EffectGuids.kFxNam]: "../images/equipment/pedals/colourful-pedal2.png",
  fx_nam: "../images/equipment/pedals/colourful-pedal2.png",
  [EffectGuids.kWasmHost]: "../images/equipment/colourful-pedal2.png",
  wasm_host: "../images/equipment/pedals/colourful-pedal2.png",
};

function getEffectVisualizationEquipmentImage(node: GraphNode): string {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  const directTypeMatch = EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE[resolvedType]
    || EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE[node.type];
  if (directTypeMatch) {
    return directTypeMatch;
  }
  const category = getNodeCategory(node);
  return EFFECT_VISUAL_EQUIPMENT_IMAGES[category] || "";
}

layoutDesigner.onClose(() => {
  refreshSelectedNodeParams();
  renderSignalPathBar();
});

function updateEffectVisualization(node?: GraphNode): void {
  if (!effectVisualizationElement) {
    return;
  }

  if (!node) {
    effectVisualizationElement.classList.remove("has-selection");
    effectVisualizationElement.classList.remove("has-equipment-image");
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
}

export function handleHostedPluginResourceLoadFailed(payload: {
  nodeId?: string;
  resourceType?: string;
  resourceId?: string;
  filePath?: string;
  resourceIndex?: number;
  message?: string;
}): void {
  if (payload.resourceType && payload.resourceType !== "plugin") {
    return;
  }

  const nodeId = payload.nodeId ?? "";
  if (!nodeId) {
    return;
  }

  const resource = (payload.resourceId ? getLibraryResource("plugin", payload.resourceId) : undefined)
    ?? ({ filePath: payload.filePath ?? "" } satisfies PluginResourceSupportInfo);
  const message = payload.message?.trim() || "The selected plugin cannot be hosted by this build.";
  const failure: HostedPluginLoadFailure = {
    selectionKey: getPluginResourceSelectionKey(payload.resourceId, payload.filePath),
    resourceIndex: typeof payload.resourceIndex === "number" ? payload.resourceIndex : undefined,
    resource,
    message,
  };
  hostedPluginLoadFailures.set(nodeId, failure);

  const selectedNode = getSelectedSignalPathNode(getSignalPathPreset());
  if (selectedNode?.id === nodeId && getPluginResourceIndex(selectedNode) !== null) {
    renderHostedPluginWarningIntoOpenPanel(nodeId, failure.resourceIndex, buildHostedPluginLoadErrorMarkup(failure));
    updateEffectVisualization(selectedNode);
  }
}

function updateLastSelectedNode(node: GraphNode): void {
  lastSelectedNodeType = node.type || null;
  lastSelectedNodeCategory = getNodeCategory(node) || null;
}

function buildDefaultParamsForEffect(effectType: string): Record<string, number> {
  const typeInfo = EffectTypeRegistry.get(effectType);
  if (!typeInfo) {
    return {};
  }

  return Object.fromEntries(
    typeInfo.parameters.map((paramDef) => [paramDef.key, paramDef.default]),
  );
}

type CustomEffectDragPayload = {
  customEffectId: string;
  baseEffectType: string;
  name: string;
  category: string;
  moduleResourceType: string;
  moduleResourceId: string;
  defaultParams?: Record<string, number>;
};

function normalizeCustomEffectDefaultParams(value: unknown): Record<string, number> {
  if (!value || typeof value !== "object") {
    return {};
  }

  const result: Record<string, number> = {};
  Object.entries(value as Record<string, unknown>).forEach(([key, rawValue]) => {
    if (typeof rawValue === "number") {
      result[key] = rawValue;
    }
  });
  return result;
}

function parseCustomEffectDragPayload(payloadRaw: string): CustomEffectDragPayload | null {
  if (!payloadRaw) {
    return null;
  }

  try {
    const parsed = JSON.parse(payloadRaw) as Partial<CustomEffectDragPayload>;
    if (
      typeof parsed.customEffectId !== "string" ||
      typeof parsed.baseEffectType !== "string" ||
      typeof parsed.moduleResourceType !== "string" ||
      typeof parsed.moduleResourceId !== "string" ||
      !parsed.customEffectId ||
      !parsed.baseEffectType ||
      !parsed.moduleResourceType ||
      !parsed.moduleResourceId
    ) {
      return null;
    }

    return {
      customEffectId: parsed.customEffectId,
      baseEffectType: parsed.baseEffectType,
      name: typeof parsed.name === "string" ? parsed.name : "Custom Effect",
      category: typeof parsed.category === "string" ? parsed.category : "utility",
      moduleResourceType: parsed.moduleResourceType,
      moduleResourceId: parsed.moduleResourceId,
      defaultParams: normalizeCustomEffectDefaultParams(parsed.defaultParams),
    };
  } catch {
    return null;
  }
}

function parseCustomEffectDefaultParamsDataset(value: string | undefined): Record<string, number> {
  if (!value) {
    return {};
  }

  try {
    return normalizeCustomEffectDefaultParams(JSON.parse(decodeURIComponent(value)));
  } catch {
    return {};
  }
}

function buildCustomEffectNodeOptions(payload: CustomEffectDragPayload): SignalPathNodeOptions {
  const params = payload.defaultParams ?? {};
  return {
    config: { customEffectId: payload.customEffectId },
    label: payload.name || undefined,
    category: payload.category || undefined,
    params: Object.keys(params).length ? params : undefined,
    resources: [
      {
        resourceType: payload.moduleResourceType,
        resourceId: payload.moduleResourceId,
      },
    ],
  };
}

function applyOptimisticNodeReplacement(
  targetNode: GraphNode,
  newEffectType: string,
  preset: Preset,
  options?: SignalPathNodeOptions,
): void {
  const typeInfo = EffectTypeRegistry.get(newEffectType);
  if (!typeInfo) {
    return;
  }

  targetNode.type = newEffectType;
  targetNode.displayName = options?.label || typeInfo.displayName || newEffectType;
  targetNode.category = options?.category || typeInfo.category || targetNode.category;
  targetNode.params = buildDefaultParamsForEffect(newEffectType);
  if (options?.params) {
    targetNode.params = { ...targetNode.params, ...options.params };
  }
  targetNode.config = options?.config ? { ...options.config } : {};
  targetNode.resources = options?.resources?.length
    ? options.resources.map((resource) => ({
        ...resource,
        parameters: resource.parameters ? { ...resource.parameters } : undefined,
      }))
    : (typeInfo.requiresResource ? [] : undefined);
  (targetNode as unknown as { enabled?: boolean }).enabled = true;
  targetNode.bypassed = false;

  selectedNodeId = targetNode.id;
  updateLastSelectedNode(targetNode);
  renderSignalPathBar();
  showNodeParamsPanel(targetNode, preset);

  const visualizerButton = document.querySelector(
    '.icon-bar .icon-btn[data-panel="visualizer"]',
  ) as HTMLElement | null;
  if (visualizerButton && !visualizerButton.classList.contains("active")) {
    visualizerButton.click();
  }
}

function selectNodeForPreset(preset: Preset, presetChanged: boolean): void {
  const nodes = preset.graph?.nodes ?? [];
  if (!nodes.length) {
    selectedNodeId = null;
    nodeParamsPanelElement?.classList.remove("visible");
    updateEffectVisualization();
    return;
  }

  // Keep the current node only while rendering the same preset.
  const currentNode = selectedNodeId ? nodes.find((node) => node.id === selectedNodeId) : undefined;
  if (!presetChanged && currentNode) {
    return;
  }

  const matchesCategory = (node: GraphNode): boolean => {
    if (!lastSelectedNodeCategory) return true;
    return getNodeCategory(node) === lastSelectedNodeCategory;
  };

  let replacement: GraphNode | undefined;

  // On first render of a newly selected preset, prefer the first optimized NAM amp node.
  if (presetChanged) {
    replacement = nodes.find((node) => node.type === EffectGuids.kAmpNamOptimized || node.type === "amp_nam_optimized");
  }

  if (!replacement && lastSelectedNodeType) {
    replacement = nodes.find((node) => node.type === lastSelectedNodeType && matchesCategory(node));
    if (!replacement) {
      replacement = nodes.find((node) => node.type === lastSelectedNodeType);
    }
  }
  if (!replacement && lastSelectedNodeCategory) {
    replacement = nodes.find((node) => getNodeCategory(node) === lastSelectedNodeCategory);
  }
  if (!replacement) {
    replacement = currentNode ?? nodes[0];
  }

  selectedNodeId = replacement?.id ?? null;
  if (replacement) {
    updateLastSelectedNode(replacement);
  }

  if (nodeParamsPanelElement?.classList.contains("visible") && replacement) {
    showNodeParamsPanel(replacement, preset);
  } else {
    updateEffectVisualization(replacement);
  }
}

function getNodeIcon(nodeType: string): string {
  return getFxEffectIcon(nodeType);
}

function getCategoryClass(category: string): string {
  const categoryMap: Record<string, string> = {
    "dynamics": "dynamics",
    "amp": "amp",
    "pedal": "amp",
    "preamp": "amp",
    "full-rig": "amp",
    "channel": "amp",
    "cab": "cab",
    "eq": "eq",
    "modulation": "modulation",
    "delay": "delay",
    "reverb": "reverb",
    "utility": "utility",
  };
  return categoryMap[category] || "utility";
}

function getResourceBaseName(filePath: string): string {
  const normalized = filePath.replace(/\\/g, "/");
  return normalized.split("/").pop() || filePath;
}

function getLibraryResource(resourceType: string | undefined, resourceId: string): LibraryResource | undefined {
  if (!resourceType || !resourceId) return undefined;
  const resources = uiState.resourceLibrary[resourceType] || [];
  return resources.find((res) => res.id === resourceId);
}

function getLibraryResourceName(resourceType: string | undefined, resourceId: string): string {
  const match = getLibraryResource(resourceType, resourceId);
  return match?.name?.trim() ?? "";
}

function getPluginResourceIndex(node: GraphNode): number | null {
  const typeInfo = getNodeEffectInfo(node);
  if (typeInfo?.resourceType === "plugin") {
    return 0;
  }

  const exposedPluginResource = typeInfo?.exposedResources?.find((resource) => resource.resourceType === "plugin");
  if (exposedPluginResource) {
    return exposedPluginResource.resourceIndex ?? 0;
  }

  const resources = (node as unknown as { resources?: ResourceRef[] }).resources;
  if (Array.isArray(resources)) {
    const index = resources.findIndex((resource) => (resource.resourceType ?? resource.type) === "plugin");
    if (index >= 0) {
      return index;
    }
  }

  return null;
}

function getPluginResourceSelectionKey(resourceId?: string, filePath?: string): string {
  if (resourceId) {
    return `id:${resourceId}`;
  }
  if (filePath) {
    return `file:${filePath.replace(/\\/g, "/").toLowerCase()}`;
  }
  return "";
}

function getPluginResourceSupportInfoAtIndex(node: GraphNode, resourceIndex: number): PluginResourceSupportInfo | null {
  const current = getNodeResourceAtIndex(node, resourceIndex);
  if (current.id) {
    const resource = getLibraryResource("plugin", current.id);
    if (resource) {
      return resource;
    }
  }

  if (current.filePath) {
    return { filePath: current.filePath };
  }

  return null;
}

function getHostedPluginLoadFailureForResource(node: GraphNode, resourceIndex: number): HostedPluginLoadFailure | null {
  const failure = hostedPluginLoadFailures.get(node.id);
  if (!failure) {
    return null;
  }

  if (failure.resourceIndex !== undefined && failure.resourceIndex !== resourceIndex) {
    return null;
  }

  const current = getNodeResourceAtIndex(node, resourceIndex);
  const currentSelectionKey = getPluginResourceSelectionKey(current.id, current.filePath);
  if (failure.selectionKey && currentSelectionKey && failure.selectionKey !== currentSelectionKey) {
    return null;
  }

  return failure;
}

function buildHostedPluginLoadErrorMarkup(failure: HostedPluginLoadFailure): string {
  const unsupportedPlugin = getUnsupportedPluginSelection(failure.resource);
  const title = unsupportedPlugin ? "Selected Plugin Type Not Supported" : "Plugin Load Error";
  const detail = failure.message.trim() || "The selected plugin cannot be hosted by this build.";
  return buildHostedPluginWarningMarkup(title, detail);
}

function buildUnsupportedPluginWarningMarkup(resource: PluginResourceSupportInfo | null | undefined): string {
  const unsupportedPlugin = getUnsupportedPluginSelection(resource);
  if (!unsupportedPlugin) {
    return "";
  }

  return buildHostedPluginWarningMarkup(
    "Selected Plugin Type Not Supported",
    `${unsupportedPlugin.label} plugins cannot be hosted by this build.`,
  );
}

function buildHostedPluginWarningMarkup(title: string, detail: string): string {
  return `
    <div class="plugin-host-load-error" role="status" aria-live="polite">
      <div class="plugin-host-load-error-title">${escapeHtml(title)}</div>
      <div class="plugin-host-load-error-detail">${escapeHtml(detail)}</div>
    </div>
  `;
}

function buildHostedPluginLoadErrorHtml(node: GraphNode, resourceIndex: number): string {
  const failure = getHostedPluginLoadFailureForResource(node, resourceIndex);
  if (failure) {
    return buildHostedPluginLoadErrorMarkup(failure);
  }

  return buildUnsupportedPluginWarningMarkup(getPluginResourceSupportInfoAtIndex(node, resourceIndex));
}

function renderHostedPluginWarningIntoOpenPanel(nodeId: string, resourceIndex: number | undefined, warningHtml: string): void {
  if (!nodeParamsPanelElement?.classList.contains("visible")) {
    return;
  }

  const pluginControls = Array.from(
    nodeParamsPanelElement.querySelectorAll<HTMLElement>(`.resource-dropdown[data-resource-type="plugin"], .resource-picker-btn[data-resource-type="plugin"]`),
  );
  const targetControl = pluginControls.find((control) => {
    if (control.dataset.nodeId !== nodeId) {
      return false;
    }
    if (resourceIndex === undefined) {
      return true;
    }
    const controlResourceIndex = control.dataset.resourceIndex ? parseInt(control.dataset.resourceIndex, 10) : 0;
    return controlResourceIndex === resourceIndex;
  });
  const container = targetControl?.closest(".node-resource-selector");
  if (!container) {
    return;
  }

  container.querySelector(".plugin-host-load-error")?.remove();
  const controlsRow = container.querySelector(".resource-controls");
  if (warningHtml) {
    controlsRow?.insertAdjacentHTML("afterend", warningHtml);
  }
}

function clearInlineHostedPluginLoadError(source: Element): void {
  source.closest(".node-resource-selector")?.querySelector(".plugin-host-load-error")?.remove();
}

function getNodeResourceDisplayName(node: GraphNode, index = 0, overrideResourceType?: string): string {
  const typeInfo = getNodeEffectInfo(node);
  const resourceType = overrideResourceType || typeInfo?.resourceType;
  const resource = getNodeResourceAtIndex(node, index);

  if (resource.filePath) {
    return getResourceBaseName(resource.filePath);
  }

  const libraryName = getLibraryResourceName(resourceType, resource.id);
  return libraryName || resource.id;
}

function getNodeResourceSummary(node: GraphNode): string {
  const anyNode = node as unknown as { resources?: unknown };
  if (Array.isArray(anyNode.resources)) {
    const names = anyNode.resources
      .map((_, index) => getNodeResourceDisplayName(node, index))
      .filter((name) => Boolean(name));
    if (names.length > 1) return names.join(" + ");
    if (names.length === 1) return names[0];
  }

  return getNodeResourceDisplayName(node, 0);
}

function getMissingResourceEntries(node: GraphNode): Array<{ resourceType?: string; resourceId?: string; filePath?: string }> {
  const typeInfo = getNodeEffectInfo(node);
  const backendMissing = (uiState.missingNodeResources ?? []).filter((entry) => entry.nodeId === node.id);

  const refs: Array<{ resourceType?: string; resourceId?: string; filePath?: string }> = [];
  const addRef = (ref?: ResourceRef): void => {
    if (!ref) return;
    const resourceType = ref.type || typeInfo?.resourceType;
    const resourceId = ref.id;
    const filePath = ref.filePath;
    refs.push({ resourceType, resourceId, filePath });
  };

  if (Array.isArray(node.resources)) {
    node.resources.forEach((ref) => addRef(ref));
  }

  const isBackendMissing = (entry: { resourceType?: string; resourceId?: string; filePath?: string }): boolean => {
    return backendMissing.some((missing) => {
      if (entry.filePath && missing.filePath) {
        return entry.filePath === missing.filePath;
      }
      if (entry.resourceType && entry.resourceId && missing.resourceType && missing.resourceId) {
        return entry.resourceType === missing.resourceType && entry.resourceId === missing.resourceId;
      }
      return false;
    });
  };

  return refs.filter((entry) => {
    if (entry.resourceType && entry.resourceId) {
      const resource = getLibraryResource(entry.resourceType, entry.resourceId);
      if (!resource) {
        return true;
      }
      if (resource.fileMissing === true) {
        return true;
      }
      return false;
    }

    if (entry.filePath) {
      return isBackendMissing(entry);
    }

    return true;
  });
}

function buildMissingResourceTooltip(entries: Array<{ resourceType?: string; resourceId?: string; filePath?: string }>): string {
  if (!entries.length) {
    return "";
  }
  const details = entries.map((entry) => {
    if (entry.filePath) {
      return entry.filePath;
    }
    if (entry.resourceType && entry.resourceId) {
      return `${entry.resourceType}:${entry.resourceId}`;
    }
    return "Missing resource";
  });
  return `Missing resource file: ${details.join(", ")}`;
}

function getNodeDisplayName(node: GraphNode): string {
  // Support backend presets that use label/enabled instead of displayName/bypassed.
  const anyNode = node as unknown as { id?: unknown; type?: unknown; displayName?: unknown; label?: unknown };
  const nodeId = typeof anyNode.id === "string" ? anyNode.id : "";
  const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";

  if (nodeId === "__input__" || nodeType === "input") return "Input";
  if (nodeId === "__output__" || nodeType === "output") return "Output";

  const explicit = typeof anyNode.displayName === "string" && anyNode.displayName.trim()
    ? anyNode.displayName.trim()
    : (typeof anyNode.label === "string" && anyNode.label.trim() ? anyNode.label.trim() : "");
  const typeInfo = getNodeEffectInfo(node);
  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (blendId) {
    const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
    if (blend?.name) {
      return blend.name;
    }
  }

  const customEffectId = (node as unknown as { config?: Record<string, string> }).config?.customEffectId;
  if (customEffectId) {
    const customEffect = uiState.customEffectLibrary?.find((entry) => entry.id === customEffectId);
    if (customEffect?.name) {
      return customEffect.name;
    }
  }

  const resourceTitle = typeInfo?.requiresResource ? getNodeResourceSummary(node) : "";
  if (resourceTitle) return resourceTitle;

  if (explicit && explicit !== (typeInfo?.displayName || "")) {
    return explicit;
  }

  if (explicit) return explicit;
  return typeInfo?.displayName || nodeType || "(Unknown)";
}

function getLinkedCustomEffectEntry(node: GraphNode): CustomEffectLibraryEntry | undefined {
  const customEffectId = (node as unknown as { config?: Record<string, string> }).config?.customEffectId ?? "";
  return customEffectId ? getCustomEffectEntry(customEffectId) : undefined;
}

function hasCustomEffectModuleSelection(node: GraphNode): boolean {
  const resource = getNodeResourceAtIndex(node, 0);
  return Boolean(resource.id || resource.filePath);
}

function buildCustomEffectActionStatus(node: GraphNode): string {
  const linkedEntry = getLinkedCustomEffectEntry(node);
  if (linkedEntry?.name) {
    return `Linked to ${linkedEntry.name} in My Custom Effects. You can also prompt a new revision for this node.`;
  }

  if (hasCustomEffectModuleSelection(node)) {
    return `Current module: ${getNodeResourceDisplayName(node, 0, "wasm") || "WASM module selected"}. Prompt a new module or save this one to My Custom Effects.`;
  }

  return "Describe a Custom Effect to generate a new module for this node, then save or apply it here.";
}

function buildCustomEffectActions(node: GraphNode): string {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kWasmHost) {
    return "";
  }

  if (!isFeatureEnabled(Features.CustomEffects)) {
    return "";
  }

  const hasModule = hasCustomEffectModuleSelection(node);
  const linkedEntry = getLinkedCustomEffectEntry(node);
  const saveLabel = linkedEntry ? "Update My Custom Effect" : "Save To My Custom Effects";

  return `
    <div class="node-resource-selector node-custom-effect-actions" data-node-id="${node.id}">
      <label>Custom Effect Designer</label>
      <div class="resource-controls">
        <button type="button" class="primary-btn custom-effect-design-btn" data-node-id="${node.id}">Design With AI</button>
        <button type="button" class="primary-btn custom-effect-save-btn" data-node-id="${node.id}" ${hasModule ? "" : "disabled"}>${saveLabel}</button>
        <button type="button" class="secondary-btn custom-effect-use-btn" data-node-id="${node.id}" ${hasModule ? "" : "disabled"}>Use This Effect</button>
      </div>
      <div class="resource-path-info">${escapeHtml(buildCustomEffectActionStatus(node))}</div>
    </div>
  `;
}

function promptSaveCurrentCustomEffect(node: GraphNode, applyToNode: boolean): void {
  if (!hasCustomEffectModuleSelection(node)) {
    showNotification("Custom Effect save failed", "Select a WASM module first");
    return;
  }

  const linkedEntry = getLinkedCustomEffectEntry(node);
  const typeInfo = getNodeEffectInfo(node);

  const suggestedName = linkedEntry?.name
    || getNodeDisplayName(node)
    || typeInfo?.displayName
    || "Custom Effect";
  const rawName = window.prompt("Custom Effect name", suggestedName);
  if (rawName === null) {
    return;
  }

  const name = rawName.trim();
  if (!name) {
    showNotification("Custom Effect save failed", "A name is required");
    return;
  }

  const suggestedCategory = linkedEntry?.category
    || getNodeCategory(node)
    || typeInfo?.category
    || "utility";
  const rawCategory = window.prompt("Category", suggestedCategory);
  if (rawCategory === null) {
    return;
  }

  const descriptionDefault = linkedEntry?.description ?? typeInfo?.description ?? "";
  const rawDescription = window.prompt("Description", descriptionDefault);
  if (rawDescription === null) {
    return;
  }

  saveCurrentCustomEffect(node.id, {
    ...(linkedEntry?.id ? { id: linkedEntry.id } : {}),
    name,
    category: rawCategory.trim() || suggestedCategory,
    description: rawDescription.trim(),
    origin: linkedEntry?.origin ?? "imported",
  }, applyToNode);
}

function getNodeCategory(node: GraphNode): string {
  const anyNode = node as unknown as { category?: unknown; type?: unknown };
  const explicit = typeof anyNode.category === "string" ? anyNode.category : "";
  if (explicit) return explicit;
  const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";
  const typeInfo = getNodeEffectInfo(node);
  const category = typeInfo?.category || "utility";
  if (category === "pedal" || category === "preamp" || category === "full-rig") {
    return "amp";
  }
  return category;
}

function isNodeBypassed(node: GraphNode): boolean {
  const anyNode = node as unknown as { bypassed?: unknown; enabled?: unknown };
  if (typeof anyNode.bypassed === "boolean") return anyNode.bypassed;
  if (typeof anyNode.enabled === "boolean") return !anyNode.enabled;
  return false;
}

function applySignalPathNodeBypassState(node: GraphNode, preset: Preset, bypassed: boolean): void {
  sendSignalPathNodeBypassUpdate(node.id, preset.id, bypassed);
  (node as unknown as { bypassed?: boolean }).bypassed = bypassed;
  (node as unknown as { enabled?: boolean }).enabled = !bypassed;
  renderSignalPathBar();
  if (selectedNodeId === node.id && nodeParamsPanelElement?.classList.contains("visible")) {
    showNodeParamsPanel(node, preset);
  }
  if (selectedNodeId === node.id) {
    queueMicrotask(() => {
      const selectedNode = signalPathNodesElement?.querySelector<HTMLElement>(`.signal-node[data-node-id="${node.id}"]`);
      selectedNode?.focus({ preventScroll: true });
    });
  }
}

function toggleSignalPathNodeBypass(node: GraphNode, preset: Preset): void {
  applySignalPathNodeBypassState(node, preset, !isNodeBypassed(node));
}

function isProtectedSignalPathNode(node: GraphNode): boolean {
  return node.type === EffectGuids.kSplitter || node.type === EffectGuids.kMixer;
}

function isToggleableSignalPathNode(node: GraphNode | null | undefined): node is GraphNode {
  if (!node) {
    return false;
  }

  return !isProtectedSignalPathNode(node);
}

function getSelectedSignalPathNode(preset: Preset | null | undefined): GraphNode | null {
  if (!selectedNodeId || !preset?.graph) {
    return null;
  }

  return preset.graph.nodes.find((node) => node.id === selectedNodeId) ?? null;
}

function isTextEntryElement(element: HTMLElement | null): boolean {
  if (!element) {
    return false;
  }

  if (element.isContentEditable) {
    return true;
  }

  const editableRoot = element.closest("input, textarea, select, [contenteditable=''], [contenteditable='true'], [role='textbox']");
  return Boolean(editableRoot);
}

function isSignalPathShortcutSuppressedElement(element: HTMLElement | null): boolean {
  if (!element) {
    return false;
  }

  // Allow Space to toggle bypass in visualization/panel context unless the user
  // is actively editing a value (text/range/knob/select/etc.).
  return Boolean(
    element.closest(
      "input, textarea, select, [contenteditable=''], [contenteditable='true'], [role='textbox'], [role='checkbox'], [role='slider'], .node-param-knob, .resource-dropdown, .resource-param-value",
    ),
  );
}

function isSignalPathShortcutContext(element: HTMLElement | null): boolean {
  if (!element) {
    return false;
  }

  return Boolean(element.closest("#signal-path-bar, #signal-path-nodes, #node-params-panel, #effect-visualization"));
}

function resolveSignalPathShortcutNode(target: HTMLElement | null, preset: Preset | null | undefined): GraphNode | null {
  if (!preset?.graph) {
    return null;
  }

  const activeElement = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  const focusedNodeElement = (target?.closest(".signal-node[data-node-id]") as HTMLElement | null)
    ?? (activeElement?.closest(".signal-node[data-node-id]") as HTMLElement | null);
  const focusedNodeId = focusedNodeElement?.dataset.nodeId;
  if (focusedNodeId) {
    return preset.graph.nodes.find((node) => node.id === focusedNodeId) ?? null;
  }

  return getSelectedSignalPathNode(preset);
}

function toggleSelectedSignalPathNodeBypass(): boolean {
  const preset = getSignalPathPreset();
  const node = getSelectedSignalPathNode(preset);
  if (!preset || !isToggleableSignalPathNode(node)) {
    return false;
  }

  toggleSignalPathNodeBypass(node, preset);
  return true;
}

function handleSignalPathShortcutKeyDown(event: KeyboardEvent): void {
  if (event.code !== "Space" || event.repeat || event.altKey || event.ctrlKey || event.metaKey) {
    return;
  }

  const target = event.target instanceof HTMLElement ? event.target : null;
  const activeElement = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  const shortcutContextElement = target ?? activeElement;
  if (!isSignalPathShortcutContext(shortcutContextElement)) {
    return;
  }

  if (isTextEntryElement(shortcutContextElement) || isSignalPathShortcutSuppressedElement(shortcutContextElement)) {
    return;
  }

  const preset = getSignalPathPreset();
  const node = resolveSignalPathShortcutNode(target, preset);
  if (!preset || !isToggleableSignalPathNode(node)) {
    return;
  }

  selectedNodeId = node.id;
  updateLastSelectedNode(node);
  toggleSignalPathNodeBypass(node, preset);

  event.preventDefault();
  event.stopImmediatePropagation();
}

document.addEventListener("keydown", handleSignalPathShortcutKeyDown, true);

function updateNodeDragPoint(event: DragEvent): void {
  if (Number.isFinite(event.clientX) && Number.isFinite(event.clientY)) {
    lastNodeDragPoint = { x: event.clientX, y: event.clientY };
  }
}

function shouldToggleNodeBypassFromDrag(event: DragEvent): boolean {
  if (!nodeDragStartPoint || nodeDragDropHandled || !draggedNodeId) {
    return false;
  }

  const endX = Number.isFinite(event.clientX) && event.clientX !== 0 ? event.clientX : lastNodeDragPoint?.x;
  const endY = Number.isFinite(event.clientY) && event.clientY !== 0 ? event.clientY : lastNodeDragPoint?.y;

  if (typeof endX !== "number" || typeof endY !== "number") {
    return false;
  }

  const deltaX = endX - nodeDragStartPoint.x;
  const deltaY = endY - nodeDragStartPoint.y;
  return Math.abs(deltaY) >= NODE_BYPASS_DRAG_DISTANCE_PX
    && Math.abs(deltaY) > Math.abs(deltaX) * NODE_BYPASS_DRAG_DIRECTION_RATIO;
}

function getNodeResourceAtIndex(node: GraphNode, index = 0): { id: string; filePath: string; parameterValue?: number } {
  const anyNode = node as unknown as {
    resources?: unknown;
  };

  if (Array.isArray(anyNode.resources)) {
    const res = anyNode.resources[index] as { id?: unknown; resourceId?: unknown; embeddedId?: unknown; filePath?: unknown; parameterValue?: unknown } | undefined;
    const id = typeof res?.id === "string"
      ? res.id
      : (typeof res?.resourceId === "string"
        ? res.resourceId
        : (typeof res?.embeddedId === "string" ? res.embeddedId : ""));
    const filePath = typeof res?.filePath === "string" ? res.filePath : "";
    const parameterValue = typeof res?.parameterValue === "number" ? res.parameterValue : undefined;
    return { id, filePath, parameterValue };
  }

  return { id: "", filePath: "" };
}

export function renderSignalPathBar(): void {
  if (!signalPathNodesElement) {
    return;
  }

  const signalPathBar = document.getElementById("signal-path-bar");
  const sceneToolbarHost = document.getElementById("signal-path-scene-toolbar");
  const toolbarRow = document.getElementById("signal-path-toolbar");
  signalPathBar?.classList.toggle("mix-tab-active", mixTabActive);

  // Show/hide composite edit mode banner
  updateCompositeEditBanner();

  // Render preset selection tabs and scene controls in a single bar.
  renderMixerPresetTabs();

  // Show inline mixer panel instead of signal chain when Mix tab is active
  const scroll = document.querySelector<HTMLElement>(".signal-path-scroll");
  if (mixTabActive) {
    if (scroll) scroll.hidden = true;
    if (sceneToolbarHost) sceneToolbarHost.innerHTML = "";
    toolbarRow?.classList.add("scene-toolbar-empty");
    renderInlineMixer();
    return;
  }
  if (scroll) scroll.hidden = false;
  removeInlineMixer();

  const activePresetId = uiState.activePresetId;
  const activePreset = getSignalPathPreset() ?? undefined;
  if (signalPathToolbarSceneButton) {
    const showToolbarSceneButton = Boolean(activePreset) && !mixTabActive;
    signalPathToolbarSceneButton.disabled = !activePreset;
    signalPathToolbarSceneButton.setAttribute("aria-hidden", String(!showToolbarSceneButton));
  }
  // Track the rendered preset's own ID so that switching mixer tabs (which
  // changes focusedMixerPresetId but NOT activePresetId) is also detected.
  const renderedPresetId = activePreset?.id ?? activePresetId;
  const presetChanged = renderedPresetId !== lastRenderedPresetId;
  lastRenderedPresetId = renderedPresetId ?? null;
  
  if (!activePreset) {
    signalPathNodesElement.innerHTML = "";
    if (sceneToolbarHost) sceneToolbarHost.innerHTML = "";
    toolbarRow?.classList.add("scene-toolbar-empty");
    updateEffectVisualization();
    return;
  }

  const editablePreset = getEditableSignalPathPreset(activePreset);

  // Render graph-based signal path (supports parallel paths)
  if (editablePreset.graph?.nodes) {
    selectNodeForPreset(editablePreset, presetChanged);
    renderGraphSignalPath(editablePreset);
  } else {
    // Empty preset - show only input/output
    signalPathNodesElement.innerHTML = `
      <div class="signal-graph-container">
        <div class="signal-graph-row">
          <div class="signal-node input-node" data-node-id="__input__">
            <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/guitar.svg')" aria-hidden="true"></span></div>
            <div class="node-info">
              <div class="node-name">Input</div>
            </div>
            <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
          </div>
          <div class="signal-connector-wrapper">
            <div class="signal-connector"></div>
            <button class="signal-add-btn" 
                    data-insert-after="__input__"
                    title="Add Effect">
              <span class="add-icon">+</span>
            </button>
          </div>
          <div class="signal-node output-node" data-node-id="__output__">
            <div class="node-icon">🔈</div>
            <div class="node-info">
              <div class="node-name">Output</div>
            </div>
            <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
          </div>
        </div>
      </div>
    `;

    // Bind minimal handlers (legacy fallback uses insertAfter=__input__)
    bindAddButtonHandlers();
  }

  if (sceneToolbarHost) {
    const activeSceneId = normalizePresetScenes(editablePreset, uiState.activePresetSceneId ?? undefined);
    uiState.activePresetSceneId = activeSceneId;
    const sceneMarkup = buildPresetScenePanelMarkup(editablePreset, activeSceneId ?? "");
    sceneToolbarHost.innerHTML = sceneMarkup;
    toolbarRow?.classList.toggle("scene-toolbar-empty", !sceneMarkup);
    const scenePanel = sceneToolbarHost.querySelector<HTMLElement>(".mixer-preset-scene-panel");
    if (scenePanel) {
      bindPresetScenePanel(scenePanel, editablePreset);
    }
  }

  updateSignalPathClipIndicators();
  if (!selectedNodeId) {
    updateEffectVisualization();
  }
}

export function refreshSelectedNodeParams(): void {
  if (!selectedNodeId) {
    return;
  }
  const activePresetId = uiState.activePresetId;
  const activePreset = getActivePresetForRender() ?? undefined;
  if (!activePreset?.graph) {
    return;
  }
  const node = activePreset.graph.nodes.find((n) => n.id === selectedNodeId);
  if (!node) {
    return;
  }
  showNodeParamsPanel(node, activePreset);
}

function getSelectedNodeDiagnostics(): import("./types.js").SignalLevelMetrics | null {
  if (!selectedNodeId) {
    return null;
  }

  const diagnostics = uiState.signalDiagnostics;
  if (!diagnostics) {
    return null;
  }

  if (selectedNodeId === "__input__") {
    return diagnostics.input ?? null;
  }
  if (selectedNodeId === "__output__") {
    return diagnostics.output ?? null;
  }

  const nodeMetrics = diagnostics.nodes.find((entry) => entry.nodeId === selectedNodeId);
  return nodeMetrics?.levels ?? null;
}

function normalizePeakDbfsForShellMeter(peakDbfs: number): number {
  const minDbfs = -48;
  const maxDbfs = 0;
  const normalized = (peakDbfs - minDbfs) / (maxDbfs - minDbfs);
  return Math.max(0, Math.min(1, normalized));
}

export function updateSelectedNodePeakMeter(): void {
  const rail = nodeParamsPanelElement?.querySelector(".default-effect-shell-rail") as HTMLElement | null;
  if (!rail) {
    return;
  }

  const meter = rail.querySelector<HTMLElement>(".default-effect-shell-meter");
  if (!meter) {
    return;
  }

  const metrics = getSelectedNodeDiagnostics();
  rail.classList.remove("is-inactive", "is-clipped");

  if (!metrics || !Number.isFinite(metrics.peakDbfs)) {
    rail.classList.add("is-inactive");
    rail.title = "No diagnostics data for this node";
    meter.style.setProperty("--meter-fill", "0%");
    return;
  }

  const normalized = normalizePeakDbfsForShellMeter(metrics.peakDbfs);
  meter.style.setProperty("--meter-fill", `${(normalized * 100).toFixed(1)}%`);

  if (metrics.clipped || metrics.peakDbfs >= -0.3) {
    rail.classList.add("is-clipped");
  }

  rail.title = `Node peak: ${metrics.peakDbfs.toFixed(1)} dBFS · Headroom: ${metrics.headroomDb.toFixed(1)} dB`;
}

type EdgeRef = SignalPathEdgeRef & { gain: number };

function normalizeEdge(edge: Partial<GraphEdge>): EdgeRef {
  return {
    from: String(edge.from ?? ""),
    to: String(edge.to ?? ""),
    fromPort: typeof edge.fromPort === "number" ? edge.fromPort : 0,
    toPort: typeof edge.toPort === "number" ? edge.toPort : 0,
    gain: typeof edge.gain === "number" ? edge.gain : 1.0,
  };
}

function parseEdgeFromDataset(el: HTMLElement): EdgeRef | null {
  const from = el.dataset.edgeFrom;
  const to = el.dataset.edgeTo;
  if (!from || !to) return null;
  const fromPort = Number(el.dataset.edgeFromPort ?? "0");
  const toPort = Number(el.dataset.edgeToPort ?? "0");
  const gain = Number(el.dataset.edgeGain ?? "1");
  return { from, to, fromPort, toPort, gain };
}

function sortEdgesByPort(edges: EdgeRef[]): EdgeRef[] {
  return edges.slice().sort((a, b) => (a.fromPort - b.fromPort) || (a.toPort - b.toPort) || a.to.localeCompare(b.to));
}

function buildGraphMaps(graph: NonNullable<Preset["graph"]>): {
  nodeById: Map<string, GraphNode>;
  outgoing: Map<string, EdgeRef[]>;
  incoming: Map<string, EdgeRef[]>;
} {
  const nodeById = new Map<string, GraphNode>(graph.nodes.map((n) => [n.id, n]));
  const outgoing = new Map<string, EdgeRef[]>();
  const incoming = new Map<string, EdgeRef[]>();

  graph.edges.forEach((e) => {
    const edge = normalizeEdge(e);
    if (!edge.from || !edge.to) return;
    if (!outgoing.has(edge.from)) outgoing.set(edge.from, []);
    if (!incoming.has(edge.to)) incoming.set(edge.to, []);
    outgoing.get(edge.from)!.push(edge);
    incoming.get(edge.to)!.push(edge);
  });

  // Normalize ordering for stable render
  outgoing.forEach((list, key) => outgoing.set(key, sortEdgesByPort(list)));
  incoming.forEach((list, key) => incoming.set(key, sortEdgesByPort(list)));

  return { nodeById, outgoing, incoming };
}

function pickPrimaryOutgoingEdge(outgoing: Map<string, EdgeRef[]>, fromId: string): EdgeRef | null {
  const outs = outgoing.get(fromId) ?? [];
  if (!outs.length) return null;
  // Prefer port 0 if present
  const port0 = outs.find((e) => e.fromPort === 0);
  return port0 ?? outs[0];
}

function renderConnectorWrapper(edge: EdgeRef, opts?: { showSplit?: boolean }): string {
  const showSplit = opts?.showSplit ?? true;
  return `
    <div class="signal-connector-wrapper"
         data-edge-from="${edge.from}"
         data-edge-to="${edge.to}"
         data-edge-from-port="${edge.fromPort}"
         data-edge-to-port="${edge.toPort}"
         data-edge-gain="${edge.gain}">
      <div class="signal-connector"></div>
      <button class="signal-add-btn"
              data-edge-from="${edge.from}"
              data-edge-to="${edge.to}"
              data-edge-from-port="${edge.fromPort}"
              data-edge-to-port="${edge.toPort}"
              data-edge-gain="${edge.gain}"
              title="Add Effect">
        <span class="add-icon">+</span>
      </button>
    </div>
  `;
}

/**
 * Renders the signal path graph with support for parallel branches.
 */
function renderGraphSignalPath(preset: Preset): void {
  if (!signalPathNodesElement || !preset.graph) {
    return;
  }

  const { nodeById, outgoing } = buildGraphMaps(preset.graph);

  const getOutgoingEdges = (nodeId: string): EdgeRef[] => outgoing.get(nodeId) ?? [];
  const isSplitPoint = (nodeId: string): boolean => getOutgoingEdges(nodeId).length >= 2;

  // Finds the first downstream node where all branches converge.
  // This intentionally supports library presets that model split/join using ordinary nodes (e.g. gain nodes).
  const findJoinNodeId = (splitterId: string, outs: EdgeRef[]): string | null => {
    if (outs.length < 2) return null;

    const walkBranch = (startNodeId: string): string[] => {
      const path: string[] = [];
      let currentId = startNodeId;
      const localVisited = new Set<string>();
      let guard = 0;
      while (currentId && !localVisited.has(currentId) && guard++ < 500) {
        localVisited.add(currentId);
        if (currentId === "__output__") break;
        path.push(currentId);

        const edge = pickPrimaryOutgoingEdge(outgoing, currentId);
        if (!edge) break;
        currentId = edge.to;
      }
      return path;
    };

    const branchPaths = outs
      .map((e) => e.to)
      .filter((to) => to && to !== "__output__")
      .map(walkBranch);

    if (branchPaths.length < 2) return null;

    const candidateSet = new Set(branchPaths[0]);
    for (let i = 1; i < branchPaths.length; i++) {
      for (const id of Array.from(candidateSet)) {
        if (!branchPaths[i].includes(id)) {
          candidateSet.delete(id);
        }
      }
    }

    candidateSet.delete(splitterId);

    // Prefer the earliest common node along the first branch.
    for (const id of branchPaths[0]) {
      if (candidateSet.has(id)) {
        return id;
      }
    }

    return null;
  };

  const visited = new Set<string>();

  const renderParallelForSplitter = (splitterId: string): { html: string; mixerId: string | null } => {
    const outs = getOutgoingEdges(splitterId);
    if (outs.length < 2) return { html: "", mixerId: null };

    const joinId = findJoinNodeId(splitterId, outs);
    if (!joinId) {
      // Unsupported/ambiguous topology - render as a linear edge fallback
      return { html: "", mixerId: null };
    }

    const joinNode = nodeById.get(joinId);
    const canCollapse = nodeById.get(splitterId)?.type === EffectGuids.kSplitter && joinNode?.type === EffectGuids.kMixer;

    const renderBranch = (firstEdge: EdgeRef): string => {
      let html = "";
      let edge: EdgeRef | null = firstEdge;
      let guard = 0;
      while (edge && guard++ < 200) {
        html += renderConnectorWrapper(edge, { showSplit: true });

        if (edge.to === joinId) {
          break;
        }

        const node = nodeById.get(edge.to);
        if (!node) {
          break;
        }

        html += renderNodeElement(node);
        if (node.type === EffectGuids.kSplitter || isSplitPoint(node.id)) {
          // Nested splits are not yet rendered; stop at the node.
          break;
        }

        edge = pickPrimaryOutgoingEdge(outgoing, node.id);
      }
      return html;
    };

    const branchesHtml = sortEdgesByPort(outs)
      .map((edge) => {
        const branchHtml = renderBranch(edge);
        return `
          <div class="parallel-branch" data-branch-port="${edge.fromPort}">
            ${branchHtml}
          </div>
        `;
      })
      .join("");

    const mixerNodeHtml = joinNode ? renderNodeElement(joinNode) : "";

    const html = `
      <div class="parallel-container" data-splitter-id="${splitterId}" data-mixer-id="${joinId}">
        <div class="parallel-split">
          <div class="split-icon" aria-hidden="true">
            <svg class="parallel-flow-icon" viewBox="0 0 24 24" role="presentation" focusable="false">
              <path d="M4 12h6" />
              <path d="M10 12c3 0 4-2 8-5" />
              <path d="M10 12c3 0 4 2 8 5" />
              <path d="M16 6l2 1-1 2" />
              <path d="M16 16l2 1-1 2" />
            </svg>
          </div>
        </div>
        <div class="parallel-branches">
          ${branchesHtml}
        </div>
        <div class="parallel-join">
          <div class="join-icon" aria-hidden="true">
            <svg class="parallel-flow-icon" viewBox="0 0 24 24" role="presentation" focusable="false">
              <path d="M6 7c3 1 4 3 8 5" />
              <path d="M6 17c3-1 4-3 8-5" />
              <path d="M14 12h6" />
              <path d="M18 10l2 2-2 2" />
            </svg>
          </div>
          ${canCollapse ? `<button class="parallel-collapse-btn" data-splitter-id="${splitterId}" data-mixer-id="${joinId}" title="Collapse split (only if empty)">×</button>` : ""}
        </div>
      </div>
      ${mixerNodeHtml}
    `;

    return { html, mixerId: joinId };
  };

  const renderMainChain = (): string => {
    let html = "";
    let currentId = "__input__";
    let guard = 0;

    while (guard++ < 500) {
      if (currentId !== "__input__") {
        if (visited.has(currentId)) break;
        visited.add(currentId);

        const node = nodeById.get(currentId);
        if (node && (node.type === EffectGuids.kSplitter || isSplitPoint(currentId))) {
          const { html: parallelHtml, mixerId } = renderParallelForSplitter(currentId);
          if (parallelHtml && mixerId) {
            html += parallelHtml;
            currentId = mixerId;
            continue;
          }
        }
      }

      const edge = pickPrimaryOutgoingEdge(outgoing, currentId);
      if (!edge) {
        break;
      }

      html += renderConnectorWrapper(edge, { showSplit: true });

      if (edge.to === "__output__") {
        break;
      }

      const nextNode = nodeById.get(edge.to);
      if (!nextNode) {
        break;
      }
      html += renderNodeElement(nextNode);
      currentId = nextNode.id;
    }

    return html;
  };

  const segmentsHtml = renderMainChain();

  signalPathNodesElement.innerHTML = `
    <div class="signal-graph-container">
      <div class="signal-graph-row">
        <div class="signal-node input-node" data-node-id="__input__">
          <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/guitar.svg')" aria-hidden="true"></span></div>
          <div class="node-info">
            <div class="node-name">Input</div>
          </div>
          <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
        </div>
        ${segmentsHtml}
        <div class="signal-node output-node" data-node-id="__output__">
          <div class="node-icon">🔈</div>
          <div class="node-info">
            <div class="node-name">Output</div>
          </div>
          <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
        </div>
      </div>
    </div>
  `;

  // Bind click handlers
  bindNodeClickHandlers(preset);
  
  // Bind drop handlers for connectors (to insert between nodes)
  bindConnectorDropHandlers(preset);

  // Bind split/collapse buttons
  bindSplitAndCollapseHandlers();
}

function sendAddEffectAtEdgeOrFallback(
  effectType: string,
  edge: EdgeRef | null,
  fallbackInsertAfter: string,
  options?: SignalPathNodeOptions,
): void {
  if (edge) {
    sendAddSignalPathNodeOnEdge(effectType, edge, options);
  } else {
    // Back-compat: linear chain insertion by node id
    sendAddSignalPathNode(effectType, fallbackInsertAfter, options);
  }
}

/**
 * Renders a single effect node.
 */
function renderNodeElement(node: GraphNode): string {
  const icon = getNodeIcon(node.type);
  const categoryClass = getCategoryClass(getNodeCategory(node));
  const bypassedClass = isNodeBypassed(node) ? "bypassed" : "";
  const selectedClass = selectedNodeId === node.id ? "selected" : "";
  const missingEntries = getMissingResourceEntries(node);
  const missingClass = missingEntries.length ? "missing-resource" : "";
  const allowDelete = node.type !== EffectGuids.kSplitter && node.type !== EffectGuids.kMixer;
  const nodeTypeInfo = getNodeEffectInfo(node);
  const firstResourceTitle = nodeTypeInfo?.requiresResource ? getNodeResourceDisplayName(node, 0) : "";
  const displayName = firstResourceTitle || getNodeDisplayName(node);
  const effectTypeName = firstResourceTitle
    ? (nodeTypeInfo?.displayName || "")
    : (nodeTypeInfo?.displayName && nodeTypeInfo.displayName !== displayName
      ? nodeTypeInfo.displayName
      : "");
  const missingTooltip = buildMissingResourceTooltip(missingEntries);
  const missingBadge = missingEntries.length
    ? `<div class="node-missing-badge" title="${escapeHtml(missingTooltip)}" aria-label="Missing resource">⚠</div>`
    : "";

  // Use the layout thumbnail as a small avatar at the top-left of the node if available.
  const blendId = (() => {
    const params = node.params as Record<string, unknown> | undefined;
    return typeof params?.blend === "string" ? params.blend : "";
  })();
  const nodeLayout = blendId
    ? (getCustomLayout(node.type, blendId) ?? getCustomLayout(node.type))
    : getCustomLayout(node.type);
  const thumbUrl = nodeLayout?.thumbnailDataUrl ?? nodeTypeInfo?.thumbnailDataUrl ?? null;
  const thumbAvatar = thumbUrl
    ? `<img class="node-layout-thumb" src="${thumbUrl.replace(/"/g, "&quot;")}" alt="" aria-hidden="true" />` 
    : "";
  const thumbClass = thumbUrl ? " has-thumb" : "";

  return `
    <div class="signal-node ${categoryClass} ${bypassedClass} ${selectedClass} ${missingClass}${thumbClass}" 
         data-node-id="${node.id}" 
         draggable="true" 
         tabindex="0">
      ${thumbAvatar}
      ${allowDelete ? '<button class="signal-node-delete" type="button" title="Remove" aria-label="Remove">×</button>' : ""}
      ${thumbUrl ? `<div class="node-icon"></div>` : `<div class="node-icon">${icon}</div>`}
      <div class="node-info">
        <div class="node-name">${displayName}</div>
        ${effectTypeName ? `<div class="node-type">${effectTypeName}</div>` : ""}
      </div>
      <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
      ${isNodeBypassed(node) ? '<div class="node-bypass-badge">OFF</div>' : ""}
      ${missingBadge}
    </div>
  `;
}

export function updateSignalPathClipIndicators(): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

  const diagnostics = uiState.signalDiagnostics;

  // Build a map of nodeId → clipped for all nodes in the diagnostics snapshot.
  // No preset-ID filtering here: effect nodes use unique UUIDs so there is no
  // collision across preset instances, and __input__/__output__ are resolved via
  // dedicated diagnostics.input / diagnostics.output fields below.
  const nodeClipMap = new Map<string, boolean>();
  if (diagnostics) {
    diagnostics.nodes.forEach((node) => {
      if (typeof node.nodeId === "string") {
        nodeClipMap.set(node.nodeId, Boolean(node.levels?.clipped));
      }
    });
  }

  nodeElements.forEach((element) => {
    const el = element as HTMLElement;
    const indicator = el.querySelector(".node-clip-indicator") as HTMLElement | null;
    if (!indicator) return;

    indicator.classList.remove("clip-on", "clip-off", "clip-inactive", "clip-unknown");

    if (!diagnostics) {
      indicator.classList.add("clip-inactive");
      indicator.title = "Waiting for diagnostics data";
      return;
    }

    const nodeId = el.dataset.nodeId ?? "";
    let clipped: boolean | undefined;

    if (nodeId === "__input__") {
      clipped = diagnostics.input?.clipped;
    } else if (nodeId === "__output__") {
      clipped = diagnostics.output?.clipped;
    } else if (nodeClipMap.has(nodeId)) {
      clipped = nodeClipMap.get(nodeId);
    }

    if (clipped === true) {
      indicator.classList.add("clip-on");
      indicator.title = "Clipping detected";
    } else if (clipped === false) {
      indicator.classList.add("clip-off");
      indicator.title = "No clipping";
    } else {
      indicator.classList.add("clip-unknown");
      indicator.title = "No diagnostics data";
    }
  });
}

function bindNodeClickHandlers(preset: Preset): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

  const deleteButtons = signalPathNodesElement?.querySelectorAll(".signal-node-delete");
  deleteButtons?.forEach((button) => {
    button.addEventListener("click", (e: Event) => {
      e.preventDefault();
      e.stopPropagation();

      const nodeEl = (button as HTMLElement).closest(".signal-node") as HTMLElement | null;
      const nodeId = nodeEl?.dataset.nodeId;
      if (!nodeId) return;

      sendSignalPathNodeDelete(nodeId);
      selectedNodeId = null;
      nodeParamsPanelElement?.classList.remove("visible");
      updateEffectVisualization();
    });
  });

  const getGraphNode = (nodeId: string): GraphNode | undefined => {
    return preset.graph?.nodes.find((n) => n.id === nodeId);
  };

  const selectNodeElement = (node: GraphNode, el: HTMLElement, focusElement: boolean): void => {
    selectedNodeId = node.id;
    showNodeParamsPanel(node, preset);

    nodeElements.forEach((n) => n.classList.remove("selected"));
    el.classList.add("selected");
    if (focusElement) {
      el.focus();
    }

    const visualizerButton = document.querySelector(
      '.icon-bar .icon-btn[data-panel="visualizer"]',
    ) as HTMLElement | null;
    if (visualizerButton && !visualizerButton.classList.contains("active")) {
      visualizerButton.click();
    }
  };

  // Bind + button click handlers
  bindAddButtonHandlers();

  nodeElements.forEach((element) => {
    const el = element as HTMLElement;
    
    // Click handler - select node
    el.addEventListener("click", () => {
      const nodeId = el.dataset.nodeId;
      if (nodeId && preset.graph) {
        const node = preset.graph.nodes.find((n) => n.id === nodeId);
        if (node) {
          selectNodeElement(node, el, true);
        }
      }
    });

    el.addEventListener("focus", () => {
      const nodeId = el.dataset.nodeId;
      if (!nodeId || !preset.graph) {
        return;
      }

      const node = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!node || selectedNodeId === node.id) {
        return;
      }

      selectNodeElement(node, el, false);
    });

    el.addEventListener("dblclick", () => {
      const nodeId = el.dataset.nodeId;
      if (!nodeId || !preset.graph) {
        return;
      }

      const node = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!node) {
        return;
      }

      focusFxSelectorCategory(getNodeCategory(node), {
        expand: true,
        clearSearch: true,
      });
    });
    
    // Drag start
    el.addEventListener("dragstart", (e: DragEvent) => {
      const nodeId = el.dataset.nodeId;
      if (nodeId) {
        draggedNodeId = nodeId;
        nodeDragStartPoint = { nodeId, x: e.clientX, y: e.clientY };
        lastNodeDragPoint = { x: e.clientX, y: e.clientY };
        nodeDragDropHandled = false;
        el.classList.add("dragging");
        e.dataTransfer?.setData("text/plain", nodeId);
        e.dataTransfer?.setData("application/x-signal-node", nodeId);
        if (e.dataTransfer) {
          e.dataTransfer.effectAllowed = "move";
        }
      }
    });
    
    // Drag over
    el.addEventListener("dragover", (e: DragEvent) => {
      e.preventDefault();
      updateNodeDragPoint(e);
      const nodeId = el.dataset.nodeId;
      
      // Check if dragging from FX library
      const fxEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-effect");
      const fxBlendType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-blend");
      const fxCustomEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-custom-effect");
      const fxResourceGroup = Array.from(e.dataTransfer?.types ?? []).includes("application/x-resource-group");
      
      if (nodeId && (nodeId !== draggedNodeId || fxEffectType || fxBlendType || fxCustomEffectType || fxResourceGroup)) {
        dragOverNodeId = nodeId;
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = (fxEffectType || fxBlendType || fxCustomEffectType || fxResourceGroup) ? "copy" : "move";
        }
      }
    });
    
    // Drag leave
    el.addEventListener("dragleave", () => {
      el.classList.remove("drag-over");
      if (el.dataset.nodeId === dragOverNodeId) {
        dragOverNodeId = null;
      }
    });
    
    // Drop
    el.addEventListener("drop", (e: DragEvent) => {
      e.preventDefault();
      updateNodeDragPoint(e);
      const targetNodeId = el.dataset.nodeId;
      
      // Check if dropping from FX library
      const fxEffectType = e.dataTransfer?.getData("application/x-fx-effect");
      const fxBlendId = e.dataTransfer?.getData("application/x-fx-blend");
      const fxBlendName = e.dataTransfer?.getData("application/x-fx-blend-name");
      const fxBlendCategory = e.dataTransfer?.getData("application/x-fx-blend-category");
      const customEffectPayloadRaw = e.dataTransfer?.getData("application/x-fx-custom-effect");
      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      
      if (resourceGroupPayload && targetNodeId && preset.graph) {
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        if (targetNode && targetNode.type === EffectGuids.kAmpNamBlend) {
          nodeDragDropHandled = true;
          handleResourceGroupDrop(resourceGroupPayload, targetNodeId, true);
        } else {
          nodeDragDropHandled = true;
          handleResourceGroupDrop(resourceGroupPayload, targetNodeId, false);
        }
      } else if (customEffectPayloadRaw && targetNodeId && preset.graph) {
        const customEffectPayload = parseCustomEffectDragPayload(customEffectPayloadRaw);
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        if (customEffectPayload && targetNode && !isProtectedSignalPathNode(targetNode)) {
          const options = buildCustomEffectNodeOptions(customEffectPayload);
          nodeDragDropHandled = true;
          applyOptimisticNodeReplacement(targetNode, customEffectPayload.baseEffectType, preset, options);
          sendReplaceSignalPathNode(targetNodeId, customEffectPayload.baseEffectType, options);
        }
      } else if ((fxEffectType || fxBlendId) && targetNodeId && preset.graph) {
        const resolvedType = fxEffectType || EffectGuids.kAmpNamBlend;
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);

        if (targetNode && !isProtectedSignalPathNode(targetNode)) {
          nodeDragDropHandled = true;
          applyOptimisticNodeReplacement(targetNode, resolvedType, preset, {
            config: fxBlendId ? { blendId: fxBlendId } : undefined,
            label: fxBlendName || undefined,
            category: fxBlendCategory || undefined,
          });
          sendReplaceSignalPathNode(targetNodeId, resolvedType, {
            config: fxBlendId ? { blendId: fxBlendId } : undefined,
            label: fxBlendName || undefined,
            category: fxBlendCategory || undefined,
          });
        }
      } else if (draggedNodeId && targetNodeId && draggedNodeId !== targetNodeId) {
        // Reordering existing nodes
        const draggedNode = getGraphNode(draggedNodeId);
        const targetNode = getGraphNode(targetNodeId);
        const blockedTypes = new Set<string>([EffectGuids.kSplitter, EffectGuids.kMixer]);
        if (draggedNode && targetNode && !blockedTypes.has(draggedNode.type) && !blockedTypes.has(targetNode.type)) {
          nodeDragDropHandled = true;
          sendSignalPathNodeReorder(draggedNodeId, targetNodeId);
        }
      }
      
      el.classList.remove("drag-over");
    });
    
    // Drag end
    el.addEventListener("dragend", (e: DragEvent) => {
      const nodeId = el.dataset.nodeId;
      const node = nodeId ? getGraphNode(nodeId) : undefined;

      el.classList.remove("dragging");
      if (node && shouldToggleNodeBypassFromDrag(e)) {
        toggleSignalPathNodeBypass(node, preset);
      }
      draggedNodeId = null;
      dragOverNodeId = null;
      nodeDragStartPoint = null;
      lastNodeDragPoint = null;
      nodeDragDropHandled = false;
      // Clean up any remaining drag-over states
      nodeElements.forEach((n) => n.classList.remove("drag-over"));
    });
    
    // Keyboard handler - Delete/Backspace to remove
    el.addEventListener("keydown", (e: KeyboardEvent) => {
      const nodeId = el.dataset.nodeId;
      if (nodeId && (e.key === "Delete" || e.key === "Backspace")) {
        e.preventDefault();

        const node = getGraphNode(nodeId);
        if (node && (node.type === EffectGuids.kSplitter || node.type === EffectGuids.kMixer)) {
          // Avoid corrupting the graph; use the collapse split button instead.
          return;
        }

        sendSignalPathNodeDelete(nodeId);
        selectedNodeId = null;
        nodeParamsPanelElement?.classList.remove("visible");
        updateEffectVisualization();
      }
    });
  });
}

function bindConnectorDropHandlers(preset: Preset): void {
  const wrapperElements = signalPathNodesElement?.querySelectorAll(".signal-connector-wrapper");
  if (!wrapperElements || !preset.graph) {
    return;
  }

  wrapperElements.forEach((element) => {
    const el = element as HTMLElement;
    
    // Drag over
    el.addEventListener("dragover", (e: DragEvent) => {
      e.preventDefault();
      updateNodeDragPoint(e);
      
      // Only accept drops from FX library
      const fxEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-effect");
      const fxBlendType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-blend");
      const fxCustomEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-custom-effect");
      const fxResourceGroup = Array.from(e.dataTransfer?.types ?? []).includes("application/x-resource-group");
      const signalNodeId = e.dataTransfer?.getData("application/x-signal-node") || "";
      const isSignalNode = Boolean(signalNodeId);
      
      if (fxEffectType || fxBlendType || fxCustomEffectType || fxResourceGroup || isSignalNode) {
        const connector = el.querySelector(".signal-connector") as HTMLElement | null;
        connector?.classList.add("drag-over");
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = (fxEffectType || fxBlendType || fxCustomEffectType || fxResourceGroup) ? "copy" : "move";
        }
      }
    });
    
    // Drag leave
    el.addEventListener("dragleave", () => {
      const connector = el.querySelector(".signal-connector") as HTMLElement | null;
      connector?.classList.remove("drag-over");
      el.classList.remove("drag-over");
    });
    
    // Drop
    el.addEventListener("drop", (e: DragEvent) => {
      e.preventDefault();
      updateNodeDragPoint(e);
      const fxEffectType = e.dataTransfer?.getData("application/x-fx-effect");
      const fxBlendId = e.dataTransfer?.getData("application/x-fx-blend");
      const fxBlendName = e.dataTransfer?.getData("application/x-fx-blend-name");
      const fxBlendCategory = e.dataTransfer?.getData("application/x-fx-blend-category");
      const customEffectPayloadRaw = e.dataTransfer?.getData("application/x-fx-custom-effect");
      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      const signalNodeId = e.dataTransfer?.getData("application/x-signal-node");

      const edge = parseEdgeFromDataset(el);
      if (resourceGroupPayload && preset.graph) {
        nodeDragDropHandled = true;
        handleResourceGroupDrop(resourceGroupPayload, null, false, edge);
      } else if (customEffectPayloadRaw && preset.graph) {
        const customEffectPayload = parseCustomEffectDragPayload(customEffectPayloadRaw);
        if (customEffectPayload) {
          nodeDragDropHandled = true;
          sendAddEffectAtEdgeOrFallback(
            customEffectPayload.baseEffectType,
            edge,
            "__input__",
            buildCustomEffectNodeOptions(customEffectPayload),
          );
        }
      } else if ((fxEffectType || fxBlendId) && preset.graph) {
        nodeDragDropHandled = true;
        const resolvedType = fxEffectType || EffectGuids.kAmpNamBlend;
        sendAddEffectAtEdgeOrFallback(resolvedType, edge, "__input__", {
          config: fxBlendId ? { blendId: fxBlendId } : undefined,
          label: fxBlendName || undefined,
          category: fxBlendCategory || undefined,
        });
      } else if (signalNodeId && edge && preset.graph) {
        const node = preset.graph.nodes.find((n) => n.id === signalNodeId);
        if (node && node.type !== EffectGuids.kSplitter && node.type !== EffectGuids.kMixer) {
          nodeDragDropHandled = true;
          sendMoveSignalPathNodeToEdge(signalNodeId, edge);
        }
      }
      
      const connector = el.querySelector(".signal-connector") as HTMLElement | null;
      connector?.classList.remove("drag-over");
      el.classList.remove("drag-over");
    });
  });
}

function bindSplitAndCollapseHandlers(): void {
  // Collapse (only safe when the split region is empty)
  const collapseButtons = signalPathNodesElement?.querySelectorAll(".parallel-collapse-btn");
  collapseButtons?.forEach((btn) => {
    btn.addEventListener("click", (e: Event) => {
      e.preventDefault();
      e.stopPropagation();
      const splitterId = (btn as HTMLElement).dataset.splitterId;
      const mixerId = (btn as HTMLElement).dataset.mixerId;
      if (splitterId && mixerId) {
        sendCollapseParallelSplit(splitterId, mixerId);
      }
    });
  });
}

function showNodeParamsPanel(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  // Tear down any existing interactive EQ curve before replacing panel content
  if (signalPathEqInteraction) {
    signalPathEqInteraction.destroy();
    signalPathEqInteraction = null;
  }
  nodeParamKnobs.clear();

  // Ensure node.params exists
  if (!node.params) {
    node.params = {};
  }

  nodeParamsPanelElement.classList.add("visible");
  updateLastSelectedNode(node);
  updateEffectVisualization(node);
  
  // Get parameter definitions from registry
  const typeInfo = getNodeEffectInfo(node);
  let paramDefs = typeInfo?.parameters || [];

  const blendState = getBlendState(node);
  const blendParamRanges = new Map<string, BlendParamRange>();
  if (blendState) {
    blendState.paramIds.forEach((paramId) => {
      const currentValue = node.params[paramId];
      const range = computeBlendParamRange(paramId, blendState.mappings, currentValue);
      blendParamRanges.set(paramId, range);
    });

    const blendParamDefs = blendState.paramIds.map((paramId) => {
      const range = blendParamRanges.get(paramId);
      return {
        key: paramId,
        name: range?.spec?.label ?? formatParamLabel(paramId),
        default: range?.defaultValue ?? 0,
        min: range?.min ?? -1,
        max: range?.max ?? 1,
        unit: "amount",
        step: 0.1,
      };
    });

    const nonBlendParams = paramDefs.filter((paramDef) => paramDef.key !== "blend");
    paramDefs = [...blendParamDefs, ...nonBlendParams];
  }
  
  const renderParamControl = (paramDef: EffectTypeInfo["parameters"][number]): string => {
    const key = paramDef.key;
    const rawValue = node.params[key];
    const label = paramDef.name || formatParamLabel(key);
    const isBlendParam = blendParamRanges.has(key);
    const blendRange = blendParamRanges.get(key);
    const min = blendRange?.min ?? paramDef.min ?? 0;
    const max = blendRange?.max ?? paramDef.max ?? 1;
    const unit = paramDef.unit || "amount";
    const defaultValue = blendRange?.defaultValue ?? paramDef.default ?? 0;
    const normalizedValue = typeof rawValue === "number"
      ? rawValue
      : (isBlendParam ? normalizeBlendValue(defaultValue, blendRange?.spec ?? null) : defaultValue);
    const displayValue = isBlendParam
      ? denormalizeBlendValue(normalizedValue, blendRange?.spec ?? null)
      : (typeof normalizedValue === "number" ? normalizedValue : defaultValue);
    const value = typeof rawValue === "number" ? rawValue : (isBlendParam ? normalizedValue : defaultValue);
    const isToggle = isToggleParam(paramDef);
    const step = typeof paramDef.step === "number" ? paramDef.step : undefined;
    const enumLabels = Array.isArray(paramDef.labels) ? paramDef.labels : [];
    const isEnum = unit === "enum" && enumLabels.length > 0;
    const labelIndex = Math.round(Math.max(min, Math.min(max, displayValue)));
    const enumValueLabel = isEnum ? (enumLabels[labelIndex] ?? `${labelIndex}`) : "";

    if (isToggle) {
      const checked = value >= 0.5;
      return `
        <div class="node-param-group">
          <span class="node-param-label">${label}</span>
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${node.id}" data-param-key="${key}" ${checked ? "checked" : ""}>
            <span class="toggle-slider"></span>
          </label>
          <span class="node-param-value">${checked ? "On" : "Off"}</span>
        </div>
      `;
    }

    if (unit === "blend") {
      const blendLabel = value <= 0.01 ? "A" : value >= 0.99 ? "B" : `${Math.round(value * 100)}%`;
      return `
        <div class="node-param-group node-param-blend-group">
          <span class="node-param-label">${label}</span>
          <div class="blend-slider-container">
            <span class="blend-endpoint-label">A</span>
            <input
              type="range"
              class="node-param-blend-slider"
              data-node-id="${node.id}"
              data-param-key="${key}"
              min="${min}"
              max="${max}"
              step="0.01"
              value="${value}"
              data-default="${defaultValue}"
            >
            <span class="blend-endpoint-label">B</span>
          </div>
          <span class="node-param-value">${blendLabel}</span>
        </div>
      `;
    }

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div 
          class="knob node-param-knob" 
          data-node-id="${node.id}" 
          data-param-key="${key}"
          data-value="${displayValue}"
          data-default="${defaultValue}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
          ${step !== undefined ? `data-step="${step}"` : ""}
          ${isEnum ? `data-labels="${enumLabels.join("|")}"` : ""}
          ${isBlendParam ? `data-blend-param="true" data-blend-spec-min="${blendRange?.spec?.min ?? 0}" data-blend-spec-max="${blendRange?.spec?.max ?? 10}" data-blend-mode="${blendState?.blendMode ?? "interpolate"}"` : ""}
        >
          ${isBlendParam ? `<div class="knob-mapped-points"></div>` : ""}
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${formatParamValue(displayValue, unit, enumLabels)}</span>
       
      </div>
    `;
  };

  const buildParamControls = (defs: EffectTypeInfo["parameters"]): string => {
    const hasGroups = defs.some((paramDef) => typeof paramDef.group === "string" && paramDef.group.trim().length > 0);
    if (!hasGroups) {
      return defs.map(renderParamControl).join("");
    }

    const groupOrder: string[] = [];
    const groupMap = new Map<string, string[]>();

    defs.forEach((paramDef) => {
      const group = paramDef.group?.trim() || "Other";
      if (!groupMap.has(group)) {
        groupMap.set(group, []);
        groupOrder.push(group);
      }
      groupMap.get(group)?.push(renderParamControl(paramDef));
    });

    return groupOrder.map((group) => `
      <div class="node-param-group-block">
        <div class="node-param-group-title">${group}</div>
        <div class="node-param-group-items">
          ${(groupMap.get(group) || []).join("")}
        </div>
      </div>
    `).join("");
  };

  let advancedParamDefs = paramDefs.filter((paramDef) => Boolean(paramDef.advanced));
  let mainParamDefs = paramDefs.filter((paramDef) => !paramDef.advanced);
  if (mainParamDefs.length === 0) {
    mainParamDefs = paramDefs;
    advancedParamDefs = [];
  }
  const hasAdvancedTab = advancedParamDefs.length > 0;

  const isEqNode = typeInfo?.category === "eq" || node.type.startsWith("eq_");
  const customEffectActions = buildCustomEffectActions(node);
  const eqVisualizer = isEqNode ? `
    <div class="eq-visualizer" data-node-id="${node.id}">
      <div class="eq-visualizer-header">
        <span>EQ Curve</span>
        <span class="eq-visualizer-range">±18 dB</span>
      </div>
      <canvas class="eq-curve-canvas" data-node-id="${node.id}"></canvas>
    </div>
  ` : "";

  // Build mixer input controls for mixer nodes
  let mixerInputControls = "";
  if (node.type === EffectGuids.kMixer && preset.graph?.nodes && preset.graph?.edges) {
    try {
      const { incoming } = buildGraphMaps(preset.graph);
      const incomingEdges = incoming.get(node.id) ?? [];
      
      // Get list of unique input port indices
      const inputPorts = [...new Set(incomingEdges.map(e => e.toPort))].sort((a, b) => a - b);
      
      if (inputPorts.length > 0) {
        const renderMixerInputControl = (portIndex: number): string => {
          // Get source node name for this input
          const edge = incomingEdges.find(e => e.toPort === portIndex);
          const sourceNode = edge ? preset.graph?.nodes?.find(n => n.id === edge.from) : null;
          const sourceTypeInfo = sourceNode ? getNodeEffectInfo(sourceNode) : null;
          const inputLabel = sourceTypeInfo?.displayName ?? sourceNode?.type ?? `Input ${portIndex + 1}`;
          
          // Get current values from node params
          const levelKey = `level_${portIndex}`;
          const panKey = `pan_${portIndex}`;
          const delayKey = `delay_${portIndex}`;
          const muteKey = `mute_${portIndex}`;
          
          const levelValue = typeof node.params[levelKey] === "number" ? node.params[levelKey] : 0;
          const panValue = typeof node.params[panKey] === "number" ? node.params[panKey] : 0;
          const delayValue = typeof node.params[delayKey] === "number" ? node.params[delayKey] : 0;
          const muteValue = typeof node.params[muteKey] === "number" ? node.params[muteKey] >= 0.5 : false;
          
          return `
            <div class="mixer-input-group" data-port-index="${portIndex}">
              <div class="mixer-input-header">
                <span class="mixer-input-label">${escapeHtml(inputLabel)}</span>
                <label class="toggle-switch mixer-mute-toggle">
                  <input class="node-param-toggle mixer-input-mute" type="checkbox" 
                         data-node-id="${node.id}" data-param-key="${muteKey}" ${muteValue ? "checked" : ""}>
                  <span class="toggle-slider"></span>
                </label>
                <span class="mixer-mute-label">${muteValue ? "Muted" : "Active"}</span>
              </div>
              <div class="mixer-input-controls">
                <div class="node-param-group mixer-param">
                  <span class="node-param-label">Level</span>
                  <div class="knob node-param-knob" 
                       data-node-id="${node.id}" 
                       data-param-key="${levelKey}"
                       data-value="${levelValue}"
                       data-default="0"
                       data-min="-60"
                       data-max="12"
                       data-unit="dB">
                    <div class="knob-indicator"></div>
                  </div>
                  <span class="node-param-value">${levelValue.toFixed(1)}dB</span>
                </div>
                <div class="node-param-group mixer-param">
                  <span class="node-param-label">Pan</span>
                  <div class="knob node-param-knob" 
                       data-node-id="${node.id}" 
                       data-param-key="${panKey}"
                       data-value="${panValue}"
                       data-default="0"
                       data-min="-1"
                       data-max="1"
                       data-unit="pan">
                    <div class="knob-indicator"></div>
                  </div>
                  <span class="node-param-value">${panValue === 0 ? "C" : (panValue < 0 ? `L${Math.abs(panValue * 100).toFixed(0)}` : `R${(panValue * 100).toFixed(0)}`)}</span>
                </div>
                <div class="node-param-group mixer-param">
                  <span class="node-param-label">Delay</span>
                  <div class="knob node-param-knob" 
                       data-node-id="${node.id}" 
                       data-param-key="${delayKey}"
                       data-value="${delayValue}"
                       data-default="0"
                       data-min="0"
                       data-max="500"
                       data-unit="ms">
                    <div class="knob-indicator"></div>
                  </div>
                  <span class="node-param-value">${delayValue.toFixed(1)}ms</span>
                </div>
              </div>
            </div>
          `;
        };
        
        mixerInputControls = `
          <div class="mixer-inputs-section">
            <div class="mixer-inputs-header">Input Channels</div>
            ${inputPorts.map(renderMixerInputControl).join("")}
          </div>
        `;
      } else {
        // Show placeholder when no inputs connected
        mixerInputControls = `
          <div class="mixer-inputs-section">
            <div class="mixer-inputs-header">Input Channels</div>
            <div class="mixer-no-inputs">No inputs connected. Connect effects to the mixer to control per-input levels.</div>
          </div>
        `;
      }
    } catch (e) {
      console.error("Error building mixer input controls:", e);
    }
  }

  // Build resource selector if this node type requires a resource,
  // or if a composite node surfaces inner resources.
  let resourceSelector = "";
  const customLayoutResourceControls: LayoutResourceControlDef[] = [];
  const exposedResources = typeInfo?.exposedResources ?? [];
  if (exposedResources.length > 0) {
    resourceSelector = exposedResources
      .map((exposedResource, exposedResourceIndex) => {
        const resourceType = exposedResource.resourceType;
        const resourceIndex = exposedResource.resourceIndex ?? exposedResourceIndex;
        const browseAccept = resourceType === "nam"
          ? ".nam,.json"
          : resourceType === "ir"
            ? ".wav"
            : resourceType === "wasm"
              ? ".wasm"
              : "*";
        const resources = uiState.resourceLibrary[resourceType] || [];
        const emptyDisplayName = resourceType === "ir"
          ? "No IR selected"
          : resourceType === "nam"
            ? "No model selected"
            : "No resource selected";
        const current = getNodeResourceAtIndex(node, resourceIndex);
        const displayName = current.id
          ? getNodeResourceDisplayName(node, resourceIndex, resourceType)
          : emptyDisplayName;
        const isMissing = Boolean(current.id)
          && !current.filePath
          && !getLibraryResource(resourceType, current.id);
        const missingClass = isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
        const canBrowseFile = exposedResource.allowBrowseFile ?? true;
        const isLibraryPicker = resourceType === "nam" || resourceType === "ir";
        const resourceOptions = resources.map((res: LibraryResource) => {
          const selected = current.id === res.id && !current.filePath ? "selected" : "";
          return `<option value="${res.id}" ${selected}>${res.name}</option>`;
        }).join("");
        const customOption = current.filePath
          ? `<option value="__custom__" selected>Custom: ${current.filePath.split("/").pop()}</option>`
          : "";
        const hostedPluginOpenButton = resourceType === "plugin"
          ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${node.id}">Open Plugin UI</button>`
          : "";
        const hostedPluginLoadError = resourceType === "plugin"
          ? buildHostedPluginLoadErrorHtml(node, resourceIndex)
          : "";

        customLayoutResourceControls.push({
          resourceControlKey: `__resource__:${exposedResource.resourceId}:${resourceIndex}`,
          displayName: exposedResource.displayName || exposedResource.resourceId,
          resourceType,
          resourceIndex,
          exposedResourceId: exposedResource.resourceId,
          allowBrowseFile: canBrowseFile,
          currentDisplayName: displayName,
          currentFilePath: current.filePath,
          isMissing,
        });

        return `
          <div class="node-resource-selector" data-node-id="${node.id}">
            <label>${escapeHtml(exposedResource.displayName || exposedResource.resourceId)}</label>
            <div class="resource-controls">
              ${isLibraryPicker ? `
                <button
                  class="resource-picker-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                >Browse</button>
                <div
                  class="${missingClass}"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  title="${escapeHtml(displayName)}"
                >${escapeHtml(displayName)}</div>
              ` : `
                <select
                  class="resource-selector resource-dropdown"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                >
                  <option value="">${escapeHtml(emptyDisplayName)}</option>
                  ${resourceOptions}
                  ${customOption}
                </select>
              `}
              ${canBrowseFile ? `
                <button
                  class="resource-browse-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  data-accept="${browseAccept}"
                  title="Browse for file..."
                >${renderIcon("folder", "resource-browse-icon")}</button>
              ` : ""}
              ${hostedPluginOpenButton}
            </div>
            ${hostedPluginLoadError}
            ${current.filePath ? `<div class="resource-path-info" title="${current.filePath}">${current.filePath}</div>` : ""}
          </div>
        `;
      })
      .join("");
  } else if (typeInfo?.requiresResource && typeInfo.resourceType) {
    const resourceType = typeInfo.resourceType;
    const resources = uiState.resourceLibrary[resourceType] || [];
    const browseAccept = resourceType === "nam" ? ".nam,.json" : resourceType === "ir" ? ".wav" : "*";
    const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
    if (blendId) {
      const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
      const blendModels = blend?.models ?? [];
      resourceSelector = `
        <div class="node-resource-selector" data-node-id="${node.id}">
          <label>Blend</label>
          <div class="resource-controls">
            <button class="blend-open-btn" data-node-id="${node.id}">Edit Blend</button>
          </div>
          <div class="resource-path-info">Models: ${blendModels.length}</div>
        </div>
      `;
    } else {

    const buildOptions = (currentId: string) => resources.map((res: LibraryResource) => {
      const selected = res.id === currentId ? "selected" : "";
      return `<option value="${res.id}" ${selected}>${res.name}</option>`;
    }).join("");

    const buildSelector = (index: number, label: string, includeIndexAttr: boolean) => {
      const current = getNodeResourceAtIndex(node, index);
      const resourceOptions = buildOptions(current.id);
      const customOption = current.filePath
        ? `<option value="__custom__" selected>Custom: ${current.filePath.split("/").pop()}</option>`
        : "";
      const indexAttr = includeIndexAttr ? `data-resource-index="${index}"` : "";
      const isLibraryPicker = resourceType === "nam" || resourceType === "ir";
      const displayName = current.id
        ? getNodeResourceDisplayName(node, index)
        : resourceType === "ir" ? "No IR selected" : "No model selected";
      const isMissing = Boolean(current.id)
        && !current.filePath
        && !getLibraryResource(resourceType, current.id);
      const missingClass = isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
      const hostedPluginOpenButton = resourceType === "plugin"
        ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${node.id}">Open Plugin UI</button>`
        : "";
      const hostedPluginLoadError = resourceType === "plugin"
        ? buildHostedPluginLoadErrorHtml(node, index)
        : "";

      customLayoutResourceControls.push({
        resourceControlKey: `__resource__:primary:${index}`,
        displayName: label,
        resourceType,
        resourceIndex: index,
        allowBrowseFile: true,
        currentDisplayName: displayName,
        currentFilePath: current.filePath,
        isMissing,
      });

      return `
        <div class="node-resource-selector">
          <label>${label}</label>
          <div class="resource-controls">
            ${isLibraryPicker ? `
              <button
                class="resource-picker-btn"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
              >Browse</button>
              <div
                class="${missingClass}"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
                title="${escapeHtml(displayName)}"
              >${escapeHtml(displayName)}</div>
            ` : `
              <select
                class="resource-dropdown"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
              >
                <option value="">-- Select from Library --</option>
                ${resourceOptions}
                ${customOption}
              </select>
            `}
            <button
              class="resource-browse-btn"
              data-node-id="${node.id}"
              data-resource-type="${resourceType}"
              ${indexAttr}
              data-accept="${browseAccept}"
              title="Browse for file..."
            >${renderIcon("folder", "resource-browse-icon")}</button>
            ${hostedPluginOpenButton}
          </div>
          ${hostedPluginLoadError}
          ${current.filePath ? `<div class="resource-path-info" title="${current.filePath}">${current.filePath}</div>` : ""}
        </div>
      `;
    };

      if (node.type === EffectGuids.kAmpNamBlend) {
        const items = (node as unknown as { resources?: unknown[] }).resources ?? [];
        const modelSelectors = items.length ? items.map((_, index) => {
          const paramValue = getNodeResourceAtIndex(node, index).parameterValue ?? index;
          return `
            ${buildSelector(index, `Model ${index + 1}`, true)}
            <div class="node-resource-meta">
              <label>Model ${index + 1} Value</label>
              <input class="resource-param-value" type="number" step="0.1" data-node-id="${node.id}" data-resource-index="${index}" value="${paramValue}" />
            </div>
          `;
        }).join("") : `
          ${buildSelector(0, "Model 1", true)}
          <div class="node-resource-meta">
            <label>Model 1 Value</label>
            <input class="resource-param-value" type="number" step="0.1" data-node-id="${node.id}" data-resource-index="0" value="0" />
          </div>
        `;
        resourceSelector = modelSelectors;
      } else if (node.type === EffectGuids.kCabIr) {
        const irSlotA = buildSelector(0, "IR A", true);
        const irSlotB = buildSelector(1, "IR B", true);
        resourceSelector = `${irSlotA}${irSlotB}`;
      } else {
        resourceSelector = buildSelector(0, resourceType === "nam" ? "Model" : resourceType === "ir" ? "IR" : resourceType === "plugin" ? "Plugin" : "Resource", false);
      }
    }
  }

  // Check for custom layout (blend-aware: per-blend first, then fall back to effect type)
  const nodeBlendId = blendState?.blend?.id || "";
  const customLayout = nodeBlendId
    ? (getCustomLayout(node.type, nodeBlendId) ?? getCustomLayout(node.type))
    : getCustomLayout(node.type);

  // When useDefaultControls is true the layout provides only the visual backdrop; the
  // standard auto-generated controls are rendered on top rather than positioned controls.
  const useDefaultControls = customLayout?.useDefaultControls === true;

  const customLayoutHtml = customLayout && !useDefaultControls
    ? renderCustomLayout(node, customLayout, paramDefs, customLayoutResourceControls)
    : null;
  const layoutIncludesResourceControls = Boolean(
    customLayout && !useDefaultControls && customLayout.controls.some((control) => control.bindingType === "resource" || control.paramKey.startsWith("__resource__:")),
  );
  const shellTitle = escapeHtml(getNodeDisplayName(node));
  const shellCategoryLabel = escapeHtml(
    getNodeCategory(node)
      .replace(/[-_]/g, " ")
      .replace(/\b\w/g, (char) => char.toUpperCase())
  );
  const shellTypeLabel = escapeHtml(typeInfo?.displayName || shellCategoryLabel);
  const shellStatusLabel = isNodeBypassed(node) ? "BYPASSED" : "ENABLED";
  const shellBypassTitle = isNodeBypassed(node) ? "Enable effect" : "Bypass effect";
  const shellBlendId = getBlendState(node)?.blend?.id || "";
  const shellLayoutButton = isFeatureEnabled(Features.EffectLayout) ? `
    <button
      class="effect-visualization-toolbar-btn node-customize-layout-btn"
      data-node-id="${node.id}"
      data-effect-type="${node.type}"
      data-blend-id="${shellBlendId}"
      type="button"
      title="Customize layout"
      aria-label="Customize layout"
    >
      ${renderIcon("gear", "effect-visualization-toolbar-icon customize-layout-icon")}
    </button>
  ` : "";
  const equipmentImage = getEffectVisualizationEquipmentImage(node);
  const shellEquipmentPanel = equipmentImage ? `
    <aside class="default-effect-shell-equipment-panel" aria-hidden="true">
      <img class="default-effect-shell-equipment-image" src="${equipmentImage}" alt="" loading="lazy" decoding="async" />
    </aside>
  ` : "";
  const shellMainContent = customLayoutHtml ? `
    ${layoutIncludesResourceControls ? "" : resourceSelector}
    ${customEffectActions}
    ${eqVisualizer}
    ${mixerInputControls}
    <div class="default-effect-section default-effect-section-controls default-effect-section-custom-layout">
      ${customLayoutHtml}
    </div>
  ` : (() => {
    // Build the standard default controls HTML
    const defaultControlsHtml = hasAdvancedTab ? `
      <div class="node-param-tabs" role="tablist" aria-label="Parameter Groups">
        <button class="node-param-tab is-active" data-tab="main" type="button" role="tab" aria-selected="true">Main</button>
        <button class="node-param-tab" data-tab="advanced" type="button" role="tab" aria-selected="false">Advanced</button>
      </div>
      <div class="node-param-tab-panels">
        <div class="node-param-tab-panel is-active" data-tab="main" role="tabpanel">
          <div class="params-controls">
            ${buildParamControls(mainParamDefs)}
          </div>
        </div>
        <div class="node-param-tab-panel" data-tab="advanced" role="tabpanel">
          <div class="params-controls">
            ${buildParamControls(advancedParamDefs)}
          </div>
        </div>
      </div>
    ` : `
      <div class="params-controls">
        ${buildParamControls(paramDefs)}
      </div>
    `;
    // If a backdrop layout exists, wrap the default controls inside it
    const renderedControls = useDefaultControls && customLayout
      ? renderCustomLayoutBackdrop(node, customLayout, defaultControlsHtml)
      : defaultControlsHtml;
    return `
      ${layoutIncludesResourceControls ? "" : resourceSelector}
      ${customEffectActions}
      ${eqVisualizer}
      ${mixerInputControls}
      <div class="default-effect-section default-effect-section-controls">
        ${renderedControls}
      </div>
    `;
  })();

  nodeParamsPanelElement.innerHTML = `
    
    <div class="node-params-body">
      <section class="default-effect-shell${isNodeBypassed(node) ? " is-bypassed" : ""}">
        <div class="default-effect-shell-header">
          <div class="default-effect-shell-identity">
            <span class="default-effect-shell-led" aria-hidden="true"></span>
            <div class="default-effect-shell-titles">
              <div class="default-effect-shell-title">${shellTitle}</div>
              <div class="default-effect-shell-subtitle">${shellCategoryLabel} · ${shellTypeLabel}</div>
            </div>
          </div>
          <div class="default-effect-shell-meta" aria-label="Module status">
            <button
              class="default-effect-shell-chip default-effect-shell-chip-status node-bypass-btn ${isNodeBypassed(node) ? "bypassed" : ""}"
              data-node-id="${node.id}"
              type="button"
              title="${shellBypassTitle}"
              aria-label="${shellBypassTitle}"
            >${shellStatusLabel}</button>
            ${shellLayoutButton}
          </div>
          <button class="close-params-btn" type="button" aria-label="Close effect panel" title="Close effect panel">×</button>
        </div>
        <div class="default-effect-shell-rail" aria-hidden="true">
          <span class="default-effect-shell-meter" style="--meter-fill: 0%"></span>
        </div>
        <div class="default-effect-shell-content${equipmentImage ? " has-equipment-image" : ""}">
          ${shellEquipmentPanel}
          <div class="default-effect-shell-main">
            ${shellMainContent}
          </div>
        </div>
      </section>
    </div>
  `;

  if (isEqNode) {
    updateEqVisualization(node);
  }

  // Bind controls
  bindNodeParamControls(node, preset);
  bindLayoutOverlayBypassToggles(node, preset);
  bindResourceControls(node, preset);
  bindHostedPluginActionControls(node);
  bindCustomEffectActionControls(node);
  bindBlendEditorControls(nodeParamsPanelElement, node);
  bindCloseButton();
  bindBypassButton(node, preset);
  bindCustomizeLayoutButton(node);
  bindParamTabs();
  updateSelectedNodePeakMeter();
}

function bindLayoutOverlayBypassToggles(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  overlayBypassClickCleanup?.();

  const clickHandler = (event: Event) => {
    const target = event.target as HTMLElement | null;
    const overlay = target?.closest('.custom-layout-overlay[data-toggle-bypass="true"]') as HTMLElement | null;
    if (!overlay) {
      return;
    }

    event.preventDefault();
    event.stopPropagation();

    toggleSignalPathNodeBypass(node, preset);
  };

  nodeParamsPanelElement.addEventListener("click", clickHandler);
  overlayBypassClickCleanup = () => {
    nodeParamsPanelElement?.removeEventListener("click", clickHandler);
  };
}

function bindParamTabs(): void {
  const tabButtons = nodeParamsPanelElement?.querySelectorAll(".node-param-tab");
  const tabPanels = nodeParamsPanelElement?.querySelectorAll(".node-param-tab-panel");
  if (!tabButtons || !tabPanels || tabButtons.length === 0 || tabPanels.length === 0) {
    return;
  }

  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tab = (button as HTMLElement).dataset.tab;
      if (!tab) return;

      tabButtons.forEach((btn) => {
        const active = (btn as HTMLElement).dataset.tab === tab;
        btn.classList.toggle("is-active", active);
        btn.setAttribute("aria-selected", active ? "true" : "false");
      });
      tabPanels.forEach((panel) => {
        const active = (panel as HTMLElement).dataset.tab === tab;
        panel.classList.toggle("is-active", active);
      });
    });
  });
}

function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function isToggleParam(paramDef: { key: string; min?: number; max?: number; unit?: string }): boolean {
  return paramDef.unit==="toggle";
}

/**
 * Build the default parameter controls HTML using only default values.
 * Produces the same DOM structure as the live renderParamControl path so the
 * layout designer can render a faithful preview without a live node.
 * nodeId is used for data attributes so knob CSS still applies correctly.
 */
export function buildDefaultParamControlsHtml(
  paramDefs: import("./presetV2.js").ParameterDef[],
  nodeId = "preview"
): string {
  const renderOne = (p: import("./presetV2.js").ParameterDef): string => {
    const label = p.name || formatParamLabel(p.key);
    const value = p.default ?? 0;
    const min = p.min ?? 0;
    const max = p.max ?? 1;
    const unit = p.unit || "amount";
    const isToggle = isToggleParam(p);
    const isEnum = unit === "enum" && Array.isArray(p.labels) && p.labels.length > 0;
    const enumLabels = Array.isArray(p.labels) ? p.labels : [];
    const labelIndex = Math.round(Math.max(min, Math.min(max, value)));
    const enumValueLabel = isEnum ? (enumLabels[labelIndex] ?? `${labelIndex}`) : "";

    if (isToggle) {
      const checked = value >= 0.5;
      return `
        <div class="node-param-group">
          <span class="node-param-label">${label}</span>
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${nodeId}" data-param-key="${p.key}" ${checked ? "checked" : ""} disabled>
            <span class="toggle-slider"></span>
          </label>
          <span class="node-param-value">${checked ? "On" : "Off"}</span>
        </div>`;
    }

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div class="knob node-param-knob"
          data-node-id="${nodeId}"
          data-param-key="${p.key}"
          data-value="${value}"
          data-default="${value}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
          ${p.step !== undefined ? `data-step="${p.step}"` : ""}
          ${isEnum ? `data-labels="${enumLabels.join("|")}"` : ""}
        >
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${formatParamValue(value, unit, enumLabels)}</span>
      </div>`;
  };

  const hasGroups = paramDefs.some((p) => typeof p.group === "string" && p.group.trim().length > 0);
  if (!hasGroups) {
    return paramDefs.map(renderOne).join("");
  }

  const groupOrder: string[] = [];
  const groupMap = new Map<string, string[]>();
  paramDefs.forEach((p) => {
    const group = p.group?.trim() || "Other";
    if (!groupMap.has(group)) {
      groupMap.set(group, []);
      groupOrder.push(group);
    }
    groupMap.get(group)!.push(renderOne(p));
  });

  return groupOrder.map((group) => `
    <div class="node-param-group-block">
      <div class="node-param-group-title">${group}</div>
      <div class="node-param-group-items">
        ${groupMap.get(group)!.join("")}
      </div>
    </div>`).join("");
}

function bindNodeParamControls(node: GraphNode, preset: Preset): void {
  // Bind slider inputs from custom layouts
  const sliders = nodeParamsPanelElement?.querySelectorAll(".node-param-slider");
  sliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = parseFloat(input.value);
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);

        // Update associated value display
        const parentControl = input.closest(".custom-layout-control");
        const valueEl = parentControl?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueEl) {
          const paramDef = getNodeEffectInfo(node)?.parameters.find((p) => p.key === paramKey);
          if (paramDef) {
            if (paramDef.unit === "dB" || paramDef.unit === "ms" || paramDef.unit === "Hz") {
              valueEl.textContent = `${value.toFixed(1)}${paramDef.unit}`;
            } else if (paramDef.unit === "enum" && Array.isArray(paramDef.labels)) {
              valueEl.textContent = paramDef.labels[Math.round(value)] ?? `${Math.round(value)}`;
            } else {
              valueEl.textContent = value.toFixed(2);
            }
          }
        }
      }
    });
  });

  // Bind blend slider inputs (irBlend-style A/B range controls)
  const blendSliders = nodeParamsPanelElement?.querySelectorAll(".node-param-blend-slider");
  blendSliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = parseFloat(input.value);
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);

        const valueEl = input.closest(".node-param-blend-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueEl) {
          valueEl.textContent = value <= 0.01 ? "A" : value >= 0.99 ? "B" : `${Math.round(value * 100)}%`;
        }
      }
    });
  });

  const toggles = nodeParamsPanelElement?.querySelectorAll(".node-param-toggle");
  toggles?.forEach((toggleEl) => {
    const input = toggleEl as HTMLInputElement;
    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = input.checked ? 1 : 0;
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);
        
        // Handle standard toggle labels
        const valueLabel = input.closest(".node-param-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueLabel) {
          valueLabel.textContent = input.checked ? "On" : "Off";
        }
        
        // Handle mixer mute labels
        const muteLabel = input.closest(".mixer-input-header")?.querySelector(".mixer-mute-label") as HTMLElement | null;
        if (muteLabel) {
          muteLabel.textContent = input.checked ? "Muted" : "Active";
        }
        
        updateEqVisualization(node);
      }
    });
  });

  const knobs = nodeParamsPanelElement?.querySelectorAll(".node-param-knob");
  if (!knobs) {
    return;
  }

  const blendState = getBlendState(node);

  const findClosestBlendMappingForParam = (
    activeParamId: string,
    targetValue: number,
    target: Record<string, number>,
  ): BlendModelMapping | null => {
    if (!blendState) {
      return null;
    }

    let best: BlendModelMapping | null = null;
    let bestDelta = Number.POSITIVE_INFINITY;
    let bestSecondary = Number.POSITIVE_INFINITY;

    blendState.mappings.forEach((mapping) => {
      const params = buildParameterMapFromLegacy(mapping);
      const mappedValue = params[activeParamId];
      if (typeof mappedValue !== "number") {
        return;
      }

      const delta = Math.abs(mappedValue - targetValue);
      let secondary = 0;

      blendState.paramIds.forEach((paramId) => {
        if (paramId === activeParamId) {
          return;
        }
        const targetOther = target[paramId];
        const mappedOther = params[paramId];
        if (typeof targetOther !== "number" || typeof mappedOther !== "number") {
          secondary += 4;
          return;
        }
        const diff = mappedOther - targetOther;
        secondary += diff * diff;
      });

      const isBetter = delta < bestDelta - BLEND_MAPPING_EPS
        || (Math.abs(delta - bestDelta) <= BLEND_MAPPING_EPS && secondary < bestSecondary);
      if (isBetter) {
        best = mapping;
        bestDelta = delta;
        bestSecondary = secondary;
      }
    });

    return best;
  };

  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const valueDisplay = knob.parentElement?.querySelector(".node-param-value") as HTMLElement | null;
    
    const nodeId = knob.dataset.nodeId;
    const paramKey = knob.dataset.paramKey;
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    const unit = knob.dataset.unit || "amount";
    const defaultValue = parseFloat(knob.dataset.default || knob.dataset.value || "0");
    const sensitivity = (max - min) / 200;
    const step = knob.dataset.step ? parseFloat(knob.dataset.step) : undefined;
    const labels = (knob.dataset.labels || "").split("|").filter(Boolean);
    const isEnum = unit === "enum" && labels.length > 0;
    const isBlendParam = knob.dataset.blendParam === "true";
    const isPitchShiftSemitones = node.type === "pitch_shift" && paramKey === "semitones";
    const isPitchShiftStepMode = node.type === "pitch_shift" && paramKey === "stepMode";
    const isPitchShiftMin = node.type === "pitch_shift" && paramKey === "minSemitones";
    const isPitchShiftMax = node.type === "pitch_shift" && paramKey === "maxSemitones";
    const blendSpecMin = knob.dataset.blendSpecMin ? parseFloat(knob.dataset.blendSpecMin) : 0;
    const blendSpecMax = knob.dataset.blendSpecMax ? parseFloat(knob.dataset.blendSpecMax) : 10;
    const blendMode = (knob.dataset.blendMode ?? "interpolate") as BlendMode;
    const blendSpec: BlendParamSpec | null = isBlendParam
      ? { id: paramKey ?? "", label: paramKey ?? "", min: blendSpecMin, max: blendSpecMax }
      : null;

    const snapValue = (rawValue: number): number => {
      if (isPitchShiftSemitones && (node.params.stepMode ?? 1) >= 0.5) {
        const minBound = typeof node.params.minSemitones === "number" ? node.params.minSemitones : -12;
        const maxBound = typeof node.params.maxSemitones === "number" ? node.params.maxSemitones : 12;
        const range = Math.max(0.0, maxBound - minBound);
        if (range <= 0.0) return Math.max(min, Math.min(max, rawValue));
        const mapped = minBound + (rawValue + 1) * 0.5 * range;
        const snappedSemitones = Math.max(minBound, Math.min(maxBound, Math.round(mapped)));
        const snappedControl = ((snappedSemitones - minBound) / range) * 2 - 1;
        return Math.max(min, Math.min(max, snappedControl));
      }
      if (!step || step <= 0) return rawValue;
      const snapped = Math.round((rawValue - min) / step) * step + min;
      return Math.max(min, Math.min(max, snapped));
    };

    const formatValue = (rawValue: number): string => {
      if (isEnum) {
        const index = Math.round(rawValue);
        return labels[index] ?? `${index}`;
      }
      if (isBlendParam) {
        return rawValue.toFixed(1);
      }
      // Special formatting for pan values
      if (unit === "pan") {
        if (Math.abs(rawValue) < 0.01) return "C";
        return rawValue < 0 
          ? `L${Math.abs(rawValue * 100).toFixed(0)}`
          : `R${(rawValue * 100).toFixed(0)}`;
      }
      return `${rawValue.toFixed(2)}${unit === "amount" ? "" : unit}`;
    };

    const knobInstance = new GenericKnob({
      knobElement: knob,
      paramId: `${nodeId ?? "node"}_${paramKey ?? "param"}`,
      minValue: min,
      maxValue: max,
      defaultValue,
      displayFormat: (value) => formatValue(value),
      valueDisplay,
      labelElement: knob.parentElement?.querySelector(".node-param-label, .custom-control-label") as HTMLElement | null,
      sensitivity,
      stepValue: step,
      sendParameter: false,
      onValueChange: (value) => {
        if (!nodeId || !paramKey) return;
        const finalValue = snapValue(value);
        if (finalValue !== value) {
          knobInstance.setValue(finalValue);
        }

        const normalizedValue = isBlendParam ? normalizeBlendValue(finalValue, blendSpec) : finalValue;

        if (isBlendParam && blendMode === "snap" && blendState) {
          const target: Record<string, number> = { ...node.params, [paramKey]: normalizedValue };
          const closest = findClosestBlendMappingForParam(paramKey, normalizedValue, target);
          if (closest) {
            const params = buildParameterMapFromLegacy(closest);
            let updated = false;
            blendState.paramIds.forEach((paramId) => {
              const mappedValue = params[paramId];
              if (typeof mappedValue === "number" && node.params[paramId] !== mappedValue) {
                node.params[paramId] = mappedValue;
                sendSignalPathNodeParamUpdate(nodeId, paramId, mappedValue);
                updated = true;
              }
            });
            if (updated) {
              showNodeParamsPanel(node, preset);
            }
            return;
          }
        }

        node.params[paramKey] = normalizedValue;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, normalizedValue);

        if (isPitchShiftMin || isPitchShiftMax) {
          const minBound = typeof node.params.minSemitones === "number" ? node.params.minSemitones : -12;
          const maxBound = typeof node.params.maxSemitones === "number" ? node.params.maxSemitones : 12;
          let nextMin = minBound;
          let nextMax = maxBound;

          if (isPitchShiftMin) {
            nextMin = Math.max(-12, Math.min(12, normalizedValue));
            nextMax = Math.max(nextMin, maxBound);
          } else {
            nextMax = Math.max(-12, Math.min(12, normalizedValue));
            nextMin = Math.min(nextMax, minBound);
          }

          if (nextMin !== minBound) {
            node.params.minSemitones = nextMin;
            sendSignalPathNodeParamUpdate(nodeId, "minSemitones", nextMin);
          }
          if (nextMax !== maxBound) {
            node.params.maxSemitones = nextMax;
            sendSignalPathNodeParamUpdate(nodeId, "maxSemitones", nextMax);
          }

          const currentSemitones = typeof node.params.semitones === "number" ? node.params.semitones : 0;
          const clampedSemitones = Math.max(nextMin, Math.min(nextMax, currentSemitones));
          if (clampedSemitones !== currentSemitones) {
            node.params.semitones = clampedSemitones;
            sendSignalPathNodeParamUpdate(nodeId, "semitones", clampedSemitones);
          }

          showNodeParamsPanel(node, preset);
          return;
        }

        if (isPitchShiftStepMode && normalizedValue >= 0.5) {
          const currentControl = typeof node.params.semitones === "number" ? node.params.semitones : 0;
          const snappedControl = snapValue(currentControl);
          if (snappedControl !== currentControl) {
            node.params.semitones = snappedControl;
            sendSignalPathNodeParamUpdate(nodeId, "semitones", snappedControl);
            showNodeParamsPanel(node, preset);
            return;
          }
        }
        updateEqVisualization(node);

        if (isBlendParam && blendState) {
          updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
        }
      },
    });

    // Store knob instance for live EQ curve sync
    if (paramKey) {
      nodeParamKnobs.set(paramKey, knobInstance);
    }
  });

  if (blendState) {
    updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
  }
}

function updateEqVisualization(node: GraphNode): void {
  const typeInfo = getNodeEffectInfo(node);
  if (!typeInfo || (typeInfo.category !== "eq" && !node.type.startsWith("eq_"))) {
    return;
  }

  const canvas = nodeParamsPanelElement?.querySelector(".eq-curve-canvas") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const width = Math.max(1, canvas.clientWidth);
  const height = Math.max(1, canvas.clientHeight);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  const bandConfigs = buildEqBandConfigsFromParams(node.params ?? {});

  if (signalPathEqInteraction) {
    // Update existing interaction in place
    signalPathEqInteraction.updateBands(bandConfigs);
  } else {
    // Create new interactive curve
    const preset = getActivePresetForRender();
    signalPathEqInteraction = new EqCurveInteraction(
      canvas,
      bandConfigs,
      (bandIndex, freq, gainDb, q) => {
        // Lightweight onChange: update params, send to plugin, and sync knobs live
        const changed = eqBandChangeToParams(bandIndex, freq, gainDb, q);
        for (const [key, value] of Object.entries(changed)) {
          node.params[key] = value;
          sendSignalPathNodeParamUpdate(node.id, key, value);
          // Sync corresponding knob display
          const knob = nodeParamKnobs.get(key);
          if (knob) knob.setValue(value);
        }
      },
      (bandIndex, freq, gainDb, q) => {
        // onCommit: full update including panel rebuild for knob display sync
        const changed = eqBandChangeToParams(bandIndex, freq, gainDb, q);
        for (const [key, value] of Object.entries(changed)) {
          node.params[key] = value;
          sendSignalPathNodeParamUpdate(node.id, key, value);
        }
        if (preset) {
          showNodeParamsPanel(node, preset);
        }
      }
    );
  }
}

function bindResourceControls(node: GraphNode, preset: Preset): void {
  // Bind resource dropdowns
  const dropdowns = nodeParamsPanelElement?.querySelectorAll(".resource-dropdown") as NodeListOf<HTMLSelectElement> | null;
  dropdowns?.forEach((dropdown) => {
    dropdown.addEventListener("change", () => {
      const nodeId = dropdown.dataset.nodeId;
      const resourceType = dropdown.dataset.resourceType;
      const resourceId = dropdown.value;
      const resourceIndex = dropdown.dataset.resourceIndex ? parseInt(dropdown.dataset.resourceIndex, 10) : undefined;

      if (resourceType === "plugin") {
        if (nodeId) {
          hostedPluginLoadFailures.delete(nodeId);
        }
        clearInlineHostedPluginLoadError(dropdown);
        const selectedResource = resourceId && resourceId !== "__custom__"
          ? getLibraryResource("plugin", resourceId)
          : null;
        if (nodeId) {
          renderHostedPluginWarningIntoOpenPanel(nodeId, resourceIndex, buildUnsupportedPluginWarningMarkup(selectedResource));
        }
      }
      
      if (nodeId && resourceType && resourceId && resourceId !== "__custom__") {
        sendNodeResourceUpdate(nodeId, resourceType, resourceId, "", resourceIndex);
      }
    });
  });

  const resourcePickers = nodeParamsPanelElement?.querySelectorAll(".resource-picker-btn, .resource-picker-label") as NodeListOf<HTMLElement> | null;
  resourcePickers?.forEach((picker) => {
    picker.addEventListener("click", () => {
      const nodeId = picker.dataset.nodeId;
      const resourceType = picker.dataset.resourceType as "nam" | "ir" | undefined;
      const resourceIndex = picker.dataset.resourceIndex ? parseInt(picker.dataset.resourceIndex, 10) : 0;
      const exposedResourceId = picker.dataset.exposedResourceId;
      if (!nodeId || !resourceType || (resourceType !== "nam" && resourceType !== "ir")) {
        return;
      }

      const current = getNodeResourceAtIndex(node, resourceIndex);
      resourceBrowserModal.open({
        resourceType,
        currentId: current.id,
        nodeId,
        resourceIndex,
        onSelect: (resourceId) => {
          sendNodeResourceUpdate(nodeId, resourceType, resourceId, "", resourceIndex, undefined, exposedResourceId);
          const label = getLibraryResourceName(resourceType, resourceId) || resourceId || "";
          const labelText = label || (resourceType === "ir" ? "No IR selected" : "No model selected");
          const labelCandidates = nodeParamsPanelElement?.querySelectorAll(
            `.resource-picker-label[data-node-id="${nodeId}"]`,
          ) as NodeListOf<HTMLElement> | null;
          const labelEl = findMatchingResourcePickerLabel(
            labelCandidates,
            nodeId,
            resourceType,
            resourceIndex,
            exposedResourceId,
          );

          if (labelEl) {
            labelEl.textContent = labelText;
            labelEl.title = labelText;
            const missing = Boolean(resourceId) && !getLibraryResource(resourceType, resourceId);
            labelEl.classList.toggle("is-missing", missing);
          }
        },
      });
    });
  });
  
  // Bind browse buttons
  const browseBtns = nodeParamsPanelElement?.querySelectorAll(".resource-browse-btn") as NodeListOf<HTMLButtonElement> | null;
  browseBtns?.forEach((browseBtn) => {
    browseBtn.addEventListener("click", () => {
      const nodeId = browseBtn.dataset.nodeId;
      const resourceType = browseBtn.dataset.resourceType;
      const resourceIndex = browseBtn.dataset.resourceIndex ? parseInt(browseBtn.dataset.resourceIndex, 10) : undefined;
      const exposedResourceId = browseBtn.dataset.exposedResourceId;
      
      if (resourceType === "plugin") {
        if (nodeId) {
          hostedPluginLoadFailures.delete(nodeId);
        }
        clearInlineHostedPluginLoadError(browseBtn);
      }

      if (nodeId && resourceType) {
        sendBrowseNodeResource(nodeId, resourceType, resourceIndex, exposedResourceId);
      }
    });
  });

  // Bind parameter value inputs for blend models
  const valueInputs = nodeParamsPanelElement?.querySelectorAll(".resource-param-value") as NodeListOf<HTMLInputElement> | null;
  valueInputs?.forEach((input) => {
    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const resourceIndex = input.dataset.resourceIndex ? parseInt(input.dataset.resourceIndex, 10) : undefined;
      const value = parseFloat(input.value);
      if (nodeId && resourceIndex !== undefined && !Number.isNaN(value)) {
        sendNodeResourceUpdate(nodeId, "nam", "", "", resourceIndex, value);
      }
    });
  });
}

function bindCustomEffectActionControls(node: GraphNode): void {
  const designButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-design-btn");
  designButton?.addEventListener("click", () => {
    void openCustomEffectDesigner(node);
  });

  const saveButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-save-btn");
  saveButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, false);
  });

  const useButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-use-btn");
  useButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, true);
  });
}

function bindHostedPluginActionControls(node: GraphNode): void {
  const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(".plugin-host-open-btn");
  openButtons?.forEach((openButton) => openButton.addEventListener("click", () => {
    sendSignalPathNodeConfigUpdate(node.id, "showPluginEditor", "1", false);
  }));
}

function getNodeResourceIds(node: GraphNode): string[] {
  const anyNode = node as unknown as { resources?: unknown };
  if (!Array.isArray(anyNode.resources)) {
    const fallback = getNodeResourceAtIndex(node, 0).id;
    return fallback ? [fallback] : [];
  }

  const ids: string[] = [];
  anyNode.resources.forEach((res, index) => {
    const ref = res as { id?: unknown; resourceId?: unknown; embeddedId?: unknown } | undefined;
    const id = typeof ref?.id === "string"
      ? ref.id
      : (typeof ref?.resourceId === "string" ? ref.resourceId : (typeof ref?.embeddedId === "string" ? ref.embeddedId : ""));
    if (id) {
      ids.push(id);
    } else {
      const fallback = getNodeResourceAtIndex(node, index).id;
      if (fallback) {
        ids.push(fallback);
      }
    }
  });

  return Array.from(new Set(ids));
}

function bindCloseButton(): void {
  const closeBtn = nodeParamsPanelElement?.querySelector(".close-params-btn");
  if (closeBtn) {
    closeBtn.addEventListener("click", () => {
      nodeParamsPanelElement?.classList.remove("visible");
      selectedNodeId = null;
      updateEffectVisualization();
      
      // Deselect all nodes
      const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node");
      nodeElements?.forEach((el) => el.classList.remove("selected"));
    });
  }
}

function bindBypassButton(node: GraphNode, preset: Preset): void {
  const bypassButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-bypass-btn");
  bypassButtons.forEach((bypassBtn) => {
    bypassBtn.addEventListener("click", () => {
      toggleSignalPathNodeBypass(node, preset);
    });
  });
}

function bindCustomizeLayoutButton(node: GraphNode): void {
  const layoutButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-customize-layout-btn");
  if (!layoutButtons.length) {
    return;
  }

  layoutButtons.forEach((layoutBtn) => {
    layoutBtn.addEventListener("click", () => {
      const effectType = layoutBtn.dataset.effectType || node.type;
      const blendId = layoutBtn.dataset.blendId || "";

      // For blend effects, try per-blend layout first, then fall back to effect-type layout
      const existingLayout = blendId
        ? (getCustomLayout(effectType, blendId) ?? getCustomLayout(effectType))
        : getCustomLayout(effectType);

      // Resolve blend params so the designer shows all available controls
      let blendName = "";
      let blendParamDefs: Array<{ key: string; name: string; default: number; min: number; max: number; unit: string; step?: number }> | undefined;
      if (blendId) {
        const blendState = getBlendState(node);
        if (blendState) {
          blendName = blendState.blend?.name || blendId;
          // Include ALL blend param specs so every possible knob is available in the designer
          const allBlendParams = BLEND_PARAM_SPECS.map((spec) => ({
            key: spec.id,
            name: spec.label,
            default: 0,
            min: spec.min,
            max: spec.max,
            unit: "amount",
            step: 0.1,
          }));
          const typeInfo = getNodeEffectInfo(node);
          const baseParams = (typeInfo?.parameters || []).filter((p) => p.key !== "blend");
          blendParamDefs = [...allBlendParams, ...baseParams];
        }
      }

      layoutDesigner.open(effectType, existingLayout ?? undefined, {
        blendId: blendId || undefined,
        blendName: blendName || undefined,
        blendParamDefs,
      });
    });
  });
}

function sendSignalPathNodeParamUpdate(nodeId: string, paramKey: string, value: number): void {
  const presetId = uiState.activePresetId ?? undefined;
  postMessage({
    type: "updateSignalPathNodeParam",
    nodeId,
    paramKey,
    value,
    ...(presetId ? { presetId } : {}),
  });
  setPresetDirty(true);
}

function sendSignalPathNodeBypassUpdate(nodeId: string, presetId: string, bypassed: boolean): void {
  postMessage({
    type: "updateSignalPathNodeBypass",
    nodeId,
    presetId,
    bypassed,
  });
  setPresetDirty(true);
}

function sendSignalPathNodeConfigUpdate(nodeId: string, key: string, value: string, persist = true, capture = false): void {
  postMessage({
    type: "updateSignalPathNodeConfig",
    nodeId,
    key,
    value,
    persist,
    capture,
  });
  if (persist && !capture) {
    setPresetDirty(true);
  }
}

function sendNodeResourceUpdate(
  nodeId: string,
  resourceType: string,
  resourceId: string,
  filePath: string,
  resourceIndex?: number,
  parameterValue?: number,
  exposedResourceId?: string,
): void {
  postMessage({
    type: "updateNodeResource",
    nodeId,
    resourceType,
    resourceId,
    filePath,
    resourceIndex,
    parameterValue,
    exposedResourceId,
  });
  setPresetDirty(true);
}

function sendBrowseNodeResource(
  nodeId: string,
  resourceType: string,
  resourceIndex?: number,
  exposedResourceId?: string,
): void {
  postMessage({
    type: "browseNodeResource",
    nodeId,
    resourceType,
    resourceIndex,
    exposedResourceId,
  });
}

function sendSignalPathNodeReorder(nodeId: string, targetNodeId: string): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    targetNodeId,
  });
  setPresetDirty(true);
}

function sendSignalPathNodeDelete(nodeId: string): void {
  postMessage({
    type: "deleteSignalPathNode",
    nodeId,
  });
  setPresetDirty(true);
}

function sendReplaceSignalPathNode(
  nodeId: string,
  newEffectType: string,
  options?: SignalPathNodeOptions,
): void {
  postMessage({
    type: "replaceSignalPathNode",
    nodeId,
    newEffectType,
    config: options?.config,
    label: options?.label,
    category: options?.category,
    params: options?.params,
    resources: options?.resources,
  });
  setPresetDirty(true);
}

function sendMoveSignalPathNodeToEdge(nodeId: string, edge: SignalPathEdgeRef): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    edge,
  });
  setPresetDirty(true);
}

function sendCollapseParallelSplit(splitterId: string, mixerId: string): void {
  postMessage({
    type: "collapseSignalPathSplit",
    splitterId,
    mixerId,
  });
  setPresetDirty(true);
}

/**
 * Show/hide the composite edit mode banner in the signal path area.
 */
function updateCompositeEditBanner(): void {
  const banner = document.getElementById("composite-edit-banner");
  if (!banner) return;

  if (isCompositeEditMode()) {
    const def = uiState.compositeEditDefinition;
    const nameEl = document.getElementById("composite-edit-banner-name");
    if (nameEl && def) nameEl.textContent = def.name;
    banner.style.display = "";
  } else {
    banner.style.display = "none";
  }
}

function buildPresetScenePanelMarkup(preset: Preset, activeSceneId: string): string {
  const scenes = preset.scenes ?? [];
  if (scenes.length <= 1) {
    return "";
  }

  const activeScene = findPresetScene(preset, activeSceneId) ?? scenes[0] ?? null;
  const tabsHtml = scenes.map((scene) => {
    const active = scene.id === activeSceneId;
    return `<button class="preset-scene-tab${active ? " active" : ""}" type="button" data-scene-id="${escapeHtml(scene.id)}">${escapeHtml(scene.title)}</button>`;
  }).join("");

  return `
    <div class="mixer-preset-scene-panel" data-scene-panel-for="${escapeHtml(preset.id)}">
      <div class="preset-scene-tab-strip">${tabsHtml}</div>
      <div class="preset-scene-controls">
        <input class="preset-scene-title-input" type="text" value="${escapeHtml(activeScene?.title ?? "")}" maxlength="80" placeholder="Scene title" />
        <button class="preset-scene-action" type="button" data-scene-action="remove" title="Remove scene" ${scenes.length <= 1 ? "disabled" : ""}>Remove</button>
      </div>
    </div>
  `;
}

function addSceneFromToolbar(): void {
  const activePreset = getSignalPathPreset();
  if (!activePreset) {
    return;
  }

  const editablePreset = getEditableSignalPathPreset(activePreset);
  const newScene = createPresetScene(editablePreset, uiState.activePresetSceneId ?? undefined);
  uiState.activePresetSceneId = newScene.id;
  setPresetDirty(true);
  pushScenePresetToBackend(editablePreset);
  renderSignalPathBar();
}

function bindPresetScenePanel(panel: HTMLElement, renderedPreset: Preset): void {
  panel.querySelectorAll<HTMLButtonElement>(".preset-scene-tab").forEach((button) => {
    button.addEventListener("click", () => {
      const nextSceneId = button.dataset.sceneId ?? "";
      if (!nextSceneId || nextSceneId === uiState.activePresetSceneId) {
        return;
      }
      const editablePreset = getEditableSignalPathPreset(renderedPreset);
      uiState.activePresetSceneId = selectPresetScene(editablePreset, nextSceneId);
      pushScenePresetToBackend(editablePreset);
      renderSignalPathBar();
    });
  });

  const titleInput = panel.querySelector<HTMLInputElement>(".preset-scene-title-input");
  titleInput?.addEventListener("change", () => {
    const editablePreset = getEditableSignalPathPreset(renderedPreset);
    const selectedScene = findPresetScene(editablePreset, uiState.activePresetSceneId ?? undefined);
    if (!selectedScene) {
      return;
    }
    const nextTitle = titleInput.value.trim() || "Scene";
    if (selectedScene.title === nextTitle) {
      return;
    }
    selectedScene.title = nextTitle;
    setPresetDirty(true);
    pushScenePresetToBackend(editablePreset);
    renderSignalPathBar();
  });

  panel.querySelector<HTMLButtonElement>("[data-scene-action='remove']")?.addEventListener("click", () => {
    const editablePreset = getEditableSignalPathPreset(renderedPreset);
    if ((editablePreset.scenes?.length ?? 0) <= 1) {
      showNotification("A preset must keep at least one scene");
      return;
    }
    const nextSceneId = removePresetScene(editablePreset, uiState.activePresetSceneId ?? "");
    uiState.activePresetSceneId = nextSceneId;
    setPresetDirty(true);
    pushScenePresetToBackend(editablePreset);
    renderSignalPathBar();
  });
}

signalPathToolbarSceneButton?.addEventListener("click", () => {
  addSceneFromToolbar();
});

const mixerPresetTabCollator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

function renderMixerPresetTabs(): void {
  let tabBar = document.getElementById("mixer-preset-tabs");
  const signalPathBar = document.getElementById("signal-path-bar");
  const mixer = uiState.mixer;

  const renderedPreset = getSignalPathPreset();
  const shouldShowTabs = !isCompositeEditMode() && !!renderedPreset;

  if (!shouldShowTabs) {
    if (tabBar) tabBar.remove();
    mixTabActive = false;
    return;
  }

  const multiPresetMode = !!mixer && mixer.activePresetIds.length > 1;
  const activePreset = getEditableSignalPathPreset(renderedPreset);

  if (!tabBar) {
    tabBar = document.createElement("div");
    tabBar.id = "mixer-preset-tabs";
    tabBar.className = "mixer-preset-tabs";
    const scroll = signalPathBar?.querySelector(".signal-path-scroll");
    if (scroll) {
      signalPathBar!.insertBefore(tabBar, scroll);
    } else if (signalPathBar) {
      signalPathBar.prepend(tabBar);
    }
  }

  const presetIds = multiPresetMode ? [...(mixer?.activePresetIds ?? [])] : [activePreset.id];
  if (multiPresetMode) {
    presetIds.sort((leftId, rightId) => {
      const leftName = uiState.presetCache.get(leftId)?.name ?? mixer?.presets[leftId]?.name ?? leftId;
      const rightName = uiState.presetCache.get(rightId)?.name ?? mixer?.presets[rightId]?.name ?? rightId;
      const nameComparison = mixerPresetTabCollator.compare(leftName, rightName);
      if (nameComparison !== 0) {
        return nameComparison;
      }
      return mixerPresetTabCollator.compare(leftId, rightId);
    });
  }
  const focusedId = multiPresetMode ? (uiState.focusedMixerPresetId ?? presetIds[0]) : activePreset.id;

  const presetTabsHtml = presetIds.map((id) => {
    const name = uiState.presetCache.get(id)?.name ?? mixer?.presets[id]?.name ?? id;
    const ps = mixer?.presets[id];
    const muted = ps?.mute ?? false;
    const soloed = ps?.solo ?? false;
    const active = !mixTabActive && id === focusedId;
    const indicators = [
      muted ? `<span class="tab-indicator muted" title="Muted">M</span>` : "",
      soloed ? `<span class="tab-indicator soloed" title="Solo">S</span>` : "",
    ].join("");
    const closeBtn = multiPresetMode
      ? `<span class="mixer-tab-close" data-close-preset-id="${escapeHtml(id)}" title="Remove from mixer" role="button" aria-label="Remove ${escapeHtml(name)}">×</span>`
      : "";
    return `<button class="mixer-preset-tab${active ? " active" : ""}" data-preset-id="${escapeHtml(id)}" type="button">${escapeHtml(name)}${indicators}${closeBtn}</button>`;
  }).join("");

  const mixTabHtml = multiPresetMode
    ? `<button class="mixer-preset-tab mixer-tab-mix${mixTabActive ? " active" : ""}" data-mix-tab="1" type="button">⚖ Mix</button>`
    : "";

  tabBar.innerHTML = `<div class="mixer-preset-tab-row">${presetTabsHtml}${mixTabHtml}</div>`;

  tabBar.querySelectorAll<HTMLButtonElement>(".mixer-preset-tab-row .mixer-preset-tab:not([data-mix-tab])").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      // Don't switch tab when close button was clicked
      if ((e.target as HTMLElement).closest(".mixer-tab-close")) return;
      const pid = btn.dataset.presetId ?? "";
      if (pid) {
        mixTabActive = false;
        uiState.activePresetId = pid;
        setFocusedMixerPresetId(pid);
        document.dispatchEvent(new CustomEvent("mixerPresetTabSelected", {
          detail: { presetId: pid },
        }));
        renderSignalPathBar();
      }
    });
  });

  // Close (×) buttons — remove preset from mixer
  tabBar.querySelectorAll<HTMLElement>(".mixer-tab-close").forEach((closeEl) => {
    closeEl.addEventListener("click", (e) => {
      e.stopPropagation();
      const pid = closeEl.dataset.closePresetId ?? "";
      if (!pid) return;
      removeActivePreset(pid);
      if (uiState.mixer) {
        uiState.mixer.activePresetIds = uiState.mixer.activePresetIds.filter((id) => id !== pid);
        delete uiState.mixer.presets[pid];
      }
      if (uiState.focusedMixerPresetId === pid) {
        uiState.focusedMixerPresetId = uiState.mixer?.activePresetIds[0] ?? null;
      }
      // Update any "✓ In Mixer" button in the preset list for this preset
      document.querySelectorAll<HTMLButtonElement>(`.preset-add-to-mixer-btn[data-preset-id="${CSS.escape(pid)}"]`).forEach((btn) => {
        btn.textContent = "+ Mixer";
        btn.classList.remove("in-mixer");
        btn.title = "Add to mixer";
      });
      renderSignalPathBar();
    });
  });

  tabBar.querySelector<HTMLButtonElement>(".mixer-preset-tab-row [data-mix-tab]")?.addEventListener("click", () => {
    mixTabActive = !mixTabActive;
    renderSignalPathBar();
  });
}

function getEditableSignalPathPreset(sourcePreset: Preset): Preset {
  const existingDraft = uiState.activePresetDraft;
  if (existingDraft && existingDraft.id === sourcePreset.id) {
    normalizePresetScenes(existingDraft, uiState.activePresetSceneId ?? undefined);
    return existingDraft;
  }

  const draft = clonePreset(sourcePreset);
  uiState.activePresetId = sourcePreset.id;
  setFocusedMixerPresetId(sourcePreset.id);
  uiState.activePresetSceneId = normalizePresetScenes(draft, uiState.activePresetSceneId ?? undefined);
  setActivePresetDraft(draft);
  return uiState.activePresetDraft ?? draft;
}

function pushScenePresetToBackend(preset: Preset): void {
  const sceneId = normalizePresetScenes(preset, uiState.activePresetSceneId ?? undefined);
  uiState.activePresetSceneId = sceneId;
  uiState.activePresetId = preset.id;
  setFocusedMixerPresetId(preset.id);
  setActivePresetDraft(preset);
  postMessage({
    type: "loadPreset",
    preset: uiState.activePresetDraft ?? preset,
    ...(sceneId ? { sceneId } : {}),
  });
}

function buildInlineMixerHtml(): string {
  const mixer = uiState.mixer;
  if (!mixer || !mixer.activePresetIds.length) return "";

  const strips = mixer.activePresetIds.map((id) => {
    const name = uiState.presetCache.get(id)?.name ?? mixer.presets[id]?.name ?? id;
    const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
    return `
      <div class="iml-strip" data-preset-id="${escapeHtml(id)}" style="--accent:${idAccentColor(id)}">
        <div class="iml-strip-name">${escapeHtml(name)}</div>
        <div class="iml-strip-controls">
          <label class="iml-label">Mix<input type="range" class="iml-mix" min="0" max="1" step="0.01" value="${ps.mix}"/></label>
          <label class="iml-label">Pan<input type="range" class="iml-pan" min="-1" max="1" step="0.01" value="${ps.pan}"/></label>
          <div class="iml-toggles">
            <button type="button" class="iml-mute-btn${ps.mute ? " active" : ""}" title="Mute">M</button>
            <button type="button" class="iml-solo-btn${ps.solo ? " active" : ""}" title="Solo">S</button>
          </div>
        </div>
      </div>`;
  }).join("");

  return `
    <div class="iml-strips">${strips}</div>
    <div class="iml-master">
      <label class="iml-label">Master Gain<input type="range" id="iml-master-gain" min="0" max="2" step="0.01" value="${mixer.masterGain}"/></label>
      <label class="iml-toggle"><input type="checkbox" id="iml-limiter" ${mixer.limiterEnabled ? "checked" : ""}/> Limiter</label>
      <button type="button" id="iml-save-multi-rig" class="secondary-btn iml-save-multi-rig-btn" title="Save current mixer as a Multi-Rig preset">Save Multi-Rig…</button>
    </div>`;
}

function renderInlineMixer(): void {
  const signalPathBar = document.getElementById("signal-path-bar");
  let panel = document.getElementById("inline-mixer-panel");
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "inline-mixer-panel";
    panel.className = "inline-mixer-panel";
    signalPathBar?.appendChild(panel);
  }
  panel.innerHTML = buildInlineMixerHtml();
  bindInlineMixerControls(panel);
}

function removeInlineMixer(): void {
  document.getElementById("inline-mixer-panel")?.remove();
}

function bindInlineMixerControls(panel: HTMLElement): void {
  panel.querySelectorAll<HTMLElement>(".iml-strip").forEach((strip) => {
    const pid = strip.dataset.presetId ?? "";
    if (!pid) return;

    const mixInput = strip.querySelector<HTMLInputElement>(".iml-mix");
    if (mixInput) {
      enhanceRangeInput(mixInput);
      mixInput.addEventListener("input", (e) => {
        const v = parseFloat((e.target as HTMLInputElement).value);
        setPresetMix(pid, isFinite(v) ? v : 1.0);
      });
    }

    const panInput = strip.querySelector<HTMLInputElement>(".iml-pan");
    if (panInput) {
      enhanceRangeInput(panInput);
      panInput.addEventListener("input", (e) => {
        const v = parseFloat((e.target as HTMLInputElement).value);
        setPresetPan(pid, isFinite(v) ? v : 0.0);
      });
    }

    const muteBtn = strip.querySelector<HTMLButtonElement>(".iml-mute-btn");
    muteBtn?.addEventListener("click", () => {
      const nowMuted = !muteBtn.classList.contains("active");
      setPresetMute(pid, nowMuted);
      muteBtn.classList.toggle("active", nowMuted);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].mute = nowMuted;
      renderMixerPresetTabs(); // refresh M/S indicators in tabs
    });

    const soloBtn = strip.querySelector<HTMLButtonElement>(".iml-solo-btn");
    soloBtn?.addEventListener("click", () => {
      const nowSolo = !soloBtn.classList.contains("active");
      setPresetSolo(pid, nowSolo);
      soloBtn.classList.toggle("active", nowSolo);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].solo = nowSolo;
      renderMixerPresetTabs();
    });
  });

  const masterGainInput = panel.querySelector<HTMLInputElement>("#iml-master-gain");
  if (masterGainInput) {
    enhanceRangeInput(masterGainInput);
    masterGainInput.addEventListener("input", (e) => {
      const v = parseFloat((e.target as HTMLInputElement).value);
      setMasterGain(isFinite(v) ? v : 1.0);
    });
  }

  panel.querySelector<HTMLInputElement>("#iml-limiter")?.addEventListener("change", (e) => {
    setLimiterEnabled((e.target as HTMLInputElement).checked);
  });

  panel.querySelector<HTMLButtonElement>("#iml-save-multi-rig")?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("mixerSaveMultiRig"));
  });
}

/**
 * Bind click handlers for + buttons between nodes.
 */
function bindAddButtonHandlers(): void {
  const addButtons = signalPathNodesElement?.querySelectorAll(".signal-add-btn");
  if (!addButtons) return;

  addButtons.forEach((button) => {
    button.addEventListener("click", (e: Event) => {
      e.stopPropagation();
      const edge = parseEdgeFromDataset(button as HTMLElement);
      showEffectSelectionDropdown(button as HTMLElement, edge);
    });
  });
}

/**
 * Show a dropdown menu to select an effect to add.
 */
function showEffectSelectionDropdown(buttonElement: HTMLElement, edge: EdgeRef | null): void {
  // Remove any existing dropdown
  const existing = document.querySelector(".effect-selection-dropdown");
  if (existing) existing.remove();

  const dropdown = document.createElement("div");
  dropdown.className = "effect-selection-dropdown";

  const dropdownItems = getFxLibraryItems({ excludeTypes: [EffectGuids.kMixer] });
  const effectsByCategory = new Map<string, FxLibraryItem[]>();

  dropdownItems.forEach((effect) => {
    if (!effectsByCategory.has(effect.category)) {
      effectsByCategory.set(effect.category, []);
    }
    effectsByCategory.get(effect.category)!.push(effect);
  });

  const categoryOrder = getOrderedFxCategories(dropdownItems);
  
  let dropdownHtml = '<div class="effect-dropdown-header">Add Effect</div>';
  
  categoryOrder.forEach((categoryId) => {
    const effects = effectsByCategory.get(categoryId) ?? [];
    if (effects.length > 0) {
      const categoryInfo = CATEGORY_METADATA[categoryId];
      const categoryColor = categoryInfo?.color || "var(--color-accent)";
      dropdownHtml += `
        <div class="effect-dropdown-category" style="--category-color: ${escapeHtml(categoryColor)}">
          <div class="effect-dropdown-category-name">
            ${categoryInfo?.name || categoryId}
          </div>
          ${effects.map((effect) => {
              const thumb = effect.blendId
                ? (getCustomLayout(effect.type, effect.blendId) ?? getCustomLayout(effect.type))?.thumbnailDataUrl
                : (getCustomLayout(effect.type)?.thumbnailDataUrl ?? effect.thumbnailDataUrl);
            const icon = thumb
              ? `<img src="${thumb.replace(/"/g, '&quot;')}" alt="" aria-hidden="true" class="effect-dropdown-thumb" />`
              : `<span class="effect-dropdown-icon">${effect.blendId ? getBadgeIcon("blend", "Custom blend") : getNodeIcon(effect.type)}</span>`;
              return `
              <div class="effect-dropdown-item"
                data-effect-type="${effect.type}"
                data-blend-id="${escapeHtml(effect.blendId ?? "")}"
                data-blend-name="${escapeHtml(effect.blendId ? effect.displayName : "")}"
                data-blend-category="${escapeHtml(effect.blendCategory ?? "") }"
                data-effect-category="${escapeHtml(effect.category ?? "utility") }"
                data-custom-effect-id="${escapeHtml(effect.customEffectId ?? "") }"
                data-custom-effect-resource-type="${escapeHtml(effect.moduleResourceType ?? "") }"
                data-custom-effect-resource-id="${escapeHtml(effect.moduleResourceId ?? "") }"
                data-custom-effect-default-params="${escapeHtml(encodeURIComponent(JSON.stringify(effect.defaultParams ?? {})))}"
                style="--category-color: ${escapeHtml(categoryColor)}">
              ${icon}
              <span class="effect-dropdown-name">${escapeHtml(effect.displayName)}</span>
            </div>
          `;
          }).join('')}
        </div>
      `;
    }
  });
  
  dropdown.innerHTML = dropdownHtml;
  document.body.appendChild(dropdown);

  // Position dropdown near the button
  const buttonRect = buttonElement.getBoundingClientRect();
  dropdown.style.left = `${buttonRect.left}px`;
  dropdown.style.top = `${buttonRect.bottom + 5}px`;

  // Bind effect selection
  const effectItems = dropdown.querySelectorAll(".effect-dropdown-item");
  effectItems.forEach((item) => {
    item.addEventListener("click", () => {
      const effectType = (item as HTMLElement).dataset.effectType;
      const blendId = (item as HTMLElement).dataset.blendId;
      const blendName = (item as HTMLElement).dataset.blendName;
      const blendCategory = (item as HTMLElement).dataset.blendCategory;
      const customEffectId = (item as HTMLElement).dataset.customEffectId;
      if (effectType) {
        if (customEffectId) {
          const payload: CustomEffectDragPayload = {
            customEffectId,
            baseEffectType: effectType,
            name: item.querySelector(".effect-dropdown-name")?.textContent ?? "Custom Effect",
            category: (item as HTMLElement).dataset.effectCategory ?? "utility",
            moduleResourceType: (item as HTMLElement).dataset.customEffectResourceType ?? "",
            moduleResourceId: (item as HTMLElement).dataset.customEffectResourceId ?? "",
            defaultParams: parseCustomEffectDefaultParamsDataset((item as HTMLElement).dataset.customEffectDefaultParams),
          };
          sendAddEffectAtEdgeOrFallback(effectType, edge, edge?.from ?? "__input__", buildCustomEffectNodeOptions(payload));
        } else {
          sendAddEffectAtEdgeOrFallback(effectType, edge, edge?.from ?? "__input__", {
            config: blendId ? { blendId } : undefined,
            label: blendName || undefined,
            category: blendCategory || undefined,
          });
        }
        dropdown.remove();
      }
    });
  });

  // Close dropdown when clicking outside
  setTimeout(() => {
    const closeHandler = (e: MouseEvent) => {
      if (!dropdown.contains(e.target as Node)) {
        dropdown.remove();
        document.removeEventListener("click", closeHandler);
      }
    };
    document.addEventListener("click", closeHandler);
  }, 0);
}

type ResourceGroupPayload = {
  groupId: string;
  title: string;
  category: string;
  modelIds: string[];
  modelMappings?: BlendModelMapping[];
};

function handleResourceGroupDrop(
  payloadRaw: string,
  targetNodeId: string | null,
  updateOnly: boolean,
  edge?: SignalPathEdgeRef | null,
): void {
  let payload: ResourceGroupPayload | null = null;
  try {
    payload = JSON.parse(payloadRaw) as ResourceGroupPayload;
  } catch {
    payload = null;
  }
  if (!payload || !payload.modelIds?.length) {
    return;
  }

  const modelMappings = payload.modelMappings?.length
    ? payload.modelMappings
    : buildBlendModelMappingsFromIds(payload.modelIds, uiState.resourceLibrary);

  const existingBlendId = targetNodeId
    ? (getActivePresetForRender()?.graph?.nodes.find((n) => n.id === targetNodeId)?.config?.blendId ?? "")
    : "";

  const blendId = existingBlendId || (typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`);

  const blendName = existingBlendId
    ? (uiState.blendLibrary?.find((blend) => blend.id === existingBlendId)?.name ?? payload.title)
    : payload.title;

  postMessage({
    type: "saveBlendDefinition",
    blend: {
      id: blendId,
      name: blendName,
      category: payload.category,
      models: modelMappings.map((mapping: BlendModelMapping) => mapping.id),
      modelMappings,
      blendMode: "interpolate",
    },
  });

  if (updateOnly && targetNodeId) {
    sendReplaceSignalPathNode(targetNodeId, EffectGuids.kAmpNamBlend, {
      config: { blendId },
      label: blendName,
      category: payload.category,
    });
    return;
  }

  if (targetNodeId) {
    sendReplaceSignalPathNode(targetNodeId, EffectGuids.kAmpNamBlend, {
      config: { blendId },
      label: blendName,
      category: payload.category,
    });
    return;
  }

  const normalizedEdge = edge ? { ...edge, gain: edge.gain ?? 1.0 } : null;
  sendAddEffectAtEdgeOrFallback(EffectGuids.kAmpNamBlend, normalizedEdge, edge?.from ?? "__input__", {
    config: { blendId },
    label: blendName,
    category: payload.category,
  });
}

