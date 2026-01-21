import { uiState } from "./state.js";
import type { Preset, GraphNode, GraphEdge, LibraryResource, BlendModelMapping, BlendLibrary } from "./types.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { getBadgeIcon, getFxCategoryIcon, getFxEffectIcon, renderIcon } from "./iconAssets.js";
import { sendAddSignalPathNode, sendAddSignalPathNodeOnEdge, type SignalPathEdgeRef } from "./fxSelector.js";
import { GenericKnob } from "./controls.js";
import { buildBlendModelMappingsFromIds } from "./blendUtils.js";
import { BlendEditorModal } from "./blendEditor.js";

const signalPathNodesElement = document.getElementById("signal-path-nodes");
const nodeParamsPanelElement = document.getElementById("node-params-panel");
const effectVisualizationElement = document.getElementById("effect-visualization");
const effectVisualizationTitle = document.getElementById("effect-visualization-title");
const effectVisualizationSubtitle = document.getElementById("effect-visualization-subtitle");
const blendEditorModal = new BlendEditorModal({
  getBlendLibrary: () => uiState.blendLibrary ?? ([] as BlendLibrary),
  getResourceLibrary: () => uiState.resourceLibrary,
});

// Drag-drop state
let draggedNodeId: string | null = null;
let dragOverNodeId: string | null = null;
let selectedNodeId: string | null = null;

const DEFAULT_VISUALIZATION_TITLE = "Effect Visualization";
const DEFAULT_VISUALIZATION_SUBTITLE = "Select a node in the signal chain to edit parameters";
const EFFECT_VISUAL_BACKGROUNDS: Record<string, string> = {
  amp: "url('../images/equipment/amp-01.png')",
  cab: "url('../images/equipment/cab-01.png')",
  eq: "linear-gradient(145deg, rgba(56, 96, 132, 0.95) 0%, rgba(18, 24, 44, 0.95) 100%)",
  dynamics: "linear-gradient(145deg, rgba(132, 64, 64, 0.95) 0%, rgba(38, 18, 24, 0.95) 100%)",
  modulation: "linear-gradient(145deg, rgba(88, 64, 132, 0.95) 0%, rgba(26, 18, 44, 0.95) 100%)",
  delay: "linear-gradient(145deg, rgba(64, 132, 112, 0.95) 0%, rgba(18, 34, 38, 0.95) 100%)",
  reverb: "linear-gradient(145deg, rgba(64, 92, 132, 0.95) 0%, rgba(18, 24, 38, 0.95) 100%)",
  utility: "linear-gradient(145deg, rgba(86, 86, 96, 0.95) 0%, rgba(26, 26, 30, 0.95) 100%)",
};

