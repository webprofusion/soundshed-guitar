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
  StoredEffectPreset,
} from "./types.js";
import { postMessage, setAppSetting, setPresetMix, setPresetPan, setPresetMute, setPresetSolo, setMasterGain, setLimiterEnabled, removeActivePreset, focusMixerPreset } from "./bridge.js";
import { requestResourceData } from "./archiveUtils.js";
import { escapeHtml, idAccentColor, base64ToArrayBuffer, arrayBufferToBase64, findResourceById } from "./utils.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import { EffectTypeRegistry, getNodeEffectInfo, type EffectTypeInfo, type ParameterDef } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon, renderIcon } from "./iconAssets.js";
import { deduplicateResourcesByHashAndPath, resolveResourceIdAlias } from "./resourceDedup.js";
import {
  CATEGORY_METADATA,
  expandFxSelector,
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
import { getUnsupportedPluginSelection, inferPluginFormat, type PluginResourceSupportInfo } from "./pluginSupport.js";
import {
  EqCurveInteraction,
  buildEqBandConfigsFromParams,
  drawEqCurve,
  eqBandChangeToParams,
  GRAPHIC_EQ_FREQUENCIES,
  buildGraphicEqBandConfigs,
  clampGraphicEqFrequency,
  graphicEqFrequencyBounds,
} from "./eqCurve.js";
import {
  SpatialPannerInteraction,
  type SpatialLiveState,
  type SpatialPosition,
} from "./spatialPanner.js";
import { resourceBrowserModal, type ResourceNavigationResult as ResourceNavigationSelection } from "./resourceBrowser.js";
import { findMatchingResourcePickerLabel } from "./resourcePickerLabel.js";
import { hasCustomLayout, getCustomLayout, renderCustomLayout, renderCustomLayoutBackdrop, formatParamValue, type LayoutResourceControlDef } from "./layoutRenderer.js";
import { areEffectLayoutsEnabled, buildLayoutMatchText, findLayoutById, resolveLayoutForNode } from "./layoutPreferences.js";
import type { EffectLayout } from "./layoutTypes.js";
import { closeLayoutPicker, hasSelectableLayouts, openLayoutPicker } from "./layoutPicker.js";
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
  updateBlendMatchSummary,
  renderBlendInfoHtml,
  getBlendEntriesForCategory,
  initializeBlendEditorModal,
  openBlendEditorWithDefinition,
  bindBlendEditorControls,
} from "./signalPathBlend.js";
import { getCustomEffectEntry, saveCurrentCustomEffect } from "./customEffects.js";
import { openCustomEffectDesigner } from "./customEffectDesigner.js";
import { createPresetScene, findPresetScene, normalizePresetScenes, removePresetScene, selectPresetScene } from "./presetScenes.js";
import { getCurrentUiSettings, updateUiSettings } from "./windowSettings.js";
import { themeSwitcher } from "./theme-switcher.js";
import { MAX_PANEL_KNOBS } from "./amp3d/ampLayout.js";
import {
  isSignalChain3dEnabled,
  isWebglSupported,
  setSignalChain3dEnabled,
} from "./amp3d/ampSupport.js";
import type {
  Amp3dKnobSpec,
  BuildChainLayoutOptions,
  Chain3dView,
  Chain3dViewOptions,
} from "./amp3d/index.js";
export { initializeBlendEditorModal, openBlendEditorWithDefinition } from "./signalPathBlend.js";

const signalPathNodesElement = document.getElementById("signal-path-nodes");
const nodeParamsPanelElement = document.getElementById("node-params-panel");
const signalPathAddMenu = document.getElementById("signal-path-add-menu");
const signalPathAddMenuTrigger = document.getElementById("signal-path-add-menu-trigger") as HTMLButtonElement | null;
const signalPathAddMenuOptions = document.querySelector<HTMLElement>("#signal-path-add-menu-options");
const signalPathAddSceneButton = document.getElementById("signal-path-add-scene") as HTMLButtonElement | null;

/** Whether the Mix tab is currently active in the multi-preset tab bar. */
let mixTabActive = false;
let signalPathEqInteraction: EqCurveInteraction | null = null;
let signalPathSpatialInteraction: SpatialPannerInteraction | null = null;
let signalPathSpatialNodeId: string | null = null;
/** Knob instances for the current node params panel, keyed by param key. */
const nodeParamKnobs = new Map<string, GenericKnob>();
const effectVisualizationElement = document.getElementById("effect-visualization");

// Drag-drop state
let draggedNodeId: string | null = null;
let dragOverNodeId: string | null = null;
let selectedNodeId: string | null = null;
let lastSelectedNodeType: string | null = null;
let lastSelectedNodeCategory: string | null = null;
let selectedNodeDspStatusVisible = false;
let selectedNodeDspStatusNodeId: string | null = null;
const selectedNodeDspStatusAverages = new Map<string, number>();
let lastDspStatusAverageRenderAt = 0;
const DSP_STATUS_AVERAGE_SMOOTHING = 0.01;
const DSP_STATUS_AVERAGE_RENDER_INTERVAL_MS = 500;
const inferredNamArchitectureByResourceId = new Map<string, string>();
const pendingNamArchitectureResourceIds = new Set<string>();
const unavailableNamArchitectureResourceIds = new Set<string>();
let lastRenderedPresetId: string | null = null;
let overlayBypassClickCleanup: (() => void) | null = null;
let layoutScaleObserverCleanups: (() => void)[] = [];
let nodeDragStartPoint: { nodeId: string; x: number; y: number } | null = null;
let lastNodeDragPoint: { x: number; y: number } | null = null;
let nodeDragDropHandled = false;
let effectVisualizationDropCleanup: (() => void) | null = null;
type HostedPluginLoadFailure = {
  selectionKey: string;
  resourceIndex?: number;
  resource: PluginResourceSupportInfo;
  message: string;
  errorCode?: string;
};
const hostedPluginLoadFailures = new Map<string, HostedPluginLoadFailure>();

type HostedPluginPendingLoad = {
  resourceIndex: number;
  startedAt: number;
};
/** Hosted-plugin loads in flight, keyed by node id. Drives the inline loading indicator. */
const hostedPluginPendingLoads = new Map<string, HostedPluginPendingLoad>();
/** Safety valve: never keep a loading indicator around longer than this. */
const HOSTED_PLUGIN_PENDING_LOAD_MAX_AGE_MS = 120000;
const HOSTED_PLUGIN_FAVORITES_SETTING = "plugins.hostFavorites";
const HOSTED_PLUGIN_NAME_COLLATOR = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });
const analyzerSpectrogramHistoryByNode = new Map<string, number[][]>();
const ANALYZER_SPECTROGRAM_HISTORY_FRAMES = 160;

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
  cab: "../images/equipment/cabs/cab-02.png",
  delay: "../images/equipment/fx/studio-rack-delay.png",
  reverb: "../images/equipment/fx/studio-rack-reverb.png",
};

const EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE: Record<string, string> = {
  [EffectGuids.kPluginHost]:"../images/equipment/fx/studio-rack-multifx.png",
  [EffectGuids.kDelayDigital]: "../images/equipment/fx/studio-rack-delay.png",
  [EffectGuids.kDelayDoubler]: "../images/equipment/fx/studio-rack-delay.png",
  [EffectGuids.kFxNam]: "../images/equipment/pedals/colourful-pedal2.png",
  fx_nam: "../images/equipment/pedals/colourful-pedal2.png",
  [EffectGuids.kWasmHost]: "../images/equipment/pedals/colourful-pedal2.png",
  wasm_host: "../images/equipment/pedals/colourful-pedal2.png",
  
};

/**
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

function getNodeResourceArtworkImage(node: GraphNode): string {
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

/** The stock artwork for this effect type/category, ignoring any loaded model. */
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

function getEffectVisualizationEquipmentImage(node: GraphNode): string {
  return getNodeResourceArtworkImage(node) || getEffectVisualizationStockImage(node);
}

layoutDesigner.onClose(() => {
  refreshSelectedNodeParams();
  renderSignalPathBar();
});

// ── Signal-chain 3D stage ────────────────────────────────────────────────────
// One immersive WebGL stage for the whole graph. Neural FX uses a generic pedal
// + dock model chooser; amps/cabs cluster; everything else is a rack unit.

let chain3dView: Chain3dView | null = null;
let chain3dMountToken = 0;
/** True while the params panel is showing the chain stage for the current selection. */
let chain3dPanelActive = false;

/**
 * Master switch for the experimental 3D chain stage.
 *
 * The stage is disabled: effect visualisation is served entirely by the standard
 * controls and custom layouts, which the user picks between from the effect
 * header (see `layoutPicker.ts`). The scene code in `amp3d/` is kept — it is only
 * reachable through a dynamic import — so the experiment can be revived by
 * flipping this flag, but nothing loads it while it is false.
 */
const CHAIN_3D_VIEW_ENABLED = false;

function canOfferChain3dView(): boolean {
  return CHAIN_3D_VIEW_ENABLED && isWebglSupported();
}

function shouldRenderChain3dView(hasPositionedLayout: boolean): boolean {
  // Custom positioned layouts keep their 2D designer canvas; 3D mode is for the
  // default shell / generic controls path.
  return !hasPositionedLayout && canOfferChain3dView() && isSignalChain3dEnabled();
}

interface Amp3dParamSplit {
  knobDefs: ParameterDef[];
  extraDefs: ParameterDef[];
}

/**
 * Continuous params become physical knobs on the 3D unit; everything else stays
 * as HTML controls in the floating dock so no control is lost.
 */
function splitAmp3dParamDefs(paramDefs: ParameterDef[]): Amp3dParamSplit {
  const knobDefs: ParameterDef[] = [];
  const extraDefs: ParameterDef[] = [];
  paramDefs.forEach((paramDef) => {
    const isContinuous = !paramDef.advanced
      && !isToggleParam(paramDef)
      && paramDef.unit !== "enum"
      && !Array.isArray(paramDef.labels)
      && Number.isFinite(paramDef.min)
      && Number.isFinite(paramDef.max)
      && paramDef.max > paramDef.min;
    if (isContinuous && knobDefs.length < MAX_PANEL_KNOBS) {
      knobDefs.push(paramDef);
    } else {
      extraDefs.push(paramDef);
    }
  });
  return { knobDefs, extraDefs };
}

function buildAmp3dKnobSpecs(node: GraphNode, knobDefs: ParameterDef[]): Amp3dKnobSpec[] {
  return knobDefs.map((paramDef) => {
    const min = paramDef.min;
    const max = paramDef.max;
    const defaultValue = Math.min(max, Math.max(min, paramDef.default));
    const current = node.params?.[paramDef.key];
    const value = typeof current === "number" && Number.isFinite(current)
      ? Math.min(max, Math.max(min, current))
      : defaultValue;
    return {
      key: paramDef.key,
      label: paramDef.name || formatParamLabel(paramDef.key),
      subLabel: undefined,
      value,
      defaultValue,
      min,
      max,
      step: typeof paramDef.step === "number" && paramDef.step > 0 ? paramDef.step : undefined,
      unit: paramDef.unit || "",
    };
  });
}

function continuousKnobDefsForNode(node: GraphNode): ParameterDef[] {
  const typeInfo = getNodeEffectInfo(node) ?? EffectTypeRegistry.get(node.type);
  const paramDefs = typeInfo?.parameters || [];
  return splitAmp3dParamDefs(paramDefs).knobDefs;
}

function buildChain3dLayoutOptions(preset: Preset): BuildChainLayoutOptions {
  const graph = preset.graph;
  const nodes = graph?.nodes ?? [];
  const edges = graph?.edges ?? [];
  const knobsByNodeId: Record<string, Amp3dKnobSpec[]> = {};
  const displayTextByNodeId: Record<string, string> = {};
  const fullRigByNodeId: Record<string, boolean> = {};

  nodes.forEach((node) => {
    knobsByNodeId[node.id] = buildAmp3dKnobSpecs(node, continuousKnobDefsForNode(node));
    // Prefer the effect title for 3D unit labels; resource summary is secondary.
    const title = (node.displayName && node.displayName.trim()) || "";
    const resource = getNodeResourceSummary(node) || "";
    displayTextByNodeId[node.id] = title || resource || node.id;
    fullRigByNodeId[node.id] = nodeUsesFullRigNamCategory(node);
  });

  return {
    graph: {
      nodes: nodes.map((node) => ({
        id: node.id,
        type: node.type,
        displayName: node.displayName,
        category: getNodeCategory(node),
        bypassed: isNodeBypassed(node),
        enabled: (node as { enabled?: boolean }).enabled,
        params: node.params,
        resources: Array.isArray((node as { resources?: unknown[] }).resources)
          ? ((node as { resources?: Array<Record<string, unknown>> }).resources ?? []).map((res) => ({
            id: typeof res?.id === "string" ? res.id : undefined,
            resourceId: typeof res?.resourceId === "string" ? res.resourceId : undefined,
            embeddedId: typeof res?.embeddedId === "string" ? res.embeddedId : undefined,
            filePath: typeof res?.filePath === "string" ? res.filePath : undefined,
          }))
          : undefined,
      })),
      edges: edges.map((edge) => ({
        from: edge.from,
        to: edge.to,
        fromPort: edge.fromPort,
        toPort: edge.toPort,
        gain: edge.gain,
      })),
    },
    knobsByNodeId,
    displayTextByNodeId,
    fullRigByNodeId,
    resolveType: (type) => EffectTypeRegistry.resolve(type),
  };
}

function buildChain3dViewOptions(preset: Preset, selectedId: string | null): Chain3dViewOptions {
  return {
    theme: themeSwitcher.getCurrentTheme(),
    layoutOptions: buildChain3dLayoutOptions(preset),
    selectedNodeId: selectedId,
    onSelectNode: (nodeId) => {
      if (!preset.graph) return;
      const target = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!target) return;
      if (selectedNodeId === target.id) {
        chain3dView?.focusNode(target.id, false);
        return;
      }
      selectedNodeId = target.id;
      renderSignalPathBar();
      showNodeParamsPanel(target, preset);
    },
    onParamChange: (nodeId, key, value) => {
      if (!preset.graph) return;
      const target = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!target) return;
      if (!target.params) target.params = {};
      target.params[key] = value;
      sendSignalPathNodeParamUpdate(nodeId, key, value);
    },
    onBypassToggle: (nodeId) => {
      if (!preset.graph) return;
      const target = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!target) return;
      toggleSignalPathNodeBypass(target, preset);
    },
  };
}

function disposeChain3dView(): void {
  chain3dMountToken += 1;
  chain3dView?.dispose();
  chain3dView = null;
  chain3dPanelActive = false;
}

/** Hides the node params panel and releases any live 3D chain resources. */
function hideNodeParamsPanel(): void {
  nodeParamsPanelElement?.classList.remove("visible");
  closeLayoutPicker();
  setAmp3dImmersiveMode(false);
  disposeChain3dView();
}

const AMP3D_DOCK_COLLAPSED_KEY = "guitarfx.amp3dDockCollapsed";

/**
 * Chain stage markup: one WebGL viewport for the whole graph plus a floating
 * dock with the selected node's model chooser and overflow parameters.
 */
function renderChain3dViewportHtml(
  node: GraphNode,
  modelChooserHtml: string,
  extraControlsHtml: string,
): string {
  const canCollapse = Boolean(extraControlsHtml);
  const dockCollapsed = canCollapse && localStorage.getItem(AMP3D_DOCK_COLLAPSED_KEY) === "true";
  const collapseBtn = canCollapse
    ? `
      <button class="amp3d-dock-collapse-btn" type="button"
        aria-expanded="${dockCollapsed ? "false" : "true"}"
        aria-label="${dockCollapsed ? "Expand chain controls" : "Collapse chain controls"}"
        title="${dockCollapsed ? "Expand chain controls" : "Collapse chain controls"}">
        <svg viewBox="0 0 16 16" width="11" height="11" aria-hidden="true">
          <path d="M3.5 5.75 8 10.25l4.5-4.5" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
        <span class="amp3d-dock-collapsed-label" aria-hidden="true"><span>ADVANCED</span></span>
      </button>
    `
    : "";
  const dock = modelChooserHtml || extraControlsHtml
    ? `
      <div class="amp3d-dock${dockCollapsed ? " is-collapsed" : ""}${canCollapse ? " has-advanced" : ""}">
        ${collapseBtn ? `<div class="amp3d-dock-header">${collapseBtn}</div>` : ""}
        ${modelChooserHtml ? `<div class="amp3d-dock-models">${modelChooserHtml}</div>` : ""}
        ${extraControlsHtml ? `
        <div class="amp3d-dock-controls">
          <div class="amp3d-dock-controls-inner">
            <div class="params-controls">${extraControlsHtml}</div>
          </div>
        </div>
        ` : ""}
      </div>
    `
    : "";
  return `
    <div class="amp3d-stage chain3d-stage has-cabinet">
      <div class="amp3d-viewport is-loading has-cabinet" data-node-id="${escapeHtml(node.id)}" data-chain3d="1">
        <div class="amp3d-placeholder">Loading 3D chain&hellip;</div>
      </div>
      ${dock}
    </div>
  `;
}

function bindAmp3dDockCollapse(): void {
  const dock = nodeParamsPanelElement?.querySelector<HTMLElement>(".amp3d-dock.has-advanced");
  const btn = dock?.querySelector<HTMLButtonElement>(".amp3d-dock-collapse-btn");
  if (!dock || !btn) {
    return;
  }

  const apply = (collapsed: boolean) => {
    dock.classList.toggle("is-collapsed", collapsed);
    btn.setAttribute("aria-expanded", collapsed ? "false" : "true");
    btn.setAttribute("aria-label", collapsed ? "Expand chain controls" : "Collapse chain controls");
    btn.title = collapsed ? "Expand chain controls" : "Collapse chain controls";
  };

  apply(localStorage.getItem(AMP3D_DOCK_COLLAPSED_KEY) === "true");

  btn.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const collapsed = !dock.classList.contains("is-collapsed");
    apply(collapsed);
    localStorage.setItem(AMP3D_DOCK_COLLAPSED_KEY, String(collapsed));
  });
}

/** Legacy hook: single-amp glow used the selected node's peak; chain stage has no global glow. */
function updateAmp3dSignalLevel(): void {
  // Intentionally empty — chain units do not currently animate a global signal glow.
}

/**
 * Immersive mode lets the chain stage claim every pixel between the effect shell
 * header and the window footer.
 */
function setAmp3dImmersiveMode(enabled: boolean): void {
  document.body.classList.toggle("amp3d-immersive", enabled);
}

/**
 * Mounts (or re-attaches) the chain 3D stage inside the params panel. The panel
 * rebuilds innerHTML on every refresh, so an existing view is moved into the
 * new container instead of being torn down.
 */
function bindChain3dView(node: GraphNode, preset: Preset): void {
  const container = nodeParamsPanelElement?.querySelector<HTMLElement>(".amp3d-viewport[data-chain3d]");
  if (!container) {
    disposeChain3dView();
    return;
  }

  chain3dPanelActive = true;
  const options = buildChain3dViewOptions(preset, node.id);

  if (chain3dView) {
    container.classList.remove("is-loading");
    container.innerHTML = "";
    container.appendChild(chain3dView.element);
    void chain3dView.update(options).catch((error) => {
      console.warn("[amp3d] failed to update 3D chain view", error);
    });
    return;
  }

  disposeChain3dView();
  chain3dPanelActive = true;
  const token = chain3dMountToken;

  void (async () => {
    try {
      const module = await import("./amp3d/index.js");
      if (token !== chain3dMountToken || !container.isConnected) {
        return;
      }
      const view = await module.Chain3dView.create(container, options);
      if (token !== chain3dMountToken || !container.isConnected) {
        view.dispose();
        return;
      }
      chain3dView = view;
      container.classList.remove("is-loading");
      container.querySelector(".amp3d-placeholder")?.remove();
    } catch (error) {
      console.warn("[amp3d] failed to load 3D chain view", error);
      if (token !== chain3dMountToken || !container.isConnected) {
        return;
      }
      container.classList.remove("is-loading");
      container.classList.add("is-error");
      container.innerHTML = `<div class="amp3d-placeholder">3D chain view unavailable. Switch back to the standard controls.</div>`;
    }
  })();
}

// ── Standard / custom layout switching ──────────────────────────────────────

/**
 * Header control for choosing how this effect is presented: the standard
 * auto-generated controls or one of the custom layouts available for it. Also the
 * only route into the layout designer, so it stays available while the layout
 * feature is on even before the effect has any layouts of its own.
 */
function renderLayoutSwitchButtonHtml(node: GraphNode, blendId: string, usingCustomLayout: boolean): string {
  const canDesign = isFeatureEnabled(Features.EffectLayout);
  if (!canDesign && !hasSelectableLayouts(node.type, blendId || undefined)) {
    return "";
  }
  const state = !areEffectLayoutsEnabled()
    ? "standard controls (custom layouts turned off)"
    : usingCustomLayout ? "custom layout" : "standard controls";
  const label = `Effect layout: ${state}`;
  return `
    <button
      class="effect-visualization-toolbar-btn node-layout-switch-btn${usingCustomLayout ? " is-active" : ""}"
      data-node-id="${escapeHtml(node.id)}"
      data-effect-type="${escapeHtml(node.type)}"
      data-blend-id="${escapeHtml(blendId)}"
      type="button"
      aria-haspopup="dialog"
      aria-expanded="false"
      title="${label} — choose layout"
      aria-label="${label}. Choose effect layout"
    >
      ${renderIcon("layout", "effect-visualization-toolbar-icon layout-switch-icon")}
    </button>
  `;
}

function bindLayoutSwitchButton(node: GraphNode, preset: Preset): void {
  const button = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".node-layout-switch-btn");
  if (!button) {
    return;
  }
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const effectType = button.dataset.effectType || node.type;
    const blendId = button.dataset.blendId || "";
    openLayoutPicker(button, {
      effectType,
      blendId: blendId || undefined,
      nodeLabel: getNodeDisplayName(node),
      matchText: buildNodeLayoutMatchText(node),
      presetId: uiState.activePresetId,
      presetName: preset.name || "",
      onApplied: () => {
        refreshSelectedNodeParams();
        renderSignalPathBar();
      },
      onDesignLayout: isFeatureEnabled(Features.EffectLayout)
        ? (layoutId) => openLayoutDesignerForNode(node, effectType, blendId, layoutId)
        : undefined,
    });
  });
}

function renderAmp3dToggleButtonHtml(_node?: GraphNode): string {
  if (!canOfferChain3dView()) {
    return "";
  }
  const enabled = isSignalChain3dEnabled();
  const label = enabled ? "Show standard controls" : "Show 3D chain";
  return `
    <button
      class="effect-visualization-toolbar-btn node-amp3d-toggle-btn${enabled ? " is-active" : ""}"
      type="button"
      aria-pressed="${enabled ? "true" : "false"}"
      title="${label}"
      aria-label="${label}"
    >
      ${renderIcon(enabled ? "sliders" : "amp", "effect-visualization-toolbar-icon amp3d-toggle-icon")}
    </button>
  `;
}

