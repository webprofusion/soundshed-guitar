import { uiState, getActivePresetForRender, getSignalPathPreset, isCompositeEditMode } from "./state.js";
import { setActivePresetSceneId } from "./presetLibraryStore.js";
import {
  RESOURCE_GROUP_DRAG_START_EVENT,
  buildBlendModelMappingsFromIds,
  findBlendForToneGroup,
  type ResourceGroupDragPayload,
  type ResourceGroupDragStartDetail,
} from "./blendUtils.js";
import type {
  BlendModelMapping,
  GraphNode,
  Preset,
} from "./types.js";
import { postMessage } from "./bridge.js";
import { revealCompactNodeDetail } from "./compactStage.js";
import { escapeHtml, safeImageDataUrl } from "./utils.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "./presetV2.js";
import { EffectGuids } from "./effectGuids.js";
import { renderIcon } from "./iconAssets.js";
import {
  sendAddSignalPathNode,
  sendAddSignalPathNodeOnEdge,
  type FxPointerDragPayload,
  type SignalPathEdgeRef,
  type SignalPathNodeOptions,
} from "./fxSelector.js";
import { fxPayloadIdentity } from "./fxLibrary/catalog.js";
import { recordFxRecent } from "./fxLibrary/favorites.js";
import { TOUCH_DRAG_HOLD_MS, beginPointerDrag, type PointerDragGesture } from "./pointerDrag.js";
import { resolveNodeDropAction, type NodeDropTarget } from "./signalPathDropTargets.js";
import { resolveLayoutForNode } from "./layoutPreferences.js";
import { layoutDesigner } from "./layoutDesigner.js";
import { closeLayoutPicker } from "./layoutPicker.js";
import { normalizePresetScenes } from "./presetScenes.js";
import { isNodeBypassed } from "./graphNodes.js";
import {
  getLastSelectedNodeCategory,
  getLastSelectedNodeType,
  getSelectedNodeId,
  nodeParamsPanelElement,
  setLastSelectedNode,
  setSelectedNodeId,
  signalPathNodesElement,
} from "./signalPath/state.js";
import { sendCollapseParallelSplit, sendMoveSignalPathNodeToEdge, sendReplaceSignalPathNode, sendSignalPathNodeDelete, sendSignalPathNodeReorder } from "./signalPath/commands.js";
import { getSelectedSignalPathNode } from "./signalPath/nodeResources.js";
import { getEffectVisualizationEquipmentImage, showVisualizerPanel, updateEffectVisualization } from "./signalPath/visualization.js";
import { expectAddedNode, forgetExpectedAddedNode, takeAddedNode } from "./signalPath/addedNode.js";
import { getCategoryClass, getNodeCategory, getNodeIcon, handleNamIrFileDrop, inferResourceTypeFromFile, isNamOrCabIrNode, nodeAcceptsResourceType } from "./signalPath/nodeTypes.js";
import { getEditableSignalPathPreset, removeInlineMixer, renderInlineMixer, renderMixerPresetTabs } from "./signalPath/mixer.js";
import { isMixTabActive } from "./signalPath/state.js";
import { setNodeParamsRefresher, setSignalPathRenderer } from "./signalPath/render.js";
import { SIGNAL_PATH_FULL_HEIGHT, scheduleSignalPathLayoutAdapt } from "./signalPath/layout.js";
import { renderBoundaryNode, renderConnectorWrapper, renderSignalChainRow, signalChainLineAt } from "./signalPath/chainRow.js";
import type { EdgeRef } from "./signalPath/graph.js";
import { buildGraphMaps, normalizeEdge, parseEdgeFromDataset, pickPrimaryOutgoingEdge, sortEdgesByPort } from "./signalPath/graph.js";
import { collapseSplitTitle, redrawNodeParamsPanelIfOutOfDate, showNodeParamsPanel } from "./signalPath/paramsPanel.js";
import { buildMissingResourceTooltip, buildNodeLayoutMatchText, getMissingResourceEntries, getNodeArchitectureBadge, getNodeDisplayName, getNodeResourceDisplayName, getNodeUserTitle } from "./signalPath/nodeLabels.js";
import { isProtectedSignalPathNode, isToggleableSignalPathNode, toggleSignalPathNodeBypass } from "./signalPath/bypass.js";
import { updateSignalPathClipIndicators } from "./signalPath/telemetry.js";
import { reanchorAddEffectDropdown, showAddEffectDropdown } from "./signalPath/addEffectDropdown.js";
import { initSignalPathAddMenu, updateSignalPathAddMenuAvailability } from "./signalPath/addMenu.js";
import { openReplaceChooser, syncReplaceChooser } from "./signalPath/replaceChooser.js";
import { clearSceneBadges, renderSceneBadges } from "./signalPath/sceneBadges.js";
import { addSceneFromToolbar, sceneBadgeActions } from "./signalPath/sceneActions.js";
export { applySignalPathNodeBypassState, isToggleableSignalPathNode } from "./signalPath/bypass.js";
export { applySpatialPositionUpdate, buildDefaultParamControlsHtml } from "./signalPath/paramsPanel.js";
export { closeEffectPresetsFlyout, refreshEffectPresetsFlyout } from "./signalPath/effectPresets.js";
export { showEngineNodeParamChange } from "./signalPath/engineParamChange.js";
export { initSignalPathResize } from "./signalPath/layout.js";
export { getSignalPathScrollHeight, scheduleSignalPathLayoutAdapt, setSignalPathScrollHeight, updateSignalPathLayoutAdapt } from "./signalPath/layout.js";
export { handleHostedPluginResourceLoadCompleted, handleHostedPluginResourceLoadFailed, handleNodeResourceBrowseCancelled } from "./signalPath/hostedPlugins.js";
export { updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter, updateSignalPathClipIndicators } from "./signalPath/telemetry.js";
export { isNodeBypassed };
export { initializeBlendEditorModal, openBlendEditorWithDefinition } from "./signalPathBlend.js";

