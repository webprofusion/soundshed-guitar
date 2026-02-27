const ICON_BASE = "images/icons";
import { EffectGuids, resolveEffectType } from "./effectGuids.js";

export type IconKey =
  | "amp"
  | "bolt"
  | "speaker"
  | "sliders"
  | "wave"
  | "clock"
  | "hall"
  | "wrench"
  | "door"
  | "meter"
  | "bulb"
  | "flame"
  | "blend"
  | "megaphone"
  | "split"
  | "mixer"
  | "note"
  | "gear"
  | "folder"
  | "flask"
  | "sparkle"
  | "microscope"
  | "link"
  | "package"
  | "mute";

export function renderIcon(icon: IconKey, className: string, title?: string): string {
  const titleAttr = title ? ` title=\"${title}\"` : "";
  return `<img class=\"${className}\" src=\"${ICON_BASE}/${icon}.svg\" alt=\"\" aria-hidden=\"true\"${titleAttr}>`;
}

const categoryIcons: Record<string, IconKey> = {
  amp:        "amp",
  cab:        "speaker",
  drive:      "flame",
  dynamics:   "bolt",
  eq:         "sliders",
  modulation: "wave",
  pitch:      "note",
  delay:      "clock",
  reverb:     "hall",
  synth:      "note",
  utility:    "wrench",
};

const effectIcons: Record<string, IconKey> = {
  // Dynamics
  [EffectGuids.kDynamicsGate]:     "door",
  [EffectGuids.kCompressorVca]:    "meter",
  [EffectGuids.kCompressorOpto]:   "bulb",
  [EffectGuids.kOverdrive]:        "flame",
  [EffectGuids.kDistortion]:       "flame",
  [EffectGuids.kFuzz]:             "flame",

  // Amps
  [EffectGuids.kAmpBuiltin]:       "amp",
  [EffectGuids.kAmpNam]:           "amp",
  [EffectGuids.kAmpNamOptimized]:  "amp",
  [EffectGuids.kFxNam]:            "sparkle",
  [EffectGuids.kAmpNamBlend]:      "blend",

  // Cabs
  [EffectGuids.kCabIr]:            "speaker",
  [EffectGuids.kCabSimple]:        "speaker",

  // EQ
  [EffectGuids.kEqParametric]:     "sliders",

  // Modulation
  [EffectGuids.kChorus]:           "wave",
  [EffectGuids.kFlanger]:          "wave",
  [EffectGuids.kPhaser]:           "wave",
  [EffectGuids.kTremolo]:          "wave",
  [EffectGuids.kAutoWah]:          "mixer",
  [EffectGuids.kOctave]:           "note",
  [EffectGuids.kPitchShift]:       "note",
  [EffectGuids.kTranspose]:        "note",

  // Delay
  [EffectGuids.kDelayDigital]:     "clock",
  [EffectGuids.kDelayDoubler]:     "clock",

  // Reverb
  [EffectGuids.kReverbRoom]:       "hall",
  [EffectGuids.kReverbHall]:       "hall",
  [EffectGuids.kReverbPlate]:      "hall",
  [EffectGuids.kReverbChamber]:    "hall",
  [EffectGuids.kReverbSpring]:     "hall",
  [EffectGuids.kReverbShimmer]:    "sparkle",
  [EffectGuids.kReverbAmbient]:    "sparkle",
  [EffectGuids.kReverbAdvanced]:   "sparkle",
  [EffectGuids.kReverbIr]:         "hall",

  // Synth
  [EffectGuids.kSynthSaw]:         "note",

  // Utility
  [EffectGuids.kGain]:             "megaphone",
  [EffectGuids.kSplitter]:         "split",
  [EffectGuids.kMixer]:            "mixer",
  [EffectGuids.kLimiterBrickwall]: "bolt",
};

export function getFxCategoryIcon(categoryId: string): string {
  const icon = categoryIcons[categoryId] ?? "gear";
  return renderIcon(icon, "fx-category-icon");
}

export function getFxEffectIcon(effectType: string): string {
  const icon = effectIcons[resolveEffectType(effectType)] ?? "gear";
  return renderIcon(icon, "fx-effect-icon");
}

export function getBadgeIcon(type: "resource" | "blend", titleOverride?: string): string {
  const icon = type === "resource" ? "folder" : "flask";
  const title = titleOverride ?? (type === "resource" ? "Requires resource" : "Custom blend");
  return renderIcon(icon, "fx-badge-icon", title);
}