function bindAmp3dToggleButton(): void {
  const button = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".node-amp3d-toggle-btn");
  if (!button) {
    return;
  }
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const enabled = !isSignalChain3dEnabled();
    setSignalChain3dEnabled(enabled);
    disposeChain3dView();
    refreshSelectedNodeParams();
  });
}

// Lighting presets are theme-specific, so re-render while the 3D stage is on.
window.addEventListener("themeChanged", () => {
  if (chain3dView || chain3dPanelActive) {
    refreshSelectedNodeParams();
  }
});

function updateEffectVisualization(node?: GraphNode): void {
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

export function handleHostedPluginResourceLoadFailed(payload: {
  nodeId?: string;
  resourceType?: string;
  resourceId?: string;
  filePath?: string;
  resourceIndex?: number;
  message?: string;
  errorCode?: string;
}): void {
  if (payload.resourceType && payload.resourceType !== "plugin") {
    return;
  }

  const nodeId = payload.nodeId ?? "";
  if (!nodeId) {
    return;
  }

  clearHostedPluginLoadPending(nodeId);

  const resource = (payload.resourceId ? getLibraryResource("plugin", payload.resourceId) : undefined)
    ?? ({ filePath: payload.filePath ?? "" } satisfies PluginResourceSupportInfo);
  const message = payload.message?.trim() || "The selected plugin cannot be hosted by this build.";
  const failure: HostedPluginLoadFailure = {
    selectionKey: getPluginResourceSelectionKey(payload.resourceId, payload.filePath),
    resourceIndex: typeof payload.resourceIndex === "number" ? payload.resourceIndex : undefined,
    resource,
    message,
    errorCode: payload.errorCode?.trim() || undefined,
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
    hideNodeParamsPanel();
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

  const findFirstNamAmpNode = (): GraphNode | undefined =>
    nodes.find((node) =>
      node.type === EffectGuids.kAmpNam
      || node.type === EffectGuids.kAmpNamOptimized
      || node.type === EffectGuids.kAmpNamBlend
      || node.type === "amp_nam"
      || node.type === "amp_nam_optimized"
      || node.type === "amp_nam_blend");

  let replacement: GraphNode | undefined;

  // Prefer the first NAM amp node on first render for a preset or when no prior selection is available.
  if (presetChanged || selectedNodeId === null) {
    replacement = findFirstNamAmpNode();
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
  return findResourceById(resources, resourceId);
}

function getDeduplicatedLibraryResources(
  resourceType: string | undefined,
  preferredResourceIds: Iterable<string> = [],
): {
  resources: LibraryResource[];
  aliasById: Map<string, string>;
} {
  const resources = resourceType ? (uiState.resourceLibrary[resourceType] || []) : [];
  if (resourceType !== "nam" && resourceType !== "ir") {
    return {
      resources,
      aliasById: new Map(),
    };
  }

  const dedupResult = deduplicateResourcesByHashAndPath(resources, { preferredResourceIds });
  return {
    resources: dedupResult.deduped,
    aliasById: dedupResult.aliasById,
  };
}

function getCanonicalLibraryResourceId(resourceType: string | undefined, resourceId: string): string {
  if (!resourceId) {
    return resourceId;
  }

  const { aliasById } = getDeduplicatedLibraryResources(resourceType);
  return resolveResourceIdAlias(resourceId, aliasById);
}

function collectPreferredNodeResourceIds(node: GraphNode, resourceType: string): string[] {
  const resources = (node as unknown as { resources?: ResourceRef[] }).resources ?? [];
  return resources
    .filter((resource) => {
      const refType = resource.resourceType ?? resource.type ?? "";
      // Legacy refs may only carry id/resourceId with no type; still treat those
      // ids as preferred so deduplication never removes the currently selected item.
      return !refType || refType === resourceType;
    })
    .filter((resource) => Boolean(resource.resourceId ?? resource.id))
    .map((resource) => resource.resourceId ?? resource.id ?? "")
    .filter((resourceId) => Boolean(resourceId));
}

function getLibraryResourceName(resourceType: string | undefined, resourceId: string): string {
  const match = getLibraryResource(resourceType, getCanonicalLibraryResourceId(resourceType, resourceId));
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
  const baseDetail = failure.message.trim() || "The selected plugin cannot be hosted by this build.";
  const detail = failure.errorCode ? `${baseDetail} (code: ${failure.errorCode})` : baseDetail;
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
    nodeParamsPanelElement.querySelectorAll<HTMLElement>(`.resource-dropdown[data-resource-type="plugin"], .resource-picker-btn[data-resource-type="plugin"], .plugin-host-list[data-resource-type="plugin"]`),
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
  const anchorRow = container.querySelector(".plugin-host-list") ?? container.querySelector(".resource-controls");
  if (warningHtml) {
    anchorRow?.insertAdjacentHTML("afterend", warningHtml);
  }
}

function clearInlineHostedPluginLoadError(source: Element): void {
  source.closest(".node-resource-selector")?.querySelector(".plugin-host-load-error")?.remove();
}

function containsCaseInsensitive(text: string | undefined, token: string): boolean {
  return Boolean(text && token && text.toLowerCase().includes(token.toLowerCase()));
}

function isBlockedHostedPluginLibraryEntry(resourceId: string): boolean {
  const resource = getLibraryResource("plugin", resourceId);
  if (!resource) {
    return false;
  }

  return containsCaseInsensitive(resource.name, "soundshed")
    || containsCaseInsensitive(resource.filePath, "soundshed")
    || containsCaseInsensitive(resource.id, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginName, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginIdentifier, "soundshed")
    || containsCaseInsensitive(resource.metadata?.pluginStableId, "soundshed");
}

function getHostedPluginFavoriteIds(resources?: LibraryResource[]): Set<string> {
  const raw = uiState.appSettings?.[HOSTED_PLUGIN_FAVORITES_SETTING];
  if (!Array.isArray(raw)) {
    return new Set<string>();
  }

  const values = raw
    .filter((value): value is string => typeof value === "string")
    .map((value) => value.trim())
    .filter((value) => value.length > 0);

  if (!resources) {
    return new Set(values);
  }

  const validIds = new Set(resources.map((resource) => resource.id));
  return new Set(values.filter((value) => validIds.has(value)));
}

function persistHostedPluginFavoriteIds(favoriteIds: Set<string>): void {
  const payload = Array.from(favoriteIds).sort((a, b) => HOSTED_PLUGIN_NAME_COLLATOR.compare(a, b));
  uiState.appSettings[HOSTED_PLUGIN_FAVORITES_SETTING] = payload;
  setAppSetting(HOSTED_PLUGIN_FAVORITES_SETTING, payload);
}

function toggleHostedPluginFavorite(resourceId: string): void {
  const favorites = getHostedPluginFavoriteIds();
  if (favorites.has(resourceId)) {
    favorites.delete(resourceId);
  } else {
    favorites.add(resourceId);
  }
  persistHostedPluginFavoriteIds(favorites);
}

function sortHostedPluginResources(resources: LibraryResource[]): LibraryResource[] {
  const favorites = getHostedPluginFavoriteIds(resources);
  return [...resources].sort((a, b) => {
    const aFavorite = favorites.has(a.id);
    const bFavorite = favorites.has(b.id);
    if (aFavorite !== bFavorite) {
      return aFavorite ? -1 : 1;
    }
    return HOSTED_PLUGIN_NAME_COLLATOR.compare(a.name, b.name);
  });
}

function getHostedPluginPendingLoad(nodeId: string, resourceIndex: number): HostedPluginPendingLoad | null {
  const pending = hostedPluginPendingLoads.get(nodeId);
  if (!pending || pending.resourceIndex !== resourceIndex) {
    return null;
  }
  if (Date.now() - pending.startedAt > HOSTED_PLUGIN_PENDING_LOAD_MAX_AGE_MS) {
    hostedPluginPendingLoads.delete(nodeId);
    return null;
  }
  return pending;
}

function setHostedPluginLoadingIndicatorVisible(nodeId: string, resourceIndex: number, visible: boolean): void {
  const indicators = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
    `.plugin-host-loading[data-node-id="${nodeId}"]`,
  );
  indicators?.forEach((indicator) => {
    const indicatorIndex = indicator.dataset.resourceIndex ? parseInt(indicator.dataset.resourceIndex, 10) : 0;
    if (indicatorIndex !== resourceIndex) {
      return;
    }
    indicator.hidden = !visible;
    indicator.closest(".node-resource-selector")
      ?.querySelector(".plugin-host-list")
      ?.classList.toggle("is-loading", visible);
  });
}

function markHostedPluginLoadPending(nodeId: string, resourceIndex: number): void {
  hostedPluginPendingLoads.set(nodeId, { resourceIndex, startedAt: Date.now() });
  setHostedPluginLoadingIndicatorVisible(nodeId, resourceIndex, true);
}

function clearHostedPluginLoadPending(nodeId: string): void {
  const pending = hostedPluginPendingLoads.get(nodeId);
  if (!pending) {
    return;
  }
  hostedPluginPendingLoads.delete(nodeId);
  setHostedPluginLoadingIndicatorVisible(nodeId, pending.resourceIndex, false);
}

export function handleHostedPluginResourceLoadCompleted(payload: {
  nodeId?: string;
  resourceType?: string;
}): void {
  if (payload.resourceType && payload.resourceType !== "plugin") {
    return;
  }
  if (payload.nodeId) {
    clearHostedPluginLoadPending(payload.nodeId);
  }
}

export function handleNodeResourceBrowseCancelled(payload: {
  nodeId?: string;
  resourceType?: string;
}): void {
  if (payload.resourceType !== "plugin") {
    return;
  }
  if (payload.nodeId) {
    clearHostedPluginLoadPending(payload.nodeId);
  }
}

function buildHostedPluginLoadingIndicatorHtml(node: GraphNode, resourceIndex: number): string {
  const pending = getHostedPluginPendingLoad(node.id, resourceIndex);
  return `
    <div
      class="plugin-host-loading"
      data-node-id="${node.id}"
      data-resource-index="${resourceIndex}"
      role="status"
      aria-live="polite"
      ${pending ? "" : "hidden"}
    >
      <span class="plugin-host-loading-spinner" aria-hidden="true"></span>
      <span class="plugin-host-loading-label">Loading plugin…</span>
    </div>
  `;
}

function buildHostedPluginListHtml(node: GraphNode, resourceIndex: number, exposedResourceId?: string): string {
  const resources = uiState.resourceLibrary["plugin"] || [];
  const sortedResources = sortHostedPluginResources(resources);
  const favoriteIds = getHostedPluginFavoriteIds(resources);
  const current = getNodeResourceAtIndex(node, resourceIndex);
  const isLoading = Boolean(getHostedPluginPendingLoad(node.id, resourceIndex));
  const exposedAttr = exposedResourceId ? ` data-exposed-resource-id="${escapeHtml(exposedResourceId)}"` : "";

  const rows = sortedResources.map((res: LibraryResource) => {
    const isSelected = current.id === res.id && !current.filePath;
    const isFavorite = favoriteIds.has(res.id);
    const format = inferPluginFormat(res);
    const formatLabel = format ? format.toUpperCase() : "";
    const path = res.filePath || "";
    const favoriteTitle = isFavorite ? "Remove from favorites" : "Add to favorites";
    return `
      <div
        class="plugin-host-item${isSelected ? " is-selected" : ""}${isFavorite ? " is-favorite" : ""}"
        data-node-id="${node.id}"
        data-resource-id="${escapeHtml(res.id)}"
        data-resource-index="${resourceIndex}"
        ${exposedAttr}
        role="button"
        aria-selected="${isSelected ? "true" : "false"}"
        tabindex="0"
        title="${escapeHtml(path || res.name)}"
      >
        <div class="plugin-host-item-info">
          <div class="plugin-host-item-name">
            <span class="plugin-host-item-title">${escapeHtml(res.name)}</span>
            ${formatLabel ? `<span class="plugin-host-item-format">${escapeHtml(formatLabel)}</span>` : ""}
          </div>
          ${path ? `<div class="plugin-host-item-path">${escapeHtml(path)}</div>` : ""}
        </div>
        <div class="plugin-host-item-actions">
          <button
            type="button"
            class="plugin-host-favorite-btn${isFavorite ? " is-favorite" : ""}"
            data-node-id="${node.id}"
            data-resource-id="${escapeHtml(res.id)}"
            data-resource-name="${escapeHtml(res.name)}"
            title="${favoriteTitle}"
            aria-label="${favoriteTitle}: ${escapeHtml(res.name)}"
            aria-pressed="${isFavorite ? "true" : "false"}"
          >${renderIcon("star", "plugin-host-favorite-icon")}</button>
          <button
            type="button"
            class="plugin-host-remove-btn"
            data-node-id="${node.id}"
            data-resource-id="${escapeHtml(res.id)}"
            data-resource-name="${escapeHtml(res.name)}"
            title="Remove plugin from library"
            aria-label="Remove ${escapeHtml(res.name)} from library"
          >${renderIcon("close", "plugin-host-remove-icon")}</button>
        </div>
      </div>
    `;
  }).join("");

  const customRow = current.filePath ? `
    <div class="plugin-host-item is-selected is-custom" title="${escapeHtml(current.filePath)}">
      <div class="plugin-host-item-info">
        <div class="plugin-host-item-name">
          <span class="plugin-host-item-title">${escapeHtml(getResourceBaseName(current.filePath))}</span>
          ${(() => {
            const format = inferPluginFormat({ filePath: current.filePath });
            return format ? `<span class="plugin-host-item-format">${escapeHtml(format.toUpperCase())}</span>` : "";
          })()}
        </div>
        <div class="plugin-host-item-path">${escapeHtml(current.filePath)}</div>
      </div>
    </div>
  ` : "";

  const emptyRow = !rows && !customRow
    ? `<div class="plugin-host-list-empty">No plugins added yet. Use the folder button to browse for a plugin.</div>`
    : "";

  return `
    <div
      class="plugin-host-list${isLoading ? " is-loading" : ""}"
      data-node-id="${node.id}"
      data-resource-type="plugin"
      data-resource-index="${resourceIndex}"
      role="listbox"
      aria-label="Hosted plugins"
      tabindex="0"
      ${exposedAttr}
    >
      ${rows}${customRow}${emptyRow}
    </div>
  `;
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

/**
 * Lower-cased make/model text a node is matched against by keyword layout rules:
 * its display name, any loaded resource (amp model / IR / plugin) names, and the
 * effect's own display name.
 */
function buildNodeLayoutMatchText(node: GraphNode): string {
  const typeInfo = getNodeEffectInfo(node) ?? EffectTypeRegistry.get(node.type);
  return buildLayoutMatchText([
    getNodeDisplayName(node),
    getNodeResourceSummary(node),
    typeInfo?.displayName,
  ]);
}

function isNeuralModelNode(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  return resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam;
}

function isNamOrCabIrNode(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  return resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam
    || resolvedType === EffectGuids.kCabIr;
}

/**
 * Infers the resource type ("nam" | "ir" | null) from a dropped File's extension.
 */
function inferResourceTypeFromFile(file: File): "nam" | "ir" | null {
  const lower = file.name.trim().toLowerCase();
  if (lower.endsWith(".nam")) {
    return "nam";
  }
  if (lower.endsWith(".wav") || lower.endsWith(".ir") || lower.endsWith(".flac")) {
    return "ir";
  }
  return null;
}

/**
 * Returns whether this node accepts a given resource type via file drop.
 * NAM nodes accept .nam files; cab IR nodes accept IR files.
 */
function nodeAcceptsResourceType(node: GraphNode, resourceType: "nam" | "ir"): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  if (resourceType === "nam") {
    return resolvedType === EffectGuids.kAmpNam
      || resolvedType === EffectGuids.kAmpNamOptimized
      || resolvedType === EffectGuids.kFxNam;
  }
  if (resourceType === "ir") {
    return resolvedType === EffectGuids.kCabIr;
  }
  return false;
}

/**
 * Saves a dropped NAM or IR file and loads it into the given node.
 * The C++ side deduplicates by hash and calls updateNodeResource automatically
 * when a nodeId is provided.
 */
async function handleNamIrFileDrop(file: File, nodeId: string, resourceIndex?: number): Promise<void> {
  const resourceType = inferResourceTypeFromFile(file);
  if (!resourceType) {
    return;
  }

  const name = file.name.replace(/\.[^.]+$/, "");
  const data = arrayBufferToBase64(await file.arrayBuffer());

  postMessage({
    type: "saveLocalLibraryResource",
    resourceType,
    data,
    fileName: file.name,
    name,
    nodeId,
    ...(resourceIndex !== undefined ? { resourceIndex } : {}),
    category: "Local",
    metadata: { provider: "local" },
  });
}


function resolveResourceBrowserTone3000CategoryFilter(
  node: GraphNode,
  preset: Preset,
): "pedal" | "amp" | "full-rig" | undefined {
  const resolvedType = EffectTypeRegistry.resolve(node.type);

  if (resolvedType === EffectGuids.kFxNam) {
    return "pedal";
  }

  if (resolvedType === EffectGuids.kAmpNam || resolvedType === EffectGuids.kAmpNamOptimized) {
    return hasCabIrInSameSignalPath(node.id, preset) ? "amp" : "full-rig";
  }

  return undefined;
}

function resolveResourceBrowserLibraryCategoryHint(
  node: GraphNode,
  resourceType: "nam" | "ir",
): "ir" | "reverb" | undefined {
  if (resourceType !== "ir") {
    return undefined;
  }

  const category = getNodeEffectInfo(node)?.category || getNodeCategory(node);
  if (category === "reverb") {
    return "reverb";
  }
  if (category === "cab") {
    return "ir";
  }

  return undefined;
}

function resolveResourceNavigationCategoryHint(
  node: GraphNode,
  preset: Preset,
  resourceType: "nam" | "ir",
): string | undefined {
  if (resourceType === "nam") {
    return resolveResourceBrowserTone3000CategoryFilter(node, preset);
  }

  return resolveResourceBrowserLibraryCategoryHint(node, resourceType);
}

/// Identifies the kind of node a resource is being picked for, so folder browsing
/// and next/prev navigation are remembered per effect role rather than globally.
/// Deliberately coarser than the category hint: a NAM Amp keeps one remembered
/// folder whether or not it is currently acting as a full rig.
function resolveResourceContextKey(node: GraphNode, resourceType: "nam" | "ir"): string {
  if (resourceType === "nam") {
    return EffectTypeRegistry.resolve(node.type) === EffectGuids.kFxNam ? "nam-fx" : "nam-amp";
  }

  return resolveResourceBrowserLibraryCategoryHint(node, resourceType) === "reverb"
    ? "ir-reverb"
    : "ir-cab";
}

function hasCabIrInSameSignalPath(nodeId: string, preset: Preset): boolean {
  const graph = preset.graph;
  if (!graph) {
    return false;
  }

  const { nodeById, outgoing, incoming } = buildGraphMaps(graph);
  if (!nodeById.has(nodeId)) {
    return false;
  }

  const isCabNode = (candidateId: string): boolean => {
    const candidate = nodeById.get(candidateId);
    if (!candidate) {
      return false;
    }
    return EffectTypeRegistry.resolve(candidate.type) === EffectGuids.kCabIr;
  };

  const search = (direction: "downstream" | "upstream"): boolean => {
    const visited = new Set<string>([nodeId]);
    const queue: string[] = [nodeId];

    while (queue.length > 0) {
      const current = queue.shift();
      if (!current) {
        continue;
      }

      const edges = direction === "downstream"
        ? (outgoing.get(current) ?? [])
        : (incoming.get(current) ?? []);
      for (const edge of edges) {
        const nextId = direction === "downstream" ? edge.to : edge.from;
        if (!nextId || visited.has(nextId)) {
          continue;
        }
        if (isCabNode(nextId)) {
          return true;
        }
        visited.add(nextId);
        queue.push(nextId);
      }
    }

    return false;
  };

  return search("downstream") || search("upstream");
}

function shouldShowFullRigCabModelNote(node: GraphNode, preset: Preset): boolean {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kCabIr) {
    return false;
  }
  return hasFullRigNamInSameSignalPath(node.id, preset);
}

function hasFullRigNamInSameSignalPath(nodeId: string, preset: Preset): boolean {
  const graph = preset.graph;
  if (!graph) {
    return false;
  }

  const { nodeById, outgoing, incoming } = buildGraphMaps(graph);
  if (!nodeById.has(nodeId)) {
    return false;
  }

  const isFullRigNamNode = (candidateId: string): boolean => {
    const candidate = nodeById.get(candidateId);
    if (!candidate) {
      return false;
    }
    return nodeUsesFullRigNamCategory(candidate);
  };

  const visited = new Set<string>([nodeId]);
  const queue: string[] = [nodeId];
  while (queue.length > 0) {
    const current = queue.shift();
    if (!current) {
      continue;
    }

    const neighbors = [
      ...(outgoing.get(current) ?? []).map((edge) => edge.to),
      ...(incoming.get(current) ?? []).map((edge) => edge.from),
    ];
    for (const nextId of neighbors) {
      if (!nextId || visited.has(nextId)) {
        continue;
      }
      if (isFullRigNamNode(nextId)) {
        return true;
      }
      visited.add(nextId);
      queue.push(nextId);
    }
  }

  return false;
}

function nodeUsesFullRigNamCategory(node: GraphNode): boolean {
  const resolvedType = EffectTypeRegistry.resolve(node.type);
  const isNamEffect = resolvedType === EffectGuids.kAmpNam
    || resolvedType === EffectGuids.kAmpNamOptimized
    || resolvedType === EffectGuids.kFxNam
    || resolvedType === EffectGuids.kAmpNamBlend;
  if (!isNamEffect) {
    return false;
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];
  const resourceCount = Math.max(1, resources.length);
  for (let index = 0; index < resourceCount; index += 1) {
    const resource = getNodeResourceAtIndex(node, index);
    if (!resource.id) {
      continue;
    }

    const libraryResource = getLibraryResource("nam", resource.id);
    const category = normalizeNamGearCategory(
      String(libraryResource?.category ?? libraryResource?.metadata?.gear ?? ""),
    );
    if (category === "full-rig") {
      return true;
    }

    // Fall back to NAM gear_type metadata (e.g. "amp_cab" / "amp+cab") from the
    // model file header. If gear_type contains "cab", the capture includes a
    // cabinet model and should be treated as a full rig.
    const gearType = String(libraryResource?.metadata?.gear_type ?? "").toLowerCase();
    if (gearType && gearType.includes("cab")) {
      return true;
    }
  }

  return false;
}

function normalizeNamGearCategory(raw: string): "pedal" | "preamp" | "amp" | "full-rig" | "cab" | "" {
  const value = raw.trim().toLowerCase();
  if (!value) {
    return "";
  }
  if (value === "outboard") {
    return "preamp";
  }
  if (value === "pedal" || value === "preamp" || value === "amp" || value === "full-rig" || value === "cab") {
    return value;
  }
  return "";
}

function normalizeArchitectureBadge(raw: string): string {
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }
  if (normalized === "2" || normalized === "a2") {
    return "A2";
  }
  if (normalized === "1" || normalized === "a1") {
    return "A1";
  }
  if (normalized === "custom") {
    return "Custom";
  }
  return "";
}

