#pragma once

/**
 * DynamicsPresets.h — Factory presets for the VCA and Opto compressors and the noise gate.
 *
 * Thresholds are in dBFS, not relative to the nominal operating level, so the compressor
 * presets are voiced for a guitar at about the level of the demo DI (median peaks near
 * -7 dBFS, RMS near -23 dBFS). Makeup brings each one back to about the level it came in at,
 * so switching one on changes the dynamics, not the volume; the default, being the parameter
 * defaults, has none and sits a few dB down. A hotter rig compresses harder; turn Threshold
 * up to match.
 *
 * The gate's presets leave Threshold out: it belongs to the player's pickups and how much
 * noise their rig makes, and choosing how a gate closes should not move where it closes.
 * All three leave Stereo Link out, as a routing choice rather than a sound.
 */

#include "dsp/effects/FactoryPresetSupport.h"

#include <vector>

namespace guitarfx::dynamics_presets
{
// Keep ids stable once shipped; the UIs list them.

[[nodiscard]] inline std::vector<EffectPresetDefinition> Vca(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"stereoLink"});

    return {
        b.Defaults("classic-vca", "Classic VCA"),
        b.Make("chicken-pickin", "Chicken Pickin'",
               {{"threshold", -32.0},
                {"ratio", 8.0},
                {"attack", 4.0},
                {"release", 150.0},
                {"knee", 3.0},
                {"makeup", 13.0},
                {"softClip", 0.2}}),
        b.Make("funk-clean", "Funk Clean",
               {{"threshold", -28.0}, {"ratio", 4.0}, {"attack", 15.0}, {"release", 80.0}, {"makeup", 5.5}}),
        b.Make("transparent-leveler", "Transparent Leveler",
               {{"threshold", -24.0},
                {"ratio", 2.0},
                {"attack", 20.0},
                {"release", 250.0},
                {"knee", 12.0},
                {"makeup", 3.0}}),
        b.Make("parallel-squash", "Parallel Squash",
               {{"threshold", -38.0},
                {"ratio", 12.0},
                {"attack", 1.0},
                {"release", 80.0},
                {"knee", 0.0},
                {"makeup", 18.0},
                {"mix", 0.4}}),
        b.Make("lead-sustain", "Lead Sustain",
               {{"threshold", -36.0},
                {"ratio", 10.0},
                {"attack", 5.0},
                {"release", 400.0},
                {"makeup", 18.0},
                {"softClip", 0.3}}),
        b.Make("peak-catcher", "Peak Catcher",
               {{"threshold", -10.0},
                {"ratio", 20.0},
                {"attack", 0.5},
                {"release", 60.0},
                {"knee", 0.0},
                {"makeup", 1.0}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Opto(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"stereoLink"});

    return {
        b.Defaults("smooth-opto", "Smooth Opto"),
        b.Make("gentle-leveler", "Gentle Leveler",
               {{"threshold", -26.0}, {"ratio", 2.0}, {"attack", 30.0}, {"release", 600.0}, {"makeup", 3.5}}),
        b.Make("studio-leveling", "Studio Leveling",
               {{"threshold", -30.0}, {"ratio", 4.0}, {"attack", 10.0}, {"release", 500.0}, {"makeup", 9.0}}),
        b.Make("clean-sustain", "Clean Sustain",
               {{"threshold", -34.0}, {"ratio", 6.0}, {"attack", 15.0}, {"release", 1200.0}, {"makeup", 14.0}}),
        b.Make("slow-bloom", "Slow Bloom",
               {{"threshold", -32.0}, {"ratio", 4.0}, {"attack", 120.0}, {"release", 1500.0}, {"makeup", 9.5}}),
        b.Make("parallel-bloom", "Parallel Bloom",
               {{"threshold", -36.0},
                {"ratio", 8.0},
                {"attack", 40.0},
                {"release", 800.0},
                {"makeup", 15.0},
                {"mix", 0.5}}),
        b.Make("opto-limit", "Opto Limit",
               {{"threshold", -20.0},
                {"ratio", 20.0},
                {"attack", 5.0},
                {"release", 250.0},
                {"makeup", 2.5},
                {"softClip", 0.2}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Gate(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"threshold", "stereoLink"});

    return {
        b.Defaults("standard-gate", "Standard Gate"),
        b.Make("soft-reduction", "Soft Reduction",
               {{"attack", 2.0}, {"hold", 100.0}, {"release", 200.0}, {"hysteresis", 6.0}, {"range", -18.0}}),
        b.Make("high-gain-tight", "High-Gain Tight",
               {{"attack", 0.5}, {"hold", 20.0}, {"release", 25.0}, {"hysteresis", 6.0}, {"range", -80.0}}),
        b.Make("staccato-chug", "Staccato Chug",
               {{"attack", 0.2}, {"hold", 5.0}, {"release", 10.0}, {"hysteresis", 8.0}, {"range", -90.0}}),
        b.Make("natural-decay", "Natural Decay",
               {{"attack", 2.0}, {"hold", 150.0}, {"release", 300.0}, {"hysteresis", 10.0}, {"range", -60.0}}),
        b.Make("ambient-friendly", "Ambient Friendly",
               {{"attack", 5.0}, {"hold", 250.0}, {"release", 500.0}, {"hysteresis", 12.0}, {"range", -30.0}}),
        // Every note fades in, as if the volume knob were rolled up after each pick.
        b.Make("volume-swell", "Volume Swell",
               {{"mode", 1.0}, {"hold", 80.0}, {"release", 250.0}, {"hysteresis", 10.0}, {"swell", 600.0}}),
        b.Make("slow-swell", "Slow Swell",
               {{"mode", 1.0}, {"hold", 150.0}, {"release", 500.0}, {"hysteresis", 12.0}, {"swell", 1100.0}}),
    };
}
} // namespace guitarfx::dynamics_presets
