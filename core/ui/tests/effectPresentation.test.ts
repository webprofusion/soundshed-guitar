/**
 * The web UI's effect presentation now comes from core/ui/data/effect-presentation.json
 * (through the generated ts/generated/effectPresentation.ts), shared with Soundshed Guitar
 * Nano. These hold what it produces to the tables the web UI hard-coded before, so moving
 * onto the shared file changed nothing anyone sees.
 */
import { describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";

const { CATEGORY_METADATA } = await import("../ts/fxSelector.js");
const { getFxCategoryIcon, getFxEffectIcon } = await import("../ts/iconAssets.js");
const { getCategoryClass, getNodeCategory } = await import("../ts/signalPath/nodeTypes.js");
const {
  EFFECT_VISUAL_BACKGROUNDS,
  EFFECT_VISUAL_EQUIPMENT_IMAGES,
  EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE,
} = await import("../ts/signalPath/visualization.js");
const { BLEND_CATEGORIES, DEFAULT_BLEND_CATEGORY } = await import("../ts/generated/effectPresentation.js");

const iconOf = (html: string) => /images\/icons\/([\w-]+)\.svg/.exec(html)?.[1];

describe("the shared presentation table reproduces the web UI's old tables", () => {
  it("FX library categories: names, colours and order", () => {
    expect(Object.entries(CATEGORY_METADATA)).toEqual([
      ["amp", { name: "Amplifiers", color: "#e07848" }],
      ["cab", { name: "Cabinets", color: "#a86830" }],
      ["drive", { name: "Drive", color: "#e04848" }],
      ["dynamics", { name: "Dynamics", color: "#e08030" }],
      ["eq", { name: "Equalizers", color: "#48a8e0" }],
      ["modulation", { name: "Modulation", color: "#9048e0" }],
      ["pitch", { name: "Pitch", color: "#c040e0" }],
      ["delay", { name: "Delay", color: "#48e0a8" }],
      ["reverb", { name: "Reverb", color: "#4878e0" }],
      ["synth", { name: "Synth", color: "#7a8a02" }],
      ["utility", { name: "Utility", color: "#808080" }],
    ]);
  });

  it("the effect view's backgrounds, as the old gradient strings", () => {
    expect(EFFECT_VISUAL_BACKGROUNDS.amp).toBe("linear-gradient(145deg, rgba(44, 62, 94, 0.92) 0%, rgba(15, 20, 32, 0.96) 100%)");
    expect(EFFECT_VISUAL_BACKGROUNDS).toEqual({
      amp: "linear-gradient(145deg, rgba(44, 62, 94, 0.92) 0%, rgba(15, 20, 32, 0.96) 100%)",
      cab: "linear-gradient(145deg, rgba(62, 76, 96, 0.92) 0%, rgba(16, 20, 30, 0.96) 100%)",
      eq: "linear-gradient(145deg, rgba(56, 96, 132, 0.95) 0%, rgba(18, 24, 44, 0.95) 100%)",
      dynamics: "linear-gradient(145deg, rgba(132, 64, 64, 0.95) 0%, rgba(38, 18, 24, 0.95) 100%)",
      modulation: "linear-gradient(145deg, rgba(88, 64, 132, 0.95) 0%, rgba(26, 18, 44, 0.95) 100%)",
      delay: "linear-gradient(145deg, rgba(64, 132, 112, 0.95) 0%, rgba(18, 34, 38, 0.95) 100%)",
      reverb: "linear-gradient(145deg, rgba(64, 92, 132, 0.95) 0%, rgba(18, 24, 38, 0.95) 100%)",
      channel: "linear-gradient(145deg, rgba(148, 108, 48, 0.95) 0%, rgba(38, 28, 12, 0.95) 100%)",
      utility: "linear-gradient(145deg, rgba(86, 86, 96, 0.95) 0%, rgba(26, 26, 30, 0.95) 100%)",
    });
  });

  it("stock artwork by category and by effect type, at the web UI's paths", () => {
    expect(EFFECT_VISUAL_EQUIPMENT_IMAGES).toEqual({
      amp: "../images/equipment/amps/full-rig-1.jpg",
      cab: "../images/equipment/cabs/cab-02.png",
      delay: "../images/equipment/fx/studio-rack-delay.png",
      reverb: "../images/equipment/fx/studio-rack-reverb.png",
    });
    expect(EFFECT_VISUAL_EQUIPMENT_IMAGES_BY_TYPE).toEqual({
      [EffectGuids.kPluginHost]: "../images/equipment/fx/studio-rack-multifx.png",
      [EffectGuids.kDelayDigital]: "../images/equipment/fx/studio-rack-delay.png",
      [EffectGuids.kDelayTape]: "../images/equipment/fx/studio-rack-delay.png",
      [EffectGuids.kDelayAnalog]: "../images/equipment/fx/studio-rack-delay.png",
      [EffectGuids.kDelayDoubler]: "../images/equipment/fx/studio-rack-delay.png",
      [EffectGuids.kFxNam]: "../images/equipment/pedals/colourful-pedal2.png",
      fx_nam: "../images/equipment/pedals/colourful-pedal2.png",
      [EffectGuids.kWasmHost]: "../images/equipment/pedals/colourful-pedal2.png",
      wasm_host: "../images/equipment/pedals/colourful-pedal2.png",
    });
  });

  it("icons by category and by effect, with gear for anything unlisted", () => {
    const categoryIcons = ["amp", "cab", "drive", "dynamics", "eq", "modulation", "pitch", "delay", "reverb", "synth", "utility", "channel", "unknown"]
      .map((id) => iconOf(getFxCategoryIcon(id)));
    expect(categoryIcons).toEqual(["amp", "speaker", "flame", "bolt", "sliders", "wave", "note", "delay", "reverb", "note", "wrench", "gear", "gear"]);

    const effectIcons: Array<[string, string]> = [
      [EffectGuids.kDynamicsGate, "door"], [EffectGuids.kCompressorVca, "meter"], [EffectGuids.kCompressorOpto, "bulb"],
      [EffectGuids.kOverdrive, "flame"], [EffectGuids.kAmpNamOptimized, "amp"], [EffectGuids.kFxNam, "pedal"],
      [EffectGuids.kAmpNamBlend, "blend"], [EffectGuids.kCabSimple, "speaker"], [EffectGuids.kEqGraphic, "sliders"],
      [EffectGuids.kRingMod, "wave"], [EffectGuids.kWah, "mixer"], [EffectGuids.kTranspose, "note"],
      [EffectGuids.kDelayDoubler, "doubler"], [EffectGuids.kReverbAdvanced, "reverb-advanced"],
      [EffectGuids.kReverbAmbient, "reverb-ambient"], [EffectGuids.kSynthSaw, "note"], [EffectGuids.kGain, "megaphone"],
      [EffectGuids.kSplitter, "split"], [EffectGuids.kInputAnalyzer, "microscope"], [EffectGuids.kLimiterBrickwall, "bolt"],
      [EffectGuids.kPluginHost, "gear"], [EffectGuids.kWasmHost, "gear"], ["no-such-effect", "gear"],
      // A legacy id resolves to its effect first.
      ["amp_nam", "amp"],
    ];
    expect(effectIcons.map(([type]) => [type, iconOf(getFxEffectIcon(type))])).toEqual(effectIcons);
  });

  it("chain node classes and the gear categories shown as amps", () => {
    const classes = ["dynamics", "amp", "pedal", "preamp", "full-rig", "channel", "cab", "eq", "modulation", "delay", "reverb", "utility", "drive", "pitch", "synth", ""]
      .map(getCategoryClass);
    expect(classes).toEqual(["dynamics", "amp", "amp", "amp", "amp", "amp", "cab", "eq", "modulation", "delay", "reverb", "utility", "utility", "utility", "utility", "utility"]);

    expect(getNodeCategory({ id: "n", type: "x", category: "pedal", params: {} } as never)).toBe("pedal");
  });

  it("blends by gear category", () => {
    const listed = ["pedal", "preamp", "amp", "full-rig", "cab", "other"].map((category) => BLEND_CATEGORIES[category] ?? DEFAULT_BLEND_CATEGORY);
    expect(listed).toEqual(["utility", "amp", "amp", "amp", "cab", "amp"]);
  });
});
