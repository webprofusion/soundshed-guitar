#pragma once

#include "dsp/EffectRegistry.h"

#include <initializer_list>
#include <utility>
#include <vector>

namespace guitarfx
{
namespace reverb_presets
{
/// A factory preset for a reverb declaring `params`: every declared default, then `overrides`.
/// It sets every voicing control, so choosing one never leaves the last preset's settings
/// behind, but not Output (Ambient's outputGain): the level the player has set stays theirs.
/// Mix is part of a preset, as on the delays: how far back the reverb sits is most of its voice.
[[nodiscard]] inline EffectPresetDefinition MakePreset(const std::vector<ParameterDef>& params, const char* id,
                                                       const char* name, bool isDefault,
                                                       std::initializer_list<std::pair<const char*, double>> overrides)
{
    EffectPresetDefinition preset;
    preset.id = id;
    preset.displayName = name;
    preset.isFactory = true;
    preset.isDefault = isDefault;

    for (const auto& p : params)
    {
        if (p.id == "outputGain")
        {
            continue;
        }

        preset.parameters[p.id] = p.defaultValue;
        preset.parameterOrder.emplace_back(p.id);
    }

    for (const auto& [key, value] : overrides)
    {
        preset.parameters[key] = value;
    }

    return preset;
}

// Each list opens with its default: the parameter defaults themselves, so a node added fresh
// and one set to the default preset agree. Keep ids stable once shipped; the UI lists them.

[[nodiscard]] inline std::vector<EffectPresetDefinition> Room(const std::vector<ParameterDef>& p)
{
    return {
        MakePreset(p, "studio-room", "Studio Room", true, {}),
        MakePreset(p, "small-room", "Small Room", false,
                   {{"decay", 0.22}, {"size", 0.2}, {"damping", 0.6}, {"preDelay", 3.0}, {"mix", 0.2}}),
        MakePreset(p, "bright-room", "Bright Room", false,
                   {{"decay", 0.5}, {"size", 0.45}, {"damping", 0.22}, {"preDelay", 6.0}, {"mix", 0.2}}),
        MakePreset(p, "live-room", "Live Room", false,
                   {{"decay", 0.65}, {"size", 0.7}, {"damping", 0.45}, {"preDelay", 12.0}, {"mix", 0.24}}),
        MakePreset(p, "large-room", "Large Room", false,
                   {{"decay", 0.85}, {"size", 0.95}, {"damping", 0.5}, {"preDelay", 20.0}, {"mix", 0.25}}),
        MakePreset(p, "warm-room", "Warm Room", false,
                   {{"decay", 0.55}, {"size", 0.55}, {"damping", 0.85}, {"preDelay", 10.0}, {"mix", 0.22}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Chamber(const std::vector<ParameterDef>& p)
{
    return {
        MakePreset(p, "echo-chamber", "Echo Chamber", true, {}),
        MakePreset(p, "small-chamber", "Small Chamber", false,
                   {{"decay", 0.35}, {"size", 0.35}, {"tone", 0.55}, {"preDelay", 8.0}, {"mix", 0.2}}),
        MakePreset(p, "bright-chamber", "Bright Chamber", false,
                   {{"decay", 0.6}, {"size", 0.55}, {"tone", 0.8}, {"preDelay", 12.0}, {"mix", 0.22}}),
        MakePreset(p, "dark-chamber", "Dark Chamber", false,
                   {{"decay", 0.65}, {"size", 0.6}, {"tone", 0.22}, {"preDelay", 18.0}, {"mix", 0.25}}),
        MakePreset(p, "large-chamber", "Large Chamber", false,
                   {{"decay", 0.85}, {"size", 0.9}, {"tone", 0.5}, {"preDelay", 28.0}, {"mix", 0.26}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Advanced(const std::vector<ParameterDef>& p)
{
    return {
        MakePreset(p, "wide-room", "Wide Room", true, {}),
        MakePreset(p, "concert-hall", "Concert Hall", false,
                   {{"character", 1.0},
                    {"decay", 0.82},
                    {"size", 0.92},
                    {"mix", 0.26},
                    {"damping", 0.42},
                    {"preDelay", 30.0},
                    {"tone", 0.55},
                    {"width", 1.15},
                    {"diffusion", 0.8},
                    {"lowCut", 110.0},
                    {"highCut", 9000.0},
                    {"modRate", 0.5},
                    {"modDepth", 0.55},
                    {"ducking", 0.04}}),
        MakePreset(p, "bright-plate", "Bright Plate", false,
                   {{"decay", 0.66},
                    {"size", 0.4},
                    {"mix", 0.22},
                    {"damping", 0.22},
                    {"preDelay", 0.0},
                    {"tone", 0.8},
                    {"diffusion", 1.0},
                    {"lowCut", 220.0},
                    {"highCut", 15000.0},
                    {"modRate", 0.8},
                    {"modDepth", 0.4},
                    {"ducking", 0.05}}),
        MakePreset(p, "ducked-lead", "Ducked Lead", false,
                   {{"character", 1.0},
                    {"decay", 0.78},
                    {"size", 0.8},
                    {"mix", 0.35},
                    {"damping", 0.45},
                    {"preDelay", 45.0},
                    {"width", 1.1},
                    {"diffusion", 0.75},
                    {"lowCut", 200.0},
                    {"highCut", 10000.0},
                    {"modRate", 0.6},
                    {"modDepth", 0.5},
                    {"ducking", 0.65}}),
        MakePreset(p, "lush-hall", "Lush Hall", false,
                   {{"character", 1.0},
                    {"decay", 0.9},
                    {"size", 0.85},
                    {"mix", 0.3},
                    {"damping", 0.35},
                    {"preDelay", 22.0},
                    {"tone", 0.65},
                    {"width", 1.2},
                    {"diffusion", 0.85},
                    {"lowCut", 160.0},
                    {"highCut", 11000.0},
                    {"modRate", 0.85},
                    {"modDepth", 0.75}}),
        MakePreset(p, "gritty-spring", "Gritty Spring", false,
                   {{"character", 2.0},
                    {"decay", 0.5},
                    {"size", 0.4},
                    {"mix", 0.25},
                    {"damping", 0.4},
                    {"preDelay", 4.0},
                    {"tone", 0.55},
                    {"width", 0.7},
                    {"diffusion", 0.55},
                    {"lowCut", 220.0},
                    {"highCut", 7000.0},
                    {"modRate", 0.9},
                    {"modDepth", 0.1},
                    {"ducking", 0.05},
                    {"drive", 0.45}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Spring(const std::vector<ParameterDef>& p)
{
    return {
        MakePreset(p, "amp-spring", "Amp Spring", true, {}),
        MakePreset(p, "surf-spring", "Surf Spring", false,
                   {{"decay", 0.7}, {"tone", 0.65}, {"drive", 0.45}, {"mix", 0.4}}),
        MakePreset(p, "short-spring", "Short Spring", false,
                   {{"decay", 0.18}, {"tone", 0.5}, {"drive", 0.1}, {"mix", 0.18}}),
        MakePreset(p, "dark-spring", "Dark Spring", false,
                   {{"decay", 0.45}, {"tone", 0.2}, {"drive", 0.12}, {"mix", 0.22}}),
        MakePreset(p, "bright-spring", "Bright Spring", false,
                   {{"decay", 0.5}, {"tone", 0.85}, {"drive", 0.25}, {"mix", 0.22}}),
        MakePreset(p, "long-spring", "Long Spring", false,
                   {{"decay", 0.9}, {"tone", 0.45}, {"drive", 0.3}, {"mix", 0.3}}),
    };
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> Ambient(const std::vector<ParameterDef>& p)
{
    return {
        MakePreset(p, "wide-bloom", "Wide Bloom", true, {}),
        MakePreset(p, "tight-ambience", "Tight Ambience", false,
                   {{"decay", 0.15},
                    {"space", 0.4},
                    {"diffusion", 0.0},
                    {"preDelay", 12.0},
                    {"tone", 0.45},
                    {"width", 1.0},
                    {"modRate", 0.2},
                    {"modDepth", 0.2},
                    {"mix", 0.22}}),
        MakePreset(p, "lead-halo", "Lead Halo", false,
                   {{"decay", 0.38},
                    {"space", 0.6},
                    {"diffusion", 0.1},
                    {"preDelay", 80.0},
                    {"tone", 0.5},
                    {"width", 1.12},
                    {"modRate", 0.22},
                    {"modDepth", 0.35},
                    {"mix", 0.25}}),
        MakePreset(p, "dark-swell", "Dark Swell", false,
                   {{"decay", 0.68},
                    {"space", 0.8},
                    {"diffusion", 0.55},
                    {"preDelay", 30.0},
                    {"tone", 0.18},
                    {"width", 1.1},
                    {"modRate", 0.1},
                    {"modDepth", 0.3},
                    {"mix", 0.35}}),
        MakePreset(p, "cloud", "Cloud", false,
                   {{"decay", 0.78},
                    {"space", 0.92},
                    {"diffusion", 0.7},
                    {"preDelay", 60.0},
                    {"tone", 0.6},
                    {"width", 1.2},
                    {"modRate", 0.25},
                    {"modDepth", 0.55},
                    {"mix", 0.32}}),
        MakePreset(p, "infinite-wash", "Infinite Wash", false,
                   {{"decay", 0.9},
                    {"space", 1.0},
                    {"diffusion", 0.8},
                    {"preDelay", 40.0},
                    {"tone", 0.38},
                    {"width", 1.25},
                    {"modRate", 0.12},
                    {"modDepth", 0.6},
                    {"mix", 0.28}}),
        // The tail climbs an octave each pass round the tank.
        MakePreset(p, "shimmer", "Shimmer", false,
                   {{"decay", 0.75},
                    {"space", 0.9},
                    {"diffusion", 0.7},
                    {"preDelay", 40.0},
                    {"tone", 0.55},
                    {"width", 1.2},
                    {"modRate", 0.2},
                    {"modDepth", 0.5},
                    {"mix", 0.32},
                    {"shimmer", 0.6}}),
        MakePreset(p, "fifth-halo", "Fifth Halo", false,
                   {{"decay", 0.7},
                    {"space", 0.85},
                    {"diffusion", 0.6},
                    {"preDelay", 60.0},
                    {"tone", 0.5},
                    {"width", 1.15},
                    {"modRate", 0.18},
                    {"modDepth", 0.45},
                    {"mix", 0.3},
                    {"shimmer", 0.55},
                    {"shimmerPitch", 1.0}}),
        // An octave down instead: an organ-like swell under the note.
        MakePreset(p, "deep-shimmer", "Deep Shimmer", false,
                   {{"decay", 0.72},
                    {"space", 0.95},
                    {"diffusion", 0.75},
                    {"preDelay", 30.0},
                    {"tone", 0.35},
                    {"width", 1.2},
                    {"modRate", 0.15},
                    {"modDepth", 0.5},
                    {"mix", 0.3},
                    {"shimmer", 0.5},
                    {"shimmerPitch", 3.0}}),
    };
}
} // namespace reverb_presets
} // namespace guitarfx
