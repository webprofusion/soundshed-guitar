const ICON_BASE = "images/icons";

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
  dynamics: "bolt",
  amp: "amp",
  cab: "speaker",
  eq: "sliders",
  modulation: "wave",
  delay: "clock",
  reverb: "hall",
  utility: "wrench",
};

const effectIcons: Record<string, IconKey> = {
  // Dynamics
  dynamics_gate: "door",
  compressor_vca: "meter",
  compressor_opto: "bulb",
  compressor_fet: "bolt",
  overdrive: "flame",
  distortion: "flame",
  fuzz: "flame",

  // Amps
  amp_builtin: "amp",
  amp_nam: "amp",
  amp_nam_optimized: "amp",
  amp_nam_blend: "blend",
  amp_clean: "amp",
  amp_crunch: "amp",

  // Cabs
  cab_ir: "speaker",
  cab_simple: "speaker",

  // EQ
  eq_parametric: "sliders",
  eq_graphic: "sliders",
  eq_tilt: "split",

  // Modulation
  chorus_analog: "wave",
  chorus_digital: "wave",
  flanger: "wave",
  phaser: "wave",
  tremolo: "wave",
  auto_wah: "mixer",
  octave: "note",
  vibrato: "wave",

  // Delay
  delay_digital: "clock",
  delay_tape: "clock",
  delay_analog: "clock",

  // Reverb
  reverb_room: "hall",
  reverb_hall: "hall",
  reverb_plate: "hall",
  reverb_spring: "hall",
  reverb_shimmer: "sparkle",

  // Utility
  gain: "megaphone",
  splitter: "split",
  mixer: "mixer",
};

export function getFxCategoryIcon(categoryId: string): string {
  const icon = categoryIcons[categoryId] ?? "gear";
  return renderIcon(icon, "fx-category-icon");
}

export function getFxEffectIcon(effectType: string): string {
  const icon = effectIcons[effectType] ?? "gear";
  return renderIcon(icon, "fx-effect-icon");
}

export function getBadgeIcon(type: "resource" | "blend", titleOverride?: string): string {
  const icon = type === "resource" ? "folder" : "flask";
  const title = titleOverride ?? (type === "resource" ? "Requires resource" : "Custom blend");
  return renderIcon(icon, "fx-badge-icon", title);
}