function mapNamArchitectureTokenToBadge(raw: string): string {
  const normalized = raw.trim().toLowerCase();
  if (!normalized) {
    return "";
  }

  if (normalized.includes("slimmablecontainer") || normalized.includes("slimmable")) {
    return "A2";
  }
  if (normalized.includes("wavenet")) {
    return "A1";
  }

  return "";
}

function inferNamArchitectureBadgeFromData(base64Data: string): string {
  let text = "";
  try {
    text = new TextDecoder().decode(base64ToArrayBuffer(base64Data));
  } catch {
    return "";
  }

  const architectureMatch = text.match(/"architecture(?:_version|Version)?"\s*:\s*(?:"([^"]+)"|(\d+(?:\.\d+)?))/i);
  const explicitToken = (architectureMatch?.[1] || architectureMatch?.[2] || "").trim();
  const explicitBadge = normalizeArchitectureBadge(explicitToken) || mapNamArchitectureTokenToBadge(explicitToken);
  if (explicitBadge) {
    return explicitBadge;
  }

  if (/"architecture"\s*:\s*"slimmablecontainer"/i.test(text) || /"submodels"\s*:/i.test(text)) {
    return "A2";
  }
  if (/"architecture"\s*:\s*"wavenet"/i.test(text)) {
    return "A1";
  }

  return "";
}

function requestNamArchitectureInference(resource: LibraryResource): void {
  const resourceId = resource.id;
  if (!resourceId
      || inferredNamArchitectureByResourceId.has(resourceId)
      || pendingNamArchitectureResourceIds.has(resourceId)
      || unavailableNamArchitectureResourceIds.has(resourceId)) {
    return;
  }

  pendingNamArchitectureResourceIds.add(resourceId);

  void (async () => {
    try {
      const data = await requestResourceData("nam", resourceId);
      if (!data) {
        unavailableNamArchitectureResourceIds.add(resourceId);
        return;
      }

      const badge = inferNamArchitectureBadgeFromData(data);
      if (!badge) {
        unavailableNamArchitectureResourceIds.add(resourceId);
        return;
      }

      inferredNamArchitectureByResourceId.set(resourceId, badge);

      const existingBadge = normalizeArchitectureBadge(
        resource.metadata?.architectureVersion
        || resource.metadata?.architecture_version
        || resource.metadata?.architecture
        || "",
      );

      if (!existingBadge) {
        const metadata = {
          ...(resource.metadata ?? {}),
          architectureVersion: badge,
        };
        resource.metadata = metadata;

        postMessage({
          type: "updateLibraryResource",
          resourceType: "nam",
          resourceId,
          name: resource.name,
          category: resource.category,
          description: resource.description,
          metadata,
        });
      }

      renderSignalPathBar();
    } catch {
      unavailableNamArchitectureResourceIds.add(resourceId);
    } finally {
      pendingNamArchitectureResourceIds.delete(resourceId);
    }
  })();
}

function getNodeArchitectureBadge(node: GraphNode): string {
  if (!isNeuralModelNode(node)) {
    return "";
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];

  for (let index = 0; index < Math.max(1, resources.length); index += 1) {
    const current = getNodeResourceAtIndex(node, index);
    if (!current.id) {
      continue;
    }

    const resource = getLibraryResource("nam", current.id);
    const metadata = resource?.metadata ?? {};
    const label = normalizeArchitectureBadge(
      metadata.architectureVersion
      || metadata.architecture_version
      || metadata.architecture
      || "",
    );
    if (label) {
      return label;
    }

    if (resource?.id && inferredNamArchitectureByResourceId.has(resource.id)) {
      const inferred = inferredNamArchitectureByResourceId.get(resource.id);
      if (inferred) {
        return inferred;
      }
    }

    if (resource) {
      requestNamArchitectureInference(resource);
    }
  }

  return "";
}

function hasNamCalibrationMetadataValue(value: string | undefined): boolean {
  if (typeof value !== "string") {
    return false;
  }
  const trimmed = value.trim();
  if (!trimmed.length) {
    return false;
  }
  const parsed = Number(trimmed);
  return Number.isFinite(parsed);
}

function isNodeFullyCalibratedFromNamMetadata(node: GraphNode): boolean {
  if (!isNeuralModelNode(node)) {
    return false;
  }

  const resources = Array.isArray((node as unknown as { resources?: unknown[] }).resources)
    ? (node as unknown as { resources?: unknown[] }).resources ?? []
    : [];
  const resourceCount = Math.max(1, resources.length);

  let modelsWithCalibration = 0;
  let consideredModels = 0;
  for (let index = 0; index < resourceCount; index += 1) {
    const current = getNodeResourceAtIndex(node, index);
    if (!current.id) {
      continue;
    }
    consideredModels += 1;
    const resource = getLibraryResource("nam", current.id);
    const metadata = resource?.metadata ?? {};
    const hasInputCalibration = hasNamCalibrationMetadataValue(metadata.inputLevelDbu);
    const hasOutputCalibration = hasNamCalibrationMetadataValue(metadata.outputLevelDbu);
    if (hasInputCalibration && hasOutputCalibration) {
      modelsWithCalibration += 1;
    }
  }

  return consideredModels > 0 && modelsWithCalibration === consideredModels;
}

function getNodeNamCalibrationMetadataChip(node: GraphNode): string {
  if (!isNodeFullyCalibratedFromNamMetadata(node)) {
    return "";
  }

  return `<span class="default-effect-shell-chip default-effect-shell-chip-calibration default-effect-shell-chip-calibration-complete" title="Loaded model includes both input and output calibration metadata (inputLevelDbu/outputLevelDbu).">Calibrated</span>`;
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

export function isNodeBypassed(node: GraphNode): boolean {
  const anyNode = node as unknown as { bypassed?: unknown; enabled?: unknown };
  if (typeof anyNode.bypassed === "boolean") return anyNode.bypassed;
  if (typeof anyNode.enabled === "boolean") return !anyNode.enabled;
  return false;
}

export function applySignalPathNodeBypassState(node: GraphNode, preset: Preset, bypassed: boolean): void {
  sendSignalPathNodeBypassUpdate(node.id, preset.id, bypassed);
  (node as unknown as { bypassed?: boolean }).bypassed = bypassed;
  (node as unknown as { enabled?: boolean }).enabled = !bypassed;
  renderSignalPathBar();
  if (selectedNodeId === node.id && nodeParamsPanelElement?.classList.contains("visible")) {
    updateSelectedNodeBypassControl(bypassed);
  }
  if (selectedNodeId === node.id) {
    queueMicrotask(() => {
      const selectedNode = signalPathNodesElement?.querySelector<HTMLElement>(`.signal-node[data-node-id="${node.id}"]`);
      selectedNode?.focus({ preventScroll: true });
    });
  }

  function updateSelectedNodeBypassControl(bypassed: boolean): void {
    const toggle = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".node-bypass-btn");
    const shell = nodeParamsPanelElement?.querySelector<HTMLElement>(".default-effect-shell");
    const label = toggle?.querySelector<HTMLElement>(".default-effect-shell-toggle-label");
    if (!toggle || !shell || !label) {
      return;
    }

    toggle.classList.toggle("bypassed", bypassed);
    toggle.setAttribute("aria-checked", String(!bypassed));
    toggle.title = bypassed ? "Enable effect" : "Bypass effect";
    toggle.setAttribute("aria-label", toggle.title);
    label.textContent = bypassed ? "Off" : "On";
    shell.classList.toggle("is-bypassed", bypassed);
  }
}

function toggleSignalPathNodeBypass(node: GraphNode, preset: Preset): void {
  applySignalPathNodeBypassState(node, preset, !isNodeBypassed(node));
}

function isProtectedSignalPathNode(node: GraphNode): boolean {
  return node.type === EffectGuids.kSplitter || node.type === EffectGuids.kMixer;
}

export function isToggleableSignalPathNode(node: GraphNode | null | undefined): node is GraphNode {
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
document.addEventListener("resource-browser:navigation-cache-updated", () => {
  refreshSelectedNodeParams();
});

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

  try {
    renderSignalPathBarContent();
  } finally {
    scheduleSignalPathLayoutAdapt();
  }
}

function renderSignalPathBarContent(): void {
  if (!signalPathNodesElement) {
    return;
  }

  const signalPathBar = document.getElementById("signal-path-bar");
  const sceneToolbarHost = document.getElementById("signal-path-scene-toolbar");
  const toolbarRow = document.getElementById("signal-path-toolbar");
  signalPathBar?.classList.toggle("mix-tab-active", mixTabActive);
  signalPathBar?.classList.toggle("preset-loading", Boolean(uiState.presetLoadingId));

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
    updateSignalPathAddMenuAvailability(false);
    // Pin the mixer to the same height as the full-size signal chain,
    // independent of whichever compact/full density was active before
    // switching to Mix (updateSignalPathLayoutAdapt() skips recomputing
    // --signal-path-scroll-height while mixTabActive, so without this it
    // would otherwise keep a stale, possibly-compact height).
    signalPathBar?.style.setProperty("--signal-path-scroll-height", `${SIGNAL_PATH_FULL_HEIGHT}px`);
    renderInlineMixer();
    return;
  }
  if (scroll) scroll.hidden = false;
  removeInlineMixer();

  const activePresetId = uiState.activePresetId;
  const activePreset = getSignalPathPreset() ?? undefined;
  updateSignalPathAddMenuAvailability(Boolean(activePreset));
  // Track the rendered preset's own ID so that switching mixer tabs (which
  // changes focusedMixerPresetId but NOT activePresetId) is also detected.
  const renderedPresetId = activePreset?.id ?? activePresetId;
  const presetChanged = renderedPresetId !== lastRenderedPresetId;
  lastRenderedPresetId = renderedPresetId ?? null;
  
  if (!activePreset) {
    // Show placeholder signal chain (no preset loaded yet)
    if (signalPathNodesElement) {
      signalPathNodesElement.innerHTML = `
        <div class="signal-node input-node" data-node-id="__input__" title="Input" aria-label="Input">
          <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/amp.svg')" aria-hidden="true"></span>
          </div>
          <span class="node-label">Input</span>
        </div>
        <div class="signal-connector"></div>
        <div class="signal-node" data-node-id="placeholder" title="No Preset" aria-label="No Preset">
          <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/bolt.svg')" aria-hidden="true"></span>
          </div>
          <span class="node-label">No Preset</span>
        </div>
        <div class="signal-connector"></div>
        <div class="signal-node output-node" data-node-id="__output__" title="Output" aria-label="Output">
          <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/speaker.svg')" aria-hidden="true"></span>
          </div>
          <span class="node-label">Output</span>
        </div>`;
    }
    if (sceneToolbarHost) sceneToolbarHost.innerHTML = "";
    toolbarRow?.classList.add("scene-toolbar-empty");
    updateSignalPathAddMenuAvailability(false);
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
          <div class="signal-node input-node" data-node-id="__input__" title="Input" aria-label="Input">
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
          <div class="signal-node output-node" data-node-id="__output__" title="Output" aria-label="Output">
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

function getSelectedNodeDiagnosticsEntry(): import("./types.js").SignalLevelNodeMetrics | null {
  if (!selectedNodeId) {
    return null;
  }

  const diagnostics = uiState.signalDiagnostics;
  if (!diagnostics) {
    return null;
  }

  if (selectedNodeId === "__input__") {
    if (!diagnostics.input) {
      return null;
    }
    return {
      scope: "pre",
      nodeId: "__input__",
      nodeType: "input",
      levels: diagnostics.input,
    };
  }
  if (selectedNodeId === "__output__") {
    if (!diagnostics.output) {
      return null;
    }
    return {
      scope: "post",
      nodeId: "__output__",
      nodeType: "output",
      levels: diagnostics.output,
    };
  }

  return diagnostics.nodes.find((entry) => entry.nodeId === selectedNodeId) ?? null;
}

function getSelectedNodeDiagnostics(): import("./types.js").SignalLevelMetrics | null {
  return getSelectedNodeDiagnosticsEntry()?.levels ?? null;
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
    updateAmp3dSignalLevel();
    return;
  }

  const meter = rail.querySelector<HTMLElement>(".default-effect-shell-meter");
  if (!meter) {
    updateAmp3dSignalLevel();
    return;
  }

  const metrics = getSelectedNodeDiagnostics();
  rail.classList.remove("is-inactive", "is-clipped");

  if (!metrics || !Number.isFinite(metrics.peakDbfs)) {
    rail.classList.add("is-inactive");
    rail.title = "No diagnostics data for this node";
    meter.style.setProperty("--meter-fill-scale", "0");
    updateAmp3dSignalLevel();
    return;
  }

  const normalized = normalizePeakDbfsForShellMeter(metrics.peakDbfs);
  meter.style.setProperty("--meter-fill-scale", normalized.toFixed(3));

  if (metrics.clipped || metrics.peakDbfs >= -0.3) {
    rail.classList.add("is-clipped");
  }

  rail.title = `Node peak: ${metrics.peakDbfs.toFixed(1)} dBFS · Headroom: ${metrics.headroomDb.toFixed(1)} dB`;
  // Keep the 3D amp glow in step with the same diagnostics stream as the meter.
  // Prefer the DSP-status peak average when available (updated just after this).
  updateAmp3dSignalLevel();
}

function formatDspStatusDb(value: number | null | undefined): string {
  return Number.isFinite(value) ? `${value!.toFixed(1)} dBFS` : "—";
}

function formatDspStatusHeadroom(value: number | null | undefined): string {
  return Number.isFinite(value) ? `${value!.toFixed(1)} dB` : "—";
}

function getSelectedNodeDspStatusPerformanceKey(
  node: import("./types.js").SignalLevelNodeMetrics,
): string {
  return node.scope === "preset"
    ? `${node.presetId ?? uiState.activePresetId ?? ""}::${node.nodeId}`
    : `${node.scope}::${node.nodeId}`;
}

function getSelectedNodeDspStatusTimeUs(
  node: import("./types.js").SignalLevelNodeMetrics | null,
): number | null {
  if (!node) {
    return null;
  }

  const performance = uiState.dspPerformance;
  const scopedTime = performance?.scopedNodeProcessingTimesUs?.[
    getSelectedNodeDspStatusPerformanceKey(node)
  ];
  if (typeof scopedTime === "number" && Number.isFinite(scopedTime)) {
    return scopedTime;
  }

  const legacyTime = performance?.nodeProcessingTimesUs?.[node.nodeId];
  return typeof legacyTime === "number" && Number.isFinite(legacyTime) ? legacyTime : null;
}

function getSelectedNodeDspStatusLatencySamples(
  node: import("./types.js").SignalLevelNodeMetrics | null,
): number | null {
  if (!node) {
    return null;
  }

  const performance = uiState.dspPerformance;
  const scopedLatency = performance?.scopedNodeLatencySamples?.[
    getSelectedNodeDspStatusPerformanceKey(node)
  ];
  if (typeof scopedLatency === "number" && Number.isFinite(scopedLatency)) {
    return scopedLatency;
  }

  const legacyLatency = performance?.nodeLatencySamples?.[node.nodeId];
  return typeof legacyLatency === "number" && Number.isFinite(legacyLatency) ? legacyLatency : null;
}

function formatDspStatusTime(timeUs: number | null): string {
  if (timeUs === null) {
    return "—";
  }

  const totalTimeUs = uiState.dspPerformance?.totalProcessingTimeUs;
  const share = typeof totalTimeUs === "number" && totalTimeUs > 0
    ? ` (${((timeUs / totalTimeUs) * 100).toFixed(1)}%)`
    : "";
  return `${timeUs.toFixed(1)} μs${share}`;
}

function formatDspStatusLatency(latencySamples: number | null): string {
  if (latencySamples === null || latencySamples <= 0) {
    return "—";
  }

  const sampleRate = uiState.dspPerformance?.sampleRate;
  const milliseconds = typeof sampleRate === "number" && sampleRate > 0
    ? ` (${((latencySamples / sampleRate) * 1000).toFixed(2)} ms)`
    : "";
  return `${latencySamples} smp${milliseconds}`;
}

function addDspStatusSample(name: string, value: number | null): number | null {
  if (value === null || !Number.isFinite(value)) {
    return null;
  }

  const previous = selectedNodeDspStatusAverages.get(name);
  const average = previous === undefined
    ? value
    : previous + DSP_STATUS_AVERAGE_SMOOTHING * (value - previous);
  selectedNodeDspStatusAverages.set(name, average);
  return average;
}

export function updateSelectedNodeDspStatus(): void {
  const status = nodeParamsPanelElement?.querySelector<HTMLElement>(".effect-dsp-status");
  const diagnostics = getSelectedNodeDiagnosticsEntry();
  const metrics = diagnostics?.levels;
  // Always keep the peak average warm so the 3D amp glow can use it even when
  // the DSP status panel is hidden.
  addDspStatusSample("peak", metrics?.peakDbfs ?? null);
  updateAmp3dSignalLevel();

  if (!status || !selectedNodeDspStatusVisible) {
    return;
  }

  const timeUs = getSelectedNodeDspStatusTimeUs(diagnostics);
  const latencySamples = getSelectedNodeDspStatusLatencySamples(diagnostics);
  const values: Record<string, string> = {
    peak: formatDspStatusDb(selectedNodeDspStatusAverages.get("peak") ?? null),
    rms: formatDspStatusDb(addDspStatusSample("rms", metrics?.rmsDbfs ?? null)),
    headroom: formatDspStatusHeadroom(addDspStatusSample("headroom", metrics?.headroomDb ?? null)),
    processing: formatDspStatusTime(addDspStatusSample("processing", timeUs)),
    latency: formatDspStatusLatency(addDspStatusSample("latency", latencySamples)),
  };

  const now = performance.now();
  const shouldRenderAverage = lastDspStatusAverageRenderAt === 0
    || now - lastDspStatusAverageRenderAt >= DSP_STATUS_AVERAGE_RENDER_INTERVAL_MS;
  Object.entries(values).forEach(([name, value]) => {
    const average = status.querySelector<HTMLElement>(`[data-dsp-status-average="${name}"]`);
    if (average && shouldRenderAverage && average.textContent !== value) {
      average.textContent = value;
    }
  });
  if (shouldRenderAverage) {
    lastDspStatusAverageRenderAt = now;
  }
}

function bindSelectedNodeDspStatusToggle(): void {
  const meterToggle = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".default-effect-shell-meter-toggle");
  const dspBadge = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".dsp-badge-toggle");
  const status = nodeParamsPanelElement?.querySelector<HTMLElement>(".effect-dsp-status");
  if (!status) {
    return;
  }

  function setDspVisible(visible: boolean): void {
    selectedNodeDspStatusVisible = visible;
    meterToggle?.setAttribute("aria-expanded", String(visible));
    if (meterToggle) {
      meterToggle.title = visible ? "Hide DSP status" : "Show DSP status";
    }
    if (dspBadge) {
      dspBadge.setAttribute("aria-expanded", String(visible));
      dspBadge.title = visible ? "Hide DSP status" : "Show DSP status";
      dspBadge.classList.toggle("is-active", visible);
    }
    status!.hidden = !visible;
    updateSelectedNodeDspStatus();
  }

  meterToggle?.addEventListener("click", () => setDspVisible(!selectedNodeDspStatusVisible));
  dspBadge?.addEventListener("click", () => setDspVisible(!selectedNodeDspStatusVisible));

  status.querySelector<HTMLButtonElement>(".effect-dsp-status-close")?.addEventListener("click", () => {
    setDspVisible(false);
  });
}

function formatAnalyzerNumeric(value: number, unit: string, fractionDigits = 1): string {
  if (!Number.isFinite(value)) {
    return "—";
  }
  return `${value.toFixed(fractionDigits)} ${unit}`;
}

function formatAnalyzerChannelMode(levels: import("./types.js").InputAnalyzerLevelTelemetry): string {
  const isStereo = typeof levels.channelMode === "string"
    ? levels.channelMode.toLowerCase() === "stereo"
    : Boolean(levels.stereo);
  const label = isStereo ? "Stereo" : "Mono";
  const channels = levels.activeChannelCount;
  if (Number.isFinite(channels) && (channels as number) > 0) {
    return `${label} (${channels} ch)`;
  }
  return label;
}

function formatAnalyzerLufs(value: number | undefined, enabled = true): string {
  if (!enabled || !Number.isFinite(value)) {
    return "—";
  }
  return `${value!.toFixed(1)} LUFS`;
}

function percentFsToDbfs(percentFs: number): number {
  if (!Number.isFinite(percentFs)) {
    return Number.NaN;
  }
  const linear = Math.max(0, Math.min(1, percentFs / 100));
  if (linear <= 1.0e-9) {
    return -120;
  }
  return 20 * Math.log10(linear);
}

function drawAnalyzerSpectrogram(canvas: HTMLCanvasElement, history: number[][], minDbfs: number, maxDbfs: number): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = Math.max(1, Math.floor(canvas.clientWidth || 1));
  const height = Math.max(1, Math.floor(canvas.clientHeight || 1));
  const dpr = window.devicePixelRatio || 1;
  const targetWidth = Math.max(1, Math.round(width * dpr));
  const targetHeight = Math.max(1, Math.round(height * dpr));
  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    canvas.width = targetWidth;
    canvas.height = targetHeight;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "rgba(8, 10, 18, 0.92)";
  ctx.fillRect(0, 0, width, height);

  if (!history.length) {
    return;
  }

  const bins = history[0]?.length ?? 0;
  if (!bins) {
    return;
  }

  const dbRange = Math.max(1, maxDbfs - minDbfs);
  const columns = Math.min(width, history.length);
  const start = Math.max(0, history.length - columns);
  const rowHeight = height / bins;

  for (let x = 0; x < columns; ++x) {
    const frame = history[start + x];
    if (!Array.isArray(frame)) {
      continue;
    }
    for (let y = 0; y < bins; ++y) {
      const db = Number(frame[y]);
      const norm = Math.max(0, Math.min(1, (db - minDbfs) / dbRange));
      if (norm <= 0.001) {
        continue;
      }
      const hue = 230 - Math.round(norm * 190);
      const saturation = 76 + Math.round(norm * 18);
      const lightness = 12 + Math.round(norm * 56);
      ctx.fillStyle = `hsl(${hue} ${saturation}% ${lightness}%)`;
      ctx.fillRect(x, height - (y + 1) * rowHeight, 1, Math.max(1, rowHeight + 0.5));
    }
  }
}

