/**
 * The IR Cabinet's IR A and IR B side by side (irAbPanel.ts): each slot's picker and its own
 * controls on its card, the blend between them, the rest under the tabs; and what the cards say
 * and fade as the blend, L/R Split, Mics and the slots change.
 */
import { describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset, ResourceRef } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; nodeId?: string; paramKey?: string; value?: number };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");

// As the engine declares them (IRCabEffect.h), less the ones these tests do not need.
EffectTypeRegistry.register(EffectGuids.kCabIr, {
  type: EffectGuids.kCabIr,
  displayName: "IR Cabinet",
  category: "cab",
  requiresResource: true,
  resourceType: "ir",
  parameters: [
    { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount", group: "Level" },
    { key: "irBlend", name: "IR Blend", default: 0, min: 0, max: 1, unit: "blend", group: "Blend" },
    { key: "lowCutHz", name: "Low Cut", default: 20, min: 20, max: 1000, unit: "Hz", group: "Tone" },
    { key: "micEmulation", name: "Mic Emulation", default: 0, min: 0, max: 1, unit: "toggle", group: "Tone", advanced: true },
    { key: "lrSplit", name: "L/R Split", default: 0, min: 0, max: 1, unit: "toggle", group: "Blend", advanced: true },
    { key: "slotAGain", name: "IR A Level", default: 0, min: -24, max: 24, unit: "dB", group: "IR A", advanced: true },
    { key: "slotAPan", name: "IR A Pan", default: 0, min: -1, max: 1, unit: "pan", group: "IR A", advanced: true },
    { key: "slotAPolarity", name: "IR A Invert", default: 0, min: 0, max: 1, unit: "toggle", group: "IR A", advanced: true },
    { key: "micRadialA", name: "Mic A Radial", default: 0, min: 0, max: 1, unit: "amount", group: "IR A", advanced: true },
    { key: "micProximityA", name: "Mic A Proximity", default: 0, min: 0, max: 1, unit: "amount", group: "IR A", advanced: true },
    { key: "slotBPolarity", name: "IR B Invert", default: 0, min: 0, max: 1, unit: "toggle", group: "IR B", advanced: true },
    { key: "slotBGain", name: "IR B Level", default: 0, min: -24, max: 24, unit: "dB", group: "IR B", advanced: true },
    { key: "slotBPan", name: "IR B Pan", default: 0, min: -1, max: 1, unit: "pan", group: "IR B", advanced: true },
    { key: "micRadialB", name: "Mic B Radial", default: 0, min: 0, max: 1, unit: "amount", group: "IR B", advanced: true },
    { key: "micProximityB", name: "Mic B Proximity", default: 0, min: 0, max: 1, unit: "amount", group: "IR B", advanced: true },
    { key: "quality", name: "Quality", default: 1, min: 0, max: 3, unit: "enum", group: "Tone", advanced: true, labels: ["Economy", "Standard", "High", "Full"] },
  ],
  exposedResources: [
    { resourceId: "irA", displayName: "IR A", nodeId: "", resourceType: "ir", resourceIndex: 0, allowBrowseFile: true },
    { resourceId: "irB", displayName: "IR B", nodeId: "", resourceType: "ir", resourceIndex: 1, allowBrowseFile: true },
  ],
});

const IR_A: ResourceRef = { resourceType: "ir", filePath: "C:/irs/421.wav" };
const IR_B: ResourceRef = { resourceType: "ir", filePath: "C:/irs/57.wav" };

function cab(id: string, resources: ResourceRef[], params: Record<string, number> = {}): GraphNode {
  return { id, type: EffectGuids.kCabIr, displayName: "IR Cabinet", category: "cab", bypassed: false, params: { irBlend: 0.5, ...params }, config: {}, resources };
}

function presetWith(node: GraphNode): Preset {
  return { id: "user-test", name: "Test", graph: { nodes: [node], edges: [] } } as Preset;
}

function panel(): HTMLElement {
  return document.getElementById("node-params-panel") as HTMLElement;
}

function card(slot: 0 | 1): HTMLElement {
  return panel().querySelector<HTMLElement>(`.ir-ab-slot[data-slot="${slot}"]`)!;
}

const shares = (): string[] => Array.from(panel().querySelectorAll("[data-ir-ab-share]")).map((share) => share.textContent ?? "");
const inactive = (): string[] => Array.from(panel().querySelectorAll<HTMLElement>(".ir-ab .is-inactive")).map((cell) => cell.dataset.irAbKey ?? "");
const labels = (root: Element): string[] => Array.from(root.querySelectorAll(".node-param-label")).map((label) => label.textContent ?? "");

describe("the IR Cabinet's A/B section", () => {
  it("puts each slot's picker and controls on its card, the blend between them, the rest under the tabs", () => {
    const node = cab("cab-ab", [IR_A, IR_B]);
    showNodeParamsPanel(node, presetWith(node));

    expect(card(0).querySelector('.node-resource-selector[data-resource-index="0"]')).not.toBeNull();
    expect(card(1).querySelector('.node-resource-selector[data-resource-index="1"]')).not.toBeNull();
    expect(labels(card(0))).toEqual(["Level", "Pan", "Radial", "Proximity"]);
    expect(labels(card(1))).toEqual(["Level", "Pan", "Radial", "Proximity"]);
    expect(card(0).querySelector('.ir-ab-invert input[data-param-key="slotAPolarity"]')).not.toBeNull();
    expect(card(1).querySelector('.ir-ab-invert input[data-param-key="slotBPolarity"]')).not.toBeNull();

    const centre = panel().querySelector(".ir-ab-centre")!;
    expect(labels(centre)).toEqual(["Blend", "L/R Split", "Mics"]);
    // A knob here, not the A–B slider the blend is elsewhere.
    expect(centre.querySelector('.node-param-knob[data-param-key="irBlend"]')).not.toBeNull();
    expect(panel().querySelector(".node-param-blend-slider")).toBeNull();
    expect(centre.querySelector('[data-param-key="irBlend"] ~ .node-param-value')?.textContent).toBe("50/50");

    // Each control once: the groups under the tabs keep only the cabinet's own.
    const tabs = panel().querySelector(".node-param-tab-panels")!;
    expect(labels(tabs)).toEqual(["Mix", "Low Cut", "Quality"]);
    for (const key of ["irBlend", "slotAGain", "slotBPan", "micRadialA", "lrSplit", "micEmulation"]) {
      expect(panel().querySelectorAll(`.node-param-knob[data-param-key="${key}"], .node-param-toggle[data-param-key="${key}"]`)).toHaveLength(1);
    }
    expect(shares()).toEqual(["50%", "50%"]);
  });

  it("fades what does nothing as things stand, and says what share each IR plays", () => {
    const node = cab("cab-fade", [IR_A, IR_B], { irBlend: 0.3 });
    showNodeParamsPanel(node, presetWith(node));
    expect(shares()).toEqual(["70%", "30%"]);
    // Mics off: the mic knobs do nothing.
    expect(inactive()).toEqual(["micRadialA", "micProximityA", "micRadialB", "micProximityB"]);

    sent = [];
    panel().querySelector<HTMLInputElement>('.ir-ab .node-param-toggle[data-param-key="micEmulation"]')!.click();
    panel().querySelector<HTMLInputElement>('.ir-ab .node-param-toggle[data-param-key="lrSplit"]')!.click();
    expect(sent.filter((m) => m.paramKey).map((m) => [m.paramKey, m.value])).toEqual([["micEmulation", 1], ["lrSplit", 1]]);
    expect(shares()).toEqual(["Left", "Right"]);
    expect(inactive()).toEqual(["slotAPan", "irBlend", "slotBPan"]);
    expect(card(0).querySelector<HTMLElement>('[data-ir-ab-key="slotAPan"]')?.title).toMatch(/L\/R Split/);
  });

  it("flips a slot from the Ø in its header", () => {
    const node = cab("cab-invert", [IR_A, IR_B]);
    showNodeParamsPanel(node, presetWith(node));
    sent = [];
    const invertB = card(1).querySelector<HTMLInputElement>(".ir-ab-invert input")!;
    invertB.click();
    expect(sent.filter((m) => m.paramKey === "slotBPolarity").map((m) => m.value)).toEqual([1]);
    expect(node.params.slotBPolarity).toBe(1);
  });

  it("marks an empty slot, and a lone IR plays in full", () => {
    const node = cab("cab-lone", [IR_A]);
    showNodeParamsPanel(node, presetWith(node));
    expect(card(1).classList.contains("is-empty")).toBe(true);
    expect(card(0).classList.contains("is-empty")).toBe(false);
    expect(shares()).toEqual(["Alone", "Empty"]);
    expect(inactive()).toEqual(expect.arrayContaining(["irBlend", "lrSplit"]));
  });
});
