/**
 * The signal chain rule publishing enforces: no drive, amp, or amp/pedal NAM capture after a
 * cabinet (an IR cab, a Simple Cab, or a full-rig NAM capture).
 */
import { beforeEach, describe, expect, it } from "vitest";
import type { GraphEdge, GraphNode, LibraryResource, Preset, ResourceRef } from "../ts/types";

const { EffectGuids } = await import("../ts/effectGuids.js");
const { uiState } = await import("../ts/state.js");
const { validatePresetSignalChain, summarizeSignalChainIssues } = await import("../ts/signalPath/chainValidation.js");
const { hasPublishableSignalChain } = await import("../ts/toneSharingPanel/itemUpload.js");

function node(id: string, type: string, resources?: ResourceRef[], title = id): GraphNode {
  return { id, type, title, displayName: id, category: "", bypassed: false, params: {}, config: {}, ...(resources ? { resources } : {}) };
}

function nam(id: string, modelId: string, type: string = EffectGuids.kAmpNamOptimized): GraphNode {
  return node(id, type, [{ type: "nam", id: modelId }]);
}

function ir(id: string, irId: string): GraphNode {
  return node(id, EffectGuids.kCabIr, [{ type: "ir", id: irId }]);
}

/** A straight chain: input, the given nodes in order, output. */
function chain(nodes: GraphNode[]): Preset {
  const all = [node("in", "input"), ...nodes, node("out", "output")];
  const edges: GraphEdge[] = all.slice(1).map((to, index) => ({ from: all[index].id, to: to.id, fromPort: 0, toPort: 0, gain: 1 }));
  return { id: "p", name: "Test", graph: { nodes: all, edges } };
}

function library(id: string, category: string, metadata: Record<string, string> = {}): LibraryResource {
  return { id, name: id, category, description: "", filePath: `C:/models/${id}`, metadata };
}

beforeEach(() => {
  uiState.resourceLibrary = {
    nam: [
      library("rig", "full-rig"),
      library("head", "amp"),
      library("pre", "preamp", { gear_type: "preamp" }),
      library("ts", "pedal"),
      library("comp", "preamp", { gear_type: "studio" }),
      library("outboard", "preamp", { tone3000Category: "outboard" }),
      library("rig-by-gear-type", "", { gear_type: "amp_cab" }),
      library("unknown", ""),
    ],
    ir: [library("v30", "cab"), library("hall", "reverb")],
  };
});

