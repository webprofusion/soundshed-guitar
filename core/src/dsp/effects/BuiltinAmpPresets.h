#pragma once

/**
 * BuiltinAmpPresets.h — Factory presets for the Heavy American.
 *
 * Unlike every other effect's, these set Output. An amp preset is a whole channel, and its
 * Output is that channel's volume. The amp holds the nominal level whatever the voice, gain,
 * stages and power section say, but not through its tone controls or Bright, so each preset's
 * Output takes out what those add: all of them sit within 1 dB of 0, and land within 0.3 dB of
 * the default, as heard, on the demo DI and riffs as recorded or at the nominal level. Browsing
 * presets never jumps.
 *
 * Every preset sets the hidden power section too, so none leaves behind settings the player
 * cannot see, and Input Trim, which sits before the distortion and so is voicing, not level.
 * The legacy per-stage gain keys alias Input Trim and are never set.
 *
 * Gain is a log-taper pot (StageDrive): below about 0.6 the Drive voice is a crunch however many
 * stages are in, and the hard knee hardly distorts until it is driven. A preset meant to be high
 * gain sits at 0.75 and up; TestFactoryPresetsReachTheirGain holds each one to its name.
 */

#include "dsp/effects/FactoryPresetSupport.h"

#include <vector>

namespace guitarfx::builtin_amp
{
// Keep ids stable once shipped; the UIs list them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});
    constexpr double kClean = 0.0;
    constexpr double kDrive = 1.0;

