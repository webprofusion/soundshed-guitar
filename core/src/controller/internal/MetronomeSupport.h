#pragma once

/**
 * MetronomeSupport.h — Metronome tuning constants, meter decoding and the bar
 * plan the click generator runs from.
 *
 * The click generator, the settings loader and the UI message handler all need
 * the same bounds and defaults. Keeping them here is what stops the three
 * copies drifting: a clamp that disagrees with the settings default silently
 * rewrites the user's stored value on the next save.
 *
 * The bar plan is the other half of that. A time signature, a grouping, an
 * accent pattern and a subdivision are four settings that only mean anything
 * together, so they are resolved once on the message thread into a plain
 * struct the audio thread can read without parsing anything.
 */

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::controller_detail
{
inline constexpr const char* kMetronomeEnabledSettingKey = "metronome.enabled";
inline constexpr const char* kMetronomeBpmSettingKey = "metronome.bpm";
inline constexpr const char* kMetronomeVolumeDbSettingKey = "metronome.volumeDb";
inline constexpr const char* kMetronomePanSettingKey = "metronome.pan";
inline constexpr const char* kMetronomeClickTypeSettingKey = "metronome.clickType";
inline constexpr const char* kMetronomeClickConfigSettingKey = "metronome.clickConfig";
inline constexpr const char* kMetronomeBeatPatternSettingKey = "metronome.beatPattern";
inline constexpr const char* kMetronomeTimeSigNumSettingKey = "metronome.timeSigNum";
inline constexpr const char* kMetronomeTimeSigDenSettingKey = "metronome.timeSigDen";
inline constexpr const char* kMetronomeGroupingSettingKey = "metronome.grouping";
inline constexpr const char* kMetronomeSubdivisionSettingKey = "metronome.subdivision";

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
inline constexpr int kMetronomeMaxBeatsPerBar = 16;
inline constexpr int kMetronomeDefaultTimeSigNum = 4;
inline constexpr int kMetronomeDefaultTimeSigDen = 4;
inline constexpr const char* kMetronomeDefaultClickType = "kit1";
inline constexpr const char* kMetronomeDefaultSubdivision = "1/4";
inline constexpr double kMetronomeClickSeconds = 0.02;
inline constexpr double kMetronomeClickFrequencyHz = 1800.0;
inline constexpr double kTwoPi = 6.28318530717958647692;

/// Level of a beat within the bar. The stored pattern is one character per
/// beat: H accent, L normal, M medium, S silent (`-` and `.` also read as S).
/// Medium is a secondary accent — the accent sound, pulled back, which is what
/// a group head in 7/8 or the 4 of 6/8 wants.
enum class BeatLevel
{
    Accent,
    Normal,
    Medium,
    Silent,
};

// Level is set here, not by the kit. A click kit is peak-normalised across
// its three one-shots so the accent reads through timbre, and these gains are
// what put the four levels in order for every kit alike:
//   accent 1.0 > medium > normal > subdivision.
inline constexpr float kMetronomeAccentBeatGain = 1.0f;
inline constexpr float kMetronomeMediumBeatGain = 0.7f;
inline constexpr float kMetronomeNormalBeatGain = 0.55f;
inline constexpr float kMetronomeSubdivisionGain = 0.3f;

/// Which sample slot a tick plays from. Sub falls back to Low when a click
/// type ships no subdivision sample.
enum class ClickVoice
{
    High,
    Low,
    Sub,
};

/// A resolved bar: everything the audio thread needs to place the next tick,
/// with no strings and nothing to parse. Rebuilt on the message thread
/// whenever the meter, pattern or subdivision changes.
struct MetronomeBarPlan
{
    struct Beat
    {
        ClickVoice voice = ClickVoice::Low;
        float gain = 1.0f;
        bool silent = false;
        BeatLevel level = BeatLevel::Normal;
    };

    int beatsPerBar = kMetronomeDefaultTimeSigNum;
    /// Beat length relative to a quarter note: 4 / denominator.
    double beatScale = 1.0;
    int ticksPerBeat = 1;
    float subGain = kMetronomeSubdivisionGain;
    std::vector<Beat> beats;
};

/// The subdivisions the UI offers, and how many ticks each puts in a beat.
struct MetronomeSubdivision
{
    const char* id;
    int ticksPerBeat;
};

inline constexpr std::array<MetronomeSubdivision, 6> kMetronomeSubdivisions = {{
    {"1/4", 1},
    {"1/8", 2},
    {"1/8T", 3},
    {"1/16", 4},
    {"1/16T", 6},
    {"1/32", 8},
}};

inline int TicksPerBeatForSubdivision(std::string_view id)
{
    for (const auto& option : kMetronomeSubdivisions)
    {
        if (id == option.id)
        {
            return option.ticksPerBeat;
        }
    }

    return 1;
}

inline std::string NormaliseSubdivisionId(std::string_view id)
{
    for (const auto& option : kMetronomeSubdivisions)
    {
        if (id == option.id)
        {
            return option.id;
        }
    }

    return kMetronomeDefaultSubdivision;
}

inline BeatLevel BeatLevelFromChar(char raw)
{
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));

    if (c == 'H')
    {
        return BeatLevel::Accent;
    }

    if (c == 'M')
    {
        return BeatLevel::Medium;
    }

    if (c == 'S' || c == '-' || c == '.')
    {
        return BeatLevel::Silent;
    }

    return BeatLevel::Normal;
}