describe("validatePresetSignalChain", () => {
  it("passes the usual order: drive, amp, cab, then time effects", () => {
    const preset = chain([
      node("drive", EffectGuids.kOverdrive),
      nam("amp", "head"),
      ir("cab", "v30"),
      node("delay", EffectGuids.kDelayDigital),
      node("verb", EffectGuids.kReverbRoom),
    ]);
    expect(validatePresetSignalChain(preset)).toEqual([]);
  });

  it("passes a drive in front of a full-rig model", () => {
    expect(validatePresetSignalChain(chain([node("drive", EffectGuids.kFuzz), nam("rig", "rig")]))).toEqual([]);
  });

  it.each([
    ["an overdrive", node("drive", EffectGuids.kOverdrive)],
    ["a distortion", node("drive", EffectGuids.kDistortion)],
    ["a fuzz", node("drive", EffectGuids.kFuzz)],
    ["a multi-band distortion", node("drive", EffectGuids.kMultibandDistortion)],
    ["the built-in amp", node("drive", EffectGuids.kAmpBuiltin)],
    ["an amp NAM model", nam("drive", "head")],
    ["a preamp NAM model", nam("drive", "pre")],
    ["a pedal NAM model in FX NAM", nam("drive", "ts", EffectGuids.kFxNam)],
    ["a full-rig NAM model", nam("drive", "rig")],
  ])("refuses %s after an IR cab", (_label, offending) => {
    const issues = validatePresetSignalChain(chain([nam("amp", "head"), ir("cab", "v30"), offending]));
    expect(issues).toHaveLength(1);
    expect(issues[0]).toMatchObject({ rule: "gain-stage-after-cab", nodeId: "drive", cabNodeId: "cab" });
  });

  it("refuses a drive after a full-rig model, found by category or by gear_type", () => {
    for (const rigId of ["rig", "rig-by-gear-type"]) {
      const issues = validatePresetSignalChain(chain([nam("amp", rigId), node("drive", EffectGuids.kOverdrive)]));
      expect(issues.map((issue) => [issue.cabNodeId, issue.nodeId])).toEqual([["amp", "drive"]]);
    }
  });

  it("refuses a drive after a Simple Cab", () => {
    const issues = validatePresetSignalChain(chain([node("cab", EffectGuids.kCabSimple), node("drive", EffectGuids.kDistortion)]));
    expect(issues).toHaveLength(1);
  });

  it("refuses an amp model whose blend includes a pedal or amp capture", () => {
    const blend = node("blend", EffectGuids.kAmpNamBlend, [{ type: "nam", id: "unknown" }, { type: "nam", id: "head" }]);
    expect(validatePresetSignalChain(chain([ir("cab", "v30"), blend]))).toHaveLength(1);
  });

  it("lets studio and outboard captures, and models it cannot classify, follow a cab", () => {
    const preset = chain([ir("cab", "v30"), nam("comp", "comp", EffectGuids.kFxNam), nam("ob", "outboard", EffectGuids.kFxNam), nam("what", "unknown")]);
    expect(validatePresetSignalChain(preset)).toEqual([]);
  });

  it("does not treat an IR cab holding a reverb IR, or an empty one, as a cab", () => {
    expect(validatePresetSignalChain(chain([ir("room", "hall"), node("drive", EffectGuids.kOverdrive)]))).toEqual([]);
    expect(validatePresetSignalChain(chain([node("empty", EffectGuids.kCabIr), node("drive", EffectGuids.kOverdrive)]))).toEqual([]);
  });

  it("uses the category the preset saved when the library does not have the model", () => {
    const missing = node("amp", EffectGuids.kAmpNamOptimized, [{ type: "nam", id: "gone", source: { category: "full-rig" } }]);
    expect(validatePresetSignalChain(chain([missing, node("drive", EffectGuids.kOverdrive)]))).toHaveLength(1);
  });

  it("follows parallel branches after the cab", () => {
    const preset: Preset = {
      id: "p",
      name: "Split",
      graph: {
        nodes: [node("in", "input"), ir("cab", "v30"), node("split", EffectGuids.kSplitter), node("clean", EffectGuids.kChorus),
          node("dirty", EffectGuids.kFuzz), node("mix", EffectGuids.kMixer), node("out", "output")],
        edges: [
          { from: "in", to: "cab", fromPort: 0, toPort: 0, gain: 1 },
          { from: "cab", to: "split", fromPort: 0, toPort: 0, gain: 1 },
          { from: "split", to: "clean", fromPort: 0, toPort: 0, gain: 1 },
          { from: "split", to: "dirty", fromPort: 1, toPort: 0, gain: 1 },
          { from: "clean", to: "mix", fromPort: 0, toPort: 0, gain: 1 },
          { from: "dirty", to: "mix", fromPort: 0, toPort: 1, gain: 1 },
          { from: "mix", to: "out", fromPort: 0, toPort: 0, gain: 1 },
        ],
      },
    };
    expect(validatePresetSignalChain(preset).map((issue) => issue.nodeId)).toEqual(["dirty"]);
  });

  it("checks scene chains and names the scene", () => {
    const good = chain([node("drive", EffectGuids.kOverdrive), nam("amp", "head"), ir("cab", "v30")]);
    const bad = chain([nam("amp", "head"), ir("cab", "v30"), node("drive", EffectGuids.kOverdrive)]);
    const preset: Preset = { ...good, scenes: [{ id: "s1", title: "Rhythm", graph: good.graph! }, { id: "s2", title: "Lead", graph: bad.graph! }] };
    const issues = validatePresetSignalChain(preset);
    expect(issues).toHaveLength(1);
    expect(issues[0].sceneTitle).toBe("Lead");
    expect(issues[0].message).toContain('scene "Lead"');
  });

  it("summarizes the first problem and counts the rest", () => {
    const preset = chain([ir("cab", "v30"), node("od", EffectGuids.kOverdrive, undefined, "Green Drive"), node("fz", EffectGuids.kFuzz)]);
    const summary = summarizeSignalChainIssues(validatePresetSignalChain(preset));
    expect(summary).toContain('"Green Drive" is a drive after the cab "cab"');
    expect(summary).toContain("(1 more chain problem)");
    expect(summarizeSignalChainIssues([])).toBe("");
  });

  it("leaves the basic publishable check unchanged", () => {
    expect(hasPublishableSignalChain(chain([node("drive", EffectGuids.kOverdrive)]))).toBe(true);
    expect(hasPublishableSignalChain(chain([]))).toBe(false);
  });
});
