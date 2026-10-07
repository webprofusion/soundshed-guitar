/**
 * Which effects the FX library offers: the experimental ones only with Experimental Effects on,
 * and Guitar to MIDI, which no longer is, always.
 */
import { beforeEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import { uiState } from "../ts/state.js";

const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { getCatalogEffects } = await import("../ts/fxSelector.js");

for (const [type, displayName, category] of [
  [EffectGuids.kGuitarToMidi, "Guitar to MIDI", "synth"],
  [EffectGuids.kTransposeStft, "Transpose (STFT)", "pitch"],
  [EffectGuids.kTransposeHybrid, "Transpose (Hybrid)", "pitch"],
]) {
  EffectTypeRegistry.register(type, { type, displayName, category, requiresResource: false, parameters: [] });
}

const offered = (): string[] => getCatalogEffects().map((effect) => EffectTypeRegistry.resolve(effect.type));

describe("FX library catalog", () => {
  beforeEach(() => {
    uiState.appSettings = {} as typeof uiState.appSettings;
  });

  it("offers Guitar to MIDI with Experimental Effects off, and hides the experimental transposes", () => {
    expect(offered()).toContain(EffectGuids.kGuitarToMidi);
    expect(offered()).not.toContain(EffectGuids.kTransposeStft);
    expect(offered()).not.toContain(EffectGuids.kTransposeHybrid);
  });

  it("adds the experimental transposes when the feature is on", () => {
    uiState.appSettings = { "features.experimentalEffects.enabled": true } as unknown as typeof uiState.appSettings;
    expect(offered()).toEqual(
      expect.arrayContaining([EffectGuids.kGuitarToMidi, EffectGuids.kTransposeStft, EffectGuids.kTransposeHybrid]),
    );
  });
});