inline char CharFromBeatLevel(BeatLevel level)
{
    switch (level)
    {
    case BeatLevel::Accent:
        return 'H';
    case BeatLevel::Medium:
        return 'M';
    case BeatLevel::Silent:
        return 'S';
    case BeatLevel::Normal:
    default:
        return 'L';
    }
}

inline int ClampBeatsPerBar(int beatsPerBar)
{
    if (beatsPerBar < 1)
    {
        return 1;
    }

    return beatsPerBar > kMetronomeMaxBeatsPerBar ? kMetronomeMaxBeatsPerBar : beatsPerBar;
}

/// Denominators the meter picker offers. Anything else falls back to 4 rather
/// than producing a beat length nothing in the UI can draw.
inline int ClampTimeSigDen(int den)
{
    switch (den)
    {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
        return den;
    default:
        return kMetronomeDefaultTimeSigDen;
    }
}

/// Splits "2+2+3" into beat counts. Returns empty when the text is malformed
/// or does not add up to `beatsPerBar` — an unusable grouping is dropped
/// rather than half-applied.
inline std::vector<int> ParseGrouping(const std::string& grouping, int beatsPerBar)
{
    std::vector<int> groups;
    int current = 0;
    bool sawDigit = false;

    for (const char ch : grouping)
    {
        if (ch >= '0' && ch <= '9')
        {
            current = (current * 10) + (ch - '0');
            sawDigit = true;
            continue;
        }

        if (ch == '+' || ch == ' ' || ch == ',' || ch == '-')
        {
            if (sawDigit)
            {
                groups.push_back(current);
            }

            current = 0;
            sawDigit = false;
            continue;
        }

        return {};
    }

    if (sawDigit)
    {
        groups.push_back(current);
    }

    int total = 0;

    for (const int value : groups)
    {
        if (value <= 0)
        {
            return {};
        }

        total += value;
    }

    if (groups.empty() || total != ClampBeatsPerBar(beatsPerBar))
    {
        return {};
    }

    return groups;
}

/// Canonical "2+2+3" text, or empty when the grouping does not apply.
inline std::string NormaliseGrouping(const std::string& grouping, int beatsPerBar)
{
    const auto groups = ParseGrouping(grouping, beatsPerBar);

    if (groups.empty())
    {
        return {};
    }

    std::string result;

    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        if (i > 0)
        {
            result += '+';
        }

        result += std::to_string(groups[i]);
    }

    return result;
}

