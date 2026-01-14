import { uiState } from "./state.js";
import type { Preset, GraphNode, GraphEdge, LibraryResource } from "./types.js";
import { postMessage } from "./bridge.js";
import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { sendAddSignalPathNode, sendAddSignalPathNodeOnEdge, type SignalPathEdgeRef } from "./fxSelector.js";

const signalPathNodesElement = document.getElementById("signal-path-nodes");
const signalPathPresetNameElement = document.getElementById("signal-path-preset-name");
const nodeParamsPanelElement = document.getElementById("node-params-panel");

// Drag-drop state
let draggedNodeId: string | null = null;
let dragOverNodeId: string | null = null;
let selectedNodeId: string | null = null;

const effectTypeIcons: Record<string, string> = {
  // Dynamics
  "dynamics_gate": "🚪",
  "compressor_vca": "📊",
  "compressor_opto": "💡",
  "compressor_fet": "⚡",
  
  // Amps
  "amp_nam": "🎸",
  "amp_clean": "🎺",
  "amp_crunch": "🎷",
  
  // Cabs
  "cab_ir": "🔊",
  "cab_simple": "📻",
  
  // EQ
  "eq_parametric": "🎚️",
  "eq_graphic": "📊",
  "eq_tilt": "↗️",
  
  // Modulation
  "chorus_analog": "🌊",
  "chorus_digital": "🌈",
  "flanger": "〰️",
  "phaser": "🌀",
  "tremolo": "📳",
  "vibrato": "🎭",
  
  // Delay
  "delay_digital": "⏱️",
  "delay_tape": "📼",
  "delay_analog": "🔄",
  
  // Reverb
  "reverb_room": "🏠",
  "reverb_hall": "🏛️",
  "reverb_plate": "📀",
  "reverb_spring": "🌸",
  "reverb_shimmer": "✨",
  
  // Utility
  "gain": "📢",
  "splitter": "↗️",
  "mixer": "🎛️",
};

function getNodeIcon(nodeType: string): string {
  return effectTypeIcons[nodeType] || "⚙️";
}

function getCategoryClass(category: string): string {
  const categoryMap: Record<string, string> = {
    "dynamics": "dynamics",
    "amp": "amp",
    "cab": "cab",
    "eq": "eq",
    "modulation": "modulation",
    "delay": "delay",
    "reverb": "reverb",
    "utility": "utility",
  };
  return categoryMap[category] || "utility";
}