    return {
        // The Clean voice: hard strums on a hot pickup just start to break up.
        b.Defaults("clean-channel", "Clean Channel"),
        // Pushed, with a little power drive, sag and bias; cleans up as you pick softer.
        b.Make("edge-of-breakup", "Edge of Breakup",
               {{"voice", kClean},
                {"gain", 0.7},
                {"character", 0.4},
                {"bright", 1.0},
                {"bass", 0.45},
                {"middle", 0.6},
                {"treble", 0.55},
                {"contour", 0.1},
                {"presence", 0.55},
                {"output", -0.5},
                {"powerDrive", 0.35},
                {"sag", 0.3},
                {"bias", 0.15},
                {"depth", 0.35},
                {"resonance", 0.35},
                {"damping", 0.4}}),
        // The Clean voice nearly dimed, as a non-master-volume amp cranked.
        b.Make("classic-crunch", "Classic Crunch",
               {{"voice", kClean},
                {"gain", 0.9},
                {"character", 0.45},
                {"bright", 1.0},
                {"preEmphasis", 0.2},
                {"stageCount", 3.0},
                {"stageGain", 3.0},
                {"bass", 0.45},
                {"middle", 0.65},
                {"treble", 0.6},
                {"contour", 0.1},
                {"presence", 0.6},
                {"output", -1.0},
                {"powerDrive", 0.4},
                {"sag", 0.3},
                {"bias", 0.1},
                {"depth", 0.35},
                {"resonance", 0.4},
                {"damping", 0.45}}),
        // Tight corners, the lows cut before the late stages and put back after them.
        b.Make("tight-modern-rhythm", "Tight Modern Rhythm",
               {{"voice", kDrive},
                {"gain", 0.75},
                {"character", 0.8},
                {"preEmphasis", 0.2},
                {"stageCount", 3.0},
                {"bass", 0.35},
                {"middle", 0.45},
                {"treble", 0.6},
                {"contour", 0.5},
                {"presence", 0.6},
                {"output", 0.0},
                {"powerDrive", 0.15},
                {"sag", 0.3},
                {"depth", 0.6},
                {"resonance", 0.5},
                {"damping", 0.45}}),
        // The hard knee and four stages: the tightest low end of the set.
        b.Make("tight-djent", "Tight Djent",
               {{"voice", kDrive},
                {"gain", 0.85},
                {"character", 1.0},
                {"stageCount", 4.0},
                {"bass", 0.25},
                {"middle", 0.45},
                {"treble", 0.55},
                {"contour", 0.5},
                {"presence", 0.55},
                {"output", -0.2},
                {"depth", 0.45},
                {"resonance", 0.55},
                {"damping", 0.4}}),
        b.Make("scooped-thrash", "Scooped Thrash",
               {{"voice", kDrive},
                {"gain", 0.9},
                {"character", 0.7},
                {"bright", 1.0},
                {"stageCount", 3.0},
                {"bass", 0.55},
                {"middle", 0.3},
                {"treble", 0.65},
                {"contour", 0.75},
                {"presence", 0.65},
                {"output", 0.0},
                {"powerDrive", 0.2},
                {"sag", 0.3},
                {"depth", 0.65},
                {"resonance", 0.55},
                {"damping", 0.4}}),
        // Soft knee and even harmonics, loose and dark, leaning on the power stage.
        b.Make("vintage-high-gain", "Vintage High Gain",
               {{"voice", kDrive},
                {"gain", 0.78},
                {"character", 0.25},
                {"bright", 1.0},
                {"preEmphasis", 0.15},
                {"stageCount", 3.0},
                {"bass", 0.55},
                {"middle", 0.6},
                {"presence", 0.45},
                {"output", -0.1},
                {"powerDrive", 0.4},
                {"sag", 0.55},
                {"bias", 0.25},
                {"depth", 0.5},
                {"damping", 0.6}}),
        // Mids forward, sag and power drive for compression: its level holds as you play softer.
        b.Make("singing-lead", "Singing Lead",
               {{"voice", kDrive},
                {"gain", 0.88},
                {"preEmphasis", 0.25},
                {"stageCount", 4.0},
                {"bass", 0.45},
                {"middle", 0.7},
                {"treble", 0.55},
                {"contour", 0.05},
                {"output", 0.0},
                {"powerDrive", 0.35},
                {"sag", 0.7},
                {"bias", 0.1},
                {"damping", 0.55}}),

        // Sepia's additions: the same amp, set up as eight more kinds of amp. Each Output is
        // fitted the way the ones above are, to take out what its tone controls and Bright add
        // (measured as loudness through the default cabinet, against the same preset with its
        // tone controls at their defaults).

        // Bright and scooped a little, with the headroom left in: for chords that have to ring.
        b.Make("glass-clean", "Glass Clean",
               {{"voice", kClean},
                {"gain", 0.35},
                {"bright", 1.0},
                {"bass", 0.45},
                {"middle", 0.4},
                {"treble", 0.65},
                {"presence", 0.65},
                {"output", 0.2},
                {"damping", 0.4}}),
        // Dark, round and well damped: neck pickup, thumb or a soft pick.
        b.Make("warm-jazz", "Warm Jazz",
               {{"voice", kClean},
                {"gain", 0.3},
                {"character", 0.4},
                {"bass", 0.6},
                {"middle", 0.55},
                {"treble", 0.3},
                {"presence", 0.3},
                {"output", -0.4},
                {"damping", 0.7}}),
        // A small combo turned up: chimes when played softly, grinds when dug into.
        b.Make("chime-combo", "Chime Combo",
               {{"voice", kClean},
                {"gain", 0.74},
                {"character", 0.55},
                {"bright", 1.0},
                {"bass", 0.45},
                {"middle", 0.5},
                {"treble", 0.68},
                {"contour", 0.1},
                {"presence", 0.6},
                {"output", -0.5},
                {"powerDrive", 0.3},
                {"sag", 0.4},
                {"bias", 0.1},
                {"damping", 0.4}}),
        // An old low-powered amp flat out: mostly power stage, saggy, all mids.
        b.Make("tweed-breakup", "Tweed Breakup",
               {{"voice", kClean},
                {"gain", 0.85},
                {"character", 0.3},
                {"bass", 0.4},
                {"middle", 0.7},
                {"treble", 0.5},
                {"contour", 0.05},
                {"output", -0.5},
                {"powerDrive", 0.55},
                {"sag", 0.6},
                {"bias", 0.2},
                {"depth", 0.3},
                {"resonance", 0.35}}),
        // The Drive voice held back to a crunch: mid-forward, two stages, cleans up with the pick.
        b.Make("british-crunch", "British Crunch",
               {{"voice", kDrive},
                {"gain", 0.5},
                {"stageCount", 2.0},
                {"bass", 0.45},
                {"middle", 0.7},
                {"treble", 0.62},
                {"contour", 0.15},
                {"presence", 0.6},
                {"output", -0.85},
                {"powerDrive", 0.35},
                {"sag", 0.35},
                {"depth", 0.45},
                {"resonance", 0.45},
                {"damping", 0.45}}),
        // The same stack with a third stage in: rock rhythm that still has its mids.
        b.Make("hot-rodded-stack", "Hot-Rodded Stack",
               {{"voice", kDrive},
                {"gain", 0.68},
                {"character", 0.6},
                {"bright", 1.0},
                {"stageCount", 3.0},
                {"bass", 0.45},
                {"middle", 0.65},
                {"treble", 0.6},
                {"contour", 0.25},
                {"presence", 0.6},
                {"output", -0.1},
                {"powerDrive", 0.3},
                {"sag", 0.35},
                {"depth", 0.5},
                {"resonance", 0.5},
                {"damping", 0.45}}),
        // Soft knee, a lot of low end and a power stage that gives: slow, loud and wooly.
        b.Make("doom-wall", "Doom Wall",
               {{"voice", kDrive},
                {"gain", 0.86},
                {"character", 0.15},
                {"stageCount", 3.0},
                {"bass", 0.7},
                {"middle", 0.5},
                {"treble", 0.4},
                {"contour", 0.3},
                {"presence", 0.35},
                {"output", -0.5},
                {"powerDrive", 0.5},
                {"sag", 0.7},
                {"bias", 0.2},
                {"depth", 0.7},
                {"resonance", 0.6},
                {"damping", 0.65}}),
        // Four stages behind a mid lift and a dark top: single notes that run into each other.
        b.Make("liquid-lead", "Liquid Lead",
               {{"voice", kDrive},
                {"gain", 0.82},
                {"character", 0.45},
                {"preEmphasis", 0.3},
                {"stageCount", 4.0},
                {"bass", 0.4},
                {"middle", 0.75},
                {"treble", 0.5},
                {"contour", 0.05},
                {"presence", 0.45},
                {"output", 0.35},
                {"powerDrive", 0.3},
                {"sag", 0.6},
                {"bias", 0.1},
                {"damping", 0.6}}),
    };
}
} // namespace guitarfx::builtin_amp
