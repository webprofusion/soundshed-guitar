import { uiState, getActivePresetForRender, getSignalPathPreset, setPresetDirty, isCompositeEditMode } from "./state.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import type {
  BlendModelMapping,
  GraphNode,
  Preset,
} from "./types.js";
import { postMessage } from "./bridge.js";
import { escapeHtml } from "./utils.js";
import { showNotification } from "./notifications.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { getBadgeIcon, renderIcon } from "./iconAssets.js";
import {
  CATEGORY_METADATA,
  expandFxSelector,
  focusFxSelectorCategory,
  getFxLibraryItems,
  getOrderedFxCategories,
  sendAddSignalPathNode,
  sendAddSignalPathNodeOnEdge,
  type FxPointerDragPayload,
  type FxLibraryItem,
  type SignalPathEdgeRef,
  type SignalPathNodeOptions,
} from "./fxSelector.js";
import { beginPointerDrag, getUiZoom, type PointerDragGesture } from "./pointerDrag.js";
import { resolveNodeDropAction, type NodeDropTarget } from "./signalPathDropTargets.js";
import { getCustomLayout } from "./layoutRenderer.js";
import { resolveLayoutForNode } from "./layoutPreferences.js";
import { layoutDesigner } from "./layoutDesigner.js";
import { createPresetScene, findPresetScene, normalizePresetScenes, removePresetScene, selectPresetScene } from "./presetScenes.js";
import { isNodeBypassed } from "./graphNodes.js";
import {
  getLastSelectedNodeCategory,
  getLastSelectedNodeType,
  getSelectedNodeId,
  nodeParamsPanelElement,
  setLastSelectedNode,
  setSelectedNodeId,
  signalPathAddMenu,
  signalPathAddMenuOptions,
  signalPathAddMenuTrigger,
  signalPathAddSceneButton,
  signalPathNodesElement,
} from "./signalPath/state.js";
import { sendCollapseParallelSplit, sendMoveSignalPathNodeToEdge, sendReplaceSignalPathNode, sendSignalPathNodeDelete, sendSignalPathNodeReorder } from "./signalPath/commands.js";
import { getSelectedSignalPathNode } from "./signalPath/nodeResources.js";
import { updateEffectVisualization } from "./signalPath/visualization.js";
import { getCategoryClass, getNodeCategory, getNodeIcon, handleNamIrFileDrop, inferResourceTypeFromFile, isNamOrCabIrNode, nodeAcceptsResourceType } from "./signalPath/nodeTypes.js";
import { getEditableSignalPathPreset, pushScenePresetToBackend, removeInlineMixer, renderInlineMixer, renderMixerPresetTabs } from "./signalPath/mixer.js";
import { isMixTabActive } from "./signalPath/state.js";
import { setNodeParamsRefresher, setSignalPathRenderer } from "./signalPath/render.js";
import { SIGNAL_PATH_FULL_HEIGHT, scheduleSignalPathLayoutAdapt } from "./signalPath/layout.js";
import type { EdgeRef } from "./signalPath/graph.js";
import { buildGraphMaps, normalizeEdge, parseEdgeFromDataset, pickPrimaryOutgoingEdge, sortEdgesByPort } from "./signalPath/graph.js";
import { showNodeParamsPanel } from "./signalPath/paramsPanel.js";
import { chain3dPanelActive, chain3dView, hideNodeParamsPanel } from "./signalPath/amp3dBridge.js";
import { buildMissingResourceTooltip, buildNodeLayoutMatchText, getMissingResourceEntries, getNodeArchitectureBadge, getNodeDisplayName, getNodeResourceDisplayName } from "./signalPath/nodeLabels.js";
import { isProtectedSignalPathNode, isToggleableSignalPathNode, toggleSignalPathNodeBypass } from "./signalPath/bypass.js";
export { applySignalPathNodeBypassState, isToggleableSignalPathNode } from "./signalPath/bypass.js";
export { applySpatialPositionUpdate, buildDefaultParamControlsHtml } from "./signalPath/paramsPanel.js";
export { closeEffectPresetsFlyout, refreshEffectPresetsFlyout } from "./signalPath/effectPresets.js";
export { initSignalPathResize } from "./signalPath/layout.js";
export { getSignalPathScrollHeight, scheduleSignalPathLayoutAdapt, setSignalPathScrollHeight, updateSignalPathLayoutAdapt } from "./signalPath/layout.js";
export { handleHostedPluginResourceLoadCompleted, handleHostedPluginResourceLoadFailed, handleNodeResourceBrowseCancelled } from "./signalPath/hostedPlugins.js";
export { updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter } from "./signalPath/telemetry.js";
export { isNodeBypassed };
export { initializeBlendEditorModal, openBlendEditorWithDefinition } from "./signalPathBlend.js";

