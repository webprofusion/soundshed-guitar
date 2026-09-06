#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace guitarfx
{
/**
 * A single slot in a composite (multi-rig) preset.
 * References a regular Preset by ID and carries per-slot mix settings.
 */
struct CompositePresetSlot
{
    std::string slotId;   // Stable DSP instance ID (e.g. "p1", "p2")
    std::string presetId; // References a regular Preset by ID
    double mix = 1.0;     // Linear gain [0.0, 1.0]
    double pan = 0.0;     // Stereo pan [-1.0, 1.0]
    bool mute = false;
    bool solo = false;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CompositePresetSlot, slotId, presetId, mix, pan, mute, solo)
};

/**
 * A saved multi-rig preset that captures the full mixer configuration:
 * which presets are active, at what mix/pan/mute/solo settings, and the
 * mix's own output level.
 */
struct CompositePreset
{
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    std::string createdAt;
    std::string modifiedAt;
    std::vector<CompositePresetSlot> slots;
    // The mix's own level in dB, applied to the summed presets ahead of the global output
    // stage. Files written before this field carried a linear "masterGain" (a copy of the
    // global output gain) and "limiterEnabled"; both are ignored when read.
    double mixGainDb = 0.0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CompositePreset, id, name, description, tags, createdAt, modifiedAt,
                                                slots, mixGainDb)
};
} // namespace guitarfx
