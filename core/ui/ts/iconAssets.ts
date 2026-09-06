const ICON_BASE = "/images/icons";
import { EffectGuids, resolveEffectType } from "./effectGuids.js";

export type IconKey =
  | "amp"
  | "guitar"
  | "pedal"
  | "bolt"
  | "speaker"
  | "output"
  | "sliders"
  | "layout"
  | "wave"
  | "delay"
  | "doubler"
  | "reverb"
  | "reverb-advanced"
  | "reverb-ambient"
  | "wrench"
  | "door"
  | "meter"
  | "bulb"
  | "flame"
  | "blend"
  | "megaphone"
  | "split"
  | "parallel-split"
  | "parallel-join"
  | "mixer"
  | "note"
  | "gear"
  | "folder"
  | "flask"
  | "microscope"
  | "link"
  | "package"
  | "mute"
  | "close"
  | "plus"
  | "trash"
  | "star"
  | "heart"
  | "heart-filled"
  | "chevron-down"
  | "chevron-left"
  | "chevron-right"
  | "arrow-left"
  | "arrow-right"
  | "play"
  | "stop"
  | "check"
  | "x"
  | "settings";

export function renderIcon(icon: IconKey, className: string, title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<span class="${className}" style="--icon-url: url('${ICON_BASE}/${icon}.svg')" aria-hidden="true"${titleAttr}></span>`;
}

const categoryIcons: Record<string, IconKey> = {
  amp:        "amp",
  cab:        "speaker",
  drive:      "flame",
  dynamics:   "bolt",
  eq:         "sliders",
  modulation: "wave",
  pitch:      "note",
  delay:      "delay",
  reverb:     "reverb",
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
  [EffectGuids.kFxNam]:            "pedal",
  [EffectGuids.kAmpNamBlend]:      "blend",

  // Cabs
  [EffectGuids.kCabIr]:            "speaker",
  [EffectGuids.kCabSimple]:        "speaker",

  // EQ
  [EffectGuids.kEqParametric]:     "sliders",
  [EffectGuids.kEqGraphic]:        "sliders",

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
  [EffectGuids.kDelayDigital]:     "delay",
  [EffectGuids.kDelayDoubler]:     "doubler",

  // Reverb
  [EffectGuids.kReverbRoom]:       "reverb",
  [EffectGuids.kReverbChamber]:    "reverb",
  [EffectGuids.kReverbSpring]:     "reverb",
  [EffectGuids.kReverbAdvanced]:   "reverb-advanced",
  [EffectGuids.kReverbIr]:         "reverb",
  [EffectGuids.kReverbAmbient]:    "reverb-ambient",

  // Synth
  [EffectGuids.kSynthSaw]:         "note",

  // Utility
  [EffectGuids.kGain]:             "megaphone",
  [EffectGuids.kSplitter]:         "split",
  [EffectGuids.kMixer]:            "mixer",
  [EffectGuids.kInputAnalyzer]:    "microscope",
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

/**
 * Inline SVG icon helpers for glyphs used in dynamic content
 */

export function getCheckmarkSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><polyline points="20 6 9 17 4 12"></polyline></svg>`;
}

export function getXSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;
}

export function getPlaySvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="currentColor"${titleAttr}><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>`;
}

export function getStopSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="currentColor"${titleAttr}><rect x="4" y="4" width="16" height="16" rx="2"></rect></svg>`;
}

export function getXMarkSvg(title?: string): string {
  const titleAttr = title ? ` title="${title}"` : "";
  return `<svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"${titleAttr}><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;
}
