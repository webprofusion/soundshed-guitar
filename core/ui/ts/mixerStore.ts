/**
 * The Multi-Rig mixer as the UI holds it — which presets are in slots, each
 * slot's mix, pan, mute and solo, and the two levels — and the commands that
 * change it.
 *
 * Six modules used to assign into `uiState.mixer`, and "take a preset out of
 * the mixer" alone was open-coded three times. These commands only record what
 * the UI shows; telling the engine is still the caller's job, through the
 * `bridge.ts` sender for the same change. `scripts/check-state-writes.js`
 * keeps the writes here.
 */

import { uiState } from "./state.js";
import type { MixerPresetState, MixerState } from "./types.js";

type SlotMix = Pick<MixerPresetState, "mix" | "pan" | "mute" | "solo">;

function defaultSlot(id: string): MixerPresetState {
  return { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
}

function ensureMixer(): MixerState {
  uiState.mixer = uiState.mixer ?? { activePresetIds: [], presets: {}, masterGain: 1.0, mixGainDb: 0 };
  return uiState.mixer;
}

/** Takes the complete mixer the engine sent with its full state. */
export function replaceMixerState(mixer: MixerState): void {
  uiState.mixer = mixer;
}

/**
 * Merges a partial mixer update from the engine: whichever of the slot list,
 * the levels and the per-slot settings it carries replace what is here.
 */
export function mergeMixerState(update: {
  activePresetIds?: string[];
  masterGain?: number;
  mixGainDb?: number;
  presets?: Record<string, Partial<SlotMix>>;
}): void {
  const mixer = ensureMixer();
  if (update.activePresetIds) mixer.activePresetIds = update.activePresetIds.slice();
  if (typeof update.masterGain === "number") mixer.masterGain = update.masterGain;
  if (typeof update.mixGainDb === "number") mixer.mixGainDb = update.mixGainDb;
  for (const [id, slot] of Object.entries(update.presets ?? {})) {
    const current = mixer.presets[id] || defaultSlot(id);
    mixer.presets[id] = {
      id,
      mix: typeof slot.mix === "number" ? slot.mix : current.mix,
      pan: typeof slot.pan === "number" ? slot.pan : current.pan,
      mute: typeof slot.mute === "boolean" ? slot.mute : current.mute,
      solo: typeof slot.solo === "boolean" ? slot.solo : current.solo,
    };
  }
}

/** Sets which presets are in slots, giving any new one default settings. */
export function setMixerSlots(presetIds: readonly string[]): void {
  const mixer = ensureMixer();
  mixer.activePresetIds = presetIds.slice();
  for (const id of presetIds) {
    if (!mixer.presets[id]) mixer.presets[id] = defaultSlot(id);
  }
}

/** Adds a preset to the end of the slots. Settings it already has are kept. Does nothing before the mixer exists. */
export function addMixerSlot(presetId: string, name?: string): void {
  const mixer = uiState.mixer;
  if (!mixer) return;
  mixer.activePresetIds.push(presetId);
  if (!mixer.presets[presetId]) mixer.presets[presetId] = { ...defaultSlot(presetId), name };
}

/** Takes a preset out of the mixer: its slot and its settings. */
export function removeMixerSlot(presetId: string): void {
  const mixer = uiState.mixer;
  if (!mixer) return;
  mixer.activePresetIds = mixer.activePresetIds.filter((id) => id !== presetId);
  delete mixer.presets[presetId];
}

/** Changes one slot's mix, pan, mute or solo. A preset not in the mixer is ignored. */
export function updateMixerSlot(presetId: string, change: Partial<SlotMix>): void {
  const slot = uiState.mixer?.presets[presetId];
  if (slot) Object.assign(slot, change);
}

/** The Multi-Rig's own level in dB. Does nothing before the mixer exists. */
export function setMixerMixGainDb(gainDb: number): void {
  if (uiState.mixer) uiState.mixer.mixGainDb = gainDb;
}