let lastRenderedPresetId: string | null = null;

layoutDesigner.onClose(() => {
  refreshSelectedNodeParams();
  renderSignalPathBar();
});

// ── Signal-chain 3D stage ────────────────────────────────────────────────────
// One immersive WebGL stage for the whole graph. Neural FX uses a generic pedal
// + dock model chooser; amps/cabs cluster; everything else is a rack unit.

// ── Standard / custom layout switching ──────────────────────────────────────

// Lighting presets are theme-specific, so re-render while the 3D stage is on.
window.addEventListener("themeChanged", () => {
  if (chain3dView || chain3dPanelActive) {
    refreshSelectedNodeParams();
  }
});

function updateLastSelectedNode(node: GraphNode): void {
  setLastSelectedNode(node.type || null, getNodeCategory(node) || null);
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

/**
 * A drop target for an FX library item: replace a node, or insert on a
 * connection between two nodes.
 */
type FxDropTarget =
  | { element: HTMLElement; kind: "node"; node: GraphNode; preset: Preset }
  | { element: HTMLElement; kind: "edge"; edge: EdgeRef };

function setSignalPathDropHighlight(element: HTMLElement, kind: "node" | "edge", highlighted: boolean): void {
  element.classList.toggle("drag-over", highlighted);
  if (kind === "edge") {
    element.querySelector(".signal-connector")?.classList.toggle("drag-over", highlighted);
  }
}

/**
 * Hit-test the signal path under the pointer. Only elements inside the signal
 * path count, so a node rendered elsewhere (a preset preview, say) is ignored.
 */
function resolveSignalPathDropElements(event: PointerEvent): {
  nodeElement: HTMLElement | null;
  connector: HTMLElement | null;
} {
  const hit = document.elementFromPoint(event.clientX, event.clientY);
  const nodeElement = hit?.closest<HTMLElement>(".signal-node[data-node-id]") ?? null;
  const connector = hit?.closest<HTMLElement>(".signal-connector-wrapper") ?? null;
  const inSignalPath = (element: HTMLElement | null): HTMLElement | null =>
    element && signalPathNodesElement?.contains(element) ? element : null;
  return { nodeElement: inSignalPath(nodeElement), connector: inSignalPath(connector) };
}

function resolveFxDropTarget(event: PointerEvent): FxDropTarget | null {
  const { nodeElement, connector } = resolveSignalPathDropElements(event);

  if (nodeElement) {
    const preset = getSignalPathPreset();
    const nodeId = nodeElement.dataset.nodeId ?? "";
    const node = preset?.graph?.nodes.find((candidate) => candidate.id === nodeId);
    if (node && preset && !isProtectedSignalPathNode(node)) {
      return { element: nodeElement, kind: "node", node, preset };
    }
    return null;
  }

  if (connector) {
    const edge = parseEdgeFromDataset(connector);
    if (edge) return { element: connector, kind: "edge", edge };
  }
  return null;
}

function applyFxDrop(target: FxDropTarget | null, payload: FxPointerDragPayload): void {
  if (!target) return;

  const customEffect = payload.customEffect;
  const options = customEffect
    ? buildCustomEffectNodeOptions({ ...customEffect, baseEffectType: payload.effectType })
    : {
      config: payload.blendId ? { blendId: payload.blendId } : undefined,
      label: payload.blendName,
      category: payload.blendCategory,
    };

  if (target.kind === "node") {
    applyOptimisticNodeReplacement(target.node, payload.effectType, target.preset, options);
    sendReplaceSignalPathNode(target.node.id, payload.effectType, options);
    return;
  }

  sendAddEffectAtEdgeOrFallback(payload.effectType, target.edge, "__input__", options);
}

/**
 * The FX selector owns its markup but not the graph, so it announces a press on
 * a library item and the signal path decides what dropping it means.
 */
document.addEventListener("fx-pointer-drag-start", (event: Event) => {
  const detail = (event as CustomEvent<{
    source: HTMLElement;
    pointerEvent: PointerEvent;
    payload: FxPointerDragPayload;
  }>).detail;
  if (!detail?.source || !detail.payload?.effectType) return;

  beginPointerDrag<FxDropTarget>(detail.pointerEvent, {
    source: detail.source,
    sourceSelector: ".fx-item",
    previewClass: "fx-item-drag-preview",
    bodyClass: "fx-dragging",
    resolveTarget: resolveFxDropTarget,
    setTargetHighlight: (target, highlighted) =>
      setSignalPathDropHighlight(target.element, target.kind, highlighted),
    onDrop: (target) => applyFxDrop(target, detail.payload),
  });
});

/**
 * A drop target for reordering an existing chain node. `drop` carries the graph
 * facts; `element` is only used for the hover highlight.
 */
type NodeDragTarget = { element: HTMLElement; drop: NodeDropTarget<EdgeRef> };

function resolveNodeDragTarget(
  event: PointerEvent,
  draggedNodeId: string,
  sourceLeft: number,
): NodeDragTarget | null {
  const { nodeElement, connector } = resolveSignalPathDropElements(event);

  if (nodeElement) {
    const preset = getSignalPathPreset();
    const nodeId = nodeElement.dataset.nodeId ?? "";
    const node = preset?.graph?.nodes.find((candidate) => candidate.id === nodeId);
    if (!node || nodeId === draggedNodeId || isProtectedSignalPathNode(node)) return null;
    return {
      element: nodeElement,
      drop: {
        kind: "node",
        nodeId,
        isLeftOfDragged: nodeElement.getBoundingClientRect().left < sourceLeft,
        incomingEdges: (preset?.graph?.edges ?? []).map(normalizeEdge).filter((edge) => edge.to === nodeId),
      },
    };
  }

  if (connector) {
    const edge = parseEdgeFromDataset(connector);
    if (edge) return { element: connector, drop: { kind: "edge", edge } };
  }
  return null;
}

function applyNodeDrop(
  draggedNodeId: string,
  target: NodeDropTarget<EdgeRef> | null,
  gesture: PointerDragGesture,
): void {
  const action = resolveNodeDropAction({ draggedNodeId, target, gesture });
  switch (action.kind) {
    case "reorderToEdge":
      sendMoveSignalPathNodeToEdge(action.nodeId, action.edge);
      return;
    case "reorderAfterNode":
      sendSignalPathNodeReorder(action.nodeId, action.targetNodeId);
      return;
    case "toggleBypass": {
      const preset = getSignalPathPreset();
      const node = preset?.graph?.nodes.find((candidate) => candidate.id === action.nodeId);
      if (preset && node) toggleSignalPathNodeBypass(node, preset);
      return;
    }
    default:
      return;
  }
}

/**
 * Start dragging a chain node. Splitter and mixer tiles anchor a parallel
 * branch and cannot be moved, and presses that land on a control inside the
 * node belong to that control.
 */
function beginNodePointerDrag(event: PointerEvent, source: HTMLElement): void {
  if (event.target instanceof Element && event.target.closest("button, input, select, textarea")) {
    return;
  }

  const nodeId = source.dataset.nodeId ?? "";
  const node = getSignalPathPreset()?.graph?.nodes.find((candidate) => candidate.id === nodeId);
  if (!node || isProtectedSignalPathNode(node)) return;

  // Captured up front: a re-render mid-drag detaches the source, and a detached
  // element reports a zero rect.
  const sourceLeft = source.getBoundingClientRect().left;

  beginPointerDrag<NodeDragTarget>(event, {
    source,
    sourceSelector: ".signal-node",
    previewClass: "signal-node-drag-preview",
    matchPreviewHeight: true,
    resolveTarget: (moveEvent) => resolveNodeDragTarget(moveEvent, nodeId, sourceLeft),
    setTargetHighlight: (target, highlighted) =>
      setSignalPathDropHighlight(target.element, target.drop.kind, highlighted),
    onDrop: (target, gesture) => applyNodeDrop(nodeId, target?.drop ?? null, gesture),
  });
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

  setSelectedNodeId(targetNode.id);
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
    setSelectedNodeId(null);
    hideNodeParamsPanel();
    updateEffectVisualization();
    return;
  }

  // Keep the current node only while rendering the same preset.
  const currentNode = getSelectedNodeId() ? nodes.find((node) => node.id === getSelectedNodeId()) : undefined;
  if (!presetChanged && currentNode) {
    return;
  }

  const matchesCategory = (node: GraphNode): boolean => {
    if (!getLastSelectedNodeCategory()) return true;
    return getNodeCategory(node) === getLastSelectedNodeCategory();
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
  if (presetChanged || getSelectedNodeId() === null) {
    replacement = findFirstNamAmpNode();
  }

  if (!replacement && getLastSelectedNodeType()) {
    replacement = nodes.find((node) => node.type === getLastSelectedNodeType() && matchesCategory(node));
    if (!replacement) {
      replacement = nodes.find((node) => node.type === getLastSelectedNodeType());
    }
  }
  if (!replacement && getLastSelectedNodeCategory()) {
    replacement = nodes.find((node) => getNodeCategory(node) === getLastSelectedNodeCategory());
  }
  if (!replacement) {
    replacement = currentNode ?? nodes[0];
  }

  setSelectedNodeId(replacement?.id ?? null);
  if (replacement) {
    updateLastSelectedNode(replacement);
  }

  if (nodeParamsPanelElement?.classList.contains("visible") && replacement) {
    showNodeParamsPanel(replacement, preset);
  } else {
    updateEffectVisualization(replacement);
  }
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

  setSelectedNodeId(node.id);
  updateLastSelectedNode(node);
  toggleSignalPathNodeBypass(node, preset);

  event.preventDefault();
  event.stopImmediatePropagation();
}

document.addEventListener("keydown", handleSignalPathShortcutKeyDown, true);
document.addEventListener("resource-browser:navigation-cache-updated", () => {
  refreshSelectedNodeParams();
});

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

// Hand the renderer to the modules that can only *request* one — see
// signalPath/render.ts for why the indirection exists.
setSignalPathRenderer(renderSignalPathBar);

function renderSignalPathBarContent(): void {
  if (!signalPathNodesElement) {
    return;
  }

  const signalPathBar = document.getElementById("signal-path-bar");
  const sceneToolbarHost = document.getElementById("signal-path-scene-toolbar");
  const toolbarRow = document.getElementById("signal-path-toolbar");
  signalPathBar?.classList.toggle("mix-tab-active", isMixTabActive());
  signalPathBar?.classList.toggle("preset-loading", Boolean(uiState.presetLoadingId));

  // Show/hide composite edit mode banner
  updateCompositeEditBanner();

  // Render preset selection tabs and scene controls in a single bar.
  renderMixerPresetTabs();

  // Show inline mixer panel instead of signal chain when Mix tab is active
  const scroll = document.querySelector<HTMLElement>(".signal-path-scroll");
  if (isMixTabActive()) {
    if (scroll) scroll.hidden = true;
    if (sceneToolbarHost) sceneToolbarHost.innerHTML = "";
    toolbarRow?.classList.add("scene-toolbar-empty");
    updateSignalPathAddMenuAvailability(false);
    // Pin the mixer to the same height as the full-size signal chain,
    // independent of whichever compact/full density was active before
    // switching to Mix (updateSignalPathLayoutAdapt() skips recomputing
    // --signal-path-scroll-height while isMixTabActive(), so without this it
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
  if (!getSelectedNodeId()) {
    updateEffectVisualization();
  }
}

export function refreshSelectedNodeParams(): void {
  if (!getSelectedNodeId()) {
    return;
  }
  const activePreset = getActivePresetForRender() ?? undefined;
  if (!activePreset?.graph) {
    return;
  }
  const node = activePreset.graph.nodes.find((n) => n.id === getSelectedNodeId());
  if (!node) {
    return;
  }
  showNodeParamsPanel(node, activePreset);
}

setNodeParamsRefresher(refreshSelectedNodeParams);

function renderConnectorWrapper(edge: EdgeRef): string {
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
        html += renderConnectorWrapper(edge);

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

      html += renderConnectorWrapper(edge);

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
  const selectedClass = getSelectedNodeId() === node.id ? "selected" : "";
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
        setSelectedNodeId(null);
        hideNodeParamsPanel();
        updateEffectVisualization();
        return;
      }

      const nodeEl = btn.closest(".signal-node") as HTMLElement | null;
      const nodeId = nodeEl?.dataset.nodeId;
      if (!nodeId) return;

      sendSignalPathNodeDelete(nodeId);
      setSelectedNodeId(null);
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
    setSelectedNodeId(node.id);
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
      if (!node || getSelectedNodeId() === node.id) {
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

    // Reordering, and the vertical flick that toggles bypass.
    el.addEventListener("pointerdown", (event: PointerEvent) => {
      beginNodePointerDrag(event, el);
    });

    // Native drag-and-drop still handles the two drags that do not originate
    // from a pointer press inside the signal path: files from the OS, and
    // resource groups from the equipment library.
    el.addEventListener("dragover", (e: DragEvent) => {
      e.preventDefault();
      const nodeId = el.dataset.nodeId;
      if (!nodeId) return;

      const types = Array.from(e.dataTransfer?.types ?? []);
      if (types.includes("application/x-resource-group")) {
        el.classList.add("drag-over");
        if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
        return;
      }

      if (types.includes("Files")) {
        const node = preset.graph?.nodes.find((n) => n.id === nodeId);
        if (node && isNamOrCabIrNode(node)) {
          el.classList.add("drag-over");
          if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
        }
      }
    });

    el.addEventListener("dragleave", () => {
      el.classList.remove("drag-over");
    });

    el.addEventListener("drop", (e: DragEvent) => {
      e.preventDefault();
      el.classList.remove("drag-over");
      const targetNodeId = el.dataset.nodeId;
      if (!targetNodeId || !preset.graph) return;

      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      if (resourceGroupPayload) {
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        handleResourceGroupDrop(resourceGroupPayload, targetNodeId, targetNode?.type === EffectGuids.kAmpNamBlend);
        return;
      }

      const file = Array.from(e.dataTransfer?.files ?? [])[0];
      if (!file) return;
      const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
      const resourceType = inferResourceTypeFromFile(file);
      if (targetNode && isNamOrCabIrNode(targetNode) && resourceType && nodeAcceptsResourceType(targetNode, resourceType)) {
        void handleNamIrFileDrop(file, targetNodeId);
      }
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
        setSelectedNodeId(null);
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

    // FX library items are dragged with pointer events (see beginPointerDrag);
    // the only native drag that lands here comes from the equipment library.
    const setHighlight = (highlighted: boolean): void => {
      el.classList.toggle("drag-over", highlighted);
      el.querySelector(".signal-connector")?.classList.toggle("drag-over", highlighted);
    };

    el.addEventListener("dragover", (e: DragEvent) => {
      e.preventDefault();
      if (!Array.from(e.dataTransfer?.types ?? []).includes("application/x-resource-group")) return;
      setHighlight(true);
      if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
    });

    el.addEventListener("dragleave", () => {
      setHighlight(false);
    });

    el.addEventListener("drop", (e: DragEvent) => {
      e.preventDefault();
      setHighlight(false);
      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      if (resourceGroupPayload && preset.graph) {
        handleResourceGroupDrop(resourceGroupPayload, null, false, parseEdgeFromDataset(el));
      }
    });
  });
}

// ─── Per-effect user presets ─────────────────────────────────────────────────
// Saved parameter snapshots scoped to an effect type, persisted by the backend in
// effect-presets.json and mirrored into uiState. Factory presets are unaffected:
// they live in the effect registry and keep their existing per-effect surfaces.

// --- Effect presets flyout ---------------------------------------------------
// Small popover launched from the "Presets" chip in the effect header, matching
// the effect layout picker. One dropdown lists factory presets then the user's
// own; selecting applies immediately. Appended to <body> with fixed positioning
// because the effect shell clips its own overflow.

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
    const uiZoom = getUiZoom();
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
    if (!preset?.graph || !getSelectedNodeId()) {
      return false;
    }
    const node = preset.graph.nodes.find((n) => n.id === getSelectedNodeId());
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
