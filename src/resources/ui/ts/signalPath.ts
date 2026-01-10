import { uiState } from "./state.js";
import type { Preset, GraphNode, GraphEdge, LibraryResource } from "./types.js";
import { postMessage } from "./bridge.js";
import { EffectTypeRegistry, type EffectTypeInfo } from "./presetV2.js";
import { sendAddSignalPathNode } from "./fxSelector.js";

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
  }
}

/**
 * Represents a segment in the signal path graph layout.
 * A segment can be a single node or a parallel split.
 */
interface PathSegment {
  type: "node" | "parallel";
  node?: GraphNode;
  branches?: PathSegment[][];
}

/**
 * Builds the visual layout structure from the graph.
 * This analyzes the graph topology and creates a layout with parallel branches.
 */
function buildGraphLayout(nodes: GraphNode[], edges: GraphEdge[]): PathSegment[] {
  const nodeMap = new Map(nodes.map((n) => [n.id, n]));
  
  // Build adjacency lists
  const outgoing = new Map<string, GraphEdge[]>();
  const incoming = new Map<string, GraphEdge[]>();
  
  edges.forEach((edge) => {
    if (!outgoing.has(edge.from)) outgoing.set(edge.from, []);
    if (!incoming.has(edge.to)) incoming.set(edge.to, []);
    outgoing.get(edge.from)!.push(edge);
    incoming.get(edge.to)!.push(edge);
  });
  
  const visited = new Set<string>();
  const segments: PathSegment[] = [];
  
  /**
   * Traverse from a starting node, building path segments.
   * Returns the segments and the set of ending node IDs.
   */
  function traverse(startId: string): { segments: PathSegment[]; endIds: Set<string> } {
    const result: PathSegment[] = [];
    const endIds = new Set<string>();
    let currentId: string | null = startId;
    
    while (currentId && !visited.has(currentId)) {
      // Skip special IDs
      if (currentId === "__input__" || currentId === "__output__") {
        const outs: GraphEdge[] = outgoing.get(currentId) || [];
        if (outs.length === 1) {
          currentId = outs[0].to;
          continue;
        } else if (outs.length > 1) {
          // Split at input - handle as parallel branches
          const branches: PathSegment[][] = [];
          outs.forEach((edge: GraphEdge) => {
            if (edge.to !== "__output__") {
              const branchResult = traverse(edge.to);
              if (branchResult.segments.length > 0) {
                branches.push(branchResult.segments);
              }
              branchResult.endIds.forEach((id) => endIds.add(id));
            }
          });
          if (branches.length > 0) {
            result.push({ type: "parallel", branches });
          }
          currentId = null;
        } else {
          currentId = null;
        }
        continue;
      }
      
      visited.add(currentId);
      const node = nodeMap.get(currentId);
      
      if (node) {
        const outs: GraphEdge[] = outgoing.get(currentId) || [];
        
        // Check if this is a splitter (multiple outputs)
        if (outs.length > 1 && node.type !== "mixer") {
          result.push({ type: "node", node });
          
          // Create parallel branches
          const branches: PathSegment[][] = [];
          const branchEndIds = new Set<string>();
          
          outs.forEach((edge: GraphEdge) => {
            if (edge.to !== "__output__" && !visited.has(edge.to)) {
              const branchResult = traverse(edge.to);
              if (branchResult.segments.length > 0) {
                branches.push(branchResult.segments);
              }
              branchResult.endIds.forEach((id) => branchEndIds.add(id));
            }
          });
          
          if (branches.length > 0) {
            result.push({ type: "parallel", branches });
          }
          
          // Find convergence point (mixer)
          branchEndIds.forEach((id) => endIds.add(id));
          currentId = null;
        } else {
          // Single path - add node and continue
          result.push({ type: "node", node });
          
          if (outs.length === 1 && outs[0].to !== "__output__") {
            currentId = outs[0].to;
          } else {
            endIds.add(currentId);
            currentId = null;
          }
        }
      } else {
        currentId = null;
      }
    }
    
    return { segments: result, endIds };
  }
  
  // Start traversal from __input__
  const { segments: mainSegments } = traverse("__input__");
  return mainSegments;
}

/**
 * Renders the signal path graph with support for parallel branches.
 */