function updateEffectVisualization(node?: GraphNode): void {
  if (!effectVisualizationElement) {
    return;
  }

  if (!node) {
    effectVisualizationElement.classList.remove("has-selection");
    effectVisualizationElement.style.removeProperty("--effect-visual-bg");
    effectVisualizationElement.dataset.effectType = "";
    effectVisualizationElement.dataset.effectCategory = "";
    if (effectVisualizationTitle) {
      effectVisualizationTitle.textContent = DEFAULT_VISUALIZATION_TITLE;
    }
    if (effectVisualizationSubtitle) {
      effectVisualizationSubtitle.textContent = DEFAULT_VISUALIZATION_SUBTITLE;
    }
    return;
  }

  const category = getNodeCategory(node);
  const typeInfo = EffectTypeRegistry.get(node.type);
  const displayName = getNodeDisplayName(node);
  const categoryLabel = (typeInfo?.category || category).toUpperCase();
  const background = EFFECT_VISUAL_BACKGROUNDS[category] || EFFECT_VISUAL_BACKGROUNDS.utility;

  effectVisualizationElement.classList.add("has-selection");
  effectVisualizationElement.style.setProperty("--effect-visual-bg", background);
  effectVisualizationElement.dataset.effectType = node.type;
  effectVisualizationElement.dataset.effectCategory = category;

  if (effectVisualizationTitle) {
    effectVisualizationTitle.textContent = displayName || DEFAULT_VISUALIZATION_TITLE;
  }
  if (effectVisualizationSubtitle) {
    effectVisualizationSubtitle.textContent = `${categoryLabel} · ${node.type}`;
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

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
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

function getNodeResourceDisplayName(node: GraphNode, index = 0): string {
  const typeInfo = EffectTypeRegistry.get(node.type);
  const resourceType = typeInfo?.resourceType;
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
  const typeInfo = nodeType ? EffectTypeRegistry.get(nodeType) : undefined;
  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (blendId) {
    const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
    if (blend?.name) {
      return blend.name;
    }
  }

  if (explicit && explicit !== (typeInfo?.displayName || "")) {
    return explicit;
  }

  const resourceTitle = typeInfo?.requiresResource ? getNodeResourceSummary(node) : "";
  if (resourceTitle) return resourceTitle;

  if (explicit) return explicit;
  return typeInfo?.displayName || nodeType || "(Unknown)";
}

function getNodeCategory(node: GraphNode): string {
  const anyNode = node as unknown as { category?: unknown; type?: unknown };
  const explicit = typeof anyNode.category === "string" ? anyNode.category : "";
  if (explicit) return explicit;
  const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";
  const typeInfo = nodeType ? EffectTypeRegistry.get(nodeType) : undefined;
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

function getNodeResourceAtIndex(node: GraphNode, index = 0): { id: string; filePath: string; parameterValue?: number } {
  const anyNode = node as unknown as {
    resource?: unknown;
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

  const res = anyNode.resource as { id?: unknown; resourceId?: unknown; embeddedId?: unknown; filePath?: unknown; parameterValue?: unknown } | undefined;
  const id = typeof res?.id === "string"
    ? res.id
    : (typeof res?.resourceId === "string"
      ? res.resourceId
      : (typeof res?.embeddedId === "string" ? res.embeddedId : ""));
  const filePath = typeof res?.filePath === "string" ? res.filePath : "";
  const parameterValue = typeof res?.parameterValue === "number" ? res.parameterValue : undefined;
  return { id, filePath, parameterValue };
}

export function renderSignalPathBar(): void {
  if (!signalPathNodesElement) {
    return;
  }

  const activePresetId = uiState.activePresetId;
  const activePreset = activePresetId
    ? (uiState.presetCache.get(activePresetId) ?? uiState.presets.find((p) => p.id === activePresetId))
    : undefined;
  
  if (!activePreset) {
    signalPathNodesElement.innerHTML = "";
    updateEffectVisualization();
    return;
  }

  // Render graph-based signal path (supports parallel paths)
  if (activePreset.graph?.nodes) {
    renderGraphSignalPath(activePreset);
  } else {
    // Empty preset - show only input/output
    signalPathNodesElement.innerHTML = `
      <div class="signal-graph-container">
        <div class="signal-graph-row">
          <div class="signal-node input-node" data-node-id="__input__">
            <div class="node-icon">🎤</div>
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
  const activePreset = activePresetId
    ? (uiState.presetCache.get(activePresetId) ?? uiState.presets.find((p) => p.id === activePresetId))
    : undefined;
  if (!activePreset?.graph) {
    return;
  }
  const node = activePreset.graph.nodes.find((n) => n.id === selectedNodeId);
  if (!node) {
    return;
  }
  showNodeParamsPanel(node, activePreset);
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
    const canCollapse = nodeById.get(splitterId)?.type === "splitter" && joinNode?.type === "mixer";

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
        if (node.type === "splitter" || isSplitPoint(node.id)) {
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
          <div class="split-icon">⤵️</div>
        </div>
        <div class="parallel-branches">
          ${branchesHtml}
        </div>
        <div class="parallel-join">
          <div class="join-icon">⤴️</div>
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
        if (node && (node.type === "splitter" || isSplitPoint(currentId))) {
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
          <div class="node-icon">🎤</div>
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
  options?: { config?: Record<string, string>; label?: string; category?: string },
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
  const allowDelete = node.type !== "splitter" && node.type !== "mixer";
  const displayName = getNodeDisplayName(node);
  const isCalibrating = uiState.namCalibrationStatus?.[node.id] === "calibrating";
  
  let resourceLabel = "";
  const resourceSummary = getNodeResourceSummary(node);
  if (resourceSummary) {
    resourceLabel = `<div class="node-resource">${resourceSummary}</div>`;
  }

  return `
    <div class="signal-node ${categoryClass} ${bypassedClass} ${selectedClass}" 
         data-node-id="${node.id}" 
         draggable="true" 
         tabindex="0">
      ${allowDelete ? '<button class="signal-node-delete" type="button" title="Remove" aria-label="Remove">×</button>' : ""}
      <div class="node-icon">${icon}</div>
      <div class="node-info">
        <div class="node-name">${displayName}</div>
        <div class="node-type">${node.type}</div>
        ${resourceLabel}
      </div>
      <span class="node-clip-indicator clip-inactive" aria-hidden="true"></span>
      ${isCalibrating ? '<div class="node-calibration-badge">CAL</div>' : ""}
      ${isNodeBypassed(node) ? '<div class="node-bypass-badge">OFF</div>' : ""}
    </div>
  `;
}

export function updateSignalPathClipIndicators(): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

  const diagnostics = uiState.signalDiagnostics;
  const enabled = Boolean(uiState.appSettings?.["diagnostics.signalLevelsEnabled"]);
  const activePresetId = uiState.activePresetId;

  const nodeClipMap = new Map<string, boolean>();
  if (enabled && diagnostics) {
    diagnostics.nodes.forEach((node) => {
      if (node.presetId && activePresetId && node.presetId !== activePresetId) {
        return;
      }
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

    if (!enabled || !diagnostics) {
      indicator.classList.add("clip-inactive");
      indicator.title = "Diagnostics disabled";
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
          selectedNodeId = nodeId;
          showNodeParamsPanel(node, preset);
          
          // Highlight selected node
          nodeElements.forEach((n) => n.classList.remove("selected"));
          el.classList.add("selected");
          el.focus();
        }
      }
    });
    
    // Drag start
    el.addEventListener("dragstart", (e: DragEvent) => {
      const nodeId = el.dataset.nodeId;
      if (nodeId) {
        draggedNodeId = nodeId;
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
      const nodeId = el.dataset.nodeId;
      
      // Check if dragging from FX library
      const fxEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-effect");
      const fxBlendType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-blend");
      const fxResourceGroup = Array.from(e.dataTransfer?.types ?? []).includes("application/x-resource-group");
      
      if (nodeId && (nodeId !== draggedNodeId || fxEffectType || fxBlendType || fxResourceGroup)) {
        dragOverNodeId = nodeId;
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = (fxEffectType || fxBlendType || fxResourceGroup) ? "copy" : "move";
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
      const targetNodeId = el.dataset.nodeId;
      
      // Check if dropping from FX library
      const fxEffectType = e.dataTransfer?.getData("application/x-fx-effect");
      const fxBlendId = e.dataTransfer?.getData("application/x-fx-blend");
      const fxBlendName = e.dataTransfer?.getData("application/x-fx-blend-name");
      const fxBlendCategory = e.dataTransfer?.getData("application/x-fx-blend-category");
      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      
      if (resourceGroupPayload && targetNodeId && preset.graph) {
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        if (targetNode && targetNode.type === "amp_nam_blend") {
          handleResourceGroupDrop(resourceGroupPayload, targetNodeId, true);
        } else {
          handleResourceGroupDrop(resourceGroupPayload, targetNodeId, false);
        }
      } else if ((fxEffectType || fxBlendId) && targetNodeId && preset.graph) {
        const resolvedType = fxEffectType || "amp_nam_blend";
        // Dropping FX library item onto existing node - replace if same category
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        const effectTypeInfo = EffectTypeRegistry.get(resolvedType);
        
        if (targetNode && effectTypeInfo) {
          if (targetNode.category === effectTypeInfo.category) {
            // Same category - replace the node
            sendReplaceSignalPathNode(targetNodeId, resolvedType, {
              config: fxBlendId ? { blendId: fxBlendId } : undefined,
              label: fxBlendName || undefined,
              category: fxBlendCategory || undefined,
            });
          }
          // Different category - ignore the drop (could show a message)
        }
      } else if (draggedNodeId && targetNodeId && draggedNodeId !== targetNodeId) {
        // Reordering existing nodes
        const draggedNode = getGraphNode(draggedNodeId);
        const targetNode = getGraphNode(targetNodeId);
        const blockedTypes = new Set(["splitter", "mixer"]);
        if (draggedNode && targetNode && !blockedTypes.has(draggedNode.type) && !blockedTypes.has(targetNode.type)) {
          sendSignalPathNodeReorder(draggedNodeId, targetNodeId);
        }
      }
      
      el.classList.remove("drag-over");
    });
    
    // Drag end
    el.addEventListener("dragend", () => {
      el.classList.remove("dragging");
      draggedNodeId = null;
      dragOverNodeId = null;
      // Clean up any remaining drag-over states
      nodeElements.forEach((n) => n.classList.remove("drag-over"));
    });
    
    // Keyboard handler - Delete/Backspace to remove
    el.addEventListener("keydown", (e: KeyboardEvent) => {
      const nodeId = el.dataset.nodeId;
      if (nodeId && (e.key === "Delete" || e.key === "Backspace")) {
        e.preventDefault();

        const node = getGraphNode(nodeId);
        if (node && (node.type === "splitter" || node.type === "mixer")) {
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
      
      // Only accept drops from FX library
      const fxEffectType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-effect");
      const fxBlendType = Array.from(e.dataTransfer?.types ?? []).includes("application/x-fx-blend");
      const fxResourceGroup = Array.from(e.dataTransfer?.types ?? []).includes("application/x-resource-group");
      const signalNodeId = e.dataTransfer?.getData("application/x-signal-node") || "";
      const isSignalNode = Boolean(signalNodeId);
      
      if (fxEffectType || fxBlendType || fxResourceGroup || isSignalNode) {
        const connector = el.querySelector(".signal-connector") as HTMLElement | null;
        connector?.classList.add("drag-over");
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = (fxEffectType || fxBlendType || fxResourceGroup) ? "copy" : "move";
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
      const fxEffectType = e.dataTransfer?.getData("application/x-fx-effect");
      const fxBlendId = e.dataTransfer?.getData("application/x-fx-blend");
      const fxBlendName = e.dataTransfer?.getData("application/x-fx-blend-name");
      const fxBlendCategory = e.dataTransfer?.getData("application/x-fx-blend-category");
      const resourceGroupPayload = e.dataTransfer?.getData("application/x-resource-group");
      const signalNodeId = e.dataTransfer?.getData("application/x-signal-node");

      const edge = parseEdgeFromDataset(el);
      if (resourceGroupPayload && preset.graph) {
        handleResourceGroupDrop(resourceGroupPayload, null, false, edge);
      } else if ((fxEffectType || fxBlendId) && preset.graph) {
        const resolvedType = fxEffectType || "amp_nam_blend";
        sendAddEffectAtEdgeOrFallback(resolvedType, edge, "__input__", {
          config: fxBlendId ? { blendId: fxBlendId } : undefined,
          label: fxBlendName || undefined,
          category: fxBlendCategory || undefined,
        });
      } else if (signalNodeId && edge && preset.graph) {
        const node = preset.graph.nodes.find((n) => n.id === signalNodeId);
        if (node && node.type !== "splitter" && node.type !== "mixer") {
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

  nodeParamsPanelElement.classList.add("visible");
  updateEffectVisualization(node);
  
  // Get parameter definitions from registry
  const typeInfo = EffectTypeRegistry.get(node.type);
  const paramDefs = typeInfo?.parameters || [];
  
  const paramControls = paramDefs.map((paramDef) => {
    const key = paramDef.key;
    const rawValue = node.params[key];
    const value = typeof rawValue === "number" ? rawValue : paramDef.default;
    const label = paramDef.name || formatParamLabel(key);
    const min = paramDef.min ?? 0;
    const max = paramDef.max ?? 1;
    const unit = paramDef.unit || "amount";
    const defaultValue = paramDef.default ?? value;
    const isToggle = isToggleParam(paramDef);
    const step = typeof paramDef.step === "number" ? paramDef.step : undefined;
    const enumLabels = Array.isArray(paramDef.labels) ? paramDef.labels : [];
    const isEnum = unit === "enum" && enumLabels.length > 0;
    const labelIndex = Math.round(Math.max(min, Math.min(max, value)));
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

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div 
          class="knob node-param-knob" 
          data-node-id="${node.id}" 
          data-param-key="${key}"
          data-value="${value}"
          data-default="${defaultValue}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
          ${step !== undefined ? `data-step="${step}"` : ""}
          ${isEnum ? `data-labels="${enumLabels.join("|")}"` : ""}
        >
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${isEnum ? enumValueLabel : `${value.toFixed(2)}${unit}`}</span>
        ${isEnum ? `
          <div class="node-param-steps">
            ${enumLabels.map((text, idx) => `
              <span class="node-param-step" data-step-index="${idx}">${text}</span>
            `).join("")}
          </div>
        ` : ""}
      </div>
    `;
  }).join("");

  const isEqNode = typeInfo?.category === "eq" || node.type.startsWith("eq_");
  const eqVisualizer = isEqNode ? `
    <div class="eq-visualizer" data-node-id="${node.id}">
      <div class="eq-visualizer-header">
        <span>EQ Curve</span>
        <span class="eq-visualizer-range">±18 dB</span>
      </div>
      <canvas class="eq-curve-canvas" data-node-id="${node.id}"></canvas>
    </div>
  ` : "";

  const bypassed = isNodeBypassed(node);
  const bypassButton = `
    <button 
      class="node-bypass-btn ${bypassed ? "bypassed" : ""}" 
      data-node-id="${node.id}"
    >
      ${bypassed ? "Enable" : "Bypass"}
    </button>
  `;

  const canRecalibrate = node.type === "amp_nam" || node.type === "amp_nam_optimized";
  const recalibrateButton = canRecalibrate
    ? `
      <button class="node-calibrate-btn" data-node-id="${node.id}">Recalibrate</button>
    `
    : "";

  // Build resource selector if this node type requires a resource
  let resourceSelector = "";
  if (typeInfo?.requiresResource && typeInfo.resourceType) {
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

    const buildSelector = (index: number, label: string) => {
      const current = getNodeResourceAtIndex(node, index);
      const resourceOptions = buildOptions(current.id);
      const customOption = current.filePath
        ? `<option value="__custom__" selected>Custom: ${current.filePath.split("/").pop()}</option>`
        : "";

      return `
        <div class="node-resource-selector">
          <label>${label}</label>
          <div class="resource-controls">
            <select
              class="resource-dropdown"
              data-node-id="${node.id}"
              data-resource-type="${resourceType}"
              data-resource-index="${index}"
            >
              <option value="">-- Select from Library --</option>
              ${resourceOptions}
              ${customOption}
            </select>
            <button
              class="resource-browse-btn"
              data-node-id="${node.id}"
              data-resource-type="${resourceType}"
              data-resource-index="${index}"
              data-accept="${browseAccept}"
              title="Browse for file..."
            >${renderIcon("folder", "resource-browse-icon")}</button>
          </div>
          ${current.filePath ? `<div class="resource-path-info" title="${current.filePath}">${current.filePath}</div>` : ""}
        </div>
      `;
    };

      if (node.type === "amp_nam_blend") {
        const items = (node as unknown as { resources?: unknown[] }).resources ?? [];
        const modelSelectors = items.length ? items.map((_, index) => {
          const paramValue = getNodeResourceAtIndex(node, index).parameterValue ?? index;
          return `
            ${buildSelector(index, `Model ${index + 1}`)}
            <div class="node-resource-meta">
              <label>Model ${index + 1} Value</label>
              <input class="resource-param-value" type="number" step="0.1" data-node-id="${node.id}" data-resource-index="${index}" value="${paramValue}" />
            </div>
          `;
        }).join("") : `
          ${buildSelector(0, "Model 1")}
          <div class="node-resource-meta">
            <label>Model 1 Value</label>
            <input class="resource-param-value" type="number" step="0.1" data-node-id="${node.id}" data-resource-index="0" value="0" />
          </div>
        `;
        resourceSelector = modelSelectors;
      } else {
        resourceSelector = buildSelector(0, resourceType === "nam" ? "Model" : resourceType === "ir" ? "IR" : "Resource");
      }
    }
  }

  nodeParamsPanelElement.innerHTML = `
    
    <div class="node-params-body">
      ${resourceSelector}
      ${eqVisualizer}
      <div class="params-controls">
        ${paramControls}
      </div>
      <div class="node-actions">
        ${bypassButton}
        ${recalibrateButton}
      </div>
    </div>
  `;

  if (isEqNode) {
    updateEqVisualization(node);
  }

  // Bind controls
  bindNodeParamControls(node, preset);
  bindResourceControls(node, preset);
  bindBlendEditorControls(node);
  bindCloseButton();
  bindBypassButton(node, preset);
  bindCalibrationButton(node);
}

function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function isToggleParam(paramDef: { key: string; min?: number; max?: number; unit?: string }): boolean {
  return paramDef.unit==="toggle";
}

function bindNodeParamControls(node: GraphNode, preset: Preset): void {
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
        const valueLabel = input.closest(".node-param-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueLabel) {
          valueLabel.textContent = input.checked ? "On" : "Off";
        }
        updateEqVisualization(node);
      }
    });
  });

  const knobs = nodeParamsPanelElement?.querySelectorAll(".node-param-knob");
  if (!knobs) {
    return;
  }

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

    const snapValue = (rawValue: number): number => {
      if (!step || step <= 0) return rawValue;
      const snapped = Math.round((rawValue - min) / step) * step + min;
      return Math.max(min, Math.min(max, snapped));
    };

    const formatValue = (rawValue: number): string => {
      if (isEnum) {
        const index = Math.round(rawValue);
        return labels[index] ?? `${index}`;
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
      sensitivity,
      sendParameter: false,
      onValueChange: (value) => {
        if (!nodeId || !paramKey) return;
        const finalValue = snapValue(value);
        if (finalValue !== value) {
          knobInstance.setValue(finalValue);
        }
        node.params[paramKey] = finalValue;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, finalValue);
        updateEqVisualization(node);
      },
    });
  });
}

