import { describe, expect, it } from "vitest";
import type { GraphNode, SignalGraph } from "../ts/types";
import {
  ChainHistory,
  createSnapshot,
  describeGraphChange,
  diffNodeStates,
  graphSignature,
  haveSameTopology,
} from "../ts/signalPath/historyModel";

function node(id: string, overrides: Partial<GraphNode> = {}): GraphNode {
  return {
    id,
    type: overrides.type ?? "drive_overdrive",
    displayName: overrides.displayName ?? id,
    category: overrides.category ?? "drive",
    bypassed: overrides.bypassed ?? false,
    params: overrides.params ?? { gain: 0.5 },
    config: overrides.config ?? {},
    ...(overrides.resources ? { resources: overrides.resources } : {}),
  };
}

function graph(nodes: GraphNode[]): SignalGraph {
  const edges = nodes.slice(0, -1).map((from, index) => ({
    from: from.id,
    to: nodes[index + 1].id,
    fromPort: 0,
    toPort: 0,
    gain: 1,
  }));
  return { nodes, edges };
}

const baseGraph = () => graph([node("drive"), node("delay", { type: "delay_digital", params: { time: 300 } })]);

function withParam(source: SignalGraph, nodeId: string, key: string, value: number): SignalGraph {
  const next = JSON.parse(JSON.stringify(source)) as SignalGraph;
  next.nodes.find((n) => n.id === nodeId)!.params[key] = value;
  return next;
}

describe("graphSignature", () => {
  it("ignores key order", () => {
    const a: SignalGraph = { nodes: [node("drive", { params: { gain: 0.5, tone: 0.2 } })], edges: [] };
    const b: SignalGraph = { nodes: [node("drive", { params: { tone: 0.2, gain: 0.5 } })], edges: [] };
    expect(graphSignature(a)).toBe(graphSignature(b));
  });

  it("ignores edge order but not edge gain", () => {
    const edges = [
      { from: "a", to: "b", fromPort: 0, toPort: 0, gain: 1 },
      { from: "b", to: "c", fromPort: 0, toPort: 0, gain: 1 },
    ];
    expect(graphSignature({ nodes: [], edges })).toBe(graphSignature({ nodes: [], edges: edges.slice().reverse() }));
    expect(graphSignature({ nodes: [], edges })).not.toBe(
      graphSignature({ nodes: [], edges: [edges[0], { ...edges[1], gain: 0.5 }] }),
    );
  });

  it("treats the legacy enabled flag as the bypass flag", () => {
    const bypassed = { ...node("drive"), bypassed: true } as GraphNode;
    const legacy = { ...node("drive"), enabled: false } as unknown as GraphNode;
    delete (legacy as unknown as { bypassed?: boolean }).bypassed;
    expect(graphSignature({ nodes: [legacy], edges: [] })).toBe(graphSignature({ nodes: [bypassed], edges: [] }));
  });
});

describe("haveSameTopology", () => {
  it("accepts a pure parameter change", () => {
    expect(haveSameTopology(baseGraph(), withParam(baseGraph(), "drive", "gain", 0.9))).toBe(true);
  });

  it("rejects a reorder, an added node and a replaced type", () => {
    const reordered = graph([node("delay", { type: "delay_digital", params: { time: 300 } }), node("drive")]);
    expect(haveSameTopology(baseGraph(), reordered)).toBe(false);
    expect(haveSameTopology(baseGraph(), graph([node("drive"), node("delay"), node("verb")]))).toBe(false);

    const replaced = JSON.parse(JSON.stringify(baseGraph())) as SignalGraph;
    replaced.nodes[0].type = "drive_fuzz";
    expect(haveSameTopology(baseGraph(), replaced)).toBe(false);
  });
});

describe("diffNodeStates", () => {
  it("reports only the parameters that moved", () => {
    const changes = diffNodeStates(baseGraph(), withParam(baseGraph(), "drive", "gain", 0.9));
    expect(changes).toEqual([{ nodeId: "drive", params: [{ key: "gain", value: 0.9 }], config: [], bypassed: null }]);
  });

  it("reports a bypass flip", () => {
    const next = JSON.parse(JSON.stringify(baseGraph())) as SignalGraph;
    next.nodes[1].bypassed = true;
    expect(diffNodeStates(baseGraph(), next)).toEqual([
      { nodeId: "delay", params: [], config: [], bypassed: true },
    ]);
  });

  it("returns an empty list for identical graphs", () => {
    expect(diffNodeStates(baseGraph(), baseGraph())).toEqual([]);
  });

  it("gives up when a structural change is involved", () => {
    expect(diffNodeStates(baseGraph(), graph([node("drive")]))).toBeNull();
  });

  it("gives up when a resource was swapped — a targeted param message cannot say that", () => {
    const next = JSON.parse(JSON.stringify(baseGraph())) as SignalGraph;
    next.nodes[0].resources = [{ resourceType: "nam", resourceId: "model-2" }];
    expect(diffNodeStates(baseGraph(), next)).toBeNull();
  });

  it("gives up when a parameter key appears, since there is no way to unset it", () => {
    const next = withParam(baseGraph(), "drive", "tone", 0.4);
    expect(diffNodeStates(baseGraph(), next)).toBeNull();
  });
});

