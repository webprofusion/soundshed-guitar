/**
 * The splitter's Branches control: the count comes from the graph, a count that would
 * remove a branch holding effects is disabled, and a click asks the engine for the count.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphEdge, GraphNode, Preset } from "../ts/types";

const posted: unknown[] = [];
vi.mock("../ts/bridge.js", () => ({ postMessage: (payload: unknown) => posted.push(payload) }));

// The panel element is looked up at import, so the page exists first.
document.body.innerHTML = `<div id="node-params-panel"></div>`;

const { EffectGuids } = await import("../ts/effectGuids.js");
const { bindSplitBranchControls, buildSplitBranchControlsHtml, getSplitBranchState } = await import(
  "../ts/signalPath/paramsPanel/splitBranches.js"
);

function node(id: string, type: string): GraphNode {
  return { id, type, displayName: id, category: "utility", bypassed: false, params: {}, config: {} };
}

function edge(from: string, to: string, fromPort = 0, toPort = 0): GraphEdge {
  return { from, to, fromPort, toPort, gain: 1 };
}

/** Input → split → [amp A | amp B | empty] → mix → output: Deathcore Three-Amp Blend's shape, one branch emptied. */
function threeWay(): Preset {
  return {
    id: "p",
    name: "Three-way",
    graph: {
      nodes: [
        node("__input__", "input"),
        node("split", EffectGuids.kSplitter),
        node("ampA", "amp"),
        node("ampB", "amp"),
        node("mix", EffectGuids.kMixer),
        node("__output__", "output"),
      ],
      edges: [
        edge("__input__", "split"),
        edge("split", "ampA", 0),
        edge("split", "ampB", 1),
        edge("split", "mix", 2, 2),
        edge("ampA", "mix", 0, 0),
        edge("ampB", "mix", 0, 1),
        edge("mix", "__output__"),
      ],
    },
  } as Preset;
}

beforeEach(() => {
  posted.length = 0;
});

describe("getSplitBranchState", () => {
  it("counts the splitter's branches and how many hold effects", () => {
    const preset = threeWay();
    const splitter = preset.graph!.nodes.find((n) => n.id === "split")!;
    expect(getSplitBranchState(splitter, preset)).toEqual({ count: 3, minCount: 2 });
  });

  it("will not go below the branches that hold effects", () => {
    const preset = threeWay();
    preset.graph!.nodes.push(node("ampC", "amp"));
    preset.graph!.edges = preset.graph!.edges.filter((e) => !(e.from === "split" && e.to === "mix"));
    preset.graph!.edges.push(edge("split", "ampC", 2), edge("ampC", "mix", 0, 2));
    const splitter = preset.graph!.nodes.find((n) => n.id === "split")!;
    expect(getSplitBranchState(splitter, preset)).toEqual({ count: 3, minCount: 3 });
  });

  it("is null for anything but a splitter", () => {
    const preset = threeWay();
    expect(getSplitBranchState(preset.graph!.nodes.find((n) => n.id === "mix")!, preset)).toBeNull();
  });
});

describe("the Branches control", () => {
  it("offers 2 to 4, marks the current count, and disables a count that would delete an effect", () => {
    const preset = threeWay();
    preset.graph!.nodes.push(node("ampC", "amp"));
    preset.graph!.edges = preset.graph!.edges.filter((e) => !(e.from === "split" && e.to === "mix"));
    preset.graph!.edges.push(edge("split", "ampC", 2), edge("ampC", "mix", 0, 2));
    const splitter = preset.graph!.nodes.find((n) => n.id === "split")!;

    const host = document.createElement("div");
    host.innerHTML = buildSplitBranchControlsHtml(splitter, preset);
    const options = Array.from(host.querySelectorAll<HTMLButtonElement>(".split-branch-option"));
    expect(options.map((o) => o.textContent)).toEqual(["2", "3", "4"]);
    expect(options.map((o) => o.getAttribute("aria-pressed"))).toEqual(["false", "true", "false"]);
    expect(options.map((o) => o.disabled)).toEqual([true, false, false]);
  });

  it("asks the engine for the chosen count, and not for the one it already has", () => {
    const preset = threeWay();
    const splitter = preset.graph!.nodes.find((n) => n.id === "split")!;
    const panel = document.getElementById("node-params-panel")!;
    panel.innerHTML = buildSplitBranchControlsHtml(splitter, preset);
    bindSplitBranchControls(splitter, preset);

    panel.querySelector<HTMLButtonElement>('[data-branch-count="3"]')!.click();
    expect(posted).toEqual([]);

    panel.querySelector<HTMLButtonElement>('[data-branch-count="4"]')!.click();
    expect(posted).toEqual([{ type: "setSplitBranchCount", splitterId: "split", count: 4 }]);
  });

  it("is nothing for other nodes", () => {
    const preset = threeWay();
    expect(buildSplitBranchControlsHtml(preset.graph!.nodes.find((n) => n.id === "ampA")!, preset)).toBe("");
  });
});
