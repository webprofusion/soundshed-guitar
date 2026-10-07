import { describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";
import type { ParameterDef } from "../ts/presetV2.js";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `<div id="node-params-panel"></div>`;
window.IPlugSendMsg = () => undefined;

const { formatParamValue } = await import("../ts/layoutRenderer.js");
const { buildDefaultParamControlsHtml } = await import("../ts/parameterControlMarkup.js");
const { enumLabel, wholeStepLabel } = await import("../ts/paramLabels.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");

/** The Auto Arpeggiator's Steps, as the engine declares it: 2 to 8, a label per value. */
const steps: ParameterDef = {
  key: "numSteps",
  name: "Steps",
  default: 4,
  min: 2,
  max: 8,
  unit: "enum",
  step: 1,
  labels: ["2", "3", "4", "5", "6", "7", "8"],
};

/** An enum from 0, as most are. */
const direction: ParameterDef = {
  key: "direction",
  name: "Direction",
  default: 0,
  min: 0,
  max: 2,
  unit: "enum",
  step: 1,
  labels: ["Up", "Down", "Up-Down"],
};

describe("enum labels", () => {
  it("counts labels from the minimum, so Steps shows its own value", () => {
    // Indexed by the value, 4 showed "6", 2 showed "4" and 7 and 8 had no label at all.
    expect([2, 3, 4, 5, 8].map((v) => enumLabel(v, steps.labels!, steps.min, steps.step))).toEqual([
      "2",
      "3",
      "4",
      "5",
      "8",
    ]);
  });

  it("is unchanged for an enum that starts at 0, with or without a step", () => {
    const models = ["TS", "Rat", "Muff"];
    expect(enumLabel(1.2, models)).toBe("Rat");
    expect(enumLabel(2, models, 0, 1)).toBe("Muff");
    expect(enumLabel(1, models, 0, 0)).toBe("Rat");
  });

  it("has no label outside the list", () => {
    expect(enumLabel(1, steps.labels!, 2, 1)).toBeUndefined();
    expect(enumLabel(9, steps.labels!, 2, 1)).toBeUndefined();
  });

  it("formats the value text the same way, falling back to the value", () => {
    expect(formatParamValue(4, "enum", steps.labels, undefined, 2, 1)).toBe("4");
    expect(formatParamValue(12, "enum", steps.labels, undefined, 2, 1)).toBe("12");
  });

  it("renders the default panel's knob text from the minimum", () => {
    const html = buildDefaultParamControlsHtml([steps]);
    expect(html).toContain(`<span class="node-param-value">4</span>`);
  });

  it("shows each value on the params panel's bound knobs", () => {
    EffectTypeRegistry.register("test-arp", {
      type: "test-arp",
      displayName: "Test Arp",
      category: "modulation",
      parameters: [direction, steps],
    });
    const panel = document.getElementById("node-params-panel")!;
    const knobText = (key: string) =>
      panel.querySelector(`.knob[data-param-key="${key}"]`)?.parentElement?.querySelector(".node-param-value")
        ?.textContent;

    for (const [value, shown] of [[2, "2"], [4, "4"], [8, "8"]] as const) {
      const node: GraphNode = {
        id: "arp",
        type: "test-arp",
        displayName: "",
        category: "modulation",
        bypassed: false,
        params: { numSteps: value, direction: 2 },
        config: {},
      };
      const preset: Preset = {
        id: "p",
        name: "P",
        graph: { nodes: [node], edges: [] },
      } as Preset;
      showNodeParamsPanel(node, preset);
      expect([knobText("numSteps"), knobText("direction")]).toEqual([shown, "Up-Down"]);
    }
  });
});

/** Guitar to MIDI's MIDI Channel and Transpose, as the engine declares them. */
const channel: ParameterDef = { key: "channel", name: "MIDI Channel", default: 1, min: 1, max: 16, unit: "", step: 1 };
const transpose: ParameterDef = { key: "transpose", name: "Transpose", default: 0, min: -24, max: 24, unit: "st", step: 1 };

describe("whole-step values", () => {
  it("shows a whole number with its unit, where the channel showed 1.00", () => {
    expect(wholeStepLabel(1, "", 1)).toBe("1");
    expect(wholeStepLabel(-12, "st", 1)).toBe("-12st");
    expect(wholeStepLabel(-1e-9, "st", 1)).toBe("0st");
    expect(wholeStepLabel(3, "amount", 1)).toBe("3");
    expect(formatParamValue(16, "", undefined, undefined, 1, 1)).toBe("16");
    expect(formatParamValue(7, "st", undefined, undefined, -24, 1)).toBe("7st");
  });

  it("leaves fractional steps, values between steps, enums and toggles alone", () => {
    expect(wholeStepLabel(5, "", 2.5)).toBeUndefined();
    expect(wholeStepLabel(0.5, "st", 1)).toBeUndefined();
    expect(wholeStepLabel(1, "enum", 1)).toBeUndefined();
    expect(wholeStepLabel(1, "toggle", 1)).toBeUndefined();
    expect(wholeStepLabel(1, "", undefined)).toBeUndefined();
    expect(formatParamValue(0.5, "amount", undefined, undefined, 0, 0.01)).toBe("0.50");
    expect(formatParamValue(-3, "dB", undefined, undefined, -24, 0.1)).toBe("-3.0dB");
  });

  it("shows the same text on the params panel's bound knobs", () => {
    EffectTypeRegistry.register("test-midi", {
      type: "test-midi",
      displayName: "Test MIDI",
      category: "synth",
      parameters: [channel, transpose],
    });
    const panel = document.getElementById("node-params-panel")!;
    const knobText = (key: string) =>
      panel.querySelector(`.knob[data-param-key="${key}"]`)?.parentElement?.querySelector(".node-param-value")
        ?.textContent;
    const node: GraphNode = {
      id: "g2m",
      type: "test-midi",
      displayName: "",
      category: "synth",
      bypassed: false,
      params: { channel: 10, transpose: -12 },
      config: {},
    };
    showNodeParamsPanel(node, { id: "p", name: "P", graph: { nodes: [node], edges: [] } } as Preset);
    expect([knobText("channel"), knobText("transpose")]).toEqual(["10", "-12st"]);
  });
});