function drawAnalyzerBarkBands(canvas: HTMLCanvasElement, bandsDb: number[], minDbfs: number, maxDbfs: number): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = Math.max(1, Math.floor(canvas.clientWidth || 1));
  const height = Math.max(1, Math.floor(canvas.clientHeight || 1));
  const dpr = window.devicePixelRatio || 1;
  const targetWidth = Math.max(1, Math.round(width * dpr));
  const targetHeight = Math.max(1, Math.round(height * dpr));
  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    canvas.width = targetWidth;
    canvas.height = targetHeight;
  }

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "rgba(8, 10, 18, 0.92)";
  ctx.fillRect(0, 0, width, height);

  if (!Array.isArray(bandsDb) || !bandsDb.length) {
    return;
  }

  const barCount = bandsDb.length;
  const barGap = 1;
  const barWidth = Math.max(1, (width - (barCount - 1) * barGap) / barCount);
  const dbRange = Math.max(1, maxDbfs - minDbfs);

  for (let i = 0; i < barCount; ++i) {
    const db = Number(bandsDb[i]);
    const normalized = Math.max(0, Math.min(1, (db - minDbfs) / dbRange));
    const barHeight = Math.max(0, normalized * height);
    const x = i * (barWidth + barGap);
    const hue = 232 - Math.round(normalized * 168);
    const saturation = 76 + Math.round(normalized * 14);
    const lightness = 14 + Math.round(normalized * 58);
    ctx.fillStyle = `hsl(${hue} ${saturation}% ${lightness}%)`;
    ctx.fillRect(x, height - barHeight, barWidth, barHeight);
  }
}