describe("describeGraphChange", () => {
  it("names the node that was added or removed", () => {
    const grown = graph([node("drive"), node("delay"), node("verb", { displayName: "Hall Reverb" })]);
    expect(describeGraphChange(baseGraph(), grown)).toBe("Add Hall Reverb");
    expect(describeGraphChange(grown, baseGraph())).toBe("Remove Hall Reverb");
  });

  it("names the parameter for a single-knob change", () => {
    expect(describeGraphChange(baseGraph(), withParam(baseGraph(), "drive", "gain", 0.9))).toBe("drive gain");
  });

  it("describes a bypass flip in on/off terms", () => {
    const next = JSON.parse(JSON.stringify(baseGraph())) as SignalGraph;
    next.nodes[0].bypassed = true;
    expect(describeGraphChange(baseGraph(), next)).toBe("drive off");
  });

  it("calls a same-node different-type edit a replace", () => {
    const next = JSON.parse(JSON.stringify(baseGraph())) as SignalGraph;
    next.nodes[0].type = "drive_fuzz";
    expect(describeGraphChange(baseGraph(), next)).toBe("Replace drive");
  });
});

describe("ChainHistory", () => {
  const snapshot = (source: SignalGraph, label = "edit") => createSnapshot(source, "preset-1", "scene-1", label);

  it("has nothing to undo until an edit is recorded", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    expect(history.canUndo()).toBe(false);
    expect(history.canRedo()).toBe(false);

    history.push(snapshot(withParam(baseGraph(), "drive", "gain", 0.9), "drive gain"));
    expect(history.canUndo()).toBe(true);
    expect(history.peekUndoLabel()).toBe("drive gain");
  });

  it("walks back and forward over the same states", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    const edited = withParam(baseGraph(), "drive", "gain", 0.9);
    history.push(snapshot(edited));

    expect(history.undo()?.signature).toBe(graphSignature(baseGraph()));
    expect(history.canRedo()).toBe(true);
    expect(history.redo()?.signature).toBe(graphSignature(edited));
    expect(history.canRedo()).toBe(false);
  });

  it("ignores a push that changes nothing", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    history.push(snapshot(baseGraph()));
    expect(history.canUndo()).toBe(false);
  });

  it("drops the redo tail when a new edit branches from a past state", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    history.push(snapshot(withParam(baseGraph(), "drive", "gain", 0.9)));
    history.undo();
    history.push(snapshot(withParam(baseGraph(), "drive", "gain", 0.1)));
    expect(history.canRedo()).toBe(false);
    expect(history.canUndo()).toBe(true);
  });

  it("amends the state under the cursor without creating a step", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    const normalized = withParam(baseGraph(), "drive", "gain", 0.5000001);
    history.amendCurrent(snapshot(normalized, "normalised"));
    expect(history.canUndo()).toBe(false);
    expect(history.getCurrent()?.signature).toBe(graphSignature(normalized));
    // The label of the amended entry is preserved — it still describes the edit.
    expect(history.getCurrent()?.label).toBe("Loaded");
  });

  it("caps the stack and keeps the newest entry current", () => {
    const history = new ChainHistory(3);
    history.reset(snapshot(baseGraph()));
    for (let i = 1; i <= 5; i += 1) {
      history.push(snapshot(withParam(baseGraph(), "drive", "gain", i / 10)));
    }
    expect(history.getCurrent()?.signature).toBe(graphSignature(withParam(baseGraph(), "drive", "gain", 0.5)));
    let steps = 0;
    while (history.canUndo()) {
      history.undo();
      steps += 1;
    }
    expect(steps).toBe(2);
  });

  it("gives A and B independent stacks, seeded from the same chain", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    expect(history.slotsMatch()).toBe(true);

    const aEdit = withParam(baseGraph(), "drive", "gain", 0.9);
    history.push(snapshot(aEdit, "drive gain"));

    // Switching returns B's own state — untouched — and does not disturb A.
    expect(history.setActiveSlot("B")?.signature).toBe(graphSignature(baseGraph()));
    expect(history.getActiveSlotId()).toBe("B");
    expect(history.canUndo()).toBe(false);

    const bEdit = withParam(baseGraph(), "delay", "time", 120);
    history.push(snapshot(bEdit, "delay time"));

    expect(history.setActiveSlot("A")?.signature).toBe(graphSignature(aEdit));
    expect(history.peekUndoLabel()).toBe("drive gain");
    expect(history.setActiveSlot("B")?.signature).toBe(graphSignature(bEdit));
    expect(history.peekUndoLabel()).toBe("delay time");
  });

  it("switching to the slot already live is a no-op", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    expect(history.setActiveSlot("A")).toBeNull();
  });

  it("copies the live chain into the other slot as an undoable step there", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    const edited = withParam(baseGraph(), "drive", "gain", 0.9);
    history.push(snapshot(edited));

    expect(history.copyActiveToOther()).toBe("B");
    expect(history.slotsMatch()).toBe(true);

    history.setActiveSlot("B");
    expect(history.getCurrent()?.signature).toBe(graphSignature(edited));
    expect(history.canUndo()).toBe(true);
    expect(history.undo()?.signature).toBe(graphSignature(baseGraph()));
  });

  it("reseeds both slots and makes A live again", () => {
    const history = new ChainHistory();
    history.reset(snapshot(baseGraph()));
    history.push(snapshot(withParam(baseGraph(), "drive", "gain", 0.9)));
    history.setActiveSlot("B");

    const other = graph([node("fuzz", { type: "drive_fuzz" })]);
    history.reset(createSnapshot(other, "preset-2", "scene-1", "Loaded"));

    expect(history.getActiveSlotId()).toBe("A");
    expect(history.canUndo()).toBe(false);
    expect(history.slotsMatch()).toBe(true);
    expect(history.setActiveSlot("B")?.presetId).toBe("preset-2");
  });

  it("keeps the two slots' graphs independent after a reset", () => {
    const history = new ChainHistory();
    const seed = snapshot(baseGraph());
    history.reset(seed);
    const fromA = history.getCurrent()!;
    history.setActiveSlot("B");
    const fromB = history.getCurrent()!;
    expect(fromA.graph).not.toBe(fromB.graph);
  });
});

