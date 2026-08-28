import { uiState, getActivePresetForRender } from "../state.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import type {
  BlendMode,
  BlendModelMapping,
  GraphNode,
  LibraryResource,
  Preset,
} from "../types.js";
import { postMessage } from "../bridge.js";
import { escapeHtml } from "../utils.js";
import { showNotification } from "../notifications.js";
import { showConfirm } from "../dialogs.js";
import { EffectTypeRegistry, getNodeEffectInfo, type EffectTypeInfo, type ParameterDef } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { renderIcon } from "../iconAssets.js";
import { resolveResourceIdAlias } from "../resourceDedup.js";
import { GenericKnob, enhanceRangeInput } from "../controls.js";
import {
  EqCurveInteraction,
  buildEqBandConfigsFromParams,
  drawEqCurve,
  eqBandChangeToParams,
  GRAPHIC_EQ_FREQUENCIES,
  buildGraphicEqBandConfigs,
  clampGraphicEqFrequency,
  graphicEqFrequencyBounds,
} from "../eqCurve.js";
import {
  SpatialPannerInteraction,
  type SpatialLiveState,
  type SpatialPosition,
} from "../spatialPanner.js";
import { resourceBrowserModal } from "../resourceBrowser.js";
import { renderCustomLayout, renderCustomLayoutBackdrop, formatParamValue, type LayoutResourceControlDef } from "../layoutRenderer.js";
import { resolveLayoutForNode } from "../layoutPreferences.js";
import {
  type BlendParamSpec,
  type BlendParamRange,
  BLEND_MAPPING_EPS,
  normalizeBlendValue,
  denormalizeBlendValue,
  buildParameterMapFromLegacy,
  computeBlendParamRange,
  getBlendState,
  updateBlendParamIndicators,
  updateBlendMatchSummary,
  renderBlendInfoHtml,
  bindBlendEditorControls,
} from "../signalPathBlend.js";
import { openCustomEffectDesigner } from "../customEffectDesigner.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { isNodeBypassed } from "../graphNodes.js";
import {
  analyzerSpectrogramHistoryByNode,
  getSelectedNodeDspStatusNodeId,
  nodeParamsPanelElement,
  setSelectedNodeDspStatusNodeId,
} from "./state.js";
import { bindSelectedNodeDspStatusToggle, isDspStatusVisible, resetDspStatusAverages, updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter } from "./telemetry.js";
import { sendNodeResourceUpdate, sendSignalPathNodeConfigUpdate, sendSignalPathNodeParamUpdate } from "./commands.js";
import { buildHostedPluginListHtml, buildHostedPluginLoadErrorHtml, buildHostedPluginLoadingIndicatorHtml, buildHostedPluginWarningMarkup, buildUnsupportedPluginWarningMarkup, clearInlineHostedPluginLoadError, hostedPluginLoadFailures, isBlockedHostedPluginLibraryEntry, markHostedPluginLoadPending, renderHostedPluginWarningIntoOpenPanel, toggleHostedPluginFavorite } from "./hostedPlugins.js";
import { getNodeResourceAtIndex } from "./nodeResources.js";
import { getEffectVisualizationEquipmentImage, getEffectVisualizationStockImage, updateEffectVisualization } from "./visualization.js";
import { collectPreferredNodeResourceIds, getDeduplicatedLibraryResources, getNodeCategory, isNeuralModelNode } from "./nodeTypes.js";
import { bindEffectPresetsButton } from "./effectPresets.js";
import { bindEquipmentImageFallback, bindResourceControls } from "./resourceControls.js";
import { resolveResourceContextKey, resolveResourceNavigationCategoryHint } from "./resourceContext.js";
import { shouldShowFullRigCabModelNote } from "./chainRules.js";
import { buildGraphMaps } from "./graph.js";
import { disposeChain3dView } from "./amp3dBridge.js";
import { toggleSignalPathNodeBypass } from "./bypass.js";
import { promptSaveCurrentCustomEffect } from "./customEffectActions.js";
import { requestNodeParamsRefresh } from "./render.js";
import { buildCustomEffectActions, buildNodeLayoutMatchText, getNodeArchitectureBadge, getNodeDisplayName, getNodeNamCalibrationMetadataChip, getNodeResourceDisplayName } from "./nodeLabels.js";
import { bindAmp3dDockCollapse, bindAmp3dToggleButton, bindChain3dView, renderAmp3dToggleButtonHtml, renderChain3dViewportHtml, setAmp3dImmersiveMode, shouldRenderChain3dView, splitAmp3dParamDefs } from "./amp3dBridge.js";
import { bindLayoutSwitchButton, renderLayoutSwitchButtonHtml } from "./layoutSwitch.js";
import { setLastSelectedNode } from "./state.js";

/** Whether the Mix tab is currently active in the multi-preset tab bar. */
export let signalPathEqInteraction: EqCurveInteraction | null = null;

export let signalPathSpatialInteraction: SpatialPannerInteraction | null = null;

export let signalPathSpatialNodeId: string | null = null;

/** Knob instances for the current node params panel, keyed by param key. */
export const nodeParamKnobs = new Map<string, GenericKnob>();

export let overlayBypassClickCleanup: (() => void) | null = null;

export let layoutScaleObserverCleanups: (() => void)[] = [];

/**
 * Per-input level controls for a mixer node, built from the graph edges that
 * feed it. Empty for any other node type.
 */
