/**
 * A split inside a branch of another split draws in full: the inner splitter, its branches
 * and its mixer sit inside the outer branch, the outer branch carries on past the inner
 * mixer, and every connection has a connector to add an effect on. Before, a branch stopped
 * at the inner splitter, so whatever followed it could be neither seen nor edited.
 */
import { beforeAll, describe, expect, it, vi } from "vitest";
import type { GraphEdge, GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
window.IPlugSendMsg = () => undefined;

const { uiState } = await import("../ts/state.js");
const { renderSignalPathBar } = await import("../ts/signalPath.js");
const { EffectGuids } = await import("../ts/effectGuids.js");

function node(id: string, type: string): GraphNode {
  return { id, type, displayName: id, category: "", bypassed: false, params: {}, config: {} };
}

function edge(from: string, to: string, fromPort = 0, toPort = 0): GraphEdge {
  return { from, to, fromPort, toPort, gain: 1 };
}

/**
 * input → amp → outer split ─┬→ a1 → inner split ─┬→ b1 ─────┬→ inner mix → a2 ─┬→ outer mix → output
 *                            │                    └→ (empty) ─┘                   │
 *                            └→ c1 ─────────────────────────────────────────────────┘
 */
function nestedPreset(): Preset {
  return {
    id: "nested",
    name: "Nested",
    graph: {
      nodes: [
        node("__input__", "input"),
        node("amp", "gain"),
        node("outerSplit", EffectGuids.kSplitter),
        node("a1", "gain"),
        node("innerSplit", EffectGuids.kSplitter),
        node("b1", "gain"),
        node("innerMix", EffectGuids.kMixer),
        node("a2", "gain"),
        node("c1", "gain"),
        node("outerMix", EffectGuids.kMixer),
        node("__output__", "output"),
      ],
      edges: [
        edge("__input__", "amp"),
        edge("amp", "outerSplit"),
        edge("outerSplit", "a1", 0, 0),
        edge("outerSplit", "c1", 1, 0),
        edge("a1", "innerSplit"),
        edge("innerSplit", "b1", 0, 0),
        edge("innerSplit", "innerMix", 1, 1),
        edge("b1", "innerMix", 0, 0),
        edge("innerMix", "a2"),
        edge("a2", "outerMix", 0, 0),
        edge("c1", "outerMix", 0, 1),
        edge("outerMix", "__output__"),
      ],
    },
  } as Preset;
}

describe("a nested split in the signal path", () => {
  let root: HTMLElement;

  beforeAll(() => {
    const preset = nestedPreset();
    uiState.presets = [preset];
    uiState.presetCache = new Map([[preset.id, preset]]);
    uiState.activePresetId = preset.id;
    root = document.getElementById("signal-path-nodes")!;
    renderSignalPathBar();
  });

  it("draws every node once", () => {
    const drawn = Array.from(root.querySelectorAll<HTMLElement>(".signal-node[data-node-id]"), (el) => el.dataset.nodeId);
    expect(drawn.sort()).toEqual(nestedPreset().graph!.nodes.map((n) => n.id).sort());
  });

  it("puts the inner split inside the outer split's first branch, before the rest of that branch", () => {
    const outer = root.querySelector<HTMLElement>('.parallel-container[data-splitter-id="outerSplit"]')!;
    expect(outer.dataset.mixerId).toBe("outerMix");
    const branches = outer.querySelectorAll(":scope > .parallel-branches > .parallel-branch");
    expect(branches.length).toBe(2);

    const first = branches[0] as HTMLElement;
    const inner = first.querySelector<HTMLElement>(':scope > .parallel-container[data-splitter-id="innerSplit"]');
    expect(inner?.dataset.mixerId).toBe("innerMix");
    const order = Array.from(first.querySelectorAll<HTMLElement>(":scope > .signal-node"), (el) => el.dataset.nodeId);
    expect(order).toEqual(["a1", "innerSplit", "innerMix", "a2"]);

    const innerBranches = inner!.querySelectorAll(":scope > .parallel-branches > .parallel-branch");
    expect(innerBranches.length).toBe(2);
    expect(innerBranches[0].querySelector<HTMLElement>(".signal-node")?.dataset.nodeId).toBe("b1");
    expect(innerBranches[1].querySelector(".signal-node")).toBeNull();
  });

  it("has a connector on every edge, so an effect can be added anywhere", () => {
    const connectors = Array.from(
      root.querySelectorAll<HTMLElement>(".signal-connector-wrapper[data-edge-from]"),
      (el) => `${el.dataset.edgeFrom}>${el.dataset.edgeTo}:${el.dataset.edgeFromPort}:${el.dataset.edgeToPort}`,
    );
    const edges = nestedPreset().graph!.edges.map((e) => `${e.from}>${e.to}:${e.fromPort}:${e.toPort}`);
    expect(connectors.sort()).toEqual(edges.sort());
  });

  it("offers to remove each split on its own mixer", () => {
    const collapse = Array.from(
      root.querySelectorAll<HTMLElement>(".signal-node-delete[data-collapse-splitter-id]"),
      (el) => `${el.dataset.collapseSplitterId}>${el.dataset.collapseMixerId}`,
    );
    expect(collapse.sort()).toEqual(["innerSplit>innerMix", "outerSplit>outerMix"]);
  });
});