export function updateSelectedNodeAnalyzerPanel(): void {
  const analyzerPanel = nodeParamsPanelElement?.querySelector<HTMLElement>(".input-analyzer-panel");
  if (!analyzerPanel || !selectedNodeId) {
    return;
  }

  const diagnostics = getSelectedNodeDiagnosticsEntry();
  const analyzer = diagnostics?.analyzer;
  const levels = analyzer?.levels;
  const spectrogram = analyzer?.spectrogram;
  const bark = analyzer?.bark;

  const peakPercentEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="peakPercent"]');
  const rmsPercentEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsPercent"]');
  const rmsDbuEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbu"]');
  const rmsDbvEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbv"]');
  const rmsVoltsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsVolts"]');
  const momentaryLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="momentaryLufs"]');
  const shortTermLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="shortTermLufs"]');
  const integratedLufsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="integratedLufs"]');
  const channelModeEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="channelMode"]');
  const peakDbfsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="peakDbfs"]');
  const rmsDbfsEl = analyzerPanel.querySelector<HTMLElement>('[data-analyzer-field="rmsDbfs"]');
  const updatedAtEl = analyzerPanel.querySelector<HTMLElement>(".input-analyzer-updated");
  const canvas = analyzerPanel.querySelector<HTMLCanvasElement>(".input-analyzer-spectrogram-canvas");
  const barkCanvas = analyzerPanel.querySelector<HTMLCanvasElement>(".input-analyzer-bark-canvas");

  if (!levels || !spectrogram || !Array.isArray(spectrogram.binsDb)) {
    if (peakPercentEl) peakPercentEl.textContent = "—";
    if (rmsPercentEl) rmsPercentEl.textContent = "—";
    if (rmsDbuEl) rmsDbuEl.textContent = "—";
    if (rmsDbvEl) rmsDbvEl.textContent = "—";
    if (rmsVoltsEl) rmsVoltsEl.textContent = "—";
    if (momentaryLufsEl) momentaryLufsEl.textContent = "—";
    if (shortTermLufsEl) shortTermLufsEl.textContent = "—";
    if (integratedLufsEl) integratedLufsEl.textContent = "—";
    if (channelModeEl) channelModeEl.textContent = "—";
    if (peakDbfsEl) peakDbfsEl.textContent = "—";
    if (rmsDbfsEl) rmsDbfsEl.textContent = "—";
    if (updatedAtEl) updatedAtEl.textContent = "Waiting for live analyzer data…";
    if (canvas) {
      drawAnalyzerSpectrogram(canvas, [], -120, 0);
    }
    if (barkCanvas) {
      drawAnalyzerBarkBands(barkCanvas, [], -96, 0);
    }
    return;
  }

  if (peakPercentEl) peakPercentEl.textContent = formatAnalyzerNumeric(levels.peakPercent, "%");
  if (rmsPercentEl) rmsPercentEl.textContent = formatAnalyzerNumeric(levels.rmsPercent, "%");
  if (rmsDbuEl) rmsDbuEl.textContent = formatAnalyzerNumeric(levels.rmsDbu, "dBu");
  if (rmsDbvEl) rmsDbvEl.textContent = formatAnalyzerNumeric(levels.rmsDbv, "dBV");
  if (rmsVoltsEl) rmsVoltsEl.textContent = formatAnalyzerNumeric(levels.rmsVolts, "Vrms", 3);
  if (momentaryLufsEl) momentaryLufsEl.textContent = formatAnalyzerLufs(levels.momentaryLufs, levels.loudnessValid !== false);
  if (shortTermLufsEl) shortTermLufsEl.textContent = formatAnalyzerLufs(levels.shortTermLufs, levels.loudnessValid !== false);
  if (integratedLufsEl) integratedLufsEl.textContent = formatAnalyzerLufs(levels.integratedLufs, levels.loudnessValid !== false);
  if (channelModeEl) channelModeEl.textContent = formatAnalyzerChannelMode(levels);
  if (peakDbfsEl) peakDbfsEl.textContent = formatAnalyzerNumeric(percentFsToDbfs(levels.peakPercent), "dBFS");
  if (rmsDbfsEl) rmsDbfsEl.textContent = formatAnalyzerNumeric(percentFsToDbfs(levels.rmsPercent), "dBFS");

  if (updatedAtEl) {
    updatedAtEl.textContent = Number.isFinite(spectrogram.generatedAtMs)
      ? `Updated: ${new Date(Number(spectrogram.generatedAtMs)).toLocaleTimeString()}`
      : "Updated: live";
  }

  const history = analyzerSpectrogramHistoryByNode.get(selectedNodeId) ?? [];
  history.push(spectrogram.binsDb.slice());
  while (history.length > ANALYZER_SPECTROGRAM_HISTORY_FRAMES) {
    history.shift();
  }
  analyzerSpectrogramHistoryByNode.set(selectedNodeId, history);

  if (canvas) {
    drawAnalyzerSpectrogram(
      canvas,
      history,
      Number.isFinite(spectrogram.minDbfs) ? spectrogram.minDbfs : -120,
      Number.isFinite(spectrogram.maxDbfs) ? spectrogram.maxDbfs : 0,
    );
  }
  if (barkCanvas) {
    drawAnalyzerBarkBands(
      barkCanvas,
      Array.isArray(bark?.bandsDb) ? bark.bandsDb : [],
      Number.isFinite(bark?.minDbfs) ? Number(bark?.minDbfs) : -96,
      Number.isFinite(bark?.maxDbfs) ? Number(bark?.maxDbfs) : 0,
    );
  }
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

    // Collapse/remove lives on the mixer node delete control (same as other effects),
    // not on the decorative join icon.
    const mixerNodeHtml = joinNode
      ? renderNodeElement(joinNode, canCollapse ? { collapseSplitterId: splitterId } : undefined)
      : "";

    const html = `
      <div class="parallel-container" data-splitter-id="${splitterId}" data-mixer-id="${joinId}">
        <div class="parallel-split">
          <div class="split-icon" aria-hidden="true">
            ${renderIcon("parallel-split", "parallel-flow-icon")}
          </div>
        </div>
        <div class="parallel-branches">
          ${branchesHtml}
        </div>
        <div class="parallel-join">
          <div class="join-icon" aria-hidden="true">
            ${renderIcon("parallel-join", "parallel-flow-icon")}
          </div>
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
        <div class="signal-node input-node" data-node-id="__input__" title="Input" aria-label="Input">
          <div class="node-icon"><span class="fx-effect-icon" style="--icon-url: url('/images/icons/guitar.svg')" aria-hidden="true"></span></div>
          <div class="node-info">
            <div class="node-name">Input</div>
          </div>
          <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
        </div>
        ${segmentsHtml}
        <div class="signal-node output-node" data-node-id="__output__" title="Output" aria-label="Output">
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

type RenderNodeElementOptions = {
  /** When set on a mixer join node, delete collapses the empty parallel split. */
  collapseSplitterId?: string;
};

/**
 * Renders a single effect node.
 */
function renderNodeElement(node: GraphNode, options?: RenderNodeElementOptions): string {
  const icon = getNodeIcon(node.type);
  const categoryClass = getCategoryClass(getNodeCategory(node));
  const nodeBypassed = isNodeBypassed(node);
  const bypassedClass = nodeBypassed ? "bypassed" : "";
  const selectedClass = selectedNodeId === node.id ? "selected" : "";
  const missingEntries = getMissingResourceEntries(node);
  const missingClass = missingEntries.length ? "missing-resource" : "";
  const collapseSplitterId = options?.collapseSplitterId;
  const canCollapseParallel =
    node.type === EffectGuids.kMixer && typeof collapseSplitterId === "string" && collapseSplitterId.length > 0;
  // Splitter tiles stay non-deletable; parallel join mixers expose collapse via the normal delete control.
  const allowDelete =
    (node.type !== EffectGuids.kSplitter && node.type !== EffectGuids.kMixer) || canCollapseParallel;
  const nodeTypeInfo = getNodeEffectInfo(node);
  const firstResourceTitle = nodeTypeInfo?.requiresResource ? getNodeResourceDisplayName(node, 0) : "";
  const displayName = firstResourceTitle || getNodeDisplayName(node);
  const effectTypeName = firstResourceTitle
    ? (nodeTypeInfo?.displayName || "")
    : (nodeTypeInfo?.displayName && nodeTypeInfo.displayName !== displayName
      ? nodeTypeInfo.displayName
      : "");
  const missingTooltip = buildMissingResourceTooltip(missingEntries);
  const architectureBadge = getNodeArchitectureBadge(node);
  const missingBadge = missingEntries.length
    ? `<div class="node-missing-badge" title="${escapeHtml(missingTooltip)}" aria-label="Missing resource">⚠</div>`
    : "";
  // Tooltip remains available when the compact viewport hides node text.
  const nodeTitle = [displayName, effectTypeName, architectureBadge]
    .filter((part) => Boolean(part && String(part).trim()))
    .join(" · ");
  const nodeTitleAttr = nodeTitle ? ` title="${escapeHtml(nodeTitle)}"` : "";
  const nodeAriaLabel = nodeTitle
    ? ` aria-label="${escapeHtml(nodeTitle)}${nodeBypassed ? " (bypassed)" : ""}"`
    : "";

  // Use the layout thumbnail as a small avatar at the top-left of the node if available.
  const blendId = (() => {
    const params = node.params as Record<string, unknown> | undefined;
    return typeof params?.blend === "string" ? params.blend : "";
  })();
  // Honour the user's layout preference so the avatar matches what the params
  // panel will actually render for this node.
  const nodeLayout = resolveLayoutForNode({
    effectType: node.type,
    blendId: blendId || undefined,
    matchText: buildNodeLayoutMatchText(node),
    presetId: uiState.activePresetId,
  });
  const thumbUrl = nodeLayout?.thumbnailDataUrl ?? nodeTypeInfo?.thumbnailDataUrl ?? null;
  const thumbAvatar = thumbUrl
    ? `<img class="node-layout-thumb" src="${thumbUrl.replace(/"/g, "&quot;")}" alt="" aria-hidden="true" />` 
    : "";
  const thumbClass = thumbUrl ? " has-thumb" : "";
  const deleteButton = allowDelete
    ? (canCollapseParallel
      ? `<button class="signal-node-delete" type="button" title="Collapse split (only if empty)" aria-label="Collapse split" data-collapse-splitter-id="${escapeHtml(collapseSplitterId!)}" data-collapse-mixer-id="${escapeHtml(node.id)}">×</button>`
      : '<button class="signal-node-delete" type="button" title="Remove" aria-label="Remove">×</button>')
    : "";
  const bypassButton = isToggleableSignalPathNode(node)
    ? `<button class="signal-node-bypass${nodeBypassed ? " bypassed" : ""}" type="button" title="${nodeBypassed ? "Enable effect" : "Bypass effect"}" aria-label="${nodeBypassed ? "Enable effect" : "Bypass effect"}" aria-pressed="${String(nodeBypassed)}">${renderIcon("output", "fx-effect-icon signal-node-bypass-icon")}</button>`
    : "";

  return `
    <div class="signal-node ${categoryClass} ${bypassedClass} ${selectedClass} ${missingClass}${thumbClass}" 
         data-node-id="${node.id}" 
         draggable="true" 
         tabindex="0"${nodeTitleAttr}${nodeAriaLabel}>
      ${thumbAvatar}
      ${deleteButton}
      ${bypassButton}
      ${thumbUrl ? `<div class="node-icon"></div>` : `<div class="node-icon">${icon}</div>`}
      <div class="node-info">
        <div class="node-name">${displayName}</div>
        ${effectTypeName ? `<div class="node-type">${effectTypeName}</div>` : ""}
        ${architectureBadge ? `<div class="node-architecture-badge" aria-label="Model architecture">${architectureBadge}</div>` : ""}
      </div>
      <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
      ${nodeBypassed ? '<div class="node-bypass-badge">OFF</div>' : ""}
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

      const btn = button as HTMLElement;
      const collapseSplitterId = btn.dataset.collapseSplitterId;
      const collapseMixerId = btn.dataset.collapseMixerId;
      if (collapseSplitterId && collapseMixerId) {
        sendCollapseParallelSplit(collapseSplitterId, collapseMixerId);
        selectedNodeId = null;
        hideNodeParamsPanel();
        updateEffectVisualization();
        return;
      }

      const nodeEl = btn.closest(".signal-node") as HTMLElement | null;
      const nodeId = nodeEl?.dataset.nodeId;
      if (!nodeId) return;

      sendSignalPathNodeDelete(nodeId);
      selectedNodeId = null;
      hideNodeParamsPanel();
      updateEffectVisualization();
    });
  });
  const bypassButtons = signalPathNodesElement?.querySelectorAll(".signal-node-bypass");
  bypassButtons?.forEach((button) => {
    button.addEventListener("click", (e: Event) => {
      e.preventDefault();
      e.stopPropagation();

      const btn = button as HTMLElement;
      const nodeEl = btn.closest(".signal-node") as HTMLElement | null;
      const nodeId = nodeEl?.dataset.nodeId;
      if (!nodeId || !preset.graph) {
        return;
      }

      const node = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!isToggleableSignalPathNode(node)) {
        return;
      }

      toggleSignalPathNodeBypass(node, preset);
    });
  });

  const getGraphNode = (nodeId: string): GraphNode | undefined => {
    return preset.graph?.nodes.find((n) => n.id === nodeId);
  };

  const selectNodeElement = (node: GraphNode, el: HTMLElement, focusElement: boolean): void => {
    selectedNodeId = node.id;
    showNodeParamsPanel(node, preset);
      // Always animate the 3D stage to the selected unit (amp head / cab / rack).
      chain3dView?.focusNode(node.id, false);

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
      } else if (nodeId && Array.from(e.dataTransfer?.types ?? []).includes("Files")) {
        // Accept file drops for NAM/IR nodes
        const node = preset.graph?.nodes.find((n) => n.id === nodeId);
        if (node && isNamOrCabIrNode(node)) {
          dragOverNodeId = nodeId;
          el.classList.add("drag-over");
          if (e.dataTransfer) {
            e.dataTransfer.dropEffect = "copy";
          }
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
      } else if (targetNodeId && preset.graph) {
        // File drop on NAM/IR node
        const files = Array.from(e.dataTransfer?.files ?? []);
        if (files.length > 0) {
          const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
          if (targetNode && isNamOrCabIrNode(targetNode)) {
            const file = files[0];
            const resourceType = inferResourceTypeFromFile(file);
            if (resourceType && nodeAcceptsResourceType(targetNode, resourceType)) {
              nodeDragDropHandled = true;
              void handleNamIrFileDrop(file, targetNodeId);
            }
          }
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
        hideNodeParamsPanel();
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

function showNodeParamsPanel(node: GraphNode, preset: Preset): void {
  if (!nodeParamsPanelElement) {
    return;
  }

  // Tear down any existing interactive EQ curve before replacing panel content
  if (signalPathEqInteraction) {
    signalPathEqInteraction.destroy();
    signalPathEqInteraction = null;
  }
  if (signalPathSpatialInteraction) {
    signalPathSpatialInteraction.destroy();
    signalPathSpatialInteraction = null;
    signalPathSpatialNodeId = null;
  }
  nodeParamKnobs.clear();

  // Ensure node.params exists
  if (!node.params) {
    node.params = {};
  }
  if (selectedNodeDspStatusNodeId !== node.id) {
    selectedNodeDspStatusNodeId = node.id;
    selectedNodeDspStatusAverages.clear();
    lastDspStatusAverageRenderAt = 0;
  }

  nodeParamsPanelElement.classList.add("visible");
  updateLastSelectedNode(node);
  updateEffectVisualization(node);
  
  // Get parameter definitions from registry
  const typeInfo = getNodeEffectInfo(node);
  let paramDefs = typeInfo?.parameters || [];
  if (EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic) {
    paramDefs = [];
  }

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
  const isGraphicEqNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic;
  const customEffectActions = buildCustomEffectActions(node);
  const eqVisualizer = isEqNode && !isGraphicEqNode ? `
    <div class="eq-visualizer" data-node-id="${node.id}">
      <div class="eq-visualizer-header">
        <span>EQ Curve</span>
        <span class="eq-visualizer-range">±18 dB</span>
      </div>
      <canvas class="eq-curve-canvas" data-node-id="${node.id}"></canvas>
    </div>
  ` : "";
  const isSpatialNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kSpatial3D;
  const spatialSpeakerMode = (node.params?.listenMode ?? 0) >= 0.5;
  const spatialVisualizer = isSpatialNode ? `
    <div class="spatial-visualizer" data-node-id="${node.id}">
      <div class="spatial-visualizer-header">
        <span>Source Position</span>
        <span class="spatial-visualizer-hint">${spatialSpeakerMode
          ? "Speaker mode &mdash; height and behind are reduced"
          : "Best on headphones"}</span>
      </div>
      <canvas class="spatial-panner-canvas" data-node-id="${node.id}" tabindex="0"></canvas>
      <p class="spatial-visualizer-help">Drag the radar to pan and set distance, drag the arc for height. Shift for fine, double-click to reset.</p>
    </div>
  ` : "";
  const graphicEqControls = isGraphicEqNode ? `
    <section class="graphic-eq-controls" data-node-id="${node.id}">
      <canvas class="graphic-eq-curve-canvas" aria-hidden="true"></canvas>
      <div class="graphic-eq-toolbar">
        <button type="button" class="graphic-eq-reset-btn" title="Reset all band gains to 0 dB">Reset</button>
      </div>
      <div class="graphic-eq-bands">
        ${GRAPHIC_EQ_FREQUENCIES.map((defaultFreq, index) => {
          const number = index + 1;
          const active = number <= (node.params.bandCount ?? 10);
          const enabled = (node.params[`band${number}Enabled`] ?? 1) >= 0.5;
          if (!active || !enabled) {
            return "";
          }
          const gain = node.params[`band${number}Gain`] ?? 0;
          const frequencyBounds = graphicEqFrequencyBounds(node.params, number);
          return `<div class="graphic-eq-band" data-band-number="${number}">
            <label class="graphic-eq-gain-value"><input class="graphic-eq-gain-value-input" data-param-key="band${number}Gain" type="number" inputmode="decimal" min="-18" max="18" step="0.1" value="${gain.toFixed(1)}"><span>dB</span></label>
            <input class="graphic-eq-gain" data-param-key="band${number}Gain" type="range" min="-18" max="18" step="0.1" value="${gain}" style="--graphic-eq-gain: ${((gain + 18) / 36) * 100}%">
            <label class="graphic-eq-frequency-label"><input class="graphic-eq-frequency" data-param-key="band${number}Freq" type="number" inputmode="numeric" min="${Math.ceil(frequencyBounds.min)}" max="${Math.floor(frequencyBounds.max)}" step="1" value="${Math.round(node.params[`band${number}Freq`] ?? defaultFreq)}"><span>Hz</span></label>
          </div>`;
        }).join("")}
      </div>
    </section>
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

  // Refresh the resource navigation cache in the background so prev/next buttons
  // become available without opening the browser, but do not block panel render.
  {
    const navCacheRequests = new Map<string, { resourceType: "nam" | "ir"; categoryHint?: string; contextKey: string }>();
    if (typeInfo?.requiresResource) {
      const rt = typeInfo.resourceType;
      if (rt === "nam" || rt === "ir") {
        const categoryHint = resolveResourceNavigationCategoryHint(node, preset, rt);
        const contextKey = resolveResourceContextKey(node, rt);
        navCacheRequests.set(`${rt}:${categoryHint ?? ""}`, { resourceType: rt, categoryHint, contextKey });
      }
    }
    (typeInfo?.exposedResources ?? []).forEach((er) => {
      if (er.resourceType === "nam" || er.resourceType === "ir") {
        const resourceType = er.resourceType as "nam" | "ir";
        const categoryHint = resolveResourceNavigationCategoryHint(node, preset, resourceType);
        const contextKey = resolveResourceContextKey(node, resourceType);
        navCacheRequests.set(`${resourceType}:${categoryHint ?? ""}`, { resourceType, categoryHint, contextKey });
      }
    });
    navCacheRequests.forEach(({ resourceType, categoryHint, contextKey }) => {
      resourceBrowserModal.preloadLibraryNavigationCache(resourceType, { categoryHint, contextKey });
    });
  }

  // Build resource selector if this node type requires a resource,
  // or if a composite node surfaces inner resources.
  let resourceSelector = "";
  const hideRedundantLibraryBrowseButton = isNeuralModelNode(node)
    || EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr;
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
        const preferredResourceIds = collectPreferredNodeResourceIds(node, resourceType);
        const { resources, aliasById } = getDeduplicatedLibraryResources(resourceType, preferredResourceIds);
        const emptyDisplayName = resourceType === "ir"
          ? "No IR selected"
          : resourceType === "nam"
            ? "No model selected"
            : "No resource selected";
        const current = getNodeResourceAtIndex(node, resourceIndex);
        const resolvedCurrentId = resolveResourceIdAlias(current.id ?? "", aliasById);
        // Folder navigation lands a file path with no library id, so key the
        // label off either: id-only would render the empty state for it.
        const displayName = current.id || current.filePath
          ? getNodeResourceDisplayName(node, resourceIndex, resourceType) || emptyDisplayName
          : emptyDisplayName;
        const hasCurrentSelection = Boolean(current.id || current.filePath);
        const isMissing = Boolean(current.id)
          && !current.filePath
          && !getLibraryResource(resourceType, current.id);
        const missingClass = isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
        const canBrowseFile = exposedResource.allowBrowseFile ?? true;
        const isLibraryPicker = resourceType === "nam" || resourceType === "ir";
        const isPluginPicker = resourceType === "plugin";
        const navigationCategoryHint = isLibraryPicker
          ? resolveResourceNavigationCategoryHint(node, preset, resourceType)
          : undefined;
        const navigationContextKey = isLibraryPicker
          ? resolveResourceContextKey(node, resourceType)
          : undefined;
        const resourceOptions = resources.map((res: LibraryResource) => {
          const selected = resolvedCurrentId === res.id && !current.filePath ? "selected" : "";
          return `<option value="${res.id}" ${selected}>${res.name}</option>`;
        }).join("");
        const customOption = current.filePath
          ? `<option value="__custom__" selected>Custom: ${current.filePath.split("/").pop()}</option>`
          : "";
        const hostedPluginOpenButton = resourceType === "plugin"
          ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${node.id}" ${hasCurrentSelection ? "" : "disabled"}>Open Plugin</button>`
          : "";
        const hostedPluginSelectionLabel = resourceType === "plugin"
          ? `<div class="plugin-host-selected-name" title="${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, resourceIndex, "plugin") : "No plugin selected")}">${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, resourceIndex, "plugin") : "No plugin selected")}</div>`
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
          navigationCategoryHint,
          navigationContextKey,
          allowBrowseFile: canBrowseFile,
          currentResourceId: current.id,
          currentDisplayName: displayName,
          currentFilePath: current.filePath,
          isMissing,
        });

        return `
          <div class="node-resource-selector" data-node-id="${node.id}" data-resource-index="${resourceIndex}" data-resource-type="${resourceType}">
            <label>${escapeHtml(exposedResource.displayName || exposedResource.resourceId)}</label>
            <div class="resource-controls">
              ${isLibraryPicker ? `
                ${hideRedundantLibraryBrowseButton ? "" : `
                  <button
                    class="resource-picker-btn"
                    data-node-id="${node.id}"
                    data-resource-type="${resourceType}"
                    data-resource-index="${resourceIndex}"
                    data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  >Browse</button>
                `}
                <div
                  class="${missingClass}"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  title="${escapeHtml(displayName)}"
                >${escapeHtml(displayName)}</div>
                <button
                  class="resource-clear-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  data-empty-label="${escapeHtml(emptyDisplayName)}"
                  title="Clear selected resource"
                  ${hasCurrentSelection ? "" : "disabled"}
                >${renderIcon("close", "resource-clear-icon")}</button>
              ` : isPluginPicker ? `` : `
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
                <button
                  class="resource-clear-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  data-empty-label="${escapeHtml(emptyDisplayName)}"
                  title="Clear selected resource"
                  ${hasCurrentSelection ? "" : "disabled"}
                >${renderIcon("close", "resource-clear-icon")}</button>
              `}
              ${canBrowseFile && !isLibraryPicker ? `
                <button
                  class="resource-browse-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  data-resource-index="${resourceIndex}"
                  data-exposed-resource-id="${escapeHtml(exposedResource.resourceId)}"
                  data-accept="${browseAccept}"
                  title="Browse for file..."
                >${renderIcon(isPluginPicker ? "plus" : "folder", "resource-browse-icon")}</button>
              ` : ""}
              ${hostedPluginOpenButton}
              ${hostedPluginSelectionLabel}
              ${isPluginPicker ? buildHostedPluginLoadingIndicatorHtml(node, resourceIndex) : ""}
            </div>
            ${isPluginPicker ? buildHostedPluginListHtml(node, resourceIndex, exposedResource.resourceId) : ""}
            ${hostedPluginLoadError}
            ${current.filePath && !isPluginPicker ? `<div class="resource-path-info" title="${current.filePath}">${current.filePath}</div>` : ""}
            ${isLibraryPicker ? `<div class="resource-drop-hint">Click to browse, or drag and drop a file here</div>` : ""}
          </div>
        `;
      })
      .join("");
  } else if (typeInfo?.requiresResource && typeInfo.resourceType) {
    const resourceType = typeInfo.resourceType;
    const preferredResourceIds = collectPreferredNodeResourceIds(node, resourceType);
    const { resources, aliasById } = getDeduplicatedLibraryResources(resourceType, preferredResourceIds);
    const browseAccept = resourceType === "nam" ? ".nam,.json" : resourceType === "ir" ? ".wav" : "*";
    const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
    if (blendId) {
      resourceSelector = blendState
        ? renderBlendInfoHtml(node, blendState)
        : `
          <div class="node-resource-selector" data-node-id="${node.id}">
            <label>Blend</label>
            <div class="resource-controls">
              ${isFeatureEnabled(Features.BlendTools) ? `<button class="blend-open-btn" data-node-id="${node.id}">Edit Blend</button>` : ""}
            </div>
          </div>
        `;
    } else {

    const buildOptions = (currentId: string) => {
      const resolvedCurrentId = resolveResourceIdAlias(currentId, aliasById);
      return resources.map((res: LibraryResource) => {
        const selected = res.id === resolvedCurrentId ? "selected" : "";
        return `<option value="${res.id}" ${selected}>${res.name}</option>`;
      }).join("");
    };

    const buildSelector = (index: number, label: string, includeIndexAttr: boolean) => {
      const current = getNodeResourceAtIndex(node, index);
      const resourceOptions = buildOptions(current.id);
      const customOption = current.filePath
        ? `<option value="__custom__" selected>Custom: ${current.filePath.split("/").pop()}</option>`
        : "";
      const indexAttr = includeIndexAttr ? `data-resource-index="${index}"` : "";
      const isLibraryPicker = resourceType === "nam" || resourceType === "ir";
      const isPluginPicker = resourceType === "plugin";
      const emptyDisplayName = resourceType === "ir" ? "No IR selected" : "No model selected";
      // Folder navigation lands a file path with no library id, so key the
      // label off either: id-only would render the empty state for it.
      const displayName = current.id || current.filePath
        ? getNodeResourceDisplayName(node, index) || emptyDisplayName
        : emptyDisplayName;
      const hasCurrentSelection = Boolean(current.id || current.filePath);
      const isMissing = Boolean(current.id)
        && !current.filePath
        && !getLibraryResource(resourceType, current.id);
      const missingClass = isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
      const navResourceType = resourceType === "nam" || resourceType === "ir" ? resourceType : null;
      const navigationCategoryHint = navResourceType
        ? resolveResourceNavigationCategoryHint(node, preset, navResourceType)
        : undefined;
      const navigationContextKey = navResourceType
        ? resolveResourceContextKey(node, navResourceType)
        : undefined;
      const navOptions = { categoryHint: navigationCategoryHint, contextKey: navigationContextKey };
      // A Tone3000 result set always has a neighbour to step to (it wraps), but
      // resolving it means fetching, so the buttons are enabled without asking.
      const tone3000NavActive = navResourceType
        ? resourceBrowserModal.isTone3000NavigationActive(navResourceType, navOptions)
        : false;
      const prevSelection = navResourceType && !tone3000NavActive
        ? resourceBrowserModal.getAdjacentResourceSelection(navResourceType, current.id ?? "", current.filePath ?? "", -1, navOptions)
        : null;
      const nextSelection = navResourceType && !tone3000NavActive
        ? resourceBrowserModal.getAdjacentResourceSelection(navResourceType, current.id ?? "", current.filePath ?? "", 1, navOptions)
        : null;
      const canNavPrev = tone3000NavActive || Boolean(prevSelection);
      const canNavNext = tone3000NavActive || Boolean(nextSelection);
      const navPrevButton = navResourceType ? `
        <button
          type="button"
          class="preset-action-btn resource-nav-btn resource-nav-prev-btn"
          data-node-id="${node.id}"
          data-resource-type="${navResourceType}"
          ${indexAttr}
          data-nav-direction="prev"
          title="Previous resource"
          aria-label="Previous resource"${canNavPrev ? "" : " disabled"}
        >${renderIcon("arrow-left", "resource-nav-icon")}</button>
      ` : "";
      const navNextButton = navResourceType ? `
        <button
          type="button"
          class="preset-action-btn resource-nav-btn resource-nav-next-btn"
          data-node-id="${node.id}"
          data-resource-type="${navResourceType}"
          ${indexAttr}
          data-nav-direction="next"
          title="Next resource"
          aria-label="Next resource"${canNavNext ? "" : " disabled"}
        >${renderIcon("arrow-right", "resource-nav-icon")}</button>
      ` : "";
      const hostedPluginOpenButton = resourceType === "plugin"
        ? `<button type="button" class="resource-picker-btn plugin-host-open-btn" data-node-id="${node.id}" ${hasCurrentSelection ? "" : "disabled"}>Open Plugin</button>`
        : "";
      const hostedPluginSelectionLabel = resourceType === "plugin"
        ? `<div class="plugin-host-selected-name" title="${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, index, "plugin") : "No plugin selected")}">${escapeHtml(hasCurrentSelection ? getNodeResourceDisplayName(node, index, "plugin") : "No plugin selected")}</div>`
        : "";
      const hostedPluginLoadError = resourceType === "plugin"
        ? buildHostedPluginLoadErrorHtml(node, index)
        : "";

      customLayoutResourceControls.push({
        resourceControlKey: `__resource__:primary:${index}`,
        displayName: label,
        resourceType,
        resourceIndex: index,
        navigationCategoryHint,
        navigationContextKey,
        allowBrowseFile: true,
        currentResourceId: current.id,
        currentDisplayName: displayName,
        currentFilePath: current.filePath,
        isMissing,
      });

      return `
        <div class="node-resource-selector" data-node-id="${node.id}" data-resource-index="${index}" data-resource-type="${resourceType}">
          <label>${label}</label>
          <div class="resource-controls">
            ${isLibraryPicker ? `
              ${hideRedundantLibraryBrowseButton ? "" : `
                <button
                  class="resource-picker-btn"
                  data-node-id="${node.id}"
                  data-resource-type="${resourceType}"
                  ${indexAttr}
                >Browse</button>
              `}
              ${navPrevButton}
              <div
                class="${missingClass}"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
                title="${escapeHtml(displayName)}"
              >${escapeHtml(displayName)}</div>
              ${navNextButton}
              <button
                class="resource-clear-btn"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
                data-empty-label="${escapeHtml(emptyDisplayName)}"
                title="Clear selected resource"
                ${hasCurrentSelection ? "" : "disabled"}
              >${renderIcon("close", "resource-clear-icon")}</button>
            ` : isPluginPicker ? `` : `
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
              <button
                class="resource-clear-btn"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
                data-empty-label="${escapeHtml(emptyDisplayName)}"
                title="Clear selected resource"
                ${hasCurrentSelection ? "" : "disabled"}
              >${renderIcon("close", "resource-clear-icon")}</button>
            `}
            ${!isLibraryPicker ? `
              <button
                class="resource-browse-btn"
                data-node-id="${node.id}"
                data-resource-type="${resourceType}"
                ${indexAttr}
                data-accept="${browseAccept}"
                title="Browse for file..."
              >${renderIcon(isPluginPicker ? "plus" : "folder", "resource-browse-icon")}</button>
            ` : ""}
            ${hostedPluginOpenButton}
            ${hostedPluginSelectionLabel}
            ${isPluginPicker ? buildHostedPluginLoadingIndicatorHtml(node, index) : ""}
          </div>
          ${isPluginPicker ? buildHostedPluginListHtml(node, index) : ""}
          ${hostedPluginLoadError}
          ${current.filePath && !isPluginPicker ? `<div class="resource-path-info" title="${current.filePath}">${current.filePath}</div>` : ""}
          ${isLibraryPicker ? `<div class="resource-drop-hint">Click to browse, or drag and drop a file here</div>` : ""}
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

  // Resolve the layout to render. Preference rules (preset > keyword > effect type)
  // decide between the standard controls and any available custom layout; without
  // rules this falls back to the layout library default, i.e. previous behaviour.
  const nodeBlendId = blendState?.blend?.id || "";
  const nodeLayoutMatchText = buildNodeLayoutMatchText(node);
  const customLayout = resolveLayoutForNode({
    effectType: node.type,
    blendId: nodeBlendId || undefined,
    matchText: nodeLayoutMatchText,
    presetId: uiState.activePresetId,
  });

  // When useDefaultControls is true the layout provides only the visual backdrop; the
  // standard auto-generated controls are rendered on top rather than positioned controls.
  const useDefaultControls = customLayout?.useDefaultControls === true;
  const hasCustomLayoutPresentation = Boolean(customLayout);

  const customLayoutHtml = customLayout && !useDefaultControls
    ? renderCustomLayout(node, customLayout, paramDefs, customLayoutResourceControls)
    : null;
  const placeNeuralResourceInControls = isNeuralModelNode(node) && !customLayoutHtml;
  const layoutIncludesResourceControls = Boolean(
    customLayout && !useDefaultControls && customLayout.controls.some((control) => control.bindingType === "resource" || control.paramKey.startsWith("__resource__:")),
  );
  const placeCabIrResourcesInControls = node.type === EffectGuids.kCabIr && !layoutIncludesResourceControls && Boolean(resourceSelector);
  const cabIrResourceSelectors = placeCabIrResourcesInControls
    ? `<div class="effect-inline-resource-selectors cab-ir-resource-selectors">${resourceSelector}</div>`
    : "";
  const shellTitle = escapeHtml(getNodeDisplayName(node));
  const isNeuralModel = isNeuralModelNode(node);
  const shellCategoryLabel = escapeHtml(
    getNodeCategory(node)
      .replace(/[-_]/g, " ")
      .replace(/\b\w/g, (char) => char.toUpperCase())
  );
  const shellTypeLabel = escapeHtml(typeInfo?.displayName || shellCategoryLabel);
  const nodeIsBypassed = isNodeBypassed(node);
  const shellStatusLabel = nodeIsBypassed ? "Off" : "On";
  const shellBypassTitle = isNodeBypassed(node) ? "Enable effect" : "Bypass effect";
  const architectureBadge = getNodeArchitectureBadge(node);
  const calibrationMetadataChip = getNodeNamCalibrationMetadataChip(node);
  const shellBlendId = getBlendState(node)?.blend?.id || "";
  const fullRigCabModelNote = shouldShowFullRigCabModelNote(node, preset)
    ? `<div class="default-effect-shell-inline-note" role="status" aria-live="polite">Signal chain already includes a Cabinet Model (a "Full Rig").</div>`
    : "";
  const equipmentImage = getEffectVisualizationEquipmentImage(node);
  const layoutSwitchButton = renderLayoutSwitchButtonHtml(node, shellBlendId, Boolean(customLayout));
  const amp3dToggleButton = renderAmp3dToggleButtonHtml(node);
    const useAmp3dView = shouldRenderChain3dView(Boolean(customLayoutHtml));
  const amp3dSplit = useAmp3dView ? splitAmp3dParamDefs(paramDefs) : null;
  const isInputAnalyzerNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kInputAnalyzer;
  if (isInputAnalyzerNode) {
    analyzerSpectrogramHistoryByNode.delete(node.id);
  }
  const analyzerSection = isInputAnalyzerNode ? `
    <section class="default-effect-section input-analyzer-panel" data-node-id="${node.id}">
      <div class="input-analyzer-header">
        <div class="input-analyzer-title">Signal Analyzer</div>
        <div class="input-analyzer-updated">Waiting for live analyzer data…</div>
      </div>
      <div class="input-analyzer-stats-grid">
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Peak (dBFS)</span><span class="input-analyzer-value" data-analyzer-field="peakDbfs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBFS)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbfs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Peak (%FS)</span><span class="input-analyzer-value" data-analyzer-field="peakPercent">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (%FS)</span><span class="input-analyzer-value" data-analyzer-field="rmsPercent">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBu)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbu">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (dBV)</span><span class="input-analyzer-value" data-analyzer-field="rmsDbv">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">RMS (Vrms)</span><span class="input-analyzer-value" data-analyzer-field="rmsVolts">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Momentary (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="momentaryLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Short-term (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="shortTermLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Integrated (LUFS)</span><span class="input-analyzer-value" data-analyzer-field="integratedLufs">—</span></div>
        <div class="input-analyzer-stat"><span class="input-analyzer-label">Channels</span><span class="input-analyzer-value" data-analyzer-field="channelMode">—</span></div>
      </div>
      <div class="input-analyzer-spectrogram-wrap">
        <canvas class="input-analyzer-spectrogram-canvas" aria-label="Live spectrogram"></canvas>
      </div>
      <div class="input-analyzer-bark-wrap">
        <div class="input-analyzer-bark-title">Bark Perception</div>
        <canvas class="input-analyzer-bark-canvas" aria-label="Bark-band perception"></canvas>
      </div>
    </section>
  ` : "";
  // The 3D amp is its own product shot, so the static equipment image is
  // dropped and the viewport takes the full shell width.
  const showEquipmentImage = Boolean(equipmentImage) && !amp3dSplit && !hasCustomLayoutPresentation;
  // Capture artwork is remote, so keep the stock image as a fallback for when it
  // cannot be fetched (offline, or the author removed it).
  const stockEquipmentImage = getEffectVisualizationStockImage(node);
  const equipmentImageFallbackAttr = stockEquipmentImage && stockEquipmentImage !== equipmentImage
    ? ` data-fallback-src="${escapeHtml(stockEquipmentImage)}"`
    : "";
  const shellEquipmentPanel = showEquipmentImage ? `
    <aside class="default-effect-shell-equipment-panel" aria-hidden="true">
      <img class="default-effect-shell-equipment-image" src="${escapeHtml(equipmentImage)}" alt="" loading="lazy" decoding="async"${equipmentImageFallbackAttr} />
    </aside>
  ` : "";
  const shellMainContent = amp3dSplit ? `
    ${fullRigCabModelNote}
    ${renderChain3dViewportHtml(
      node,
      resourceSelector,
      amp3dSplit.extraDefs.length ? buildParamControls(amp3dSplit.extraDefs) : "",
    )}
    ${customEffectActions}
  ` : customLayoutHtml ? `
    ${fullRigCabModelNote}
    ${layoutIncludesResourceControls || placeCabIrResourcesInControls ? "" : resourceSelector}
    ${customEffectActions}
    ${analyzerSection}
    ${eqVisualizer}
    ${spatialVisualizer}
    ${graphicEqControls}
    ${mixerInputControls}
    <div class="default-effect-section default-effect-section-controls default-effect-section-custom-layout">
      ${cabIrResourceSelectors}
      ${customLayoutHtml}
    </div>
  ` : (() => {
    // Build the standard default controls HTML
    const neuralResourceSelector = placeNeuralResourceInControls && resourceSelector
      ? `<div class="neural-model-resource-selector">${resourceSelector}</div>`
      : "";
    const defaultControlsHtml = `
      ${neuralResourceSelector}
      ${hasAdvancedTab ? `
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
      `}
    `;
    // If a backdrop layout exists, wrap the default controls inside it
    const renderedControls = useDefaultControls && customLayout
      ? renderCustomLayoutBackdrop(node, customLayout, defaultControlsHtml)
      : defaultControlsHtml;
    return `
      ${fullRigCabModelNote}
      ${layoutIncludesResourceControls || placeNeuralResourceInControls || placeCabIrResourcesInControls ? "" : resourceSelector}
      ${customEffectActions}
      ${analyzerSection}
      ${eqVisualizer}
      ${spatialVisualizer}
      ${graphicEqControls}
      ${mixerInputControls}
      <div class="default-effect-section default-effect-section-controls">
        ${cabIrResourceSelectors}
        ${renderedControls}
      </div>
    `;
  })();

  // Gate on the parameters the registry declares, not on paramDefs: effects that
  // render their own controls (Graphic EQ) blank paramDefs for presentation and
  // would otherwise be denied presets despite having plenty to save.
  const hasSavableParams = (typeInfo?.parameters?.length ?? 0) > 0;
  const effectPresetsButton = hasSavableParams
    ? `<button class="default-effect-shell-chip default-effect-shell-chip-presets" type="button" data-effect-presets-open title="Factory and saved presets for this effect" aria-label="Effect presets" aria-haspopup="dialog">Presets</button>`
    : "";

  nodeParamsPanelElement.innerHTML = `

    <div class="node-params-body">
      <section class="default-effect-shell${isNeuralModel ? " neural-model-effect" : ""}${isNodeBypassed(node) ? " is-bypassed" : ""}${hasCustomLayoutPresentation ? " has-custom-layout" : ""}">
        <div class="default-effect-shell-header">
          <div class="default-effect-shell-identity">
            <span class="default-effect-shell-led" aria-hidden="true"></span>
            <div class="default-effect-shell-titles">
              <div class="default-effect-shell-title">${shellTitle}</div>
              <div class="default-effect-shell-subtitle">${shellCategoryLabel} · ${shellTypeLabel}</div>
            </div>
          </div>
          <div class="default-effect-shell-rail">
            <button class="default-effect-shell-meter-toggle" type="button" aria-expanded="${selectedNodeDspStatusVisible}" title="${selectedNodeDspStatusVisible ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">
              <span class="default-effect-shell-meter" style="--meter-fill-scale: 0"></span>
            </button>
            <div class="effect-dsp-status" aria-label="Live DSP status" ${selectedNodeDspStatusVisible ? "" : "hidden"}>
              <button class="effect-dsp-status-close" type="button" aria-label="Close DSP status" title="Close DSP status">×</button>
              <div><span>Peak avg</span><strong data-dsp-status-average="peak">—</strong></div>
              <div><span>RMS avg</span><strong data-dsp-status-average="rms">—</strong></div>
              <div><span>Headroom avg</span><strong data-dsp-status-average="headroom">—</strong></div>
              <div><span>Processing avg</span><strong data-dsp-status-average="processing">—</strong></div>
              <div><span>Latency avg</span><strong data-dsp-status-average="latency">—</strong></div>
            </div>
          </div>
          <div class="default-effect-shell-meta" aria-label="Module status">
            ${effectPresetsButton}
            ${architectureBadge ? `<span class="default-effect-shell-chip default-effect-shell-chip-architecture" title="Loaded model architecture">${architectureBadge}</span>` : ""}
            ${calibrationMetadataChip}
            <button class="default-effect-shell-chip default-effect-shell-chip-dsp dsp-badge-toggle${selectedNodeDspStatusVisible ? " is-active" : ""}" type="button" aria-expanded="${selectedNodeDspStatusVisible}" title="${selectedNodeDspStatusVisible ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">DSP</button>
            <button
              class="default-effect-shell-toggle node-bypass-btn ${nodeIsBypassed ? "bypassed" : ""}"
              data-node-id="${node.id}"
              type="button"
              role="switch"
              aria-checked="${nodeIsBypassed ? "false" : "true"}"
              title="${shellBypassTitle}"
              aria-label="${shellBypassTitle}"
            ><span class="default-effect-shell-toggle-track" aria-hidden="true"></span><span class="default-effect-shell-toggle-label">${shellStatusLabel}</span></button>
            ${layoutSwitchButton}
            ${amp3dToggleButton}
          </div>
        </div>
        <div class="default-effect-shell-content${showEquipmentImage ? " has-equipment-image" : ""}">
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
  if (isSpatialNode) {
    updateSpatialVisualization(node);
  }

  // Bind controls
  bindNodeParamControls(node, preset);
  bindEffectPresetsButton(node);
  bindGraphicEqControls(node, preset);
  bindLayoutOverlayBypassToggles(node, preset);
  bindResourceControls(node, preset);
  bindEquipmentImageFallback();
  bindHostedPluginActionControls(node);
  bindHostedPluginListControls(node);
  bindCustomEffectActionControls(node);
  bindBlendEditorControls(nodeParamsPanelElement, node);
  bindBlendModeOverride(node);
  bindBypassButton(node, preset);
  bindSelectedNodeDspStatusToggle();
  bindLayoutSwitchButton(node, preset);
  bindAmp3dToggleButton();
  setAmp3dImmersiveMode(Boolean(amp3dSplit));
  if (amp3dSplit) {
      bindChain3dView(node, preset);
    bindAmp3dDockCollapse();
  } else {
      disposeChain3dView();
  }
  bindParamTabs();
  applyCustomLayoutScaling(nodeParamsPanelElement);
  updateSelectedNodePeakMeter();
  updateSelectedNodeDspStatus();
  updateSelectedNodeAnalyzerPanel();
}

/**
 * Apply uniform CSS-transform scaling to custom layout containers so that
 * background images and control placements always match the design canvas,
 * regardless of the available panel width.
 *
 * When the panel is wider than the design, the layout is centred at its native
 * size. When it is narrower, the entire layout (backgrounds + controls + labels)
 * is scaled down proportionally as a single unit so nothing diverges.
 *
 * Uses ResizeObserver so the scale stays correct if the panel is resized.
 */
function applyCustomLayoutScaling(container: HTMLElement | null): void {
  // Disconnect any previous observers from a prior render.
  layoutScaleObserverCleanups.forEach((fn) => fn());
  layoutScaleObserverCleanups = [];

  if (!container) return;

  const outers = container.querySelectorAll<HTMLElement>(".custom-layout-scale-outer");
  outers.forEach((outer) => {
    const inner = outer.querySelector<HTMLElement>(".custom-layout-container[data-design-w]");
    if (!inner) return;

    const designW = parseInt(inner.dataset.designW ?? "0", 10);
    const designH = parseInt(inner.dataset.designH ?? "0", 10);
    if (!designW || !designH) return;

    const applyScale = () => {
      const outerW = outer.offsetWidth;
      if (!outerW) return;

      if (outerW >= designW) {
        // Enough space: render at native size, centred.
        inner.style.transform = "";
        inner.style.marginLeft = "auto";
        inner.style.marginRight = "auto";
        outer.style.height = `${designH}px`;
      } else {
        // Scale the whole layout down to fit, preserving all proportions.
        const scale = outerW / designW;
        inner.style.transform = `scale(${scale})`;
        inner.style.marginLeft = "0";
        inner.style.marginRight = "0";
        outer.style.height = `${Math.round(designH * scale)}px`;
      }
    };

    applyScale();

    const ro = new ResizeObserver(applyScale);
    ro.observe(outer);
    layoutScaleObserverCleanups.push(() => ro.disconnect());
  });
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
        updateSpatialVisualization(node);

        if (isBlendParam && blendState) {
          updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
          updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
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
    updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
  }
}

function updateEqVisualization(node: GraphNode): void {
  const typeInfo = getNodeEffectInfo(node);
  if (!typeInfo || (typeInfo.category !== "eq" && !node.type.startsWith("eq_"))) {
    return;
  }

  const isGraphicEqNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic;
  const canvasSelector = isGraphicEqNode ? ".graphic-eq-curve-canvas" : ".eq-curve-canvas";
  const canvas = nodeParamsPanelElement?.querySelector(canvasSelector) as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const width = Math.max(1, canvas.clientWidth);
  const height = Math.max(1, canvas.clientHeight);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  const bandConfigs = isGraphicEqNode
    ? buildGraphicEqBandConfigs(node.params ?? {})
    : buildEqBandConfigsFromParams(node.params ?? {});

  if (isGraphicEqNode) {
    drawEqCurve(canvas, bandConfigs);
    return;
  }

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

const SPATIAL_PARAM_KEYS: ReadonlyArray<keyof SpatialPosition> = ["azimuth", "elevation", "distance"];

function readSpatialPosition(node: GraphNode): SpatialPosition {
  const params = node.params ?? {};
  return {
    azimuth: typeof params.azimuth === "number" ? params.azimuth : 0,
    elevation: typeof params.elevation === "number" ? params.elevation : 0,
    distance: typeof params.distance === "number" ? params.distance : 1.5,
  };
}

function updateSpatialVisualization(node: GraphNode): void {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kSpatial3D) {
    return;
  }
  const canvas = nodeParamsPanelElement?.querySelector(".spatial-panner-canvas") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const position = readSpatialPosition(node);
  const speakerMode = (node.params?.listenMode ?? 0) >= 0.5;

  if (signalPathSpatialInteraction && signalPathSpatialNodeId === node.id && canvas.isConnected) {
    signalPathSpatialInteraction.updatePosition(position);
    signalPathSpatialInteraction.setSpeakerMode(speakerMode);
    return;
  }

  // Either a different node or a rebuilt panel: the old canvas is gone, so the old
  // interaction's listeners point at a detached element and must be torn down.
  if (signalPathSpatialInteraction) {
    signalPathSpatialInteraction.destroy();
    signalPathSpatialInteraction = null;
  }

  const apply = (next: SpatialPosition, rebuildPanel: boolean): void => {
    for (const key of SPATIAL_PARAM_KEYS) {
      const value = next[key];
      if (node.params[key] === value) continue;
      node.params[key] = value;
      sendSignalPathNodeParamUpdate(node.id, key, value);
      const knob = nodeParamKnobs.get(key);
      if (knob) knob.setValue(value);
    }
    if (rebuildPanel) {
      const preset = getActivePresetForRender();
      if (preset) showNodeParamsPanel(node, preset);
    }
  };

  signalPathSpatialInteraction = new SpatialPannerInteraction(
    canvas,
    position,
    (next) => apply(next, false),
    // Committing does not rebuild the panel: the knobs are already synced above, and
    // a rebuild would replace the canvas mid-gesture and drop pointer capture.
    (next) => apply(next, false)
  );
  signalPathSpatialInteraction.setSpeakerMode(speakerMode);
  signalPathSpatialNodeId = node.id;
}

/**
 * Live source positions pushed by the DSP. Only the node currently on screen is of
 * interest; everything else is discarded so a chain full of spatialisers costs nothing.
 */
export function applySpatialPositionUpdate(
  nodes: Array<{ nodeId?: unknown } & Partial<SpatialLiveState>>
): void {
  if (!signalPathSpatialInteraction || !signalPathSpatialNodeId) {
    return;
  }
  const match = Array.isArray(nodes)
    ? nodes.find((entry) => entry && entry.nodeId === signalPathSpatialNodeId)
    : undefined;
  if (!match) {
    signalPathSpatialInteraction.setLiveState(null);
    return;
  }
  const num = (value: unknown, fallback: number): number =>
    typeof value === "number" && Number.isFinite(value) ? value : fallback;
  signalPathSpatialInteraction.setLiveState({
    azimuth: num(match.azimuth, 0),
    elevation: num(match.elevation, 0),
    distance: num(match.distance, 1.5),
    itdUs: num(match.itdUs, 0),
    ildDb: num(match.ildDb, 0),
    moving: match.moving === true,
  });
}

// ─── Per-effect user presets ─────────────────────────────────────────────────
// Saved parameter snapshots scoped to an effect type, persisted by the backend in
// effect-presets.json and mirrored into uiState. Factory presets are unaffected:
// they live in the effect registry and keep their existing per-effect surfaces.

/**
 * Storage key for a node's user presets. Resolved to the canonical effect type so
 * a node still carrying a legacy alias (e.g. "eq_graphic") shares one bucket with
 * nodes using the GUID, instead of quietly splitting the user's saved presets.
 */
function effectPresetStorageKey(node: GraphNode): string {
  return EffectTypeRegistry.resolve(node.type) || node.type;
}

function getUserEffectPresets(node: GraphNode): StoredEffectPreset[] {
  return uiState.effectPresets?.[effectPresetStorageKey(node)] ?? [];
}

/**
 * Apply a preset's parameters to a node.
 *
 * `order` comes from a factory preset's parameterOrder and matters where one
 * parameter constrains another — Graphic EQ's bandCount has to land before the
 * per-band values it bounds. Keys the effect no longer declares are skipped, so
 * a snapshot saved before a parameter was renamed cannot inject dead keys.
 */
function applyEffectPresetParams(
  node: GraphNode,
  preset: Preset,
  parameters: Record<string, number>,
  order?: string[],
): void {
  const known = new Set((getNodeEffectInfo(node)?.parameters ?? []).map((def) => def.key));
  const explicitOrder = order ?? [];
  const ordered = [
    ...explicitOrder.filter((key) => key in parameters),
    ...Object.keys(parameters).filter((key) => !explicitOrder.includes(key)),
  ];

  for (const key of ordered) {
    const value = parameters[key];
    if (!known.has(key) || typeof value !== "number" || !Number.isFinite(value)) {
      continue;
    }
    node.params[key] = value;
    sendSignalPathNodeParamUpdate(node.id, key, value);
  }
  // Re-render so knobs, toggles and the EQ/spatial visualisations reflect the load.
  showNodeParamsPanel(node, preset);
}

// --- Effect presets flyout ---------------------------------------------------
// Small popover launched from the "Presets" chip in the effect header, matching
// the effect layout picker. One dropdown lists factory presets then the user's
// own; selecting applies immediately. Appended to <body> with fixed positioning
// because the effect shell clips its own overflow.

let effectPresetsPopover: HTMLElement | null = null;
let closeEffectPresetsPopover: (() => void) | null = null;

/** Closes the presets flyout if it is open. Safe to call at any time. */
export function closeEffectPresetsFlyout(): void {
  closeEffectPresetsPopover?.();
}

/** Re-render in place; called when the backend re-broadcasts the preset store. */
export function refreshEffectPresetsFlyout(): void {
  effectPresetsPopover?.dispatchEvent(new CustomEvent("effect-presets-refresh"));
}

/** Option values encode which list an entry came from: "factory:id" / "user:id". */
function parseEffectPresetOptionValue(value: string): { kind: string; id: string } | null {
  const separator = value.indexOf(":");
  if (separator <= 0) return null;
  return { kind: value.slice(0, separator), id: value.slice(separator + 1) };
}

function openEffectPresetsFlyout(anchor: HTMLElement, nodeId: string): void {
  // Re-clicking the same anchor closes, matching the layout picker.
  if (effectPresetsPopover) {
    const sameAnchor = effectPresetsPopover.dataset.anchorId === nodeId;
    closeEffectPresetsFlyout();
    if (sameAnchor) return;
  }

  /** Resolve the node fresh each render: a preset switch can invalidate it. */
  const resolveTarget = (): { node: GraphNode; preset: Preset } | null => {
    const preset = getActivePresetForRender() ?? undefined;
    const node = preset?.graph?.nodes.find((candidate) => candidate.id === nodeId);
    return preset && node ? { node, preset } : null;
  };

  if (!resolveTarget()) return;

  const popover = document.createElement("div");
  popover.className = "effect-presets-popover";
  popover.dataset.anchorId = nodeId;
  popover.setAttribute("role", "dialog");
  popover.setAttribute("aria-label", "Effect presets");
  document.body.appendChild(popover);
  effectPresetsPopover = popover;

  const position = (): void => {
    const rect = anchor.getBoundingClientRect();
    const margin = 8;
    let left = rect.right - popover.offsetWidth;
    left = Math.max(margin, Math.min(left, window.innerWidth - popover.offsetWidth - margin));
    let top = rect.bottom + 6;
    if (top + popover.offsetHeight > window.innerHeight - margin) {
      top = Math.max(margin, rect.top - popover.offsetHeight - 6);
    }
    popover.style.left = `${Math.round(left)}px`;
    popover.style.top = `${Math.round(top)}px`;
  };

  const onDocumentPointerDown = (event: PointerEvent) => {
    const target = event.target as Node | null;
    if (!target) return;
    if (popover.contains(target) || anchor.contains(target)) return;
    close();
  };
  const onKeyDown = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.stopPropagation();
      close();
    }
  };
  const onReposition = () => position();

  function close(): void {
    document.removeEventListener("pointerdown", onDocumentPointerDown, true);
    document.removeEventListener("keydown", onKeyDown, true);
    window.removeEventListener("resize", onReposition);
    window.removeEventListener("scroll", onReposition, true);
    popover.remove();
    if (effectPresetsPopover === popover) {
      effectPresetsPopover = null;
      closeEffectPresetsPopover = null;
    }
    anchor.setAttribute("aria-expanded", "false");
  }

  function render(): void {
    const target = resolveTarget();
    if (!target) {
      close();
      return;
    }
    const { node } = target;
    const factoryPresets = getNodeEffectInfo(node)?.presets ?? [];
    const userPresets = getUserEffectPresets(node);

    const options = (entries: { id: string; name: string }[], kind: string): string =>
      entries
        .map((entry) => `<option value="${escapeHtml(kind)}:${escapeHtml(entry.id)}">${escapeHtml(entry.name)}</option>`)
        .join("");

    // With nothing to choose from, the flyout collapses to just the save row —
    // an empty dropdown and a permanently disabled Delete are only clutter.
    const hasAnyPresets = factoryPresets.length > 0 || userPresets.length > 0;

    popover.innerHTML = `
      ${hasAnyPresets ? `
      <select class="effect-presets-picker" aria-label="Load a preset for this effect">
        <option value="">Select a preset…</option>
        ${factoryPresets.length ? `<optgroup label="Factory">${options(factoryPresets, "factory")}</optgroup>` : ""}
        ${userPresets.length ? `<optgroup label="My presets">${options(userPresets, "user")}</optgroup>` : ""}
      </select>` : ""}
      <div class="effect-presets-popover-row">
        <input class="effect-presets-popover-name" type="text" placeholder="Save current as…" aria-label="Name for the saved effect preset" />
        <button class="effect-presets-popover-save" type="button">Save</button>
      </div>
      ${userPresets.length ? `<button class="effect-presets-popover-delete" type="button" disabled>Delete selected</button>` : ""}
    `;
    bind();
    position();
  }

  function bind(): void {
    const picker = popover.querySelector<HTMLSelectElement>(".effect-presets-picker");
    const nameInput = popover.querySelector<HTMLInputElement>(".effect-presets-popover-name");
    const saveBtn = popover.querySelector<HTMLButtonElement>(".effect-presets-popover-save");
    const deleteBtn = popover.querySelector<HTMLButtonElement>(".effect-presets-popover-delete");
    if (!nameInput || !saveBtn) return;

    picker?.addEventListener("change", () => {
      const selection = parseEffectPresetOptionValue(picker.value);
      // Only the user's own presets can be deleted; factory ones are read-only.
      if (deleteBtn) deleteBtn.disabled = selection?.kind !== "user";

      const target = resolveTarget();
      if (!target || !selection) return;

      if (selection.kind === "factory") {
        const entry = (getNodeEffectInfo(target.node)?.presets ?? []).find((c) => c.id === selection.id);
        if (entry) applyEffectPresetParams(target.node, target.preset, entry.parameters, entry.parameterOrder);
      } else {
        const entry = getUserEffectPresets(target.node).find((c) => c.id === selection.id);
        if (entry) applyEffectPresetParams(target.node, target.preset, entry.parameters);
      }
    });

    const save = async (): Promise<void> => {
      const target = resolveTarget();
      if (!target) return;
      const name = nameInput.value.trim();
      if (!name) {
        nameInput.focus();
        showNotification("Name required", "Enter a name for this effect preset.");
        return;
      }
      if (getUserEffectPresets(target.node).some((c) => c.name === name)) {
        const confirmed = await showConfirm(`Replace the saved settings named "${name}"?`, "Overwrite preset");
        if (!confirmed) return;
      }
      // The backend owns the store and re-broadcasts, which re-renders this flyout.
      postMessage({
        type: "saveEffectPreset",
        effectType: effectPresetStorageKey(target.node),
        name,
        parameters: { ...target.node.params },
      });
      nameInput.value = "";
      showNotification("Effect preset saved", name);
    };

    saveBtn.addEventListener("click", () => void save());
    nameInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        void save();
      }
    });

    deleteBtn?.addEventListener("click", () => {
      const selection = parseEffectPresetOptionValue(picker?.value ?? "");
      const target = resolveTarget();
      if (!target || selection?.kind !== "user") return;
      const entry = getUserEffectPresets(target.node).find((c) => c.id === selection.id);
      if (!entry) return;
      void (async () => {
        const confirmed = await showConfirm(`Delete the saved settings named "${entry.name}"?`, "Delete preset");
        if (!confirmed) return;
        postMessage({
          type: "deleteEffectPreset",
          effectType: effectPresetStorageKey(target.node),
          presetId: entry.id,
        });
        showNotification("Effect preset deleted", entry.name);
      })();
    });
  }

  popover.addEventListener("effect-presets-refresh", () => render());

  closeEffectPresetsPopover = close;
  anchor.setAttribute("aria-expanded", "true");
  document.addEventListener("pointerdown", onDocumentPointerDown, true);
  document.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("resize", onReposition);
  window.addEventListener("scroll", onReposition, true);

  render();
}