type EqBand = { freq: number; gainDb: number; q: number };

function updateEqVisualization(node: GraphNode): void {
  const typeInfo = EffectTypeRegistry.get(node.type);
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

  const bands = getEqBands(node, typeInfo);
  drawEqCurve(canvas, bands);
}

function getEqBands(node: GraphNode, typeInfo: EffectTypeInfo): EqBand[] {
  const getDefault = (key: string, fallback: number): number => {
    const def = typeInfo.parameters.find((param) => param.key === key)?.default;
    return typeof def === "number" ? def : fallback;
  };

  const readParam = (keys: string[], fallback: number): number => {
    for (const key of keys) {
      const value = node.params[key];
      if (typeof value === "number") {
        return value;
      }
    }
    return fallback;
  };

  const band0Gain = readParam(["band0_gain", "lowGain"], getDefault("band0_gain", 0));
  const band0Freq = readParam(["band0_freq", "lowFreq"], getDefault("band0_freq", 100));
  const band1Gain = readParam(["band1_gain", "lowMidGain"], getDefault("band1_gain", 0));
  const band1Freq = readParam(["band1_freq", "lowMidFreq"], getDefault("band1_freq", 400));
  const band1Q = readParam(["band1_q", "lowMidQ"], getDefault("band1_q", 1.0));
  const band2Gain = readParam(["band2_gain", "highMidGain"], getDefault("band2_gain", 0));
  const band2Freq = readParam(["band2_freq", "highMidFreq"], getDefault("band2_freq", 2000));
  const band2Q = readParam(["band2_q", "highMidQ"], getDefault("band2_q", 1.0));
  const band3Gain = readParam(["band3_gain", "highGain"], getDefault("band3_gain", 0));
  const band3Freq = readParam(["band3_freq", "highFreq"], getDefault("band3_freq", 8000));

  return [
    { freq: band0Freq, gainDb: band0Gain, q: 1.0 },
    { freq: band1Freq, gainDb: band1Gain, q: band1Q },
    { freq: band2Freq, gainDb: band2Gain, q: band2Q },
    { freq: band3Freq, gainDb: band3Gain, q: 1.0 },
  ];
}

