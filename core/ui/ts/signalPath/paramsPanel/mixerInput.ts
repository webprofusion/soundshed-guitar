/**
 * Per-input level controls for a mixer node, built from the graph edges that
 * feed it.
 */

import { EffectGuids } from "../../effectGuids.js";
import { getNodeEffectInfo } from "../../presetV2.js";
import type { GraphNode, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { buildGraphMaps } from "../graph.js";

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
                         data-node-id="${escapeHtml(node.id)}" data-param-key="${escapeHtml(muteKey)}" ${muteValue ? "checked" : ""}>
                  <span class="toggle-slider"></span>
                </label>
                <span class="mixer-mute-label">${muteValue ? "Muted" : "Active"}</span>
              </div>
              <div class="mixer-input-controls">
                <div class="node-param-group mixer-param">
                  <span class="node-param-label">Level</span>
                  <div class="knob node-param-knob" 
                       data-node-id="${escapeHtml(node.id)}" 
                       data-param-key="${escapeHtml(levelKey)}"
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
                       data-node-id="${escapeHtml(node.id)}" 
                       data-param-key="${escapeHtml(panKey)}"
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
                       data-node-id="${escapeHtml(node.id)}" 
                       data-param-key="${escapeHtml(delayKey)}"
                       data-value="${delayValue}"
                       data-default="0"
                       data-min="0"
                       data-max="60"
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