function bindEffectPresetsButton(node: GraphNode): void {
  const button = nodeParamsPanelElement?.querySelector<HTMLButtonElement>("[data-effect-presets-open]");
  if (!button) return;
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    openEffectPresetsFlyout(button, node.id);
  });
}

function bindGraphicEqControls(node: GraphNode, preset: Preset): void {  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kEqGraphic) {
    return;
  }

  const applyParams = (updates: Record<string, number>, rerender = false): void => {
    Object.entries(updates).forEach(([key, value]) => {
      node.params[key] = value;
      sendSignalPathNodeParamUpdate(node.id, key, value);
    });
    if (rerender) {
      showNodeParamsPanel(node, preset);
    } else {
      updateEqVisualization(node);
    }
  };

  // Reset flattens the curve: every band gain back to 0 dB. The selected profile's
  // band count, frequencies and Q values are deliberately left alone — this resets
  // the curve the user drew, not the profile they chose.
  const resetButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".graphic-eq-reset-btn");
  resetButton?.addEventListener("click", () => {
    const bandCount = node.params.bandCount ?? GRAPHIC_EQ_FREQUENCIES.length;
    const flatParams: Record<string, number> = {};
    for (let band = 1; band <= bandCount; band++) {
      flatParams[`band${band}Gain`] = 0;
    }
    applyParams(flatParams, true);
  });

  nodeParamsPanelElement?.querySelectorAll<HTMLInputElement>(".graphic-eq-gain, .graphic-eq-gain-value-input, .graphic-eq-frequency").forEach((input) => {
    input.addEventListener("input", () => {
      const key = input.dataset.paramKey;
      if (input.value.trim() === "") return;
      const rawValue = Number(input.value);
      if (!key || !Number.isFinite(rawValue)) return;
      const bandMatch = /^band(\d+)Freq$/.exec(key);
      const value = bandMatch
        ? clampGraphicEqFrequency(node.params, Number(bandMatch[1]), rawValue)
        : Math.max(-18, Math.min(18, rawValue));
      if (value !== rawValue) input.value = bandMatch ? `${Math.round(value)}` : value.toFixed(1);
      applyParams({ [key]: value });
      if (input.classList.contains("graphic-eq-gain") || input.classList.contains("graphic-eq-gain-value-input")) {
        const band = input.closest(".graphic-eq-band");
        const gainSlider = band?.querySelector<HTMLInputElement>(".graphic-eq-gain");
        const gainValueInput = band?.querySelector<HTMLInputElement>(".graphic-eq-gain-value-input");
        if (gainSlider) {
          gainSlider.value = `${value}`;
          gainSlider.style.setProperty("--graphic-eq-gain", `${((value + 18) / 36) * 100}%`);
        }
        if (gainValueInput && gainValueInput !== input) gainValueInput.value = value.toFixed(1);
      }
    });
    input.addEventListener("change", () => {
      if (input.classList.contains("graphic-eq-frequency")) showNodeParamsPanel(node, preset);
    });
  });

  nodeParamsPanelElement?.querySelectorAll<HTMLInputElement>(".graphic-eq-gain").forEach((input) => {
    let lastTouchTapAt = 0;
    let touchStart: { pointerId: number; x: number; y: number } | undefined;
    const resetGain = (): void => {
      input.value = "0";
      input.style.setProperty("--graphic-eq-gain", "50%");
      const gainValueInput = input.closest(".graphic-eq-band")?.querySelector<HTMLInputElement>(".graphic-eq-gain-value-input");
      if (gainValueInput) gainValueInput.value = "0.0";
      const key = input.dataset.paramKey;
      if (key) applyParams({ [key]: 0 });
    };

    input.addEventListener("dblclick", resetGain);
    input.addEventListener("pointerdown", (event) => {
      if (event.pointerType === "touch") {
        touchStart = { pointerId: event.pointerId, x: event.clientX, y: event.clientY };
      }
    });
    input.addEventListener("pointerup", (event) => {
      const startedAsTap = event.pointerType === "touch"
        && touchStart?.pointerId === event.pointerId
        && Math.hypot(event.clientX - touchStart.x, event.clientY - touchStart.y) <= 8;
      touchStart = undefined;
      if (!startedAsTap) return;
      const now = performance.now();
      if (now - lastTouchTapAt <= 350) {
        lastTouchTapAt = 0;
        resetGain();
        return;
      }
      lastTouchTapAt = now;
    });
  });
}

/// Resource loads round-trip through the backend, and the params panel re-renders
/// from whatever the graph says at that moment. Without this, holding down
/// next/prev repeatedly recomputes "the one after the old resource" and appears
/// to stick. Entries are dropped as soon as the graph catches up.
const pendingResourceNavSelections = new Map<string, { resourceId: string; filePath: string; at: number }>();
const PENDING_RESOURCE_NAV_TTL_MS = 5000;

/// Navigation keys with a Tone3000 step in flight (fetch + import). Keyed the
/// same way as the pending selections so prev and next share one guard and
/// cannot race each other from the same starting resource.
const inFlightTone3000NavKeys = new Set<string>();

function buildResourceNavKey(
  nodeId: string,
  resourceType: string,
  resourceIndex: number,
  exposedResourceId: string | undefined,
): string {
  return `${nodeId}:${resourceType}:${resourceIndex}:${exposedResourceId ?? ""}`;
}

/// Remote capture artwork can fail to load (offline, image withdrawn): swap in
/// the stock equipment image rather than leaving an empty panel.
function bindEquipmentImageFallback(): void {
  const images = nodeParamsPanelElement?.querySelectorAll<HTMLImageElement>(
    ".default-effect-shell-equipment-image[data-fallback-src]",
  ) ?? [];
  images.forEach((image) => {
    image.addEventListener("error", () => {
      const fallback = image.dataset.fallbackSrc;
      if (!fallback || image.src.endsWith(fallback)) {
        return;
      }
      delete image.dataset.fallbackSrc;
      image.src = fallback;
    }, { once: true });
  });
}

