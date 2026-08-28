import type {
  GraphNode,
  Preset,
} from "../types.js";
import { EffectGuids } from "../effectGuids.js";
import { isNodeBypassed } from "../graphNodes.js";
import {
  getSelectedNodeId,
  nodeParamsPanelElement,
  signalPathNodesElement,
} from "./state.js";
import { sendSignalPathNodeBypassUpdate } from "./commands.js";
import { requestSignalPathRender } from "./render.js";
export function applySignalPathNodeBypassState(node: GraphNode, preset: Preset, bypassed: boolean): void {
  sendSignalPathNodeBypassUpdate(node.id, preset.id, bypassed);
  (node as unknown as { bypassed?: boolean }).bypassed = bypassed;
  (node as unknown as { enabled?: boolean }).enabled = !bypassed;
  requestSignalPathRender();
  if (getSelectedNodeId() === node.id && nodeParamsPanelElement?.classList.contains("visible")) {
    updateSelectedNodeBypassControl(bypassed);
  }
  if (getSelectedNodeId() === node.id) {
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

export function toggleSignalPathNodeBypass(node: GraphNode, preset: Preset): void {
  applySignalPathNodeBypassState(node, preset, !isNodeBypassed(node));
}

export function isProtectedSignalPathNode(node: GraphNode): boolean {
  return node.type === EffectGuids.kSplitter || node.type === EffectGuids.kMixer;
}

export function isToggleableSignalPathNode(node: GraphNode | null | undefined): node is GraphNode {
  if (!node) {
    return false;
  }

  return !isProtectedSignalPathNode(node);
}
