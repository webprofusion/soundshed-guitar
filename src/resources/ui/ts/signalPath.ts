import { uiState } from "./state.js";
import type { Preset, GraphNode, GraphEdge } from "./types.js";
import { postMessage } from "./bridge.js";
import { EffectTypeRegistry } from "./presetV2.js";

const signalPathNodesElement = document.getElementById("signal-path-nodes");
const signalPathPresetNameElement = document.getElementById("signal-path-preset-name");
const nodeParamsPanelElement = document.getElementById("node-params-panel");

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

  // Check if V2 format with graph
  if (activePreset.formatVersion === 2 && activePreset.graph?.nodes) {
    renderV2SignalPath(activePreset);
  } else {
    // V1 format or no graph
    renderV1SignalPath(activePreset);
  }
}

function renderV2SignalPath(preset: Preset): void {
  if (!signalPathNodesElement || !preset.graph) {
    return;
  }

  const nodes = preset.graph.nodes;
  const edges = preset.graph.edges;

  // Build execution order from graph
  const executionOrder = buildExecutionOrder(nodes, edges);

  const nodeElements = executionOrder.map((node) => {
    const icon = getNodeIcon(node.type);
    const categoryClass = getCategoryClass(node.category);
    const bypassedClass = node.bypassed ? "bypassed" : "";
    
    let resourceLabel = "";
    if (node.resource) {
      resourceLabel = `<div class="node-resource">${node.resource.id || "Custom"}</div>`;
    }

    return `
      <div class="signal-node ${categoryClass} ${bypassedClass}" data-node-id="${node.id}">
        <div class="node-icon">${icon}</div>
        <div class="node-info">
          <div class="node-name">${node.displayName}</div>
          <div class="node-type">${node.type}</div>
          ${resourceLabel}
        </div>
        ${node.bypassed ? '<div class="node-bypass-badge">Bypassed</div>' : ""}
      </div>
    `;
  }).join('<div class="signal-arrow">→</div>');

  signalPathNodesElement.innerHTML = `
    <div class="signal-node input">
      <div class="node-icon">🎤</div>
      <div class="node-info">
        <div class="node-name">Input</div>
      </div>
    </div>
    <div class="signal-arrow">→</div>
    ${nodeElements}
    <div class="signal-arrow">→</div>
    <div class="signal-node output">
      <div class="node-icon">🔈</div>
      <div class="node-info">
        <div class="node-name">Output</div>
      </div>
    </div>
  `;

  // Bind click handlers
  bindNodeClickHandlers(preset);
}

function renderV1SignalPath(preset: Preset): void {
  if (!signalPathNodesElement) {
    return;
  }

  // Legacy V1 format - show amp and cab
  const nodes: string[] = [];

  if (preset.audioFxModelId || preset.customModelPath) {
    nodes.push(`
      <div class="signal-node amp">
        <div class="node-icon">🎸</div>
        <div class="node-info">
          <div class="node-name">Amp</div>
          <div class="node-type">${preset.audioFxModelId || "Custom"}</div>
        </div>
      </div>
    `);
  }

  if (preset.irId || preset.customIrPath) {
    nodes.push(`
      <div class="signal-node cab">
        <div class="node-icon">🔊</div>
        <div class="node-info">
          <div class="node-name">Cabinet</div>
          <div class="node-type">${preset.irId || "Custom"}</div>
        </div>
      </div>
    `);
  }

  signalPathNodesElement.innerHTML = `
    <div class="signal-node input">
      <div class="node-icon">🎤</div>
      <div class="node-info">
        <div class="node-name">Input</div>
      </div>
    </div>
    ${nodes.length > 0 ? '<div class="signal-arrow">→</div>' + nodes.join('<div class="signal-arrow">→</div>') + '<div class="signal-arrow">→</div>' : ""}
    <div class="signal-node output">
      <div class="node-icon">🔈</div>
      <div class="node-info">
        <div class="node-name">Output</div>
      </div>
    </div>
  `;
}