function renderGraphSignalPath(preset: Preset): void {
  if (!signalPathNodesElement || !preset.graph) {
    return;
  }

  const nodes = preset.graph.nodes;
  const edges = preset.graph.edges;

  // Build the layout structure
  const segments = buildGraphLayout(nodes, edges);
  
  // Render segments to HTML
  const segmentsHtml = renderSegments(segments);

  signalPathNodesElement.innerHTML = `
    <div class="signal-graph-container">
      <div class="signal-graph-row">
        <div class="signal-node input-node">
          <div class="node-icon">🎤</div>
          <div class="node-info">
            <div class="node-name">Input</div>
          </div>
        </div>
        ${segmentsHtml ? segmentsHtml : `
          <div class="signal-connector-wrapper">
            <div class="signal-connector"></div>
            <button class="signal-add-btn" 
                    data-insert-after="__input__"
                    title="Add Effect">
              <span class="add-icon">+</span>
            </button>
          </div>
        `}
        ${segmentsHtml}
        ${segmentsHtml ? '<div class="signal-connector"></div>' : ''}
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
}

/**
 * Renders path segments to HTML, handling parallel branches recursively.
 */
function renderSegments(segments: PathSegment[], prevNodeId: string = "__input__"): string {
  let html = '';
  let currentPrevId = prevNodeId;
  
  segments.forEach((segment, index) => {
    // Add connector with + button before each segment
    if (index > 0 || prevNodeId === "__input__") {
      html += `
        <div class="signal-connector-wrapper">
          <div class="signal-connector"></div>
          <button class="signal-add-btn" 
                  data-insert-after="${currentPrevId}"
                  title="Add Effect">
            <span class="add-icon">+</span>
          </button>
        </div>
      `;
    }
    
    if (segment.type === "node" && segment.node) {
      html += renderNodeElement(segment.node);
      currentPrevId = segment.node.id;
    } else if (segment.type === "parallel" && segment.branches) {
      html += renderParallelBranches(segment.branches);
    }
  });
  
  return html;
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

/**
 * Renders parallel branches with visual split/join indicators.
 */
function renderParallelBranches(branches: PathSegment[][]): string {
  if (branches.length === 0) return '';
  if (branches.length === 1) return renderSegments(branches[0]);
  
  const branchesHtml = branches.map((branch, index) => {
    const branchContent = renderSegments(branch);
    return `
      <div class="parallel-branch" data-branch-index="${index}">
        ${branchContent}
      </div>
    `;
  }).join('');
  
  return `
    <div class="parallel-container">
      <div class="parallel-split">
        <div class="split-icon">⤵️</div>
      </div>
      <div class="parallel-branches">
        ${branchesHtml}
      </div>
      <div class="parallel-join">
        <div class="join-icon">⤴️</div>
      </div>
    </div>
  `;
}

function bindNodeClickHandlers(preset: Preset): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

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
      const fxEffectType = e.dataTransfer?.types.includes("application/x-fx-effect");
      
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
        sendSignalPathNodeReorder(draggedNodeId, targetNodeId);
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
        sendSignalPathNodeDelete(nodeId);
        selectedNodeId = null;
        nodeParamsPanelElement?.classList.remove("visible");
      }
    });
  });
}

function bindConnectorDropHandlers(preset: Preset): void {
  const connectorElements = signalPathNodesElement?.querySelectorAll(".signal-connector");
  if (!connectorElements || !preset.graph) {
    return;
  }

  connectorElements.forEach((element) => {
    const el = element as HTMLElement;
    
    // Drag over
    el.addEventListener("dragover", (e: DragEvent) => {
      e.preventDefault();
      
      // Only accept drops from FX library
      const fxEffectType = e.dataTransfer?.types.includes("application/x-fx-effect");
      
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
      
      if (fxEffectType && preset.graph) {
        // Find the node that comes before this connector
        const prevNode = findNodeBeforeConnector(el, preset);
        const insertAfter = prevNode?.id || "__input__";
        
        // Insert the new effect
        sendAddSignalPathNode(fxEffectType, insertAfter);
      }
      
      el.classList.remove("drag-over");
    });
  });
}

function findNodeBeforeConnector(connectorEl: HTMLElement, preset: Preset): GraphNode | null {
  // Find the previous sibling that is a signal-node
  let prevSibling = connectorEl.previousElementSibling;
  
  while (prevSibling) {
    if (prevSibling.classList.contains("signal-node")) {
      const nodeId = (prevSibling as HTMLElement).dataset.nodeId;
      if (nodeId && preset.graph) {
        const node = preset.graph.nodes.find((n) => n.id === nodeId);
        if (node) {
          return node;
        }
      }
      // If it's the input node, return null (insert after __input__)
      if (prevSibling.classList.contains("input-node")) {
        return null;
      }
    }
    prevSibling = prevSibling.previousElementSibling;
  }
  
  return null;
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

/**
 * Bind click handlers for + buttons between nodes.
 */
function bindAddButtonHandlers(): void {
  const addButtons = signalPathNodesElement?.querySelectorAll(".signal-add-btn");
  if (!addButtons) return;

  addButtons.forEach((button) => {
    button.addEventListener("click", (e: Event) => {
      e.stopPropagation();
      const insertAfter = (button as HTMLElement).dataset.insertAfter;
      if (insertAfter) {
        showEffectSelectionDropdown(button as HTMLElement, insertAfter);
      }
    });
  });
}

/**
 * Show a dropdown menu to select an effect to add.
 */
function showEffectSelectionDropdown(buttonElement: HTMLElement, insertAfter: string): void {
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
        sendAddSignalPathNode(effectType, insertAfter);
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

