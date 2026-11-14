import { afterEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import {
  allowsPresetMapping,
  buildMidiLearnMenuItems,
  countCustomSlots,
  countPresetSlots,
  CURRENT_TARGET_GROUP,
  describeMidiMap,
  findPresetSlotForAddress,
  findSlotForAddress,
  formatMidiChannel,
  isBypassAddress,
  nextCustomSlotId,
  nextPresetSlotId,
  nodeIdFromSelector,
  nodeSelectorFor,
  resolveMidiLearnTarget,
  withCurrentTargetOption,
} from "../ts/midiMapping.js";
import type { AutomationTargetOption, MidiLearnMenuState, MidiLearnTargetContext } from "../ts/midiMapping.js";
import { EffectTypeRegistry } from "../ts/presetV2.js";
import type { EffectTypeInfo } from "../ts/presetV2.js";
import type { AutomationRegistryEntry, AutomationSlot, GraphNode } from "../ts/types.js";

const AMP_TYPE = "0f3c3c1e-amp";

const REGISTRY_LABELS: Record<string, string> = {
  "global.inputTrim": "Input Trim",
  "global.outputTrim": "Output Trim",
  "setlist.preset1": "Setlist Preset 1",
  "setlist.preset8": "Setlist Preset 8",
  "setlist.bankUp": "Bank Up",
  "setlist.bankDown": "Bank Down",
  "setlist.bankSelect": "Select Bank",
};

const registry: AutomationRegistryEntry[] = Object.entries(REGISTRY_LABELS).map(([address, label]) => ({
  address,
  label,
  unit: "",
  min: 0,
  max: 1,
  isStepped: false,
  isTrigger: false,
}));

const effects = new Map<string, EffectTypeInfo>([
  [AMP_TYPE, {
    type: AMP_TYPE,
    displayName: "Amp",
    category: "amp",
    requiresResource: false,
    parameters: [
      { key: "gain", name: "Gain", default: 5, min: 0, max: 10, unit: "amount" },
      { key: "bright", name: "Bright", default: 0, min: 0, max: 1, unit: "toggle" },
    ],
  } as EffectTypeInfo],
  [EffectGuids.kPluginHost, {
    type: EffectGuids.kPluginHost,
    displayName: "Plugin",
    category: "utility",
    requiresResource: false,
    parameters: [{ key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }],
  } as EffectTypeInfo],
  // The chain's boundary nodes, as ROUTING_NODE_EFFECTS declares them.
  ["input", {
    type: "input",
    displayName: "Input",
    category: "utility",
    requiresResource: false,
    parameters: [{ key: "gainDb", name: "Gain", default: 0, min: -24, max: 24, unit: "dB" }],
  } as EffectTypeInfo],
  ["output", {
    type: "output",
    displayName: "Output",
    category: "utility",
    requiresResource: false,
    parameters: [{ key: "gainDb", name: "Gain", default: 0, min: -24, max: 24, unit: "dB" }],
  } as EffectTypeInfo],
]);

const aliases = new Map([["amp_legacy", AMP_TYPE]]);

const nodes = new Map<string, GraphNode>([
  ["amp1", { id: "amp1", type: "amp_legacy", params: {} } as GraphNode],
  ["amp2", { id: "amp2", type: AMP_TYPE, title: "Clean Amp", params: {} } as GraphNode],
  ["plugin1", { id: "plugin1", type: EffectGuids.kPluginHost, params: {} } as GraphNode],
  ["__input__", { id: "__input__", type: "input", params: {} } as GraphNode],
  ["__output__", { id: "__output__", type: "output", params: {} } as GraphNode],
]);

const context: MidiLearnTargetContext = {
  registry,
  findNode: (nodeId) => nodes.get(nodeId),
  getEffectInfo: (type) => effects.get(aliases.get(type) ?? type),
};

function resolveAt(selector: string) {
  return resolveMidiLearnTarget(document.querySelector(selector), context);
}

function slot(slotId: string, address: string, isDefault: boolean): AutomationSlot {
  return { slotId, label: slotId, address, isDefault, value: 0 };
}

afterEach(() => {
  document.body.innerHTML = "";
});

describe("resolveMidiLearnTarget", () => {
  it("maps the input and output knobs to the global trims, from the knob or its readout", () => {
    document.body.innerHTML = `
      <div class="knob-control"><div class="knob" data-param="input"><div class="knob-indicator" id="in-indicator"></div></div><span class="knob-value" id="in-value">+0.0 dB</span></div>
      <div class="knob-control"><div class="knob" data-param="output" id="out-knob"></div></div>
      <div class="knob-control"><div class="knob" data-param="transpose" id="transpose-knob"></div></div>
    `;
    expect(resolveAt("#in-indicator")).toEqual({ address: "global.inputTrim", label: "Input Trim" });
    expect(resolveAt("#in-value")?.address).toBe("global.inputTrim");
    expect(resolveAt("#out-knob")?.address).toBe("global.outputTrim");
    // Transpose is not an automation address.
    expect(resolveAt("#transpose-knob")).toBeNull();
  });

  it("maps an effect parameter to its canonical type address, from the control, its label or a toggle's slider", () => {
    document.body.innerHTML = `
      <div class="node-param-group">
        <span class="node-param-label" id="gain-label">Gain</span>
        <div class="knob node-param-knob" id="gain-knob" data-node-id="amp1" data-param-key="gain"></div>
      </div>
      <div class="node-param-group">
        <label class="toggle-switch">
          <input class="node-param-toggle" type="checkbox" data-node-id="amp1" data-param-key="bright">
          <span class="toggle-slider" id="bright-slider"></span>
        </label>
      </div>
    `;
    expect(resolveAt("#gain-knob")).toEqual({ address: `node.${AMP_TYPE}.gain`, label: "Amp: Gain", nodeId: "amp1", nodeLabel: "Amp: Gain" });
    expect(resolveAt("#gain-label")?.address).toBe(`node.${AMP_TYPE}.gain`);
    expect(resolveAt("#bright-slider")).toMatchObject({ address: `node.${AMP_TYPE}.bright`, label: "Amp: Bright", nodeId: "amp1" });
  });

  it("maps the chain's Input and Output node gains, which a preset can map for itself", () => {
    document.body.innerHTML = `
      <div class="node-param-group">
        <span class="node-param-label" id="in-gain-label">Gain</span>
        <div class="knob node-param-knob" id="in-gain" data-node-id="__input__" data-param-key="gainDb"></div>
      </div>
      <div class="knob node-param-knob" id="out-gain" data-node-id="__output__" data-param-key="gainDb"></div>
    `;
    expect(resolveAt("#in-gain")).toMatchObject({ address: "node.input.gainDb", label: "Input: Gain", nodeId: "__input__" });
    expect(resolveAt("#in-gain-label")?.address).toBe("node.input.gainDb");
    expect(resolveAt("#out-gain")).toMatchObject({ address: "node.output.gainDb", label: "Output: Gain", nodeId: "__output__" });
    expect(allowsPresetMapping("node.input.gainDb")).toBe(true);
    expect(allowsPresetMapping("node.output.gainDb")).toBe(true);
  });

  it("maps an effect's on/off button to its enabled address, naming the node for a per-preset mapping", () => {
    document.body.innerHTML = `
      <div class="signal-node" data-node-id="amp2"><button class="signal-node-bypass" id="tile-bypass"><span id="tile-icon"></span></button></div>
      <button class="node-bypass-btn" data-node-id="amp1" id="panel-bypass"><span id="panel-track"></span></button>
      <div class="signal-node" data-node-id="missing"><button class="signal-node-bypass" id="missing-bypass"></button></div>
    `;
    expect(resolveAt("#tile-icon")).toEqual({
      address: `node.${AMP_TYPE}.enabled`,
      label: "Amp: On/Off",
      nodeId: "amp2",
      nodeLabel: "Clean Amp: On/Off",
    });
    expect(resolveAt("#panel-track")).toMatchObject({ address: `node.${AMP_TYPE}.enabled`, nodeId: "amp1" });
    expect(resolveAt("#missing-bypass")).toBeNull();
    expect(isBypassAddress(`node.${AMP_TYPE}.enabled`)).toBe(true);
    expect(isBypassAddress(`node.${AMP_TYPE}.gain`)).toBe(false);
  });

  it("names the node a per-preset parameter mapping drives", () => {
    document.body.innerHTML = `<div class="knob node-param-knob" id="gain2" data-node-id="amp2" data-param-key="gain"></div>`;
    expect(resolveAt("#gain2")).toEqual({ address: `node.${AMP_TYPE}.gain`, label: "Amp: Gain", nodeId: "amp2", nodeLabel: "Clean Amp: Gain" });
  });

  it("does not guess which control a shared group's label belongs to", () => {
    document.body.innerHTML = `
      <div class="node-param-group">
        <span id="shared-label">Channel</span>
        <div class="knob node-param-knob" id="first" data-node-id="amp1" data-param-key="gain"></div>
        <div class="knob node-param-knob" data-node-id="amp1" data-param-key="bright"></div>
      </div>
    `;
    expect(resolveAt("#shared-label")).toBeNull();
    expect(resolveAt("#first")?.address).toBe(`node.${AMP_TYPE}.gain`);
  });

  it("offers nothing a node address cannot drive", () => {
    document.body.innerHTML = `
      <div class="knob node-param-knob" id="missing-node" data-node-id="gone" data-param-key="gain"></div>
      <div class="knob node-param-knob" id="undeclared" data-node-id="amp1" data-param-key="presence"></div>
      <div class="knob node-param-knob" id="plugin" data-node-id="plugin1" data-param-key="mix"></div>
      <div class="node-param-group">
        <div class="knob node-param-knob" data-node-id="amp1" data-param-key="gain"></div>
        <span class="node-param-value"><input type="number" class="knob-inline-editor" id="inline-editor"></span>
      </div>
    `;
    expect(resolveAt("#missing-node")).toBeNull();
    expect(resolveAt("#undeclared")).toBeNull();
    expect(resolveAt("#plugin")).toBeNull();
    expect(resolveAt("#inline-editor")).toBeNull();
  });

  it("maps setlist pads, setlist slots and the bank controls", () => {
    document.body.innerHTML = `
      <button class="performance-setlist-pad" data-performance-action="setlist-slot" data-slot-index="0"><span id="pad-name">Clean</span></button>
      <button class="performance-setlist-pad" data-performance-action="setlist-slot" data-slot-index="3" id="unregistered-pad"></button>
      <div class="setlist-slot" data-slot-index="7" id="library-slot"></div>
      <button data-performance-action="bank-up" id="bank-up"><svg></svg></button>
      <button data-performance-action="bank-down" id="bank-down"></button>
      <div class="performance-bank-current"><strong id="bank-name">Bank 1</strong></div>
      <div class="performance-bank-current performance-bank-editor"><span id="bank-editor">Name</span></div>
    `;
    expect(resolveAt("#pad-name")).toEqual({ address: "setlist.preset1", label: "Setlist Preset 1" });
    expect(resolveAt("#library-slot")?.address).toBe("setlist.preset8");
    // Only addresses the engine registers are offered.
    expect(resolveAt("#unregistered-pad")).toBeNull();
    expect(resolveAt("#bank-up")?.address).toBe("setlist.bankUp");
    expect(resolveAt("#bank-down")?.address).toBe("setlist.bankDown");
    expect(resolveAt("#bank-name")?.address).toBe("setlist.bankSelect");
    expect(resolveAt("#bank-editor")).toBeNull();
  });

  it("offers nothing for an element that drives nothing", () => {
    document.body.innerHTML = `<div id="plain">Hello</div>`;
    expect(resolveAt("#plain")).toBeNull();
    expect(resolveMidiLearnTarget(null, context)).toBeNull();
  });
});

describe("slot lookup", () => {
  it("prefers the default slot for an address over a custom slot pointed at it", () => {
    const slots = [
      slot("custom.1", "global.inputTrim", false),
      slot("default.inputLevel", "global.inputTrim", true),
      slot("custom.2", "node.amp.gain", false),
    ];
    expect(findSlotForAddress(slots, "global.inputTrim")?.slotId).toBe("default.inputLevel");
    expect(findSlotForAddress(slots, "node.amp.gain")?.slotId).toBe("custom.2");
    expect(findSlotForAddress(slots, "node.amp.bright")).toBeUndefined();
    expect(countCustomSlots(slots)).toBe(2);
  });

  it("picks a custom id past every numbered one in use", () => {
    expect(nextCustomSlotId([])).toBe("custom.1");
    expect(nextCustomSlotId([
      slot("default.bankUp", "setlist.bankUp", true),
      slot("custom.4", "", false),
      slot("custom.54321", "", false),
      slot("custom.named", "", false),
    ])).toBe("custom.54322");
  });
});

describe("buildMidiLearnMenuItems", () => {
  const mapped: AutomationSlot = {
    ...slot("custom.3", "node.amp.gain", false),
    midiMap: { eventType: 0, channel: 0, controller: 7, mode: 0, sensitivity: 0.1, pickupRange: 0.1 },
  };
  const noMappings: MidiLearnMenuState = {
    address: "node.amp.gain",
    slot: undefined,
    presetSlot: undefined,
    presetId: null,
    armedSlotId: null,
    customSlotCount: 0,
    maxCustomSlots: 16,
    presetSlotCount: 0,
    maxPresetSlots: 16,
  };
  const menu = (state: Partial<MidiLearnMenuState>) => buildMidiLearnMenuItems({ ...noMappings, ...state })
    .map((item) => `${item.action}${item.disabled ? " (disabled)" : ""}${item.hint ? `: ${item.hint}` : ""}`);

  it("offers only learn when the control has no MIDI mapping", () => {
    expect(menu({ slot: undefined, armedSlotId: null, customSlotCount: 2, maxCustomSlots: 16 })).toEqual(["learn"]);
    // A slot that already drives the control needs no free custom slot.
    expect(menu({ slot: slot("default.bankUp", "setlist.bankUp", true), armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn"]);
  });

  it("offers Clear Mapping, naming the mapping, when the slot has one", () => {
    expect(menu({ slot: mapped, armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn", "clear: CC 7 ch1 Abs"]);
  });

  it("swaps learn for cancel while that slot listens, and keeps Clear Mapping", () => {
    expect(menu({ slot: mapped, armedSlotId: "custom.3", customSlotCount: 3, maxCustomSlots: 16 })).toEqual(["cancel: Listening…", "clear: CC 7 ch1 Abs"]);
    // Another slot listening does not change this control's menu.
    expect(menu({ slot: mapped, armedSlotId: "default.bankUp", customSlotCount: 3, maxCustomSlots: 16 })).toEqual(["learn", "clear: CC 7 ch1 Abs"]);
  });

  it("disables learn on an undriven control when every custom slot is taken", () => {
    expect(menu({ slot: undefined, armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn (disabled): All 16 custom slots in use"]);
  });

  it("offers a per-preset learn only while a preset is active", () => {
    expect(menu({ presetId: "presetA" })).toEqual(["learn", "learnPreset"]);
    expect(menu({ presetId: null })).toEqual(["learn"]);
  });

  it("offers a per-preset learn only on the signal chain's controls", () => {
    expect(menu({ presetId: "presetA", address: "global.inputTrim" })).toEqual(["learn", "learnPreset"]);
    expect(menu({ presetId: "presetA", address: "global.outputTrim" })).toEqual(["learn", "learnPreset"]);
    expect(menu({ presetId: "presetA", address: "setlist.preset1" })).toEqual(["learn"]);
    expect(menu({ presetId: "presetA", address: "setlist.bankUp" })).toEqual(["learn"]);
    expect(menu({ presetId: "presetA", address: "setlist.bankSelect" })).toEqual(["learn"]);
    expect(allowsPresetMapping("node.amp.gain")).toBe(true);
    expect(allowsPresetMapping("global.transpose")).toBe(false);
  });

  it("still clears a per-preset mapping on a control that no longer offers one", () => {
    const bankUpForPreset: AutomationSlot = {
      ...slot("preset.2", "setlist.bankUp", false),
      presetId: "presetA",
      midiMap: { eventType: 0, channel: 0, controller: 20, mode: 0, sensitivity: 0.1, pickupRange: 0.1 },
    };
    expect(menu({ presetId: "presetA", address: "setlist.bankUp", presetSlot: bankUpForPreset }))
      .toEqual(["learn", "clear: CC 20 ch1 Abs · this preset"]);
  });

  it("clears the active preset's own mapping before the global one", () => {
    const presetMapped: AutomationSlot = {
      ...slot("preset.1", "node.amp.gain", false),
      presetId: "presetA",
      midiMap: { eventType: 0, channel: 0, controller: 11, mode: 0, sensitivity: 0.1, pickupRange: 0.1 },
    };
    expect(menu({ slot: mapped, presetSlot: presetMapped, presetId: "presetA" }))
      .toEqual(["learn", "learnPreset", "clear: CC 11 ch1 Abs · this preset"]);
    expect(menu({ slot: mapped, presetSlot: undefined, presetId: "presetA" }))
      .toEqual(["learn", "learnPreset", "clear: CC 7 ch1 Abs"]);
    // While the preset's slot listens: cancel, and the preset's clear still first.
    expect(menu({ slot: mapped, presetSlot: presetMapped, presetId: "presetA", armedSlotId: "preset.1" }))
      .toEqual(["cancel: Listening…", "clear: CC 11 ch1 Abs · this preset"]);
  });

  it("offers a latch and a toggle learn for this preset on an effect's on/off", () => {
    const labels = (state: Partial<MidiLearnMenuState>) => buildMidiLearnMenuItems({ ...noMappings, ...state }).map((item) => item.label);
    expect(menu({ presetId: "presetA", address: "node.gain.enabled" }))
      .toEqual(["learn", "learnPreset: 127 on, 0 off", "learnPresetToggle: Each press switches it"]);
    expect(labels({ presetId: "presetA", address: "node.gain.enabled" }))
      .toEqual(["MIDI Learn…", "MIDI Learn latch for this preset…", "MIDI Learn toggle for this preset…"]);
    expect(menu({ presetId: null, address: "node.gain.enabled" })).toEqual(["learn"]);
    expect(menu({ presetId: "presetA", address: "node.gain.enabled", presetSlotCount: 16, maxPresetSlots: 16 })).toEqual([
      "learn",
      "learnPreset (disabled): All 16 preset slots in use",
      "learnPresetToggle (disabled): All 16 preset slots in use",
    ]);
  });

  it("disables the per-preset learn when the preset's slots are all taken", () => {
    expect(menu({ presetId: "presetA", presetSlotCount: 16, maxPresetSlots: 16 }))
      .toEqual(["learn", "learnPreset (disabled): All 16 preset slots in use"]);
  });
});

describe("per-preset slots", () => {
  const presetSlot = (slotId: string, presetId: string, address: string): AutomationSlot => ({ ...slot(slotId, address, false), presetId });
  const slots = [
    slot("default.inputLevel", "global.inputTrim", true),
    slot("custom.1", "node.amp.gain", false),
    presetSlot("preset.1", "presetA", "node.amp.gain"),
    presetSlot("preset.7", "presetB", "node.amp.gain"),
  ];

  it("stay out of global lookups and the custom slot count", () => {
    expect(findSlotForAddress(slots, "node.amp.gain")?.slotId).toBe("custom.1");
    expect(findSlotForAddress([presetSlot("preset.1", "presetA", "node.amp.bass")], "node.amp.bass")).toBeUndefined();
    expect(countCustomSlots(slots)).toBe(1);
  });

  it("are found and counted per preset, with ids of their own", () => {
    expect(findPresetSlotForAddress(slots, "presetB", "node.amp.gain")?.slotId).toBe("preset.7");
    expect(findPresetSlotForAddress(slots, "presetC", "node.amp.gain")).toBeUndefined();
    expect(countPresetSlots(slots, "presetA")).toBe(1);
    expect(nextPresetSlotId(slots)).toBe("preset.8");
    expect(nextCustomSlotId(slots)).toBe("custom.2");
  });

  it("are found per node when they name one, so two effects of one type keep their own", () => {
    const tied = (slotId: string, nodeId: string): AutomationSlot => ({ ...presetSlot(slotId, "presetA", "node.amp.enabled"), nodeSelector: nodeSelectorFor(nodeId) });
    const amps = [tied("preset.2", "amp1"), tied("preset.3", "amp2")];
    expect(findPresetSlotForAddress(amps, "presetA", "node.amp.enabled", "amp1")?.slotId).toBe("preset.2");
    expect(findPresetSlotForAddress(amps, "presetA", "node.amp.enabled", "amp2")?.slotId).toBe("preset.3");
    // Another node of the type has none yet.
    expect(findPresetSlotForAddress(amps, "presetA", "node.amp.enabled", "amp3")).toBeUndefined();
    // A mapping from before mappings named their node is found, to be tied to the one learned on.
    const legacy = [presetSlot("preset.4", "presetA", "node.amp.enabled")];
    expect(findPresetSlotForAddress(legacy, "presetA", "node.amp.enabled", "amp1")?.slotId).toBe("preset.4");
    expect(nodeIdFromSelector("id:amp1")).toBe("amp1");
    expect(nodeIdFromSelector("2")).toBeNull();
    expect(nodeIdFromSelector(undefined)).toBeNull();
  });
});

describe("describeMidiMap", () => {
  it("shows 1-based channels, and the mode only when there is one", () => {
    expect(describeMidiMap({ eventType: 0, channel: 0, controller: 7, mode: 0 })).toBe("CC 7 ch1 Abs");
    expect(describeMidiMap({ eventType: 2, channel: 9, controller: 60 })).toBe("NoteOn 60 ch10");
    expect(formatMidiChannel(-1)).toBe("any");
  });
});

describe("target picker options", () => {
  const catalog: AutomationTargetOption[] = [
    { address: "global.inputTrim", label: "Input Trim", group: "Global" },
    { address: `node.${AMP_TYPE}.gain`, label: "Amp: Gain", group: "amp" },
  ];
  // "MIDI Learn…" on a blend node's parameter points a custom slot at this address,
  // but the picker never builds an option for it.
  const blendAddress = `node.${EffectGuids.kAmpNamBlend}.blend`;

  it("is built from a catalog the blend type is hidden from", () => {
    expect(EffectTypeRegistry.get(EffectGuids.kAmpNamBlend)?.catalogHidden).toBe(true);
  });

  it("carries an address the catalog leaves out, so Apply cannot retarget the slot", () => {
    const options = withCurrentTargetOption(catalog, blendAddress, "NAM Blend: Blend");
    // A <select> with no option matching the slot's address selects its first one, and
    // Apply then posts that address — the option has to be there, and has to be first.
    expect(options[0]).toEqual({ address: blendAddress, label: "NAM Blend: Blend", group: CURRENT_TARGET_GROUP });
    expect(options.slice(1)).toEqual(catalog);
  });

  it("leaves the catalog alone when it already offers the slot's address", () => {
    expect(withCurrentTargetOption(catalog, `node.${AMP_TYPE}.gain`, "Amp: Gain")).toEqual(catalog);
  });

  it("leaves a freshly added slot, pointed at nothing, picking from the catalog", () => {
    expect(withCurrentTargetOption(catalog, "", "Custom")).toEqual(catalog);
  });
});