function buildExecutionOrder(nodes: GraphNode[], edges: GraphEdge[]): GraphNode[] {
  // Simple topological sort for linear chains
  // For now, assume mostly linear signal flow
  const nodeMap = new Map(nodes.map((n) => [n.id, n]));
  const visited = new Set<string>();
  const order: GraphNode[] = [];

  function visit(nodeId: string): void {
    if (visited.has(nodeId) || nodeId === "__input__" || nodeId === "__output__") {
      return;
    }
    visited.add(nodeId);

    const node = nodeMap.get(nodeId);
    if (node) {
      order.push(node);
    }

    // Visit downstream nodes
    const outgoingEdges = edges.filter((e) => e.from === nodeId);
    outgoingEdges.forEach((edge) => visit(edge.to));
  }

  // Start from input
  const inputEdges = edges.filter((e) => e.from === "__input__");
  inputEdges.forEach((edge) => visit(edge.to));

  return order;
}

function bindNodeClickHandlers(preset: Preset): void {
  const nodeElements = signalPathNodesElement?.querySelectorAll(".signal-node[data-node-id]");
  if (!nodeElements) {
    return;
  }

  nodeElements.forEach((element) => {
    element.addEventListener("click", () => {
      const nodeId = (element as HTMLElement).dataset.nodeId;
      if (nodeId && preset.graph) {
        const node = preset.graph.nodes.find((n) => n.id === nodeId);
        if (node) {
          showNodeParamsPanel(node, preset);
          
          // Highlight selected node
          nodeElements.forEach((el) => el.classList.remove("selected"));
          element.classList.add("selected");
        }
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
    
    return `
      <div class="param-control">
        <label for="${paramId}">${label}</label>
        <div class="knob-container">
          <input 
            type="range" 
            id="${paramId}" 
            class="param-knob" 
            data-node-id="${node.id}" 
            data-param-key="${key}" 
            value="${value}" 
            min="${min}" 
            max="${max}" 
            step="${step}"
          />
          <div class="param-value">${value.toFixed(2)}${unit}</div>
        </div>
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

  nodeParamsPanelElement.innerHTML = `
    <div class="node-params-header">
      <div class="node-params-title">
        <span class="node-icon">${getNodeIcon(node.type)}</span>
        <span>${node.displayName}</span>
      </div>
      <button class="close-params-btn">×</button>
    </div>
    <div class="node-params-body">
      ${node.resource ? `
        <div class="node-resource-info">
          <strong>Resource:</strong> ${node.resource.id || "Custom"}
        </div>
      ` : ""}
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
  bindCloseButton();
  bindBypassButton(node, preset);
}

function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

function bindNodeParamControls(node: GraphNode, preset: Preset): void {
  const knobs = nodeParamsPanelElement?.querySelectorAll(".param-knob");
  if (!knobs) {
    return;
  }

  // Get parameter definitions from registry
  const typeInfo = EffectTypeRegistry.get(node.type);
  const paramDefs = typeInfo?.parameters || [];

  knobs.forEach((knob) => {
    const input = knob as HTMLInputElement;
    const valueDisplay = input.parentElement?.querySelector(".param-value");
    const paramKey = input.dataset.paramKey;
    const paramDef = paramDefs.find(p => p.key === paramKey);
    const unit = paramDef?.unit || "";
    
    input.addEventListener("input", () => {
      const value = parseFloat(input.value);
      if (valueDisplay) {
        valueDisplay.textContent = `${value.toFixed(2)}${unit}`;
      }
    });

    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const value = parseFloat(input.value);
      
      if (nodeId && paramKey) {
        sendNodeParamUpdate(nodeId, paramKey, value);
      }
    });
  });
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
      sendNodeBypassUpdate(node.id, newBypassState);
      
      // Update UI
      node.bypassed = newBypassState;
      renderSignalPathBar();
      showNodeParamsPanel(node, preset);
    });
  }
}

function sendNodeParamUpdate(nodeId: string, paramKey: string, value: number): void {
  postMessage({
    type: "updateNodeParam",
    nodeId,
    paramKey,
    value,
  });
}

function sendNodeBypassUpdate(nodeId: string, bypassed: boolean): void {
  postMessage({
    type: "updateNodeBypass",
    nodeId,
    bypassed,
  });
}