describe("describeGraphChange with an injected label resolver", () => {
  it("names nodes the way the rest of the UI does, not by their raw displayName", () => {
    // A NAM amp's displayName is the resource GUID; only the app's own resolver
    // can turn it into something a user recognises.
    const guid = "49ea214c-91e6-41f9-bd27-ad6eec0ae90a";
    const from = graph([node("amp", { displayName: guid, params: { bass: -3.7 } })]);
    const to = graph([node("amp", { displayName: guid, params: { bass: 0.3 } })]);

    expect(describeGraphChange(from, to)).toBe(`${guid} bass`);
    expect(describeGraphChange(from, to, () => "Metal Guitar")).toBe("Metal Guitar bass");
  });
});

describe("engine-owned config keys", () => {
  const withConfig = (config: Record<string, string>) =>
    graph([node("plugin", { type: "hosted_plugin", config })]);

  it("a hosted plugin capturing its own state is not an edit", () => {
    const before = withConfig({ pluginId: "vst-1", pluginStateBase64Length: "0" });
    const after = withConfig({ pluginId: "vst-1", pluginStateBase64Length: "204800" });
    expect(graphSignature(before)).toBe(graphSignature(after));
    expect(diffNodeStates(before, after)).toEqual([]);
  });

  it("the scrubbed blob appearing or disappearing is not an edit either", () => {
    // The backend scrubs the blob before the UI sees it, so its presence varies
    // by which path delivered the graph — that must not read as a change.
    const before = withConfig({ pluginId: "vst-1" });
    const after = withConfig({ pluginId: "vst-1", pluginStateBase64: "AAAA" });
    expect(graphSignature(before)).toBe(graphSignature(after));
    expect(diffNodeStates(before, after)).toEqual([]);
  });

  it("still notices a real config change on the same node", () => {
    const before = withConfig({ pluginId: "vst-1", pluginStateBase64Length: "10" });
    const after = withConfig({ pluginId: "vst-2", pluginStateBase64Length: "99" });
    expect(diffNodeStates(before, after)).toEqual([
      { nodeId: "plugin", params: [], config: [{ key: "pluginId", value: "vst-2" }], bypassed: null },
    ]);
  });
});
