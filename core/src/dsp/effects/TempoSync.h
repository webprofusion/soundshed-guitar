#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace guitarfx::tempo_sync
{
inline constexpr double kDefaultBpm = 120.0;
inline constexpr double kMinBpm = 30.0;
inline constexpr double kMaxBpm = 300.0;

inline constexpr int kSyncModeOff = 0;
inline constexpr int kSyncModeTempo = 1;

inline constexpr std::array<double, 15> kDivisionBeats = {
    4.0,       // 1/1
    2.0,       // 1/2
    3.0,       // 1/2D
    4.0 / 3.0, // 1/2T
    1.0,       // 1/4
    1.5,       // 1/4D
    2.0 / 3.0, // 1/4T
    0.5,       // 1/8
    0.75,      // 1/8D
    1.0 / 3.0, // 1/8T
    0.25,      // 1/16
    0.375,     // 1/16D
    1.0 / 6.0, // 1/16T
    0.125,     // 1/32
    1.0 / 12.0 // 1/32T
};

inline double ClampBpm(double bpm)
{
    return std::clamp(bpm, kMinBpm, kMaxBpm);
}

inline int ClampSyncMode(double value)
{
    return static_cast<int>(std::clamp(value, 0.0, 1.0));
}

inline int ClampDivision(double value)
{
    return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(kDivisionBeats.size() - 1)));
}

inline double DivisionBeats(int index)
{
    const size_t clamped = static_cast<size_t>(ClampDivision(static_cast<double>(index)));
    return kDivisionBeats[clamped];
}

inline double DivisionRateHz(double bpm, int divisionIndex)
{
    const double beatsPerCycle = DivisionBeats(divisionIndex);
    const double beatsPerSecond = ClampBpm(bpm) / 60.0;
    return beatsPerSecond / std::max(1.0e-9, beatsPerCycle);
}

inline double DivisionDelayMs(double bpm, int divisionIndex)
{
    const double beatsPerCycle = DivisionBeats(divisionIndex);
    return beatsPerCycle * 60000.0 / ClampBpm(bpm);
}

inline std::vector<std::string> SyncModeLabels()
{
    return {"Free", "Tempo"};
}

inline std::vector<std::string> DivisionLabels()
{
    return {"1/1",  "1/2",  "1/2D", "1/2T",  "1/4",   "1/4D", "1/4T", "1/8",
            "1/8D", "1/8T", "1/16", "1/16D", "1/16T", "1/32", "1/32T"};
}
} // namespace guitarfx::tempo_sync