function drawEqCurve(canvas: HTMLCanvasElement, bands: EqBand[]): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const width = canvas.width;
  const height = canvas.height;

  ctx.clearRect(0, 0, width, height);

  const padding = 8;
  const plotWidth = width - padding * 2;
  const plotHeight = height - padding * 2;
  const minDb = -18;
  const maxDb = 18;
  const minFreq = 20;
  const maxFreq = 20000;
  const sampleRate = 44100;

  ctx.strokeStyle = "rgba(255,255,255,0.08)";
  ctx.lineWidth = 1;
  const gridLines = 4;
  for (let i = 0; i <= gridLines; i += 1) {
    const y = padding + (plotHeight * i) / gridLines;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }

  const freqMarkers = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  freqMarkers.forEach((freq) => {
    const x = padding + plotWidth * (Math.log10(freq) - Math.log10(minFreq)) / (Math.log10(maxFreq) - Math.log10(minFreq));
    ctx.beginPath();
    ctx.moveTo(x, padding);
    ctx.lineTo(x, height - padding);
    ctx.stroke();
  });

  ctx.strokeStyle = "rgba(72, 168, 224, 0.9)";
  ctx.lineWidth = 2;
  ctx.beginPath();

  for (let i = 0; i <= plotWidth; i += 1) {
    const freq = minFreq * Math.pow(10, (i / plotWidth) * (Math.log10(maxFreq) - Math.log10(minFreq)));
    const magnitude = bands.reduce((acc, band) => acc * peakingMagnitude(freq, band, sampleRate), 1.0);
    const db = 20 * Math.log10(Math.max(1e-6, magnitude));
    const clampedDb = Math.max(minDb, Math.min(maxDb, db));
    const x = padding + i;
    const y = padding + (maxDb - clampedDb) / (maxDb - minDb) * plotHeight;
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
}

function peakingMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (!band || band.gainDb === 0 || band.freq <= 0) {
    return 1.0;
  }

  const w0 = 2 * Math.PI * band.freq / sampleRate;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const q = Math.max(0.1, band.q || 1.0);
  const A = Math.pow(10, band.gainDb / 40);
  const alpha = sinw0 / (2 * q);

  const b0 = 1 + alpha * A;
  const b1 = -2 * cosw0;
  const b2 = 1 - alpha * A;
  const a0 = 1 + alpha / A;
  const a1 = -2 * cosw0;
  const a2 = 1 - alpha / A;

  const w = 2 * Math.PI * freq / sampleRate;
  const cosw = Math.cos(w);
  const sinw = Math.sin(w);
  const cos2w = Math.cos(2 * w);
  const sin2w = Math.sin(2 * w);

  const numRe = b0 + b1 * cosw + b2 * cos2w;
  const numIm = b1 * -sinw + b2 * -sin2w;
  const denRe = a0 + a1 * cosw + a2 * cos2w;
  const denIm = a1 * -sinw + a2 * -sin2w;

  const numMag = Math.sqrt(numRe * numRe + numIm * numIm);
  const denMag = Math.sqrt(denRe * denRe + denIm * denIm);
  if (denMag <= 0) {
    return 1.0;
  }

  return numMag / denMag;
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
      
      if (nodeId && resourceType && resourceId && resourceId !== "__custom__") {
        sendNodeResourceUpdate(nodeId, resourceType, resourceId, "", resourceIndex);
      }
    });
  });
  
  // Bind browse buttons
  const browseBtns = nodeParamsPanelElement?.querySelectorAll(".resource-browse-btn") as NodeListOf<HTMLButtonElement> | null;
  browseBtns?.forEach((browseBtn) => {
    browseBtn.addEventListener("click", () => {
      const nodeId = browseBtn.dataset.nodeId;
      const resourceType = browseBtn.dataset.resourceType;
      const resourceIndex = browseBtn.dataset.resourceIndex ? parseInt(browseBtn.dataset.resourceIndex, 10) : undefined;
      
      if (nodeId && resourceType) {
        sendBrowseNodeResource(nodeId, resourceType, resourceIndex);
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

function bindBlendEditorControls(node: GraphNode): void {
  const blendId = (node as unknown as { config?: Record<string, string> }).config?.blendId;
  if (!blendId) {
    return;
  }

  const openButton = nodeParamsPanelElement?.querySelector(".blend-open-btn") as HTMLButtonElement | null;
  openButton?.addEventListener("click", () => {
    blendEditorModal.open(node);
  });
}

export function initializeBlendEditorModal(): void {
  blendEditorModal.initialize();
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
  const bypassBtn = nodeParamsPanelElement?.querySelector(".node-bypass-btn");
  if (bypassBtn) {
    bypassBtn.addEventListener("click", () => {
      const currentBypassed = isNodeBypassed(node);
      const newBypassState = !currentBypassed;
      sendSignalPathNodeBypassUpdate(node.id, newBypassState);
      
      // Update UI
      (node as unknown as { bypassed?: boolean }).bypassed = newBypassState;
      (node as unknown as { enabled?: boolean }).enabled = !newBypassState;
      renderSignalPathBar();
      showNodeParamsPanel(node, preset);
    });
  }
}

function bindCalibrationButton(node: GraphNode): void {
  const calibrateBtn = nodeParamsPanelElement?.querySelector(".node-calibrate-btn") as HTMLButtonElement | null;
  if (!calibrateBtn) {
    return;
  }

  calibrateBtn.addEventListener("click", () => {
    showNotification("Recalibration started", getNodeDisplayName(node));
    postMessage({
      type: "rerunNamCalibration",
      nodeId: node.id,
    });
  });
}

function sendSignalPathNodeParamUpdate(nodeId: string, paramKey: string, value: number): void {
  postMessage({
    type: "updateSignalPathNodeParam",
    nodeId,
    paramKey,
    value,
  });
}

function sendSignalPathNodeBypassUpdate(nodeId: string, bypassed: boolean): void {
  postMessage({
    type: "updateSignalPathNodeBypass",
    nodeId,
    bypassed,
  });
}

function sendNodeResourceUpdate(
  nodeId: string,
  resourceType: string,
  resourceId: string,
  filePath: string,
  resourceIndex?: number,
  parameterValue?: number,
): void {
  postMessage({
    type: "updateNodeResource",
    nodeId,
    resourceType,
    resourceId,
    filePath,
    resourceIndex,
    parameterValue,
  });
}

function sendBrowseNodeResource(nodeId: string, resourceType: string, resourceIndex?: number): void {
  postMessage({
    type: "browseNodeResource",
    nodeId,
    resourceType,
    resourceIndex,
  });
}

function sendSignalPathNodeReorder(nodeId: string, targetNodeId: string): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    targetNodeId,
  });
}

function sendSignalPathNodeDelete(nodeId: string): void {
  postMessage({
    type: "deleteSignalPathNode",
    nodeId,
  });
}

function sendReplaceSignalPathNode(
  nodeId: string,
  newEffectType: string,
  options?: { config?: Record<string, string>; label?: string; category?: string },
): void {
  postMessage({
    type: "replaceSignalPathNode",
    nodeId,
    newEffectType,
    config: options?.config,
    label: options?.label,
    category: options?.category,
  });
}

function sendMoveSignalPathNodeToEdge(nodeId: string, edge: SignalPathEdgeRef): void {
  postMessage({
    type: "reorderSignalPathNode",
    nodeId,
    edge,
  });
}

function sendCollapseParallelSplit(splitterId: string, mixerId: string): void {
  postMessage({
    type: "collapseSignalPathSplit",
    splitterId,
    mixerId,
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
  
  const allEffects = EffectTypeRegistry.getAll();
  const effectsByCategory = new Map<string, EffectTypeInfo[]>();
  
  allEffects.forEach((effect) => {
    if (!effectsByCategory.has(effect.category)) {
      effectsByCategory.set(effect.category, []);
    }
    effectsByCategory.get(effect.category)!.push(effect);
  });

  const categoryOrder = ["dynamics", "amp", "cab", "eq", "modulation", "delay", "reverb", "utility"];
  
  let dropdownHtml = '<div class="effect-dropdown-header">Add Effect</div>';
  
  categoryOrder.forEach((categoryId) => {
    const effects = effectsByCategory.get(categoryId) ?? [];
    const blendEntries = getBlendEntriesForCategory(categoryId);
    if (effects.length > 0 || blendEntries.length > 0) {
      const categoryInfo = FX_CATEGORIES.find(c => c.id === categoryId);
      dropdownHtml += `
        <div class="effect-dropdown-category">
          <div class="effect-dropdown-category-name">
            ${getFxCategoryIcon(categoryId)} ${categoryInfo?.name || categoryId}
          </div>
          ${effects.map(effect => `
            <div class="effect-dropdown-item" data-effect-type="${effect.type}">
              <span class="effect-dropdown-icon">${getNodeIcon(effect.type)}</span>
              <span class="effect-dropdown-name">${effect.displayName}</span>
            </div>
          `).join('')}
          ${blendEntries.map((blend) => `
            <div class="effect-dropdown-item" data-effect-type="amp_nam_blend" data-blend-id="${blend.id}" data-blend-name="${escapeHtml(blend.name)}" data-blend-category="${blend.originalCategory}">
              <span class="effect-dropdown-icon">${getBadgeIcon("blend", "Custom blend")}</span>
              <span class="effect-dropdown-name">${escapeHtml(blend.name)}</span>
            </div>
          `).join('')}
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
      if (effectType) {
        sendAddEffectAtEdgeOrFallback(effectType, edge, edge?.from ?? "__input__", {
          config: blendId ? { blendId } : undefined,
          label: blendName || undefined,
          category: blendCategory || undefined,
        });
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

const FX_CATEGORIES = [
  { id: "dynamics", name: "Dynamics", color: "#e04848" },
  { id: "amp", name: "Amplifiers", color: "#e07848" },
  { id: "cab", name: "Cabinets", color: "#a86830" },
  { id: "eq", name: "Equalizers", color: "#48a8e0" },
  { id: "modulation", name: "Modulation", color: "#9048e0" },
  { id: "delay", name: "Delay", color: "#48e0a8" },
  { id: "reverb", name: "Reverb", color: "#4878e0" },
  { id: "utility", name: "Utility", color: "#808080" },
];

function getBlendEntriesForCategory(categoryId: string): Array<{ id: string; name: string; category: string; originalCategory: string } > {
  const blends = uiState.blendLibrary ?? [];
  const mapCategory = (value: string): string => {
    switch (value) {
      case "cab":
        return "cab";
      case "pedal":
        return "utility";
      case "preamp":
      case "amp":
      case "full-rig":
      default:
        return "amp";
    }
  };

  return blends
    .map((blend) => ({
      id: blend.id,
      name: blend.name,
      category: mapCategory(blend.category),
      originalCategory: blend.category,
    }))
    .filter((blend) => blend.category === categoryId);
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
    ? (uiState.presetCache.get(uiState.activePresetId ?? "")?.graph?.nodes.find((n) => n.id === targetNodeId)?.config?.blendId ?? "")
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
      models: modelMappings.map((mapping) => mapping.id),
      modelMappings,
      blendMode: "interpolate",
    },
  });

  if (updateOnly && targetNodeId) {
    sendReplaceSignalPathNode(targetNodeId, "amp_nam_blend", {
      config: { blendId },
      label: blendName,
      category: payload.category,
    });
    return;
  }

  if (targetNodeId) {
    sendReplaceSignalPathNode(targetNodeId, "amp_nam_blend", {
      config: { blendId },
      label: blendName,
      category: payload.category,
    });
    return;
  }

  const normalizedEdge = edge ? { ...edge, gain: edge.gain ?? 1.0 } : null;
  sendAddEffectAtEdgeOrFallback("amp_nam_blend", normalizedEdge, edge?.from ?? "__input__", {
    config: { blendId },
    label: blendName,
    category: payload.category,
  });
}