let lastRenderedPresetId: string | null = null;
let lastRenderedSceneId: string | null = null;

layoutDesigner.onClose(() => {
  refreshSelectedNodeParams();
  renderSignalPathBar();
});

function hideNodeParamsPanel(): void {
  nodeParamsPanelElement?.classList.remove("visible");
  closeLayoutPicker();
}

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

function fxItemNodeOptions(payload: FxPointerDragPayload): SignalPathNodeOptions {
  const customEffect = payload.customEffect;
  return customEffect
    ? buildCustomEffectNodeOptions({ ...customEffect, baseEffectType: payload.effectType })
    : { config: payload.blendId ? { blendId: payload.blendId } : undefined, label: payload.blendName, category: payload.blendCategory };
}

/** Puts an FX library effect in a node's place: dropped on it, or chosen in the library opened to replace it. */
export function replaceNodeWithFxItem(node: GraphNode, preset: Preset, payload: FxPointerDragPayload): void {
  const options = fxItemNodeOptions(payload);
  applyOptimisticNodeReplacement(node, payload.effectType, preset, options);
  sendReplaceSignalPathNode(node.id, payload.effectType, options);
}

function applyFxDrop(target: FxDropTarget | null, payload: FxPointerDragPayload): void {
  if (target) {
    recordFxRecent(fxPayloadIdentity(payload));
  }
  if (target?.kind === "node") {
    replaceNodeWithFxItem(target.node, target.preset, payload);
  } else if (target) {
    sendAddEffectAtEdgeOrFallback(payload.effectType, target.edge, "__input__", fxItemNodeOptions(payload));
  }
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
 * An equipment library group lands where an FX library item does: in a node's place, or on
 * a connection. The library scrolls, so a touch rests on the group before it picks it up.
 */
document.addEventListener(RESOURCE_GROUP_DRAG_START_EVENT, (event: Event) => {
  const detail = (event as CustomEvent<ResourceGroupDragStartDetail>).detail;
  if (!detail?.source || !detail.payload?.modelIds?.length) return;

  beginPointerDrag<FxDropTarget>(detail.pointerEvent, {
    source: detail.source,
    sourceSelector: ".equipment-library-group",
    previewClass: "list-drag-preview",
    bodyClass: "fx-dragging",
    touchHoldMs: TOUCH_DRAG_HOLD_MS,
    resolveTarget: resolveFxDropTarget,
    setTargetHighlight: (target, highlighted) =>
      setSignalPathDropHighlight(target.element, target.kind, highlighted),
    onDrop: (target) => {
      if (target?.kind === "node") {
        handleResourceGroupDrop(detail.payload, target.node.id, target.node.type === EffectGuids.kAmpNamBlend);
      } else if (target) {
        handleResourceGroupDrop(detail.payload, null, false, target.edge);
      }
    },
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
  releasedOnAnotherLine: boolean,
): void {
  const action = resolveNodeDropAction({ draggedNodeId, target, gesture, releasedOnAnotherLine });
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
  const sourceLine = signalChainLineAt(signalPathNodesElement, event.clientY);

  beginPointerDrag<NodeDragTarget>(event, {
    source,
    sourceSelector: ".signal-node",
    previewClass: "signal-node-drag-preview",
    matchPreviewHeight: true,
    resolveTarget: (moveEvent) => resolveNodeDragTarget(moveEvent, nodeId, sourceLeft),
    setTargetHighlight: (target, highlighted) =>
      setSignalPathDropHighlight(target.element, target.drop.kind, highlighted),
    onDrop: (target, gesture) => {
      const releaseLine = sourceLine === null ? null : signalChainLineAt(signalPathNodesElement, event.clientY + gesture.deltaY);
      applyNodeDrop(nodeId, target?.drop ?? null, gesture, releaseLine !== null && releaseLine !== sourceLine);
    },
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
  showVisualizerPanel();
}

function selectNodeForPreset(preset: Preset, presetChanged: boolean, sceneChanged: boolean): void {
  const nodes = preset.graph?.nodes ?? [];
  if (!nodes.length) {
    setSelectedNodeId(null);
    hideNodeParamsPanel();
    updateEffectVisualization();
    return;
  }

  // An effect the user just added is the one they want to work on.
  const addedNode = takeAddedNode(preset);
  if (addedNode) {
    setSelectedNodeId(addedNode.id);
    showNodeParamsPanel(addedNode, preset);
    revealCompactNodeDetail();
    showVisualizerPanel();
    return;
  }

  // Keep the current node only while rendering the same preset. Another scene keeps it too, when
  // it has the node, but with that scene's settings: the panel is drawn again from its copy.
  const currentNode = getSelectedNodeId() ? nodes.find((node) => node.id === getSelectedNodeId()) : undefined;
  if (!presetChanged && currentNode) {
    if (sceneChanged) {
      showNodeParamsPanel(currentNode, preset);
      return;
    }
    return redrawNodeParamsPanelIfOutOfDate(currentNode, preset);
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
  signalPathBar?.classList.toggle("mix-tab-active", isMixTabActive());
  signalPathBar?.classList.toggle("preset-loading", Boolean(uiState.presetLoadingId));

  // Show/hide composite edit mode banner
  updateCompositeEditBanner();

  // Render the mixer's preset tabs (the scene badges follow once the chain is drawn).
  renderMixerPresetTabs();

  // Show inline mixer panel instead of signal chain when Mix tab is active
  const scroll = document.querySelector<HTMLElement>(".signal-path-scroll");
  if (isMixTabActive()) {
    if (scroll) scroll.hidden = true;
    clearSceneBadges();
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
    // Only before the engine's first state: it always has an active preset after that (a new
    // one when nothing else loads), so there is never a "no preset" chain to draw.
    if (signalPathNodesElement) signalPathNodesElement.innerHTML = "";
    clearSceneBadges();
    updateSignalPathAddMenuAvailability(false);
    updateEffectVisualization();
    return;
  }

  const editablePreset = getEditableSignalPathPreset(activePreset);

  // Settle the scene first: its graph is the chain drawn below.
  const activeSceneId = normalizePresetScenes(editablePreset, uiState.activePresetSceneId ?? undefined);
  setActivePresetSceneId(activeSceneId);
  const sceneChanged = !presetChanged && activeSceneId !== lastRenderedSceneId;
  lastRenderedSceneId = activeSceneId;

  // Render graph-based signal path (supports parallel paths)
  if (editablePreset.graph?.nodes) {
    selectNodeForPreset(editablePreset, presetChanged, sceneChanged);
    renderGraphSignalPath(editablePreset);
  } else {
    // Empty preset - show only input/output
    signalPathNodesElement.innerHTML = renderSignalChainRow([`
      <div class="signal-connector-wrapper">
        <div class="signal-connector"></div>
        <button class="signal-add-btn" data-insert-after="__input__" title="Add Effect">
          <span class="add-icon">+</span>
        </button>
      </div>${renderBoundaryNode("output")}`]);

    // Bind minimal handlers (legacy fallback uses insertAfter=__input__)
    bindAddButtonHandlers();
  }

  renderSceneBadges(editablePreset, activeSceneId ?? "", !isCompositeEditMode(), sceneBadgeActions(editablePreset));

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

  // Deep enough for any split a person builds; it only stops a malformed graph recursing forever
  // (SplitBranchSupport's kMaxNestedSplits).
  const MAX_NESTED_SPLITS = 16;

  const renderParallelForSplitter = (splitterId: string, depth = 0): { html: string; mixerId: string | null } => {
    if (depth > MAX_NESTED_SPLITS) return { html: "", mixerId: null };
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
        let continueFrom = node.id;
        if (node.type === EffectGuids.kSplitter || isSplitPoint(node.id)) {
          // A split inside this branch draws its own branches and mixer, and the branch
          // carries on from that mixer.
          const nested = renderParallelForSplitter(node.id, depth + 1);
          if (!nested.html || !nested.mixerId || nested.mixerId === joinId) {
            break;
          }
          html += nested.html;
          continueFrom = nested.mixerId;
        }

        edge = pickPrimaryOutgoingEdge(outgoing, continueFrom);
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
      ? renderNodeElement(joinNode, canCollapse ? { collapseSplitterId: splitterId, collapseTitle: collapseSplitTitle(outs.filter((edge) => edge.to !== joinId).length) } : undefined)
      : "";

    const html = `
      <div class="parallel-container" data-splitter-id="${escapeHtml(splitterId)}" data-mixer-id="${escapeHtml(joinId)}">
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

  // One segment per connector and what it leads into (see signalPath/chainRow.ts):
  // a wrapped chain breaks its lines only between segments.
  const renderMainChain = (): string[] => {
    const segments: string[] = [];
    const appendToSegment = (html: string): void => {
      if (segments.length) segments[segments.length - 1] += html;
      else segments.push(html);
    };
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
            appendToSegment(parallelHtml);
            currentId = mixerId;
            continue;
          }
        }
      }

      const edge = pickPrimaryOutgoingEdge(outgoing, currentId);
      if (!edge) {
        break;
      }

      segments.push(renderConnectorWrapper(edge));

      if (edge.to === "__output__") {
        appendToSegment(renderBoundaryNode("output"));
        return segments;
      }

      const nextNode = nodeById.get(edge.to);
      if (!nextNode) {
        break;
      }
      appendToSegment(renderNodeElement(nextNode));
      currentId = nextNode.id;
    }

    // The walk never reached the output: it stands alone, as it always did.
    segments.push(renderBoundaryNode("output"));
    return segments;
  };

  signalPathNodesElement.innerHTML = renderSignalChainRow(renderMainChain());

  // Bind click handlers
  bindNodeClickHandlers(preset);
  syncReplaceChooser(preset);

  // Bind split/collapse buttons
}

function sendAddEffectAtEdgeOrFallback(
  effectType: string,
  edge: EdgeRef | null,
  fallbackInsertAfter: string,
  options?: SignalPathNodeOptions,
): void {
  expectAddedNode(getSignalPathPreset());
  if (edge) {
    sendAddSignalPathNodeOnEdge(effectType, edge, options);
  } else {
    // Back-compat: linear chain insertion by node id
    sendAddSignalPathNode(effectType, fallbackInsertAfter, options);
  }
}

type RenderNodeElementOptions = {
  /** When set on a mixer join node, delete collapses the parallel split; collapseTitle is its tooltip. */
  collapseSplitterId?: string;
  collapseTitle?: string;
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
  const displayName = getNodeUserTitle(node) || firstResourceTitle || getNodeDisplayName(node);
  const effectTypeName = firstResourceTitle
    ? (getNodeUserTitle(node) ? firstResourceTitle : (nodeTypeInfo?.displayName || ""))
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

  // Small avatar over the node's top edge: layout thumbnail, else the custom-effect one, else the same artwork the visualization panel shows.
  const blendId = node.config?.blendId ?? "";
  // Honour the user's layout preference so the avatar matches what the params
  // panel will actually render for this node.
  const nodeLayout = resolveLayoutForNode({
    effectType: node.type,
    blendId: blendId || undefined,
    matchText: buildNodeLayoutMatchText(node),
    presetId: uiState.activePresetId,
  });
  // Layout and Custom Effect thumbnails are someone's own word, so inline images only; the
  // equipment pictures are the app's own assets.
  const thumbUrl = safeImageDataUrl(nodeLayout?.thumbnailDataUrl ?? nodeTypeInfo?.thumbnailDataUrl)
    || (getEffectVisualizationEquipmentImage(node) || null);
  const thumbAvatar = thumbUrl
    ? `<span class="node-layout-thumb-wrap" aria-hidden="true"><img class="node-layout-thumb" src="${escapeHtml(thumbUrl)}" alt="" /><span class="node-layout-thumb-icon">${icon}</span></span>`
    : "";
  const thumbClass = thumbUrl ? " has-thumb" : "";
  const deleteButton = allowDelete
    ? (canCollapseParallel
      ? `<button class="signal-node-delete" type="button" title="${escapeHtml(options?.collapseTitle ?? "Remove split")}" aria-label="Collapse split" data-collapse-splitter-id="${escapeHtml(collapseSplitterId!)}" data-collapse-mixer-id="${escapeHtml(node.id)}">×</button>`
      : '<button class="signal-node-delete" type="button" title="Remove" aria-label="Remove">×</button>')
    : "";
  const bypassButton = isToggleableSignalPathNode(node)
    ? `<button class="signal-node-bypass${nodeBypassed ? " bypassed" : ""}" type="button" title="${nodeBypassed ? "Enable effect" : "Bypass effect"}" aria-label="${nodeBypassed ? "Enable effect" : "Bypass effect"}" aria-pressed="${String(nodeBypassed)}">${renderIcon("output", "fx-effect-icon signal-node-bypass-icon")}</button>`
    : "";

  return `
    <div class="signal-node ${escapeHtml(categoryClass)} ${bypassedClass} ${selectedClass} ${missingClass}${thumbClass}" 
         data-node-id="${escapeHtml(node.id)}" 
         tabindex="0"${nodeTitleAttr}${nodeAriaLabel}>
      ${thumbAvatar}
      ${deleteButton}
      ${bypassButton}
      ${thumbUrl ? `<div class="node-icon"></div>` : `<div class="node-icon">${icon}</div>`}
      <div class="node-info">
        <div class="node-name">${escapeHtml(displayName)}</div>
        ${effectTypeName ? `<div class="node-type">${escapeHtml(effectTypeName)}</div>` : ""}
        ${architectureBadge ? `<div class="node-architecture-badge" aria-label="Model architecture">${escapeHtml(architectureBadge)}</div>` : ""}
      </div>
      <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
      ${nodeBypassed ? '<div class="node-bypass-badge">OFF</div>' : ""}
      ${missingBadge}
    </div>
  `;
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
    forgetExpectedAddedNode();
    setSelectedNodeId(node.id);
    showNodeParamsPanel(node, preset);
    // Compact shows the chain and the params panel one at a time, so picking a
    // node here has to bring its controls forward or the tap looks like a no-op.
    revealCompactNodeDetail();

      nodeElements.forEach((n) => n.classList.remove("selected"));
      el.classList.add("selected");
      if (focusElement) {
        el.focus();
      }
      showVisualizerPanel();
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
      const node = getGraphNode(el.dataset.nodeId ?? "");
      if (node) {
        openReplaceChooser(node, preset, replaceNodeWithFxItem);
      }
    });

    // Reordering, and the vertical flick that toggles bypass.
    el.addEventListener("pointerdown", (event: PointerEvent) => {
      beginNodePointerDrag(event, el);
    });

    // Native drag-and-drop still handles the one drag that does not start inside the
    // WebView: a file from the OS. It takes only the files it can use and leaves the rest,
    // a preset pack above all, to the global importer, which skips a drop already taken.
    el.addEventListener("dragover", (e: DragEvent) => {
      const nodeId = el.dataset.nodeId;
      if (!nodeId) return;

      const types = Array.from(e.dataTransfer?.types ?? []);
      if (types.includes("Files")) {
        const node = preset.graph?.nodes.find((n) => n.id === nodeId);
        if (node && isNamOrCabIrNode(node)) {
          e.preventDefault();
          el.classList.add("drag-over");
          if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
        }
      }
    });

    el.addEventListener("dragleave", () => {
      el.classList.remove("drag-over");
    });

    el.addEventListener("drop", (e: DragEvent) => {
      el.classList.remove("drag-over");
      const targetNodeId = el.dataset.nodeId;
      if (!targetNodeId || !preset.graph) return;

      const file = Array.from(e.dataTransfer?.files ?? [])[0];
      if (!file) return;
      const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
      const resourceType = inferResourceTypeFromFile(file);
      if (targetNode && isNamOrCabIrNode(targetNode) && resourceType && nodeAcceptsResourceType(targetNode, resourceType)) {
        e.preventDefault();
        void handleNamIrFileDrop(file, targetNodeId);
      }
    });

    // Delete/Backspace does what the node's own × does: removes an effect, collapses a split at
    // its join mixer, and nothing on a splitter or mixer that has no ×, so half a split is never deleted.
    el.addEventListener("keydown", (e: KeyboardEvent) => {
      if (el.dataset.nodeId && (e.key === "Delete" || e.key === "Backspace")) {
        e.preventDefault();
        el.querySelector<HTMLElement>(".signal-node-delete")?.click();
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

initSignalPathAddMenu({ onAddScene: addSceneFromToolbar });

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
      showAddEffectDropdown(button as HTMLElement, (payload) =>
        sendAddEffectAtEdgeOrFallback(payload.effectType, edge, edge?.from ?? "__input__", fxItemNodeOptions(payload)));
    });
  });

  // The render that called this replaced every + button, including the one an open
  // effect chooser hangs from.
  reanchorAddEffectDropdown();
}

function handleResourceGroupDrop(
  payload: ResourceGroupDragPayload,
  targetNodeId: string | null,
  updateOnly: boolean,
  edge?: SignalPathEdgeRef | null,
): void {
  if (!payload.modelIds?.length) {
    return;
  }

  // The node is pointed at a blend of the dropped group. The blend it plays now is never
  // rewritten: other presets may play it too, and it may carry mappings made in the editor.
  // A blend already made from this group with the same models is reused, edits and all.
  const existing = findBlendForToneGroup(uiState.blendLibrary ?? [], payload.groupId, payload.modelIds);
  let blendId = existing?.id ?? "";
  const blendName = existing?.name ?? payload.title;

  if (!existing) {
    blendId = typeof crypto !== "undefined" && "randomUUID" in crypto
      ? crypto.randomUUID()
      : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const modelMappings = payload.modelMappings?.length
      ? payload.modelMappings
      : buildBlendModelMappingsFromIds(payload.modelIds, uiState.resourceLibrary);

    postMessage({
      type: "saveBlendDefinition",
      blend: {
        id: blendId,
        name: blendName,
        category: payload.category,
        models: modelMappings.map((mapping: BlendModelMapping) => mapping.id),
        modelMappings,
        blendMode: "interpolate",
        toneGroupId: payload.groupId || undefined,
        toneGroupTitle: payload.title || undefined,
      },
    });
  }

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
