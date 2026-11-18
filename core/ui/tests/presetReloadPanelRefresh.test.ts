/**
 * Loading the preset already open, to discard unsaved edits, keeps the selected node, and
 * re-rendering the chain keeps that node's panel as it is. The engine's "presetLoaded" must
 * still redraw it: the knobs otherwise stayed at the discarded values while the engine played
 * the saved ones. A "state" update must not, since those arrive while a knob is being dragged.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
window.IPlugSendMsg = () => undefined;

const { uiState, setActivePresetDraft } = await import("../ts/state.js");
const { renderSignalPathBar } = await import("../ts/signalPath.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { setSelectedNodeId } = await import("../ts/signalPath/state.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { onPresetLoaded } = await import("../ts/messages/presetHandlers.js");

EffectTypeRegistry.register("test-gate", {
  type: "test-gate",
  displayName: "Test Gate",
  category: "dynamics",
  parameters: [{ key: "threshold", name: "Threshold", default: -40, min: -80, max: 0, unit: "dB" }],
});

function node(id: string, type: string, params: Record<string, number> = {}): GraphNode {
  return { id, type, displayName: "", category: "", bypassed: false, params, config: {} };
}

function presetWithGate(threshold: number): Preset {
  return {
    id: "user-gate",
    name: "Gate",
    graph: {
      nodes: [node("__input__", "input"), node("gate", "test-gate", { threshold }), node("__output__", "output")],
      edges: [
        { from: "__input__", to: "gate", fromPort: 0, toPort: 0, gain: 1 },
        { from: "gate", to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
      ],
    },
  };
}

const knob = () => document.querySelector<HTMLElement>('#node-params-panel .node-param-knob[data-param-key="threshold"]');
const shownThreshold = () => Number(knob()?.dataset.value);

describe("the selected node's panel after the preset is loaded again", () => {
  beforeEach(() => {
    // The saved preset is open on its gate, and the user has turned the threshold up.
    uiState.activePresetId = "user-gate";
    uiState.activePresetSceneId = null;
    const edited = presetWithGate(-10);
    setActivePresetDraft(edited);
    renderSignalPathBar();
    setSelectedNodeId("gate");
    showNodeParamsPanel(uiState.activePresetDraft!.graph!.nodes[1], uiState.activePresetDraft!);
    expect(shownThreshold()).toBe(-10);
  });

  it("shows the saved settings once the engine reports the reload", () => {
    onPresetLoaded({ type: "presetLoaded", preset: presetWithGate(-40), activePresetDirty: false });

    expect(uiState.activePresetDraft?.graph?.nodes[1].params.threshold).toBe(-40);
    expect(shownThreshold()).toBe(-40);
  });

  it("is left alone by a state update, as one arrives mid-drag", () => {
    const before = knob();
    setActivePresetDraft(presetWithGate(-40));
    renderSignalPathBar();

    expect(knob()).toBe(before);
  });

  it("is redrawn once only: the next state update leaves it alone again", () => {
    onPresetLoaded({ type: "presetLoaded", preset: presetWithGate(-40), activePresetDirty: false });
    const afterReload = knob();
    setActivePresetDraft(presetWithGate(-40));
    renderSignalPathBar();

    expect(knob()).toBe(afterReload);
  });
});
