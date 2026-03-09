/**
 * Effect type UUID constants — TypeScript mirror of core/src/dsp/EffectGuids.h
 *
 * These UUIDs are the permanent, stable identifiers for each registered effect
 * type. They never change regardless of display-name renames or refactoring.
 *
 * Always use these constants in code rather than embedding UUID strings directly.
 * Routing-only nodes ("input" / "output") are NOT registered effects and keep
 * their plain string IDs.
 */
export const EffectGuids = {
  // Amp
  kAmpBuiltin:           "1460a632-6690-4fef-ac6d-6432e3b983f8",
  kAmpNam:               "2eb53b40-6139-4696-8820-387ac56ffa91",
  kAmpNamOptimized:      "49ea214c-91e6-41f9-bd27-ad6eec0ae90a",
  kAmpNamBlend:          "8a22c0f8-413b-42c1-b9ba-d543cf011d9e",
  kFxNam:                "c3263344-65e4-4b7e-b102-ea625700e12f",

  // Cabinet
  kCabIr:                "94fa2577-e904-43b8-968b-9c569c511160",
  kCabSimple:            "27e0eaa3-b023-4b5a-b783-cce65254c0d3",

  // Dynamics
  kDynamicsGate:         "e8388de1-d262-4123-a123-8dbc56f657bc",
  kCompressorVca:        "72af3541-2408-4a5c-a2dc-ba164f17eac9",
  kCompressorOpto:       "9651c79e-6530-4c23-9150-aa4c0ff2f1d8",
  kLimiterBrickwall:     "f4094126-b5de-4c5d-8d05-d56bd8c312d1",

  // Drive
  kOverdrive:            "fa9e05a8-168a-4293-aa91-6b770de3da1d",
  kDistortion:           "686773c9-30ac-4f33-b0f8-9222146d45b1",
  kFuzz:                 "3a38b19c-1d97-4989-b5bb-12bcc59d1e6b",

  // EQ
  kEqParametric:         "4b4025ca-64cd-4180-be79-81873b618dba",

  // Delay
  kDelayDigital:         "673d3e7a-e9ef-4c5d-a4c4-619dff3355ed",
  kDelayDoubler:         "778aaef4-40e3-4efa-8782-6a8bfa1d1661",

  // Reverb
  kReverbRoom:           "7467cbf1-6c7f-4f07-b5dd-a303d25b475c",
  kReverbHall:           "a07ab1a5-37e5-4279-bd08-5ad640886709",
  kReverbPlate:          "9e023b65-5431-48eb-95ff-4f13e7f864a2",
  kReverbChamber:        "4ef25e86-9763-40bc-aca6-636b542df60b",
  kReverbSpring:         "0df83b32-23d0-4530-a50e-e0824a5ccf01",
  kReverbShimmer:        "7dcbb06d-8925-4f84-b412-232b7c02de26",
  kReverbAmbient:        "d663f5d8-0f6e-4721-960d-81621fe41801",
  kReverbAdvanced:       "92558944-f0da-4d97-ab75-bed8b63abc31",
  kReverbIr:             "497d3c9d-ed6b-4c71-8e6d-0f9d61564dbc",

  // Modulation
  kChorus:               "decdd132-029a-46a5-a362-edcde007a450",
  kFlanger:              "1a3f3793-7e80-4e3d-ab7b-3ce3ce032fe7",
  kPhaser:               "3aa9dc81-31c2-40d5-9b1b-b0b9d1295e9b",
  kTremolo:              "c9debb02-d7e7-43e3-8330-b387be46dcf4",
  kAutoWah:              "b06c6d84-01b3-4d0a-ad98-40eecb64438e",
  kAutoArp:              "e4a7c9d0-3b52-4f16-8a9e-2c7f1d0e5b83",

  // Pitch
  kPitchShift:           "0c15f065-8335-4932-9d2f-366d436ec30a",
  kTranspose:            "9b89cc46-e05b-4f06-981e-1d74d1f628cf",
  kTransposeStft:        "66b3a43a-72eb-4c7a-9c47-50e9ab24b718",
  kOctave:               "2e4d5380-5a79-412f-bfc0-bf84ef74d561",

  // Utility
  kGain:                 "0bcd895e-5d36-4247-a351-6bed1fcb37a8",
  kSynthSaw:             "608e846e-0e60-4064-9c83-37c0df573c38",
  kSplitter:             "f5f2541b-fcea-4cfd-9e62-eeddf583ef4e",
  kMixer:                "d7d1e40f-9c79-4582-9a82-d5fa5bbbfb97",
} as const;

export type EffectGuid = typeof EffectGuids[keyof typeof EffectGuids];

/**
 * Maps legacy string type IDs to their canonical UUID.
 * Used to migrate presets that were saved before the UUID migration.
 */
export const EFFECT_ALIAS_MAP: Record<string, string> = {
  amp_builtin:           EffectGuids.kAmpBuiltin,
  amp_nam:               EffectGuids.kAmpNam,
  amp_nam_optimized:     EffectGuids.kAmpNamOptimized,
  amp_nam_blend:         EffectGuids.kAmpNamBlend,
  fx_nam:                EffectGuids.kFxNam,
  cab_ir:                EffectGuids.kCabIr,
  ir_cab:                EffectGuids.kCabIr, // historical alias
  cab_simple:            EffectGuids.kCabSimple,
  dynamics_gate:         EffectGuids.kDynamicsGate,
  gate_noise:            EffectGuids.kDynamicsGate, // historical alias
  compressor_vca:        EffectGuids.kCompressorVca,
  compressor_opto:       EffectGuids.kCompressorOpto,
  limiter_brickwall:     EffectGuids.kLimiterBrickwall,
  overdrive:             EffectGuids.kOverdrive,
  distortion:            EffectGuids.kDistortion,
  fuzz:                  EffectGuids.kFuzz,
  eq_parametric:         EffectGuids.kEqParametric,
  delay_digital:         EffectGuids.kDelayDigital,
  delay_doubler:         EffectGuids.kDelayDoubler,
  reverb_room:           EffectGuids.kReverbRoom,
  reverb_hall:           EffectGuids.kReverbHall,
  reverb_plate:          EffectGuids.kReverbPlate,
  reverb_chamber:        EffectGuids.kReverbChamber,
  reverb_spring:         EffectGuids.kReverbSpring,
  reverb_shimmer:        EffectGuids.kReverbShimmer,
  reverb_ambient:        EffectGuids.kReverbAmbient,
  reverb_advanced:       EffectGuids.kReverbAdvanced,
  reverb_ir:             EffectGuids.kReverbIr,
  chorus:                EffectGuids.kChorus,
  flanger:               EffectGuids.kFlanger,
  phaser:                EffectGuids.kPhaser,
  tremolo:               EffectGuids.kTremolo,
  auto_wah:              EffectGuids.kAutoWah,
  arp_auto:              EffectGuids.kAutoArp,
  pitch_shift:           EffectGuids.kPitchShift,
  transpose:             EffectGuids.kTranspose,
  transpose_stft:        EffectGuids.kTransposeStft,
  octave:                EffectGuids.kOctave,
  gain:                  EffectGuids.kGain,
  synth_saw:             EffectGuids.kSynthSaw,
  splitter:              EffectGuids.kSplitter,
  mixer:                 EffectGuids.kMixer,
};

/**
 * Resolves a legacy string type ID to its canonical UUID.
 * Returns the input unchanged if it is already a UUID or an unknown type
 * (e.g. "input", "output", or composite types).
 */
export function resolveEffectType(type: string): string {
  return EFFECT_ALIAS_MAP[type] ?? type;
}