export function buildMixerInputControlsHtml(node: GraphNode, preset: Preset): string {
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
  return mixerInputControls;
}

/**
 * Warms the resource browser's navigation cache so a node's prev/next buttons
 * work without opening the browser first. Fire-and-forget: never blocks render.
 */
export function preloadResourceNavigationCaches(node: GraphNode, preset: Preset, typeInfo: EffectTypeInfo | undefined): void {
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

/** The resource slots for a node, plus any controls a custom layout can position. */
export interface NodeResourceSelector {
  html: string;
  layoutControls: LayoutResourceControlDef[];
}

/**
 * Builds the resource selector for a node: one slot per exposed resource for a
 * composite, otherwise a single slot (or an A/B pair when the node is blended).
 */
export function buildNodeResourceSelector(
  node: GraphNode,
  preset: Preset,
  typeInfo: EffectTypeInfo | undefined,
  blendState: ReturnType<typeof getBlendState>,
): NodeResourceSelector {
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
  return { html: resourceSelector, layoutControls: customLayoutResourceControls };
}

export function showNodeParamsPanel(node: GraphNode, preset: Preset): void {
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
  if (getSelectedNodeDspStatusNodeId() !== node.id) {
    setSelectedNodeDspStatusNodeId(node.id);
    resetDspStatusAverages();
  }

  nodeParamsPanelElement.classList.add("visible");
  setLastSelectedNode(node.type || null, getNodeCategory(node) || null);
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

  const mixerInputControls = buildMixerInputControlsHtml(node, preset);

  preloadResourceNavigationCaches(node, preset, typeInfo);

  const { html: resourceSelector, layoutControls: customLayoutResourceControls } =
    buildNodeResourceSelector(node, preset, typeInfo, blendState);

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
            <button class="default-effect-shell-meter-toggle" type="button" aria-expanded="${isDspStatusVisible()}" title="${isDspStatusVisible() ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">
              <span class="default-effect-shell-meter" style="--meter-fill-scale: 0"></span>
            </button>
            <div class="effect-dsp-status" aria-label="Live DSP status" ${isDspStatusVisible() ? "" : "hidden"}>
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
            <button class="default-effect-shell-chip default-effect-shell-chip-dsp dsp-badge-toggle${isDspStatusVisible() ? " is-active" : ""}" type="button" aria-expanded="${isDspStatusVisible()}" title="${isDspStatusVisible() ? "Hide DSP status" : "Show DSP status"}" aria-label="Toggle DSP status">DSP</button>
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
export function applyCustomLayoutScaling(container: HTMLElement | null): void {
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

export function bindLayoutOverlayBypassToggles(node: GraphNode, preset: Preset): void {
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

export function bindParamTabs(): void {
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

export function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

export function isToggleParam(paramDef: { key: string; min?: number; max?: number; unit?: string }): boolean {
  return paramDef.unit==="toggle";
}

/**
 * Build the default parameter controls HTML using only default values.
 * Produces the same DOM structure as the live renderParamControl path so the
 * layout designer can render a faithful preview without a live node.
 * nodeId is used for data attributes so knob CSS still applies correctly.
 */
export function buildDefaultParamControlsHtml(
  paramDefs: ParameterDef[],
  nodeId = "preview"
): string {
  const renderOne = (p: ParameterDef): string => {
    const label = p.name || formatParamLabel(p.key);
    const value = p.default ?? 0;
    const min = p.min ?? 0;
    const max = p.max ?? 1;
    const unit = p.unit || "amount";
    const isToggle = isToggleParam(p);
    const isEnum = unit === "enum" && Array.isArray(p.labels) && p.labels.length > 0;
    const enumLabels = Array.isArray(p.labels) ? p.labels : [];

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

export function bindNodeParamControls(node: GraphNode, preset: Preset): void {
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

export function updateEqVisualization(node: GraphNode): void {
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

export const SPATIAL_PARAM_KEYS: ReadonlyArray<keyof SpatialPosition> = ["azimuth", "elevation", "distance"];

export function readSpatialPosition(node: GraphNode): SpatialPosition {
  const params = node.params ?? {};
  return {
    azimuth: typeof params.azimuth === "number" ? params.azimuth : 0,
    elevation: typeof params.elevation === "number" ? params.elevation : 0,
    distance: typeof params.distance === "number" ? params.distance : 1.5,
  };
}

export function updateSpatialVisualization(node: GraphNode): void {
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

export function bindGraphicEqControls(node: GraphNode, preset: Preset): void {  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kEqGraphic) {
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

export function bindCustomEffectActionControls(node: GraphNode): void {
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

export function bindHostedPluginActionControls(node: GraphNode): void {
  const openButtons = nodeParamsPanelElement?.querySelectorAll<HTMLButtonElement>(".plugin-host-open-btn");
  openButtons?.forEach((openButton) => openButton.addEventListener("click", () => {
    sendSignalPathNodeConfigUpdate(node.id, "showPluginEditor", "1", false);
  }));
}

export function bindHostedPluginListControls(node: GraphNode): void {
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
        requestNodeParamsRefresh();
      });
    });
  });
}

export function bindBypassButton(node: GraphNode, preset: Preset): void {
  const bypassButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-bypass-btn");
  bypassButtons.forEach((bypassBtn) => {
    bypassBtn.addEventListener("click", () => {
      toggleSignalPathNodeBypass(node, preset);
    });
  });
}

export function bindBlendModeOverride(node: GraphNode): void {
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