function bindResourceControls(node: GraphNode, preset: Preset): void {
  const syncResourceNavigationButtons = (
    nodeId: string,
    resourceType: "nam" | "ir",
    resourceIndex: number,
    exposedResourceId: string | undefined,
    currentResourceId: string,
    currentFilePath: string,
  ): void => {
    const prevButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(
      `.resource-nav-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"][data-resource-index="${resourceIndex}"][data-nav-direction="prev"]`,
    );
    const nextButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(
      `.resource-nav-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"][data-resource-index="${resourceIndex}"][data-nav-direction="next"]`,
    );

    const navOptions = {
      categoryHint: resolveResourceNavigationCategoryHint(node, preset, resourceType),
      contextKey: resolveResourceContextKey(node, resourceType),
    };
    const tone3000NavActive = resourceBrowserModal.isTone3000NavigationActive(resourceType, navOptions);
    if (prevButton) {
      const prev = tone3000NavActive
        || Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, -1, navOptions));
      prevButton.disabled = !prev;
      prevButton.setAttribute("aria-disabled", prev ? "false" : "true");
    }
    if (nextButton) {
      const next = tone3000NavActive
        || Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, 1, navOptions));
      nextButton.disabled = !next;
      nextButton.setAttribute("aria-disabled", next ? "false" : "true");
    }

    const clearButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
      `.resource-clear-btn[data-node-id="${nodeId}"][data-resource-type="${resourceType}"]`,
    ) ?? [];
    clearButtons.forEach((clearBtn) => {
      const clearResourceIndex = clearBtn.dataset.resourceIndex ? parseInt(clearBtn.dataset.resourceIndex, 10) : 0;
      const clearExposedResourceId = clearBtn.dataset.exposedResourceId;
      if (clearResourceIndex === resourceIndex && (clearExposedResourceId ?? "") === (exposedResourceId ?? "")) {
        clearBtn.disabled = !(currentResourceId || currentFilePath);
      }
    });
  };

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
        sendNodeResourceUpdate(
          nodeId,
          resourceType,
          resourceId,
          "",
          resourceIndex,
          undefined,
          undefined,
          false,
        );
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
      const tone3000CategoryFilter = resourceType === "nam"
        ? resolveResourceBrowserTone3000CategoryFilter(node, preset)
        : undefined;
      const libraryCategoryHint = resourceType === "ir"
        ? resolveResourceBrowserLibraryCategoryHint(node, resourceType)
        : undefined;
      resourceBrowserModal.open({
        resourceType,
        currentId: current.id,
        nodeId,
        resourceIndex,
        exposedResourceId,
        libraryCategoryHint,
        tone3000CategoryFilter,
        contextKey: resolveResourceContextKey(node, resourceType),
        onSelect: (resourceId) => {
          // An explicit pick supersedes any in-flight next/prev step.
          pendingResourceNavSelections.set(
            buildResourceNavKey(nodeId, resourceType, resourceIndex, exposedResourceId),
            { resourceId, filePath: "", at: performance.now() },
          );
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

          syncResourceNavigationButtons(nodeId, resourceType, resourceIndex, exposedResourceId, resourceId, "");
        },
      });
    });
  });

  const resourceNavButtons = nodeParamsPanelElement?.querySelectorAll(".resource-nav-btn") as NodeListOf<HTMLButtonElement> | null;
  resourceNavButtons?.forEach((navBtn) => {
    // This runs on every panel render, i.e. whenever the graph state we render
    // from has moved: retire the in-flight selection once it has landed.
    {
      const nodeId = navBtn.dataset.nodeId;
      const resourceType = navBtn.dataset.resourceType;
      const resourceIndex = navBtn.dataset.resourceIndex ? parseInt(navBtn.dataset.resourceIndex, 10) : 0;
      if (nodeId && resourceType) {
        const navKey = buildResourceNavKey(nodeId, resourceType, resourceIndex, navBtn.dataset.exposedResourceId);
        const pending = pendingResourceNavSelections.get(navKey);
        if (pending) {
          const live = getNodeResourceAtIndex(node, resourceIndex);
          const landed = (pending.resourceId && pending.resourceId === (live.id ?? ""))
            || (pending.filePath && pending.filePath === (live.filePath ?? ""));
          if (landed || (performance.now() - pending.at) >= PENDING_RESOURCE_NAV_TTL_MS) {
            pendingResourceNavSelections.delete(navKey);
          }
        }
      }
    }

    const applyResourceNavSelection = (
      nodeId: string,
      resourceType: "nam" | "ir",
      resourceIndex: number,
      exposedResourceId: string | undefined,
      navKey: string,
      next: ResourceNavigationSelection,
    ): void => {
      pendingResourceNavSelections.set(navKey, {
        resourceId: next.resourceId ?? "",
        filePath: next.filePath ?? "",
        at: performance.now(),
      });

      sendNodeResourceUpdate(
        nodeId,
        resourceType,
        next.resourceId ?? "",
        next.filePath ?? "",
        resourceIndex,
        undefined,
        exposedResourceId,
      );

      const nextResource = next.resourceId
        ? getLibraryResource(resourceType, next.resourceId)
        : undefined;
      // A just-imported resource is not in the library snapshot yet, so fall
      // back to the name the navigation step reported.
      const labelText = nextResource?.name
        || next.displayName
        || (next.resourceId ?? "")
        || (next.filePath?.split(/[\\/]/).pop() ?? "");

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
        labelEl.textContent = labelText || (resourceType === "ir" ? "No IR selected" : "No model selected");
        labelEl.title = labelText || "";
        labelEl.classList.toggle("is-missing", Boolean(next.resourceId) && !nextResource && !next.displayName);
      }
      syncResourceNavigationButtons(nodeId, resourceType, resourceIndex, exposedResourceId, next.resourceId ?? "", next.filePath ?? "");
    };

    navBtn.addEventListener("click", () => {
      const nodeId = navBtn.dataset.nodeId;
      const resourceType = navBtn.dataset.resourceType as "nam" | "ir" | undefined;
      const resourceIndex = navBtn.dataset.resourceIndex ? parseInt(navBtn.dataset.resourceIndex, 10) : 0;
      const exposedResourceId = navBtn.dataset.exposedResourceId;
      const direction = navBtn.dataset.navDirection === "prev" ? -1 : 1;
      if (!nodeId || !resourceType || (resourceType !== "nam" && resourceType !== "ir")) {
        return;
      }

      // Step from the last selection this button requested while it is still in
      // flight, so repeated clicks keep advancing instead of recomputing the
      // same neighbour of a resource we already navigated away from.
      const navKey = buildResourceNavKey(nodeId, resourceType, resourceIndex, exposedResourceId);
      const snapshot = getNodeResourceAtIndex(node, resourceIndex);
      const pending = pendingResourceNavSelections.get(navKey);
      const current = pending && (performance.now() - pending.at) < PENDING_RESOURCE_NAV_TTL_MS
        ? { id: pending.resourceId, filePath: pending.filePath }
        : { id: snapshot.id ?? "", filePath: snapshot.filePath ?? "" };

      const navOptions = {
        categoryHint: resolveResourceNavigationCategoryHint(node, preset, resourceType),
        contextKey: resolveResourceContextKey(node, resourceType),
      };

      // Stepping a Tone3000 result set fetches (and imports) the neighbour, so
      // it runs asynchronously with the button held busy meanwhile.
      if (resourceBrowserModal.isTone3000NavigationActive(resourceType, navOptions)) {
        if (inFlightTone3000NavKeys.has(navKey)) {
          return;
        }
        inFlightTone3000NavKeys.add(navKey);
        navBtn.setAttribute("aria-busy", "true");

        void (async () => {
          try {
            const next = await resourceBrowserModal.stepTone3000Resource(
              resourceType,
              current.id ?? "",
              direction,
              navOptions,
            );
            if (next) {
              applyResourceNavSelection(nodeId, resourceType, resourceIndex, exposedResourceId, navKey, next);
            }
          } finally {
            inFlightTone3000NavKeys.delete(navKey);
            navBtn.removeAttribute("aria-busy");
          }
        })();
        return;
      }

      const next = resourceBrowserModal.getAdjacentResourceSelection(
        resourceType,
        current.id ?? "",
        current.filePath ?? "",
        direction,
        navOptions,
      );
      if (!next) {
        return;
      }

      applyResourceNavSelection(nodeId, resourceType, resourceIndex, exposedResourceId, navKey, next);
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
          markHostedPluginLoadPending(nodeId, resourceIndex ?? 0);
        }
        clearInlineHostedPluginLoadError(browseBtn);
      }

      if (nodeId && resourceType) {
        sendBrowseNodeResource(
          nodeId,
          resourceType,
          resourceIndex,
          exposedResourceId,
          resourceType === "plugin",
        );
      }
    });
  });

  // Bind clear buttons
  const clearBtns = nodeParamsPanelElement?.querySelectorAll(".resource-clear-btn") as NodeListOf<HTMLButtonElement> | null;
  clearBtns?.forEach((clearBtn) => {
    clearBtn.addEventListener("click", () => {
      void (async () => {
        if (clearBtn.disabled) {
          return;
        }

        const nodeId = clearBtn.dataset.nodeId;
        const resourceType = clearBtn.dataset.resourceType;
        const resourceIndex = clearBtn.dataset.resourceIndex ? parseInt(clearBtn.dataset.resourceIndex, 10) : undefined;
        const exposedResourceId = clearBtn.dataset.exposedResourceId;
        const emptyLabel = clearBtn.dataset.emptyLabel || "No resource selected";

        if (!nodeId || !resourceType) {
          return;
        }

        pendingResourceNavSelections.delete(
          buildResourceNavKey(nodeId, resourceType, resourceIndex ?? 0, exposedResourceId),
        );

        let selectedPluginResourceId = "";
        if (resourceType === "plugin") {
          const current = getNodeResourceAtIndex(node, resourceIndex ?? 0);
          selectedPluginResourceId = current.id || "";
          if (selectedPluginResourceId) {
            const selectedPluginName = getLibraryResourceName("plugin", selectedPluginResourceId) || selectedPluginResourceId;
            const confirmed = await showConfirm(
              `Remove "${selectedPluginName}" from your plugin library? The plugin file on disk will not be deleted.`,
              "Remove Plugin",
            );
            if (!confirmed) {
              return;
            }
          }

          hostedPluginLoadFailures.delete(nodeId);
          clearInlineHostedPluginLoadError(clearBtn);
          clearHostedPluginLoadPending(nodeId);
          const listItems = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
            `.plugin-host-list[data-node-id="${nodeId}"] .plugin-host-item`,
          );
          listItems?.forEach((item) => item.classList.remove("is-selected"));
          const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
            `.plugin-host-open-btn[data-node-id="${nodeId}"]`,
          );
          openButtons?.forEach((button) => {
            button.disabled = true;
          });
        }

        sendNodeResourceUpdate(nodeId, resourceType, "", "", resourceIndex, undefined, exposedResourceId);

        if (resourceType === "plugin" && selectedPluginResourceId) {
          postMessage({
            type: "cleanupResourceLibrary",
            scope: "plugin",
            removeFiles: false,
            resources: [{ type: "plugin", id: selectedPluginResourceId }],
          });
        }

        const dropdowns = nodeParamsPanelElement?.querySelectorAll<HTMLSelectElement>(
          `.resource-dropdown[data-node-id="${nodeId}"][data-resource-type="${resourceType}"]`,
        ) ?? [];
        dropdowns.forEach((dropdown) => {
          const controlResourceIndex = dropdown.dataset.resourceIndex ? parseInt(dropdown.dataset.resourceIndex, 10) : 0;
          const clearResourceIndex = resourceIndex ?? 0;
          const dropdownExposedResourceId = dropdown.dataset.exposedResourceId;
          if (controlResourceIndex === clearResourceIndex && (dropdownExposedResourceId ?? "") === (exposedResourceId ?? "")) {
            dropdown.value = "";
          }
        });

        const labelCandidates = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
          `.resource-picker-label[data-node-id="${nodeId}"]`,
        ) as NodeListOf<HTMLElement> | null;
        const pickerResourceType = resourceType === "nam" || resourceType === "ir" ? resourceType : null;
        const labelEl = pickerResourceType
          ? findMatchingResourcePickerLabel(
            labelCandidates,
            nodeId,
            pickerResourceType,
            resourceIndex ?? 0,
            exposedResourceId,
          )
          : null;

        if (labelEl) {
          labelEl.textContent = emptyLabel;
          labelEl.title = emptyLabel;
          labelEl.classList.remove("is-missing");
        }

        if (pickerResourceType) {
          syncResourceNavigationButtons(nodeId, pickerResourceType, resourceIndex ?? 0, exposedResourceId, "", "");
        }

        clearBtn.disabled = true;
      })();
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

  // Bind per-slot file drop on NAM/IR resource selector rows
  const resourceSelectorEls = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(
    ".node-resource-selector[data-resource-index][data-resource-type]",
  ) ?? [];
  resourceSelectorEls.forEach((selectorEl) => {
    const elResourceType = selectorEl.dataset.resourceType as "nam" | "ir" | undefined;
    const elResourceIndex = selectorEl.dataset.resourceIndex !== undefined
      ? parseInt(selectorEl.dataset.resourceIndex, 10)
      : undefined;
    const elNodeId = selectorEl.dataset.nodeId;
    if (!elNodeId || (elResourceType !== "nam" && elResourceType !== "ir") || elResourceIndex === undefined) {
      return;
    }

    selectorEl.addEventListener("dragover", (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
      selectorEl.classList.add("drag-over");
    });

    selectorEl.addEventListener("dragleave", (e: DragEvent) => {
      if (!selectorEl.contains(e.relatedTarget as Node | null)) {
        selectorEl.classList.remove("drag-over");
      }
    });

    selectorEl.addEventListener("drop", (e: DragEvent) => {
      if (!Array.from(e.dataTransfer?.types ?? []).includes("Files")) return;
      e.preventDefault();
      e.stopPropagation();
      selectorEl.classList.remove("drag-over");
      const files = Array.from(e.dataTransfer?.files ?? []);
      const file = files[0];
      if (!file) return;
      const resourceType = inferResourceTypeFromFile(file);
      if (resourceType !== elResourceType) return;
      void handleNamIrFileDrop(file, elNodeId, elResourceIndex);
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

function bindHostedPluginListControls(node: GraphNode): void {
  const lists = nodeParamsPanelElement?.querySelectorAll<HTMLElement>(".plugin-host-list");
  lists?.forEach((list) => {
    const nodeId = list.dataset.nodeId;
    const resourceIndex = list.dataset.resourceIndex ? parseInt(list.dataset.resourceIndex, 10) : 0;
    const exposedResourceId = list.dataset.exposedResourceId;
    if (!nodeId) {
      return;
    }

    const selectedItem = list.querySelector<HTMLElement>(".plugin-host-item.is-selected");
    selectedItem?.scrollIntoView({ block: "nearest" });

    const getItems = (): HTMLElement[] => Array.from(
      list.querySelectorAll<HTMLElement>(".plugin-host-item[data-resource-id]"),
    );

    const focusPluginListItem = (item: HTMLElement | null): void => {
      if (!item) {
        return;
      }
      item.focus();
      item.scrollIntoView({ block: "nearest" });
    };

    const focusItemByOffset = (origin: HTMLElement | null, offset: number): void => {
      const items = getItems();
      if (!items.length) {
        return;
      }

      const currentIndex = origin ? items.indexOf(origin) : -1;
      const nextIndex = currentIndex < 0
        ? (offset > 0 ? 0 : items.length - 1)
        : Math.max(0, Math.min(items.length - 1, currentIndex + offset));
      focusPluginListItem(items[nextIndex]);
    };

    const focusBoundaryItem = (first: boolean): void => {
      const items = getItems();
      if (!items.length) {
        return;
      }
      focusPluginListItem(first ? items[0] : items[items.length - 1]);
    };

    list.addEventListener("keydown", (event) => {
      if (list.classList.contains("is-loading")) {
        return;
      }

      const key = event.key;
      if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") {
        return;
      }

      const activeElement = document.activeElement as HTMLElement | null;
      const activeItem = activeElement?.closest<HTMLElement>(".plugin-host-item[data-resource-id]") ?? null;
      event.preventDefault();

      if (key === "ArrowDown") {
        focusItemByOffset(activeItem, 1);
      } else if (key === "ArrowUp") {
        focusItemByOffset(activeItem, -1);
      } else if (key === "Home") {
        focusBoundaryItem(true);
      } else if (key === "End") {
        focusBoundaryItem(false);
      }
    });

    list.querySelectorAll<HTMLElement>(".plugin-host-item[data-resource-id]").forEach((item) => {
      item.addEventListener("click", () => {
        if (list.classList.contains("is-loading")) {
          return;
        }
        const resourceId = item.dataset.resourceId;
        if (!resourceId || item.classList.contains("is-selected")) {
          return;
        }

        if (isBlockedHostedPluginLibraryEntry(resourceId)) {
          showNotification(
            "Blocked plugin",
            "Soundshed plugins cannot be loaded in the hosted plugin slot. Remove this entry if it is invalid.",
          );
          renderHostedPluginWarningIntoOpenPanel(
            nodeId,
            resourceIndex,
            buildHostedPluginWarningMarkup(
              "Blocked Plugin",
              "Soundshed plugins cannot be loaded in the hosted plugin slot.",
            ),
          );
          return;
        }

        hostedPluginLoadFailures.delete(nodeId);
        clearInlineHostedPluginLoadError(list);
        renderHostedPluginWarningIntoOpenPanel(
          nodeId,
          resourceIndex,
          buildUnsupportedPluginWarningMarkup(getLibraryResource("plugin", resourceId)),
        );

        list.querySelectorAll(".plugin-host-item").forEach((el) => el.classList.toggle("is-selected", el === item));
        const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(
          `.plugin-host-open-btn[data-node-id="${nodeId}"]`,
        );
        openButtons?.forEach((button) => {
          button.disabled = false;
        });
        item.scrollIntoView({ block: "nearest" });
        markHostedPluginLoadPending(nodeId, resourceIndex);
        sendNodeResourceUpdate(nodeId, "plugin", resourceId, "", resourceIndex, undefined, exposedResourceId, false);
      });
      item.addEventListener("keydown", (event) => {
        const target = event.target as HTMLElement | null;
        if (target && target !== item && target.closest("button")) {
          return;
        }

        if (event.key === "ArrowDown") {
          event.preventDefault();
          focusItemByOffset(item, 1);
          return;
        }
        if (event.key === "ArrowUp") {
          event.preventDefault();
          focusItemByOffset(item, -1);
          return;
        }
        if (event.key === "Home") {
          event.preventDefault();
          focusBoundaryItem(true);
          return;
        }
        if (event.key === "End") {
          event.preventDefault();
          focusBoundaryItem(false);
          return;
        }

        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          item.click();
        }
      });
    });

    list.querySelectorAll<HTMLButtonElement>(".plugin-host-remove-btn").forEach((removeBtn) => {
      removeBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const resourceId = removeBtn.dataset.resourceId;
        const resourceName = removeBtn.dataset.resourceName || "this plugin";
        if (!resourceId) {
          return;
        }
        void (async () => {
          const confirmed = await showConfirm(
            `Remove "${resourceName}" from your plugin library? The plugin file on disk will not be deleted.`,
            "Remove Plugin",
          );
          if (!confirmed) {
            return;
          }

          const current = getNodeResourceAtIndex(node, resourceIndex);
          if (current.id === resourceId) {
            // Clear the node's selection first so the library entry is no
            // longer in use by the active preset when the cleanup runs.
            hostedPluginLoadFailures.delete(nodeId);
            clearInlineHostedPluginLoadError(list);
            sendNodeResourceUpdate(nodeId, "plugin", "", "", resourceIndex, undefined, exposedResourceId);
          }

          postMessage({
            type: "cleanupResourceLibrary",
            scope: "plugin",
            removeFiles: false,
            resources: [{ type: "plugin", id: resourceId }],
          });
        })();
      });
    });

    list.querySelectorAll<HTMLButtonElement>(".plugin-host-favorite-btn").forEach((favoriteBtn) => {
      favoriteBtn.addEventListener("click", (event) => {
        event.stopPropagation();
        const resourceId = favoriteBtn.dataset.resourceId;
        if (!resourceId) {
          return;
        }
        toggleHostedPluginFavorite(resourceId);
        refreshSelectedNodeParams();
      });
    });
  });
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

function bindBypassButton(node: GraphNode, preset: Preset): void {
  const bypassButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-bypass-btn");
  bypassButtons.forEach((bypassBtn) => {
    bypassBtn.addEventListener("click", () => {
      toggleSignalPathNodeBypass(node, preset);
    });
  });
}

/**
 * Opens the layout designer for a node, from the layout picker.
 * `layoutId` is null to start a fresh auto-generated layout (so an effect type can
 * hold several), or an existing layout id to edit that one.
 */
function openLayoutDesignerForNode(
  node: GraphNode,
  effectType: string,
  blendId: string,
  layoutId: string | null,
): void {
  let existingLayout: EffectLayout | null = null;
  if (layoutId) {
    existingLayout = findLayoutById(layoutId, effectType, blendId || undefined);
    if (!existingLayout) {
      showNotification("That layout is no longer available", "error");
      return;
    }
  }

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

function bindBlendModeOverride(node: GraphNode): void {
  const select = nodeParamsPanelElement?.querySelector<HTMLSelectElement>(".blend-mode-select");
  if (!select) {
    return;
  }
  select.addEventListener("change", () => {
    const value = select.value;
    node.config.blendMode = value;
    sendSignalPathNodeConfigUpdate(node.id, "blendMode", value);
    // Update the matched-model summary in place to reflect the new mode.
    const blendState = getBlendState(node);
    if (blendState) {
      updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
    }
  });
}

function sendNodeResourceUpdate(
  nodeId: string,
  resourceType: string,
  resourceId: string,
  filePath: string,
  resourceIndex?: number,
  parameterValue?: number,
  exposedResourceId?: string,
  openPluginEditorAfterLoad?: boolean,
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
    openPluginEditorAfterLoad,
  });
  setPresetDirty(true);
}

function sendBrowseNodeResource(
  nodeId: string,
  resourceType: string,
  resourceIndex?: number,
  exposedResourceId?: string,
  openPluginEditorAfterLoad?: boolean,
): void {
  postMessage({
    type: "browseNodeResource",
    nodeId,
    resourceType,
    resourceIndex,
    exposedResourceId,
    openPluginEditorAfterLoad,
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

function setSignalPathAddMenuOpen(open: boolean): void {
  if (!signalPathAddMenu || !signalPathAddMenuTrigger || !signalPathAddMenuOptions) {
    return;
  }
  signalPathAddMenuOptions.hidden = !open;
  signalPathAddMenuTrigger.setAttribute("aria-expanded", String(open));
  if (open) {
    const triggerRect = signalPathAddMenuTrigger.getBoundingClientRect();
    signalPathAddMenuOptions.style.right = `${Math.max(8, window.innerWidth - triggerRect.right)}px`;
    signalPathAddMenuOptions.style.bottom = `${Math.max(8, window.innerHeight - triggerRect.top + 8)}px`;
  } else {
    signalPathAddMenuOptions.style.removeProperty("right");
    signalPathAddMenuOptions.style.removeProperty("bottom");
  }
}

function updateSignalPathAddMenuAvailability(available: boolean): void {
  signalPathAddMenu?.classList.toggle("is-disabled", !available);
  if (signalPathAddMenuTrigger) {
    signalPathAddMenuTrigger.disabled = !available;
  }
  if (!available) {
    setSignalPathAddMenuOpen(false);
  }
}

signalPathAddMenuTrigger?.addEventListener("click", (event) => {
  event.stopPropagation();
  setSignalPathAddMenuOpen(signalPathAddMenuOptions?.hidden ?? true);
});

document.getElementById("signal-path-floating-add-fx")?.addEventListener("click", () => {
  setSignalPathAddMenuOpen(false);
  expandFxSelector({ focusSearch: true });
});

signalPathAddSceneButton?.addEventListener("click", () => {
  setSignalPathAddMenuOpen(false);
  addSceneFromToolbar();
});

document.addEventListener("click", (event) => {
  if (!signalPathAddMenu?.contains(event.target as Node)) {
    setSignalPathAddMenuOpen(false);
  }
});

document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && signalPathAddMenuOptions && !signalPathAddMenuOptions.hidden) {
    setSignalPathAddMenuOpen(false);
    signalPathAddMenuTrigger?.focus();
  }
});

const mixerPresetTabCollator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });

