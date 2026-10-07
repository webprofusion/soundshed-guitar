/**
 * The IR Cabinet's Alignment section in the params panel: under the Main and Advanced tabs
 * rather than in either, only one IR's response with one IR loaded, the offset kept out of the
 * generic controls, one analysis per pair of IRs, and Auto Align and the nudges setting the
 * node's parameters.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset, ResourceRef } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
// jsdom draws nothing; without this it logs every attempt to.
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; requestId?: string; nodeId?: string; paramKey?: string; value?: number; irA?: ResourceRef; irB?: ResourceRef };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { applyIrAlignment } = await import("../ts/irAlignment.js");

EffectTypeRegistry.register(EffectGuids.kCabIr, {
  type: EffectGuids.kCabIr,
  displayName: "IR Cabinet",
  category: "cab",
  requiresResource: true,
  resourceType: "ir",
  parameters: [
    { key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount", group: "Level" },
    { key: "irBlend", name: "IR Blend", default: 0, min: 0, max: 1, unit: "blend", group: "Tone" },
    { key: "slotAPolarity", name: "IR A Invert", default: 0, min: 0, max: 1, unit: "toggle", group: "IR A", advanced: true },
    { key: "slotBPolarity", name: "IR B Invert", default: 0, min: 0, max: 1, unit: "toggle", group: "IR B", advanced: true },
    { key: "slotBOffset", name: "IR B Offset", default: 0, min: -10, max: 10, unit: "ms", group: "Alignment", advanced: true },
  ],
});

const IR_A: ResourceRef = { resourceType: "ir", filePath: "C:/irs/421 1960.wav" };
const IR_B: ResourceRef = { resourceType: "ir", filePath: "C:/irs/906 1960.wav" };

function cab(id: string, resources: ResourceRef[]): GraphNode {
  return { id, type: EffectGuids.kCabIr, displayName: "IR Cabinet", category: "cab", bypassed: false, params: { irBlend: 0.5 }, config: {}, resources };
}

function presetWith(node: GraphNode): Preset {
  return {
    id: "user-test",
    name: "Test",
    graph: {
      nodes: [node],
      edges: [],
    },
  } as Preset;
}

function panel(): HTMLElement {
  return document.getElementById("node-params-panel") as HTMLElement;
}

function section(): HTMLElement | null {
  return panel().querySelector<HTMLElement>(".ir-alignment");
}

function encode(samples: number[]): string {
  const bytes = new Uint8Array(new Float32Array(samples).buffer);
  return btoa(String.fromCharCode(...bytes));
}

function answer(request: Sent, alignedOffsetMs: number, invertB: boolean): void {
  applyIrAlignment({
    type: "irAlignment",
    requestId: request.requestId,
    sampleRate: 48000,
    waveformA: encode([0, 1, 0.5, -0.25, 0]),
    waveformB: encode([0, 0, 1, 0.5, -0.25]),
    normalizeGainA: 1,
    normalizeGainB: 1,
    alignedOffsetMs,
    invertB,
    match: 0.95,
    onsetAMs: 0.02,
    onsetBMs: 0.04,
    maxOffsetSamples: 1,
    matchByOffset: [0.95, 0.5, 0.1],
  });
}

function paramUpdates(key: string): number[] {
  return sent.filter((m) => m.type === "updateSignalPathNodeParam" && m.paramKey === key).map((m) => m.value ?? Number.NaN);
}

beforeEach(() => {
  sent = [];
});

describe("the IR Cabinet's Alignment section", () => {
  it("shows one IR's response alone, with nothing to align and no offset knob", () => {
    const node = cab("cab-one", [IR_A]);
    showNodeParamsPanel(node, presetWith(node));
    expect(section()?.querySelector(".node-param-group-title")?.textContent).toBe("IR Response");
    expect(section()?.querySelector(".ir-alignment-response")).not.toBeNull();
    for (const selector of [".ir-alignment-waves", ".ir-alignment-match", ".ir-alignment-auto", ".ir-alignment-offset-input", ".ir-alignment-invert-input"]) {
      expect(section()?.querySelector(selector)).toBeNull();
    }
    expect(panel().querySelector('[data-param-key="slotBOffset"].node-param-knob')).toBeNull();

    const requests = sent.filter((m) => m.type === "analyzeIrAlignment");
    expect(requests).toHaveLength(1);
    expect(requests[0].irA).toEqual(IR_A);
    expect("irB" in requests[0]).toBe(false);
    expect(section()?.querySelector(".ir-alignment-status")?.textContent).toBe("Analysing…");
    applyIrAlignment({ type: "irAlignment", requestId: requests[0].requestId, sampleRate: 48000, waveformA: encode([0, 1, 0.5, -0.25, 0]), normalizeGainA: 1 });
    expect(section()?.querySelector(".ir-alignment-status")?.textContent).toBe("");
    expect(section()?.querySelector(".ir-alignment-note")?.textContent).toBe("");
  });

  it("says IR B alone is not heard", () => {
    const node = cab("cab-only-b", [{} as ResourceRef, IR_B]);
    showNodeParamsPanel(node, presetWith(node));
    expect(section()?.querySelector(".ir-alignment-waves")).toBeNull();
    expect(section()?.querySelector(".eq-visualizer-header span")?.textContent).toBe("IR B");
    expect(section()?.querySelector(".ir-alignment-note")?.textContent).toBe("IR B is only heard once IR A is loaded.");
  });

  it("sits under the tabs, asks for one analysis per pair, and aligns on request", () => {
    const node = cab("cab-two", [IR_A, IR_B]);
    showNodeParamsPanel(node, presetWith(node));
    // Below both tab panels, in neither, so it shows whichever tab is open.
    const shown = section();
    const tabPanels = panel().querySelector(".node-param-tab-panels");
    expect(shown?.closest(".node-param-tab-panel")).toBeNull();
    expect(tabPanels && shown && tabPanels.compareDocumentPosition(shown) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(panel().querySelector('.node-param-tab[data-tab="alignment"]')).toBeNull();
    expect(panel().querySelector('[data-param-key="slotBOffset"].node-param-knob')).toBeNull();
    const requests = sent.filter((m) => m.type === "analyzeIrAlignment");
    expect(requests).toHaveLength(1);
    expect(requests[0]).toMatchObject({ irA: IR_A, irB: IR_B });

    const auto = panel().querySelector<HTMLButtonElement>(".ir-alignment-auto");
    expect(auto?.disabled).toBe(true);
    answer(requests[0], -0.0243, true);
    expect(auto?.disabled).toBe(false);
    expect(panel().querySelector(".ir-alignment-status")?.textContent).toBe("Lines up at -0.024 ms, B inverted");

    auto?.click();
    expect(paramUpdates("slotBOffset")).toEqual([-0.024]);
    expect(paramUpdates("slotBPolarity")).toEqual([1]);
    expect(node.params.slotBOffset).toBe(-0.024);
    expect(panel().querySelector(".ir-alignment-status")?.textContent).toBe("Lined up");
    // The IR B group's own Invert toggle follows.
    expect(panel().querySelector<HTMLInputElement>('.node-param-toggle[data-param-key="slotBPolarity"]')?.checked).toBe(true);

    // On purpose, off the aligned point: the nudges and the typed value.
    panel().querySelector<HTMLButtonElement>('[data-nudge="0.1"]')?.click();
    expect(node.params.slotBOffset).toBeCloseTo(0.076, 6);
    const input = panel().querySelector<HTMLInputElement>(".ir-alignment-offset-input");
    if (input) {
      input.value = "2.5";
      input.dispatchEvent(new Event("change"));
    }
    expect(node.params.slotBOffset).toBe(2.5);
    expect(panel().querySelector(".ir-alignment-distance")?.textContent).toBe("+2.50 ms · B 86 cm further away");

    // A rebuild of the panel does not ask again for the same IRs.
    sent = [];
    showNodeParamsPanel(node, presetWith(node));
    expect(sent.some((m) => m.type === "analyzeIrAlignment")).toBe(false);
    expect(panel().querySelector(".ir-alignment-status")?.textContent).toMatch(/^Lines up at/);
  });

  it("says when the blend leaves the offset unheard", () => {
    const node = cab("cab-blend", [IR_A, IR_B]);
    node.params.irBlend = 0;
    showNodeParamsPanel(node, presetWith(node));
    expect(panel().querySelector(".ir-alignment-note")?.textContent).toMatch(/IR Blend is all A/);
  });
});
