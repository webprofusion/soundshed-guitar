// @vitest-environment node
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import {
  EFFECT_ALIAS_MAP,
  EffectGuids,
  RETIRED_EFFECT_TYPES,
  migrateLegacyEffectType,
  resolveEffectType,
} from "../ts/effectGuids.js";
import { EffectTypeRegistry, getNodeEffectInfo, migratePresetNodeTypes } from "../ts/presetV2.js";
import type { Preset } from "../ts/types.js";

/**
 * The UI half of the effect-alias parity check: core/protocol/effect-aliases.json lists
 * the legacy ids the engine registers for each effect (EffectAliasParityTests checks the
 * engine), and a preset must resolve to the same effect in the UI as in the engine.
 */
interface ManifestEntry {
  guid: string;
  aliases: string[];
  retiredTypes?: Record<string, string>;
}

const manifest = JSON.parse(
  readFileSync(new URL("../../protocol/effect-aliases.json", import.meta.url), "utf8"),
) as { effects: Record<string, ManifestEntry> };
const entries = Object.entries(manifest.effects);
const guidOf = (name: string): string | undefined => (EffectGuids as Record<string, string>)[name];

describe("effect aliases match core/protocol/effect-aliases.json", () => {
  it("names each effect and retired type by its EffectGuids constant", () => {
    for (const [name, entry] of entries) {
      expect(guidOf(name), name).toBe(entry.guid);
      for (const [retired, guid] of Object.entries(entry.retiredTypes ?? {})) {
        expect(guidOf(retired), retired).toBe(guid);
      }
    }
  });

  it("covers every EffectGuids constant", () => {
    const listed = new Set(entries.flatMap(([name, entry]) => [name, ...Object.keys(entry.retiredTypes ?? {})]));
    expect(Object.keys(EffectGuids).filter((name) => !listed.has(name))).toEqual([]);
  });

  it("holds exactly the engine's aliases and retired types", () => {
    const aliases = Object.fromEntries(entries.flatMap(([, entry]) => entry.aliases.map((alias) => [alias, entry.guid])));
    const retired = Object.fromEntries(
      entries.flatMap(([, entry]) => Object.values(entry.retiredTypes ?? {}).map((guid) => [guid, entry.guid])),
    );
    expect(EFFECT_ALIAS_MAP).toEqual(aliases);
    expect(RETIRED_EFFECT_TYPES).toEqual(retired);
  });

  it("resolves each id the same way through both lookups", () => {
    for (const [, entry] of entries) {
      for (const id of [entry.guid, ...entry.aliases, ...Object.values(entry.retiredTypes ?? {})]) {
        expect(resolveEffectType(id), id).toBe(entry.guid);
        expect(EffectTypeRegistry.resolve(id), id).toBe(entry.guid);
      }
    }
  });
});

describe("the retired kAmpNam type", () => {
  it("stays as stored when a preset migrates, while legacy string ids become UUIDs", () => {
    expect(migrateLegacyEffectType("amp_nam")).toBe(EffectGuids.kAmpNamOptimized);
    const preset = {
      graph: { nodes: [{ type: EffectGuids.kAmpNam }, { type: "amp_nam" }, { type: "ir_cab" }], edges: [] },
    } as unknown as Preset;
    migratePresetNodeTypes(preset);
    expect(preset.graph?.nodes.map((node) => node.type)).toEqual([
      EffectGuids.kAmpNam,
      EffectGuids.kAmpNamOptimized,
      EffectGuids.kCabIr,
    ]);
  });

  it("takes its name and parameters from the optimized amp's catalog entry", () => {
    // The engine's catalog only ever describes kAmpNamOptimized.
    EffectTypeRegistry.register(EffectGuids.kAmpNamOptimized, {
      type: EffectGuids.kAmpNamOptimized,
      displayName: "Neural Amp",
      category: "amp",
      requiresResource: true,
      parameters: [{ key: "inputGain", name: "Input", default: 0, min: -24, max: 24, unit: "dB" }],
    });
    const info = getNodeEffectInfo({ type: EffectGuids.kAmpNam, config: {} });
    expect(info?.displayName).toBe("Neural Amp");
    expect(info?.parameters.map((param) => param.key)).toEqual(["inputGain"]);
  });

  // A new preset's amp is kAmpNamOptimized too: the engine builds new presets
  // (newPreset), and core/tests/PresetEditCommandsTests.cpp checks that.
});