/// The grouping a meter implies when the user has not named one: compound
/// meters (6/8, 9/8, 12/8) fall into threes, everything else stays flat.
inline std::vector<int> ImpliedGrouping(int beatsPerBar, int timeSigDen)
{
    const int beats = ClampBeatsPerBar(beatsPerBar);

    if (timeSigDen == 8 && beats > 3 && (beats % 3) == 0)
    {
        return std::vector<int>(static_cast<std::size_t>(beats / 3), 3);
    }

    return {};
}

/// Beat 1 accents, each later group head takes a medium accent, the rest are
/// normal. The
/// grouping argument wins over the one the meter implies.
inline std::string DefaultBeatPattern(int beatsPerBar, int timeSigDen, const std::string& grouping)
{
    const int beats = ClampBeatsPerBar(beatsPerBar);
    auto groups = ParseGrouping(grouping, beats);

    if (groups.empty())
    {
        groups = ImpliedGrouping(beats, timeSigDen);
    }

    std::string pattern(static_cast<std::size_t>(beats), 'L');
    pattern[0] = 'H';

    int cursor = 0;

    for (std::size_t i = 0; i + 1 < groups.size(); ++i)
    {
        cursor += groups[i];

        if (cursor > 0 && cursor < beats)
        {
            pattern[static_cast<std::size_t>(cursor)] = 'M';
        }
    }

    return pattern;
}

/// Forces a stored or user-supplied pattern to exactly one character per beat.
/// A short pattern repeats (so "HL" still alternates across a 4/4 bar) and a
/// long one is cut; an empty one becomes the meter's default.
inline std::string NormaliseBeatPattern(std::string_view raw, int beatsPerBar, int timeSigDen,
                                        const std::string& grouping)
{
    const int beats = ClampBeatsPerBar(beatsPerBar);
    std::string filtered;

    for (const char ch : raw)
    {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        if (upper == 'H' || upper == 'L' || upper == 'M' || upper == 'S' || upper == '-' || upper == '.')
        {
            filtered += CharFromBeatLevel(BeatLevelFromChar(upper));
        }
    }

    if (filtered.empty())
    {
        return DefaultBeatPattern(beats, timeSigDen, grouping);
    }

    std::string pattern;
    pattern.reserve(static_cast<std::size_t>(beats));

    for (int i = 0; i < beats; ++i)
    {
        pattern += filtered[static_cast<std::size_t>(i) % filtered.size()];
    }

    return pattern;
}

/// Resolves the four meter settings into the struct Render() runs from.
inline MetronomeBarPlan BuildBarPlan(int timeSigNum, int timeSigDen, const std::string& grouping,
                                     const std::string& beatPattern, const std::string& subdivision)
{
    MetronomeBarPlan plan;
    plan.beatsPerBar = ClampBeatsPerBar(timeSigNum);
    plan.beatScale = 4.0 / static_cast<double>(ClampTimeSigDen(timeSigDen));
    plan.ticksPerBeat = TicksPerBeatForSubdivision(subdivision);
    plan.subGain = kMetronomeSubdivisionGain;

    const std::string pattern = NormaliseBeatPattern(beatPattern, plan.beatsPerBar, timeSigDen, grouping);
    plan.beats.reserve(pattern.size());

    for (const char ch : pattern)
    {
        MetronomeBarPlan::Beat beat;
        beat.level = BeatLevelFromChar(ch);

        switch (beat.level)
        {
        case BeatLevel::Accent:
            beat.voice = ClickVoice::High;
            beat.gain = kMetronomeAccentBeatGain;
            break;
        case BeatLevel::Medium:
            beat.voice = ClickVoice::High;
            beat.gain = kMetronomeMediumBeatGain;
            break;
        case BeatLevel::Silent:
            beat.silent = true;
            beat.gain = 0.0f;
            break;
        case BeatLevel::Normal:
        default:
            beat.voice = ClickVoice::Low;
            beat.gain = kMetronomeNormalBeatGain;
            break;
        }

        plan.beats.push_back(beat);
    }

    return plan;
}
} // namespace guitarfx::controller_detail
