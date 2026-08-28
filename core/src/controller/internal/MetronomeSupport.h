#pragma once

/**
 * MetronomeSupport.h — Metronome tuning constants and beat-pattern decoding.
 *
 * The click generator, the settings loader and the UI message handler all need
 * the same bounds and defaults. Keeping them here is what stops the three
 * copies drifting: a clamp that disagrees with the settings default silently
 * rewrites the user's stored value on the next save.
 */

#include <cctype>
#include <string>

namespace guitarfx::controller_detail
{
inline constexpr const char* kMetronomeEnabledSettingKey = "metronome.enabled";
inline constexpr const char* kMetronomeBpmSettingKey = "metronome.bpm";
inline constexpr const char* kMetronomeVolumeDbSettingKey = "metronome.volumeDb";
inline constexpr const char* kMetronomePanSettingKey = "metronome.pan";
inline constexpr const char* kMetronomeClickTypeSettingKey = "metronome.clickType";
inline constexpr const char* kMetronomeClickConfigSettingKey = "metronome.clickConfig";
inline constexpr const char* kMetronomeBeatPatternSettingKey = "metronome.beatPattern";

// Pre-namespacing keys, still read when migrating an older settings file.
inline constexpr const char* kMetronomeLegacyBpmKey = "metronomeBpm";
inline constexpr const char* kMetronomeLegacyVolumeDbKey = "metronomeVolumeDb";
inline constexpr const char* kMetronomeLegacyPanKey = "metronomePan";
inline constexpr const char* kMetronomeLegacyClickTypeKey = "metronomeClickType";

inline constexpr double kMetronomeDefaultBpm = 120.0;
inline constexpr double kMetronomeMinBpm = 30.0;
inline constexpr double kMetronomeMaxBpm = 300.0;
inline constexpr double kMetronomeMinVolumeDb = -60.0;
inline constexpr double kMetronomeMaxVolumeDb = 12.0;
inline constexpr double kMetronomeDefaultVolumeDb = -12.0;
inline constexpr double kMetronomeDefaultPan = 0.0;
inline constexpr int kMetronomeBeatsPerBar = 4;
inline constexpr const char* kMetronomeDefaultClickType = "click";
inline constexpr double kMetronomeClickSeconds = 0.02;
inline constexpr double kMetronomeClickFrequencyHz = 1800.0;
inline constexpr double kTwoPi = 6.28318530717958647692;

/// Returns 'H' (High/accent), 'L' (Low), or 'S' (Silent) for a beat position.
/// Empty pattern = first beat High, rest Low.
inline char BeatAccent(const std::string& pattern, int beatIndex)
{
    if (pattern.empty())
    {
        return (beatIndex == 0) ? 'H' : 'L';
    }

    const std::size_t idx = static_cast<std::size_t>(beatIndex) % pattern.size();
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(pattern[idx])));

    if (c == 'H')
    {
        return 'H';
    }

    if (c == 'S' || c == '-' || c == '.')
    {
        return 'S';
    }

    return 'L';
}
} // namespace guitarfx::controller_detail
