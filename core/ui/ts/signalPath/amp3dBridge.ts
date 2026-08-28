import type {
  GraphNode,
  Preset,
} from "../types.js";
import { escapeHtml } from "../utils.js";
import { EffectTypeRegistry, getNodeEffectInfo, type ParameterDef } from "../presetV2.js";
import { renderIcon } from "../iconAssets.js";
import { closeLayoutPicker } from "../layoutPicker.js";
import { themeSwitcher } from "../theme-switcher.js";
import { MAX_PANEL_KNOBS } from "../amp3d/ampLayout.js";
import {
  isSignalChain3dEnabled,
  isWebglSupported,
  setSignalChain3dEnabled,
} from "../amp3d/ampSupport.js";
import type {
  Amp3dKnobSpec,
  BuildChainLayoutOptions,
  Chain3dView,
  Chain3dViewOptions,
} from "../amp3d/index.js";
import { isNodeBypassed } from "../graphNodes.js";
import {
  getSelectedNodeId,
  nodeParamsPanelElement,
  setSelectedNodeId,
} from "./state.js";
import { sendSignalPathNodeParamUpdate } from "./commands.js";
import { getNodeCategory } from "./nodeTypes.js";
import { nodeUsesFullRigNamCategory } from "./chainRules.js";
import { formatParamLabel, isToggleParam, showNodeParamsPanel } from "./paramsPanel.js";
import { requestNodeParamsRefresh, requestSignalPathRender } from "./render.js";
import { getNodeResourceSummary } from "./nodeLabels.js";
import { toggleSignalPathNodeBypass } from "./bypass.js";

export let chain3dView: Chain3dView | null = null;

export let chain3dMountToken = 0;

/** True while the params panel is showing the chain stage for the current selection. */
export let chain3dPanelActive = false;

/**
 * Master switch for the experimental 3D chain stage.
 *
 * The stage is disabled: effect visualisation is served entirely by the standard
 * controls and custom layouts, which the user picks between from the effect
 * header (see `layoutPicker.ts`). The scene code in `amp3d/` is kept — it is only
 * reachable through a dynamic import — so the experiment can be revived by
 * flipping this flag, but nothing loads it while it is false.
 */
export const CHAIN_3D_VIEW_ENABLED = false;

export function canOfferChain3dView(): boolean {
  return CHAIN_3D_VIEW_ENABLED && isWebglSupported();
}

export function shouldRenderChain3dView(hasPositionedLayout: boolean): boolean {
  // Custom positioned layouts keep their 2D designer canvas; 3D mode is for the
  // default shell / generic controls path.
  return !hasPositionedLayout && canOfferChain3dView() && isSignalChain3dEnabled();
}

export interface Amp3dParamSplit {
  knobDefs: ParameterDef[];
  extraDefs: ParameterDef[];
}

/**
 * Continuous params become physical knobs on the 3D unit; everything else stays
 * as HTML controls in the floating dock so no control is lost.
 */
export function splitAmp3dParamDefs(paramDefs: ParameterDef[]): Amp3dParamSplit {
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

export function buildAmp3dKnobSpecs(node: GraphNode, knobDefs: ParameterDef[]): Amp3dKnobSpec[] {
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

export function continuousKnobDefsForNode(node: GraphNode): ParameterDef[] {
  const typeInfo = getNodeEffectInfo(node) ?? EffectTypeRegistry.get(node.type);
  const paramDefs = typeInfo?.parameters || [];
  return splitAmp3dParamDefs(paramDefs).knobDefs;
}

export function buildChain3dLayoutOptions(preset: Preset): BuildChainLayoutOptions {
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

export function buildChain3dViewOptions(preset: Preset, selectedId: string | null): Chain3dViewOptions {
  return {
    theme: themeSwitcher.getCurrentTheme(),
    layoutOptions: buildChain3dLayoutOptions(preset),
    selectedNodeId: selectedId,
    onSelectNode: (nodeId) => {
      if (!preset.graph) return;
      const target = preset.graph.nodes.find((n) => n.id === nodeId);
      if (!target) return;
      if (getSelectedNodeId() === target.id) {
        chain3dView?.focusNode(target.id, false);
        return;
      }
      setSelectedNodeId(target.id);
      requestSignalPathRender();
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

export function disposeChain3dView(): void {
  chain3dMountToken += 1;
  chain3dView?.dispose();
  chain3dView = null;
  chain3dPanelActive = false;
}

/** Hides the node params panel and releases any live 3D chain resources. */
export function hideNodeParamsPanel(): void {
  nodeParamsPanelElement?.classList.remove("visible");
  closeLayoutPicker();
  setAmp3dImmersiveMode(false);
  disposeChain3dView();
}

export const AMP3D_DOCK_COLLAPSED_KEY = "guitarfx.amp3dDockCollapsed";

/**
 * Chain stage markup: one WebGL viewport for the whole graph plus a floating
 * dock with the selected node's model chooser and overflow parameters.
 */
export function renderChain3dViewportHtml(
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

export function bindAmp3dDockCollapse(): void {
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
/**
 * Immersive mode lets the chain stage claim every pixel between the effect shell
 * header and the window footer.
 */
export function setAmp3dImmersiveMode(enabled: boolean): void {
  document.body.classList.toggle("amp3d-immersive", enabled);
}

/**
 * Mounts (or re-attaches) the chain 3D stage inside the params panel. The panel
 * rebuilds innerHTML on every refresh, so an existing view is moved into the
 * new container instead of being torn down.
 */
export function bindChain3dView(node: GraphNode, preset: Preset): void {
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
      const module = await import("../amp3d/index.js");
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

export function renderAmp3dToggleButtonHtml(_node?: GraphNode): string {
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

export function bindAmp3dToggleButton(): void {
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
    requestNodeParamsRefresh();
  });
}