function renderMixerPresetTabs(): void {
  let tabBar = document.getElementById("mixer-preset-tabs");
  const signalPathBar = document.getElementById("signal-path-bar");
  const mixer = uiState.mixer;

  // Show tabs whenever there are 2+ active mixer slots — do NOT require the
  // preset to be in the cache first, because "+Mixer" list items are stubs
  // without graph data and getSignalPathPreset() returns null until the C++
  // round-trip completes.
  const multiPresetMode = !isCompositeEditMode()
    && !!mixer
    && mixer.activePresetIds.length > 1;

  if (!multiPresetMode) {
    if (tabBar) tabBar.remove();
    mixTabActive = false;
    return;
  }

  if (!tabBar) {
    tabBar = document.createElement("div");
    tabBar.id = "mixer-preset-tabs";
    tabBar.className = "mixer-preset-tabs";
    // Insert as a sibling immediately before ".signal-path-visualizer" — NOT
    // necessarily a direct child of #signal-path-bar. The current layout wraps
    // everything in ".signal-path-body", so insertBefore() must target the
    // actual parent of the reference node or it throws NotFoundError.
    const visualizer = signalPathBar?.querySelector(".signal-path-visualizer");
    if (visualizer?.parentElement) {
      visualizer.parentElement.insertBefore(tabBar, visualizer);
    } else if (signalPathBar) {
      signalPathBar.prepend(tabBar);
    }
  }

  const presetIds = [...mixer.activePresetIds];
  // Determine the focused tab: prefer the explicitly focused slot, then the
  // currently active preset if it's in the mixer, then the first slot.
  const focusedId = (uiState.focusedMixerPresetId && presetIds.includes(uiState.focusedMixerPresetId))
    ? uiState.focusedMixerPresetId
    : (uiState.activePresetId && presetIds.includes(uiState.activePresetId))
      ? uiState.activePresetId
      : presetIds[0];

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
    const closeBtn = `<span class="mixer-tab-close" data-close-preset-id="${escapeHtml(id)}" title="Remove from mixer" role="button" aria-label="Remove ${escapeHtml(name)}">×</span>`;
    return `<button class="mixer-preset-tab${active ? " active" : ""}" data-preset-id="${escapeHtml(id)}" type="button">${escapeHtml(name)}${indicators}${closeBtn}</button>`;
  }).join("");

  const mixTabHtml = `<button class="mixer-preset-tab mixer-tab-mix${mixTabActive ? " active" : ""}" data-mix-tab="1" type="button">⚖ Mix</button>`;

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
        focusMixerPreset(pid);
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
      // The mixer's membership no longer matches whatever Multi-Rig preset it
      // was loaded from/saved as, if any — a subsequent Save should create a
      // new one rather than silently overwrite the old one.
      uiState.activeCompositePresetId = null;
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
  focusMixerPreset(sourcePreset.id);
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

/** Matches the L/C/R pan convention used by node-param knobs elsewhere in the app. */
function formatMixerPanValue(value: number): string {
  if (Math.abs(value) < 0.01) return "C";
  return value < 0 ? `L${Math.abs(value * 100).toFixed(0)}` : `R${(value * 100).toFixed(0)}`;
}

function formatMixerPercentValue(value: number): string {
  return `${Math.round(value * 100)}%`;
}

function buildInlineMixerHtml(): string {
  const mixer = uiState.mixer;
  if (!mixer || !mixer.activePresetIds.length) return "";

  const presetIds = [...mixer.activePresetIds].sort((leftId, rightId) => {
    const leftName = uiState.presetCache.get(leftId)?.name ?? mixer.presets[leftId]?.name ?? leftId;
    const rightName = uiState.presetCache.get(rightId)?.name ?? mixer.presets[rightId]?.name ?? rightId;
    const nameComparison = mixerPresetTabCollator.compare(leftName, rightName);
    if (nameComparison !== 0) {
      return nameComparison;
    }
    return mixerPresetTabCollator.compare(leftId, rightId);
  });

  const strips = presetIds.map((id) => {
    const name = uiState.presetCache.get(id)?.name ?? mixer.presets[id]?.name ?? id;
    const ps = mixer.presets[id] ?? { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
    return `
      <div class="iml-strip" data-preset-id="${escapeHtml(id)}" style="--accent:${idAccentColor(id)}">
        <div class="iml-strip-name" title="${escapeHtml(name)}">${escapeHtml(name)}</div>
        <div class="iml-strip-row">
          <div class="iml-knobs">
            <div class="knob-control iml-knob">
              <span class="knob-label">Mix</span>
              <div class="knob iml-mix-knob" data-value="${ps.mix}"><div class="knob-indicator"></div></div>
              <span class="knob-value">${formatMixerPercentValue(ps.mix)}</span>
            </div>
            <div class="knob-control iml-knob">
              <span class="knob-label">Pan</span>
              <div class="knob iml-pan-knob" data-value="${ps.pan}"><div class="knob-indicator"></div></div>
              <span class="knob-value">${formatMixerPanValue(ps.pan)}</span>
            </div>
          </div>
          <div class="iml-toggles">
            <div class="toggle-control mini-toggle-control iml-mute-toggle">
              <span class="toggle-label">Mute</span>
              <label class="toggle-switch"><input type="checkbox" class="iml-mute"${ps.mute ? " checked" : ""}/><span class="toggle-slider"></span></label>
            </div>
            <div class="toggle-control mini-toggle-control iml-solo-toggle">
              <span class="toggle-label">Solo</span>
              <label class="toggle-switch"><input type="checkbox" class="iml-solo"${ps.solo ? " checked" : ""}/><span class="toggle-slider"></span></label>
            </div>
          </div>
        </div>
      </div>`;
  }).join("");

  return `
    <div class="iml-strips">${strips}</div>
    <div class="iml-master">
      <div class="iml-strip-name">Master</div>
      <div class="iml-strip-row">
        <div class="iml-knobs">
          <div class="knob-control iml-knob">
            <span class="knob-label">Gain</span>
            <div class="knob" id="iml-master-gain-knob" data-value="${mixer.masterGain}"><div class="knob-indicator"></div></div>
            <span class="knob-value">${formatMixerPercentValue(mixer.masterGain)}</span>
          </div>
        </div>
        <div class="iml-toggles">
          <div class="toggle-control mini-toggle-control">
            <span class="toggle-label">Limiter</span>
            <label class="toggle-switch"><input type="checkbox" id="iml-limiter"${mixer.limiterEnabled ? " checked" : ""}/><span class="toggle-slider"></span></label>
          </div>
          <div class="iml-toolbar">
            <button type="button" id="iml-save-multi-rig" class="btn btn-secondary btn-sm iml-toolbar-btn" title="Save current mixer as a Multi-Rig preset">Save</button>
            <button type="button" id="iml-delete-multi-rig" class="btn btn-secondary btn-sm iml-toolbar-btn"${uiState.activeCompositePresetId ? "" : " disabled"} title="Delete this Multi-Rig preset">Delete</button>
          </div>
        </div>
      </div>
    </div>`;
}

function renderInlineMixer(): void {
  const signalPathBar = document.getElementById("signal-path-bar");
  const resizeHandle = document.getElementById("signal-path-resize-handle");
  let panel = document.getElementById("inline-mixer-panel");
  if (!panel) {
    panel = document.createElement("div");
    panel.id = "inline-mixer-panel";
    panel.className = "inline-mixer-panel";
    // Place mixer where the scroll area sits (above the resize handle).
    // resizeHandle lives inside ".signal-path-body", not directly inside
    // #signal-path-bar, so insertBefore() must target its real parent.
    if (resizeHandle?.parentElement) {
      resizeHandle.parentElement.insertBefore(panel, resizeHandle);
    } else {
      signalPathBar?.appendChild(panel);
    }
  }
  panel.innerHTML = buildInlineMixerHtml();
  bindInlineMixerControls(panel);
}

function removeInlineMixer(): void {
  document.getElementById("inline-mixer-panel")?.remove();
}

/** Drag sensitivity matching the convention used for node-param knobs: full range over ~200px. */
function knobSensitivity(minValue: number, maxValue: number): number {
  return (maxValue - minValue) / 200;
}

function bindInlineMixerControls(panel: HTMLElement): void {
  panel.querySelectorAll<HTMLElement>(".iml-strip").forEach((strip) => {
    const pid = strip.dataset.presetId ?? "";
    if (!pid) return;

    const mixKnob = strip.querySelector<HTMLElement>(".iml-mix-knob");
    if (mixKnob) {
      new GenericKnob({
        knobElement: mixKnob,
        paramId: `mixer_${pid}_mix`,
        minValue: 0,
        maxValue: 1,
        defaultValue: 1,
        displayFormat: formatMixerPercentValue,
        valueDisplay: mixKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
        sensitivity: knobSensitivity(0, 1),
        sendParameter: false,
        onValueChange: (v) => setPresetMix(pid, v),
      });
    }

    const panKnob = strip.querySelector<HTMLElement>(".iml-pan-knob");
    if (panKnob) {
      new GenericKnob({
        knobElement: panKnob,
        paramId: `mixer_${pid}_pan`,
        minValue: -1,
        maxValue: 1,
        defaultValue: 0,
        displayFormat: formatMixerPanValue,
        valueDisplay: panKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
        sensitivity: knobSensitivity(-1, 1),
        sendParameter: false,
        onValueChange: (v) => setPresetPan(pid, v),
      });
    }

    const muteToggle = strip.querySelector<HTMLInputElement>(".iml-mute");
    muteToggle?.addEventListener("change", () => {
      const nowMuted = muteToggle.checked;
      setPresetMute(pid, nowMuted);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].mute = nowMuted;
      renderMixerPresetTabs(); // refresh M/S indicators in tabs
    });

    const soloToggle = strip.querySelector<HTMLInputElement>(".iml-solo");
    soloToggle?.addEventListener("change", () => {
      const nowSolo = soloToggle.checked;
      setPresetSolo(pid, nowSolo);
      if (uiState.mixer?.presets[pid]) uiState.mixer.presets[pid].solo = nowSolo;
      renderMixerPresetTabs();
    });
  });

  const masterGainKnob = panel.querySelector<HTMLElement>("#iml-master-gain-knob");
  if (masterGainKnob) {
    new GenericKnob({
      knobElement: masterGainKnob,
      paramId: "mixer_master_gain",
      minValue: 0,
      maxValue: 2,
      defaultValue: 1,
      displayFormat: formatMixerPercentValue,
      valueDisplay: masterGainKnob.parentElement?.querySelector<HTMLElement>(".knob-value"),
      sensitivity: knobSensitivity(0, 2),
      sendParameter: false,
      onValueChange: (v) => setMasterGain(v),
    });
  }

  panel.querySelector<HTMLInputElement>("#iml-limiter")?.addEventListener("change", (e) => {
    setLimiterEnabled((e.target as HTMLInputElement).checked);
  });

  panel.querySelector<HTMLButtonElement>("#iml-save-multi-rig")?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("mixerSaveMultiRig"));
  });

  panel.querySelector<HTMLButtonElement>("#iml-delete-multi-rig")?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("mixerDeleteMultiRig"));
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

  const positionDropdown = (): void => {
    const buttonRect = buttonElement.getBoundingClientRect();
    const margin = 8;
    const appliedZoom = Number.parseFloat(window.getComputedStyle(document.body).zoom);
    const uiZoom = Number.isFinite(appliedZoom) && appliedZoom > 0 ? appliedZoom : 1;
    const viewportWidth = window.innerWidth / uiZoom;
    const viewportHeight = window.innerHeight / uiZoom;

    dropdown.style.maxWidth = `${Math.min(300, viewportWidth - margin * 2)}px`;
    dropdown.style.maxHeight = `${Math.min(500, viewportHeight - margin * 2)}px`;
    const dropdownWidth = dropdown.offsetWidth;
    const dropdownHeight = dropdown.offsetHeight;
    const left = Math.max(
      margin,
      Math.min(buttonRect.left / uiZoom, viewportWidth - dropdownWidth - margin),
    );
    let top = buttonRect.bottom / uiZoom + 5;

    if (top + dropdownHeight > viewportHeight - margin) {
      top = Math.max(margin, buttonRect.top / uiZoom - dropdownHeight - 5);
    }

    dropdown.style.left = `${Math.round(left)}px`;
    dropdown.style.top = `${Math.round(top)}px`;
  };

  const closeDropdown = (): void => {
    window.removeEventListener("resize", positionDropdown);
    window.removeEventListener("scroll", positionDropdown, true);
    dropdown.remove();
  };

  positionDropdown();
  window.addEventListener("resize", positionDropdown);
  window.addEventListener("scroll", positionDropdown, true);

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
        closeDropdown();
      }
    });
  });

  // Close dropdown when clicking outside
  setTimeout(() => {
    const closeHandler = (e: MouseEvent) => {
      if (!dropdown.contains(e.target as Node)) {
        closeDropdown();
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

// ── Signal path mode gesture ──

const SIGNAL_PATH_FULL_HEIGHT = 96;
const SIGNAL_PATH_COMPACT_HEIGHT = 48;
const SIGNAL_PATH_LEGACY_COMPACT_HEIGHT_THRESHOLD = 80;
const SIGNAL_PATH_MODE_GESTURE_THRESHOLD = 12;

type SignalPathDensity = "compact" | "full";

let signalPathScrollHeight = SIGNAL_PATH_FULL_HEIGHT;
let signalPathResizeInitialized = false;
let signalPathLayoutAdaptRaf = 0;

function getSignalPathBarElement(): HTMLElement | null {
  return document.getElementById("signal-path-bar");
}

function getSignalPathScrollElement(): HTMLElement | null {
  return document.querySelector<HTMLElement>(".signal-path-scroll");
}

function getSignalPathResizeHandle(): HTMLElement | null {
  return document.getElementById("signal-path-resize-handle");
}

function heightForSignalPathDensity(density: SignalPathDensity): number {
  return density === "compact" ? SIGNAL_PATH_COMPACT_HEIGHT : SIGNAL_PATH_FULL_HEIGHT;
}

function densityFromSignalPathHeight(height: number): SignalPathDensity {
  return height <= SIGNAL_PATH_LEGACY_COMPACT_HEIGHT_THRESHOLD ? "compact" : "full";
}

function syncSignalPathResizeHandleAria(height: number): void {
  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }
  const density = densityFromSignalPathHeight(height);
  handle.setAttribute("aria-valuemin", String(SIGNAL_PATH_COMPACT_HEIGHT));
  handle.setAttribute("aria-valuemax", String(SIGNAL_PATH_FULL_HEIGHT));
  handle.setAttribute("aria-valuenow", String(height));
  handle.setAttribute("aria-valuetext", density === "compact" ? "Compact signal chain" : "Full signal chain");
}

/**
 * Apply the signal-chain mode selected by the splitter gesture. Node
 * dimensions remain fixed and never react to the available panel space.
 */
export function updateSignalPathLayoutAdapt(): void {
  const bar = getSignalPathBarElement();
  const scroll = getSignalPathScrollElement();
  const nodes = signalPathNodesElement;

  if (!bar || !scroll || !nodes || scroll.hidden || mixTabActive) {
    bar?.removeAttribute("data-density");
    return;
  }

  const density = densityFromSignalPathHeight(signalPathScrollHeight);
  if (bar.dataset.density !== density) {
    bar.dataset.density = density;
  }

  const minimumHeight = heightForSignalPathDensity(density);
  bar.style.setProperty("--signal-path-scroll-height", `${minimumHeight}px`);
  const viewportHeight = Math.max(minimumHeight, Math.ceil(scroll.scrollHeight));
  bar.style.setProperty("--signal-path-scroll-height", `${viewportHeight}px`);
}

export function scheduleSignalPathLayoutAdapt(): void {
  if (signalPathLayoutAdaptRaf) {
    cancelAnimationFrame(signalPathLayoutAdaptRaf);
  }
  signalPathLayoutAdaptRaf = requestAnimationFrame(() => {
    signalPathLayoutAdaptRaf = 0;
    updateSignalPathLayoutAdapt();
  });
}

/**
 * Apply one of the signal-chain's fixed visual modes. Legacy arbitrary saved
 * heights are normalized to the closest mode.
 */
export function setSignalPathScrollHeight(height: number, options?: { persist?: boolean }): number {
  const nextHeight = heightForSignalPathDensity(densityFromSignalPathHeight(height));
  signalPathScrollHeight = nextHeight;

  const bar = getSignalPathBarElement();
  if (bar) {
    bar.style.setProperty("--signal-path-scroll-height", `${nextHeight}px`);
  }
  syncSignalPathResizeHandleAria(nextHeight);
  scheduleSignalPathLayoutAdapt();

  if (options?.persist) {
    updateUiSettings({
      signalPathHeight: nextHeight,
    });
  }

  return nextHeight;
}

export function getSignalPathScrollHeight(): number {
  return signalPathScrollHeight;
}

function onSignalPathResizePointerDown(event: PointerEvent): void {
  if (event.button !== 0) {
    return;
  }

  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }

  event.preventDefault();
  const startY = event.clientY;
  const startDensity = densityFromSignalPathHeight(signalPathScrollHeight);
  let targetDensity = startDensity;

  handle.setPointerCapture(event.pointerId);
  document.body.classList.add("signal-path-resizing");

  const onMove = (moveEvent: PointerEvent): void => {
    const delta = moveEvent.clientY - startY;
    const nextDensity = delta <= -SIGNAL_PATH_MODE_GESTURE_THRESHOLD
      ? "compact"
      : delta >= SIGNAL_PATH_MODE_GESTURE_THRESHOLD
        ? "full"
        : startDensity;
    if (nextDensity === targetDensity) {
      return;
    }
    targetDensity = nextDensity;
    setSignalPathScrollHeight(heightForSignalPathDensity(targetDensity), { persist: false });
  };

  const finish = (endEvent: PointerEvent): void => {
    handle.releasePointerCapture(endEvent.pointerId);
    handle.removeEventListener("pointermove", onMove);
    handle.removeEventListener("pointerup", finish);
    handle.removeEventListener("pointercancel", finish);
    document.body.classList.remove("signal-path-resizing");
    if (endEvent.type === "pointercancel") {
      setSignalPathScrollHeight(heightForSignalPathDensity(startDensity), { persist: false });
      return;
    }
    setSignalPathScrollHeight(heightForSignalPathDensity(targetDensity), { persist: true });
  };

  handle.addEventListener("pointermove", onMove);
  handle.addEventListener("pointerup", finish);
  handle.addEventListener("pointercancel", finish);
}

function onSignalPathResizeKeyDown(event: KeyboardEvent): void {
  let nextDensity: SignalPathDensity | null = null;
  if (event.key === "ArrowUp" || event.key === "ArrowLeft") {
    nextDensity = "compact";
  } else if (event.key === "ArrowDown" || event.key === "ArrowRight") {
    nextDensity = "full";
  } else if (event.key === "Home") {
    nextDensity = "compact";
  } else if (event.key === "End") {
    nextDensity = "full";
  } else if (event.key === "Enter" || event.key === " ") {
    nextDensity = "full";
  }

  if (nextDensity === null) {
    return;
  }

  event.preventDefault();
  setSignalPathScrollHeight(heightForSignalPathDensity(nextDensity), { persist: true });
}

function onSignalPathResizeDoubleClick(event: MouseEvent): void {
  event.preventDefault();
  setSignalPathScrollHeight(SIGNAL_PATH_FULL_HEIGHT, { persist: true });
}

function applySignalPathHeightFromSettings(): void {
  const settings = getCurrentUiSettings();
  const stored = typeof settings.signalPathHeight === "number" && Number.isFinite(settings.signalPathHeight)
    ? settings.signalPathHeight
    : signalPathScrollHeight;
  const density = densityFromSignalPathHeight(stored);
  setSignalPathScrollHeight(heightForSignalPathDensity(density), { persist: false });
}

/**
 * Wire up the horizontal splitter gesture for signal-path mode selection.
 * Safe to call once during bootstrap; subsequent calls are no-ops.
 */
export function initSignalPathResize(): void {
  if (signalPathResizeInitialized) {
    applySignalPathHeightFromSettings();
    return;
  }

  const handle = getSignalPathResizeHandle();
  if (!handle) {
    return;
  }

  signalPathResizeInitialized = true;
  handle.addEventListener("pointerdown", onSignalPathResizePointerDown);
  handle.addEventListener("keydown", onSignalPathResizeKeyDown);
  handle.addEventListener("dblclick", onSignalPathResizeDoubleClick);

  window.addEventListener("uiSettingsApplied", () => {
    applySignalPathHeightFromSettings();
  });

  window.addEventListener("resize", () => {
    scheduleSignalPathLayoutAdapt();
  });

  applySignalPathHeightFromSettings();
  scheduleSignalPathLayoutAdapt();
}

/**
 * Returns a global file-drop handler for NAM/IR resource files.
 * When a .nam or IR (.wav / .ir) file is dropped anywhere in the app while a
 * compatible NAM or cab-IR node is selected, the file is saved to the local
 * resource library and loaded into that node.  This should be registered with
 * registerGlobalFileDropHandler at a priority below preset-pack drops.
 */
export function createNamIrGlobalFileDropHandler(): (files: File[], event: DragEvent) => Promise<boolean> {
  return async (files: File[]): Promise<boolean> => {
    const preset = getSignalPathPreset();
    if (!preset?.graph || !selectedNodeId) {
      return false;
    }
    const node = preset.graph.nodes.find((n) => n.id === selectedNodeId);
    if (!node || !isNamOrCabIrNode(node)) {
      return false;
    }
    const file = files[0];
    if (!file) {
      return false;
    }
    const resourceType = inferResourceTypeFromFile(file);
    if (!resourceType || !nodeAcceptsResourceType(node, resourceType)) {
      return false;
    }
    await handleNamIrFileDrop(file, node.id);
    return true;
  };
}
