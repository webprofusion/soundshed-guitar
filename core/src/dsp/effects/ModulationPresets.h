#pragma once

/**
 * ModulationPresets.h — Factory presets for the chorus, the flanger and the tremolo.
 *
 * Division is left out of a preset with Sync off, so the player's own division survives and
 * comes back when they turn Sync on; a tempo-synced preset names its division, and its Rate is
 * the same speed at 120 bpm, so turning Sync off keeps the feel. Pitch wobble is about
 * 10.9 x Rate x Depth cents. Levels are within about 2 dB of each default on the demo DI; the
 * chorus's Vibrato is fully wet and sits at the bypass level, 2.2 dB over the default's mix.
 */

#include "dsp/effects/FactoryPresetSupport.h"

#include <vector>

namespace guitarfx::modulation_presets
{
// Keep ids stable once shipped; the UIs list them.

[[nodiscard]] inline std::vector<EffectPresetDefinition> Chorus(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"syncDivision"});

    return {
        b.Defaults("studio-chorus", "Studio Chorus"),
        b.Make("ce2", "CE-2", {{"rate", 0.8}, {"depth", 2.0}, {"delay", 7.0}, {"feedback", 0.0}, {"mix", 0.5}}),
        b.Make("small-clone", "Small Clone",
               {{"rate", 1.0}, {"depth", 2.4}, {"delay", 6.0}, {"feedback", 0.0}, {"mix", 0.5}}),
        b.Make("lush-chorus", "Lush Chorus",
               {{"rate", 0.4}, {"depth", 4.0}, {"delay", 20.0}, {"feedback", 0.25}, {"mix", 0.45}}),
        b.Make("double-track", "Double Track",
               {{"rate", 0.2}, {"depth", 1.0}, {"delay", 25.0}, {"feedback", 0.0}, {"mix", 0.4}}),
        // Fully wet, so pitch alone, as a CE-1 in vibrato mode.
        b.Make("vibrato", "Vibrato", {{"rate", 5.5}, {"depth", 0.6}, {"delay", 3.0}, {"feedback", 0.0}, {"mix", 1.0}}),
        b.Make("warble", "Warble", {{"rate", 0.7}, {"depth", 5.0}, {"delay", 12.0}, {"feedback", 0.0}, {"mix", 0.6}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Flanger(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"syncDivision"});

    return {
        b.Defaults("classic-flanger", "Classic Flanger"),
        b.Make("mxr-117", "MXR 117",
               {{"rate", 0.12}, {"depth", 4.5}, {"delay", 0.2}, {"feedback", 0.75}, {"mix", 0.5}}),
        b.Make("bf2", "BF-2", {{"rate", 0.7}, {"depth", 2.5}, {"delay", 0.5}, {"feedback", 0.5}, {"mix", 0.5}}),
        b.Make("electric-mistress", "Electric Mistress",
               {{"rate", 0.1}, {"depth", 3.0}, {"delay", 2.0}, {"feedback", 0.3}, {"mix", 0.4}}),
        // Near-static: a resonant ring more than a sweep. Feedback short of its 0.85 maximum.
        b.Make("metallic-comb", "Metallic Comb",
               {{"rate", 0.05}, {"depth", 0.4}, {"delay", 1.5}, {"feedback", 0.8}, {"mix", 0.5}}),
        b.Make("fast-swirl", "Fast Swirl",
               {{"rate", 3.0}, {"depth", 1.0}, {"delay", 0.6}, {"feedback", 0.3}, {"mix", 0.5}}),
        // One sweep a bar.
        b.Make("tempo-sweep", "Tempo Sweep",
               {{"rate", 0.5},
                {"syncMode", 1.0},
                {"syncDivision", 0.0},
                {"depth", 4.0},
                {"delay", 0.3},
                {"feedback", 0.6},
                {"mix", 0.5}}),
    };
}

/// Mode is in every preset, so picking one always lands in its mode. Pattern and Crossover keep
/// their defaults outside the mode that uses them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> Tremolo(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"syncDivision"});

    return {
        b.Defaults("classic-tremolo", "Classic Tremolo"),
        b.Make("slow-throb", "Slow Throb", {{"rate", 2.2}, {"depth", 0.8}, {"shape", 0.3}}),
        // Near-square and fast: the surf amps' tremolo.
        b.Make("surf", "Surf", {{"rate", 7.0}, {"depth", 0.9}, {"shape", 0.7}}),
        b.Make("brown-harmonic", "Brown Harmonic", {{"mode", 1.0}, {"rate", 5.0}, {"depth", 0.85}}),
        b.Make("deep-harmonic", "Deep Harmonic",
               {{"mode", 1.0}, {"rate", 3.0}, {"depth", 1.0}, {"shape", 0.25}, {"crossover", 900.0}}),
        b.Make("auto-pan", "Auto-Pan", {{"mode", 2.0}, {"rate", 0.8}, {"depth", 0.9}, {"shape", 0.2}}),
        // A pan each beat.
        b.Make("tempo-pan", "Tempo Pan",
               {{"mode", 2.0}, {"rate", 1.0}, {"syncMode", 1.0}, {"syncDivision", 1.0}, {"depth", 1.0}}),
        // Sixteenth-note steps; the cut steps are silent.
        b.Make("gallop-slicer", "Gallop Slicer",
               {{"mode", 3.0},
                {"rate", 8.0},
                {"syncMode", 1.0},
                {"syncDivision", 10.0},
                {"depth", 1.0},
                {"shape", 0.15},
                {"pattern", 1.0}}),
        b.Make("half-time-chop", "Half-Time Chop",
               {{"mode", 3.0},
                {"rate", 8.0},
                {"syncMode", 1.0},
                {"syncDivision", 10.0},
                {"depth", 1.0},
                {"shape", 0.1},
                {"pattern", 6.0}}),
        b.Make("tresillo-pulse", "Tresillo Pulse",
               {{"mode", 3.0},
                {"rate", 8.0},
                {"syncMode", 1.0},
                {"syncDivision", 10.0},
                {"depth", 0.8},
                {"shape", 0.4},
                {"pattern", 4.0}}),
    };
}
} // namespace guitarfx::modulation_presets