export function renderSignalPathBar(): void {
  if (!signalPathNodesElement || !signalPathPresetNameElement) {
    return;
  }

  const activePreset = uiState.presets.find((p) => p.id === uiState.activePresetId);
  
  if (!activePreset) {
    signalPathPresetNameElement.textContent = "No preset selected";
    signalPathNodesElement.innerHTML = "";
    return;
  }

  signalPathPresetNameElement.textContent = activePreset.name || "Unnamed Preset";

  // Render graph-based signal path (supports parallel paths)
  if (activePreset.graph?.nodes) {
    renderGraphSignalPath(activePreset);
  } else {
    // Empty preset - show only input/output
    signalPathNodesElement.innerHTML = `
      <div class="signal-graph-container">
        <div class="signal-graph-row">
          <div class="signal-node input-node">
            <div class="node-icon">🎤</div>
            <div class="node-info">
              <div class="node-name">Input</div>
            </div>
          </div>
          <div class="signal-connector-wrapper">
            <div class="signal-connector"></div>
            <button class="signal-add-btn" 
                    data-insert-after="__input__"
                    title="Add Effect">
              <span class="add-icon">+</span>
            </button>
          </div>
          <div class="signal-node output-node">
            <div class="node-icon">🔈</div>
            <div class="node-info">
              <div class="node-name">Output</div>
            </div>
          </div>
        </div>
      </div>
    `;

    // Bind minimal handlers (legacy fallback uses insertAfter=__input__)
    bindAddButtonHandlers();
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
      ${showSplit ? `
        <button class="signal-split-btn"
                data-edge-from="${edge.from}"
                data-edge-to="${edge.to}"
                data-edge-from-port="${edge.fromPort}"
                data-edge-to-port="${edge.toPort}"
                data-edge-gain="${edge.gain}"
                title="Split to parallel paths">⤢</button>
      ` : ""}
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

  const visited = new Set<string>();

  const renderParallelForSplitter = (splitterId: string): { html: string; mixerId: string | null } => {
    const splitterNode = nodeById.get(splitterId);
    if (!splitterNode || splitterNode.type !== "splitter") {
      return { html: "", mixerId: null };
    }

    const outs = outgoing.get(splitterId) ?? [];
    if (outs.length < 2) {
      return { html: "", mixerId: null };
    }

    const findMixerIdForBranch = (startNodeId: string): string | null => {
      let currentId = startNodeId;
      const localVisited = new Set<string>();
      while (currentId && !localVisited.has(currentId)) {
        localVisited.add(currentId);
        const node = nodeById.get(currentId);
        if (node && node.type === "mixer") {
          return currentId;
        }
        const edge = pickPrimaryOutgoingEdge(outgoing, currentId);
        if (!edge) return null;
        if (edge.to === "__output__") return null;
        currentId = edge.to;
      }
      return null;
    };

    // Determine shared mixer id
    const mixerIds = outs
      .map((e) => (e.to === "__output__" ? null : findMixerIdForBranch(e.to)))
      .filter((id): id is string => Boolean(id));

    const mixerId = mixerIds.length ? mixerIds[0] : null;
    if (!mixerId || !mixerIds.every((id) => id === mixerId)) {
      // Unsupported/ambiguous topology - render as a linear edge fallback
      return { html: "", mixerId: null };
    }

    const renderBranch = (firstEdge: EdgeRef): string => {
      let html = "";
      let edge: EdgeRef | null = firstEdge;
      let guard = 0;
      while (edge && guard++ < 200) {
        html += renderConnectorWrapper(edge, { showSplit: true });

        if (edge.to === mixerId) {
          break;
        }

        const node = nodeById.get(edge.to);
        if (!node) {
          break;
        }

        html += renderNodeElement(node);
        if (node.type === "splitter") {
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

    const mixerNode = nodeById.get(mixerId);
    const mixerNodeHtml = mixerNode ? renderNodeElement(mixerNode) : "";

    const html = `
      <div class="parallel-container" data-splitter-id="${splitterId}" data-mixer-id="${mixerId}">
        <div class="parallel-split">
          <div class="split-icon">⤵️</div>
        </div>
        <div class="parallel-branches">
          ${branchesHtml}
        </div>
        <div class="parallel-join">
          <div class="join-icon">⤴️</div>
          <button class="parallel-collapse-btn" data-splitter-id="${splitterId}" data-mixer-id="${mixerId}" title="Collapse split (only if empty)">×</button>
        </div>
      </div>
      ${mixerNodeHtml}
    `;

    return { html, mixerId };
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
        if (node && node.type === "splitter") {
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
        <div class="signal-node input-node">
          <div class="node-icon">🎤</div>
          <div class="node-info">
            <div class="node-name">Input</div>
          </div>
        </div>
        ${segmentsHtml}
        <div class="signal-node output-node">
          <div class="node-icon">🔈</div>
          <div class="node-info">
            <div class="node-name">Output</div>
          </div>
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

function sendAddEffectAtEdgeOrFallback(effectType: string, edge: EdgeRef | null, fallbackInsertAfter: string): void {
  if (edge) {
    sendAddSignalPathNodeOnEdge(effectType, edge);
  } else {
    // Back-compat: linear chain insertion by node id
    sendAddSignalPathNode(effectType, fallbackInsertAfter);
  }
}

/**
 * Renders a single effect node.
 */
function renderNodeElement(node: GraphNode): string {
  const icon = getNodeIcon(node.type);
  const categoryClass = getCategoryClass(node.category);
  const bypassedClass = node.bypassed ? "bypassed" : "";
  const selectedClass = selectedNodeId === node.id ? "selected" : "";
  
  let resourceLabel = "";
  if (node.resource) {
    resourceLabel = `<div class="node-resource">${node.resource.id || "Custom"}</div>`;
  }

  return `
    <div class="signal-node ${categoryClass} ${bypassedClass} ${selectedClass}" 
         data-node-id="${node.id}" 
         draggable="true" 
         tabindex="0">
      <div class="node-icon">${icon}</div>
      <div class="node-info">
        <div class="node-name">${node.displayName}</div>
        <div class="node-type">${node.type}</div>
        ${resourceLabel}
      </div>
      ${node.bypassed ? '<div class="node-bypass-badge">OFF</div>' : ""}
    </div>
  `;
}

function bindNodeClickHandlers(preset: Preset): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

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
      
      if (nodeId && (nodeId !== draggedNodeId || fxEffectType)) {
        dragOverNodeId = nodeId;
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = fxEffectType ? "copy" : "move";
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
      
      if (fxEffectType && targetNodeId && preset.graph) {
        // Dropping FX library item onto existing node - replace if same category
        const targetNode = preset.graph.nodes.find((n) => n.id === targetNodeId);
        const effectTypeInfo = EffectTypeRegistry.get(fxEffectType);
        
        if (targetNode && effectTypeInfo) {
          if (targetNode.category === effectTypeInfo.category) {
            // Same category - replace the node
            sendReplaceSignalPathNode(targetNodeId, fxEffectType);
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
      
      if (fxEffectType) {
        el.classList.add("drag-over");
        if (e.dataTransfer) {
          e.dataTransfer.dropEffect = "copy";
        }
      }
    });
    
    // Drag leave
    el.addEventListener("dragleave", () => {
      el.classList.remove("drag-over");
    });
    
    // Drop
    el.addEventListener("drop", (e: DragEvent) => {
      e.preventDefault();
      const fxEffectType = e.dataTransfer?.getData("application/x-fx-effect");

      const edge = parseEdgeFromDataset(el);
      if (fxEffectType && preset.graph) {
        sendAddEffectAtEdgeOrFallback(fxEffectType, edge, "__input__");
      }
      
      el.classList.remove("drag-over");
    });
  });
}

function bindSplitAndCollapseHandlers(): void {
  // Split edge button
  const splitButtons = signalPathNodesElement?.querySelectorAll(".signal-split-btn");
  splitButtons?.forEach((btn) => {
    btn.addEventListener("click", (e: Event) => {
      e.preventDefault();
      e.stopPropagation();
      const edge = parseEdgeFromDataset(btn as HTMLElement);
      if (edge) {
        sendSplitSignalPathEdge(edge);
      }
    });
  });

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
  
  // Get parameter definitions from registry
  const typeInfo = EffectTypeRegistry.get(node.type);
  const paramDefs = typeInfo?.parameters || [];
  
  const paramControls = Object.entries(node.params).map(([key, value]) => {
    const paramId = `${node.id}_${key}`;
    const paramDef = paramDefs.find(p => p.key === key);
    const label = paramDef?.name || formatParamLabel(key);
    const min = paramDef?.min ?? 0;
    const max = paramDef?.max ?? 1;
    const step = (max - min) / 100;
    const unit = paramDef?.unit || "";
    
    // Calculate initial rotation based on value (range: -135 to 135 degrees)
    const normalizedValue = (value - min) / (max - min);
    const rotation = -135 + normalizedValue * 270;
    
    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div 
          class="node-param-knob" 
          data-node-id="${node.id}" 
          data-param-key="${key}"
          data-value="${value}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
        >
          <div class="knob-indicator" style="transform: translateX(-50%) rotate(${rotation}deg);"></div>
        </div>
        <span class="node-param-value">${value.toFixed(2)}${unit}</span>
      </div>
    `;
  }).join("");

  const bypassButton = `
    <button 
      class="node-bypass-btn ${node.bypassed ? "bypassed" : ""}" 
      data-node-id="${node.id}"
    >
      ${node.bypassed ? "Enable" : "Bypass"}
    </button>
  `;

  // Build resource selector if this node type requires a resource
  let resourceSelector = "";
  if (typeInfo?.requiresResource && typeInfo.resourceType) {
    const resourceType = typeInfo.resourceType;
    const resources = uiState.resourceLibrary[resourceType] || [];
    const currentResourceId = node.resource?.id || "";
    const currentFilePath = node.resource?.filePath || "";
    
    const resourceOptions = resources.map((res: LibraryResource) => {
      const selected = res.id === currentResourceId ? "selected" : "";
      return `<option value="${res.id}" ${selected}>${res.name}</option>`;
    }).join("");
    
    const resourceLabel = resourceType === "nam" ? "Model" : resourceType === "ir" ? "IR" : "Resource";
    const browseAccept = resourceType === "nam" ? ".nam,.json" : resourceType === "ir" ? ".wav" : "*";
    
    resourceSelector = `
      <div class="node-resource-selector">
        <label>${resourceLabel}</label>
        <div class="resource-controls">
          <select 
            class="resource-dropdown" 
            data-node-id="${node.id}" 
            data-resource-type="${resourceType}"
          >
            <option value="">-- Select from Library --</option>
            ${resourceOptions}
            ${currentFilePath ? `<option value="__custom__" selected>Custom: ${currentFilePath.split("/").pop()}</option>` : ""}
          </select>
          <button 
            class="resource-browse-btn" 
            data-node-id="${node.id}" 
            data-resource-type="${resourceType}"
            data-accept="${browseAccept}"
            title="Browse for file..."
          >📁</button>
        </div>
        ${currentFilePath ? `<div class="resource-path-info" title="${currentFilePath}">${currentFilePath}</div>` : ""}
      </div>
    `;
  }

  nodeParamsPanelElement.innerHTML = `
    <div class="node-params-header">
      <div class="node-params-title">
        <span class="node-icon">${getNodeIcon(node.type)}</span>
        <span>${node.displayName}</span>
      </div>
      <button class="close-params-btn">×</button>
    </div>
    <div class="node-params-body">
      ${resourceSelector}
      <div class="params-controls">
        ${paramControls}
      </div>
      <div class="node-actions">
        ${bypassButton}
      </div>
    </div>
  `;

  // Bind controls
  bindNodeParamControls(node, preset);
  bindResourceControls(node, preset);
  bindCloseButton();
  bindBypassButton(node, preset);
}

function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function bindNodeParamControls(node: GraphNode, preset: Preset): void {
  const knobs = nodeParamsPanelElement?.querySelectorAll(".node-param-knob");
  if (!knobs) {
    return;
  }

  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const valueDisplay = knob.parentElement?.querySelector(".node-param-value") as HTMLElement | null;
    const indicator = knob.querySelector(".knob-indicator") as HTMLElement | null;
    
    const nodeId = knob.dataset.nodeId;
    const paramKey = knob.dataset.paramKey;
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    const unit = knob.dataset.unit || "";
    let currentValue = parseFloat(knob.dataset.value || "0");
    
    let isDragging = false;
    let startY = 0;
    let startValue = 0;
    const sensitivity = (max - min) / 200; // Adjust sensitivity based on range

    const updateKnobDisplay = (value: number) => {
      const normalizedValue = (value - min) / (max - min);
      const rotation = -135 + normalizedValue * 270;
      if (indicator) {
        indicator.style.transform = `translateX(-50%) rotate(${rotation}deg)`;
      }
      if (valueDisplay) {
        valueDisplay.textContent = `${value.toFixed(2)}${unit}`;
      }
      knob.dataset.value = value.toString();
    };

    const onMouseDown = (e: MouseEvent) => {
      isDragging = true;
      startY = e.clientY;
      startValue = currentValue;
      e.preventDefault();
      document.body.style.cursor = "ns-resize";
    };

    const onMouseMove = (e: MouseEvent) => {
      if (!isDragging) return;
      
      const deltaY = startY - e.clientY;
      let newValue = startValue + deltaY * sensitivity;
      newValue = Math.max(min, Math.min(max, newValue));
      
      currentValue = newValue;
      updateKnobDisplay(newValue);
      
      // Send parameter value while dragging
      if (nodeId && paramKey) {
        sendSignalPathNodeParamUpdate(nodeId, paramKey, newValue);
      }
    };

    const onMouseUp = () => {
      if (isDragging) {
        isDragging = false;
        document.body.style.cursor = "";
      }
    };

    knob.addEventListener("mousedown", onMouseDown);
    document.addEventListener("mousemove", onMouseMove);
    document.addEventListener("mouseup", onMouseUp);
  });
}

function bindResourceControls(node: GraphNode, preset: Preset): void {
  // Bind resource dropdown
  const dropdown = nodeParamsPanelElement?.querySelector(".resource-dropdown") as HTMLSelectElement | null;
  if (dropdown) {
    dropdown.addEventListener("change", () => {
      const nodeId = dropdown.dataset.nodeId;
      const resourceType = dropdown.dataset.resourceType;
      const resourceId = dropdown.value;
      
      if (nodeId && resourceType && resourceId && resourceId !== "__custom__") {
        sendNodeResourceUpdate(nodeId, resourceType, resourceId, "");
      }
    });
  }
  
  // Bind browse button
  const browseBtn = nodeParamsPanelElement?.querySelector(".resource-browse-btn") as HTMLButtonElement | null;
  if (browseBtn) {
    browseBtn.addEventListener("click", () => {
      const nodeId = browseBtn.dataset.nodeId;
      const resourceType = browseBtn.dataset.resourceType;
      
      if (nodeId && resourceType) {
        sendBrowseNodeResource(nodeId, resourceType);
      }
    });
  }
}

function bindCloseButton(): void {
  const closeBtn = nodeParamsPanelElement?.querySelector(".close-params-btn");
  if (closeBtn) {
    closeBtn.addEventListener("click", () => {
      nodeParamsPanelElement?.classList.remove("visible");
      
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
      const newBypassState = !node.bypassed;
      sendSignalPathNodeBypassUpdate(node.id, newBypassState);
      
      // Update UI
      node.bypassed = newBypassState;
      renderSignalPathBar();
      showNodeParamsPanel(node, preset);
    });
  }
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

function sendNodeResourceUpdate(nodeId: string, resourceType: string, resourceId: string, filePath: string): void {
  postMessage({
    type: "updateNodeResource",
    nodeId,
    resourceType,
    resourceId,
    filePath,
  });
}

function sendBrowseNodeResource(nodeId: string, resourceType: string): void {
  postMessage({
    type: "browseNodeResource",
    nodeId,
    resourceType,
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

function sendReplaceSignalPathNode(nodeId: string, newEffectType: string): void {
  postMessage({
    type: "replaceSignalPathNode",
    nodeId,
    newEffectType,
  });
}

function sendSplitSignalPathEdge(edge: SignalPathEdgeRef): void {
  postMessage({
    type: "splitSignalPathEdge",
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
    const effects = effectsByCategory.get(categoryId);
    if (effects && effects.length > 0) {
      const categoryInfo = FX_CATEGORIES.find(c => c.id === categoryId);
      dropdownHtml += `
        <div class="effect-dropdown-category">
          <div class="effect-dropdown-category-name">
            ${categoryInfo?.icon || ''} ${categoryInfo?.name || categoryId}
          </div>
          ${effects.map(effect => `
            <div class="effect-dropdown-item" data-effect-type="${effect.type}">
              <span class="effect-dropdown-icon">${getNodeIcon(effect.type)}</span>
              <span class="effect-dropdown-name">${effect.displayName}</span>
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
      if (effectType) {
        sendAddEffectAtEdgeOrFallback(effectType, edge, edge?.from ?? "__input__");
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
  { id: "dynamics", name: "Dynamics", icon: "⚡", color: "#e04848" },
  { id: "amp", name: "Amplifiers", icon: "🎸", color: "#e07848" },
  { id: "cab", name: "Cabinets", icon: "🔊", color: "#a86830" },
  { id: "eq", name: "Equalizers", icon: "🎚️", color: "#48a8e0" },
  { id: "modulation", name: "Modulation", icon: "🌊", color: "#9048e0" },
  { id: "delay", name: "Delay", icon: "⏱️", color: "#48e0a8" },
  { id: "reverb", name: "Reverb", icon: "🏛️", color: "#4878e0" },
  { id: "utility", name: "Utility", icon: "🔧", color: "#808080" },
];

