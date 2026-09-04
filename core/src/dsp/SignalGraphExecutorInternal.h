/**
 * @file SignalGraphExecutorInternal.h
 * @brief Helpers shared between SignalGraphExecutor's translation units.
 *
 * SignalGraphExecutor is one class split across SignalGraphExecutor.cpp (lifecycle,
 * graph building, the per-block Process loop) and SignalGraphExecutorPlan.cpp (building
 * the resolved execution plan, and running one planned node). These are the free
 * functions both halves need; they were file-local statics until the split.
 */

#pragma once

#include "presets/PresetTypes.h"
#include "dsp/EffectGuids.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace guitarfx::executor_detail
{
struct LevelStats
{
    double peak = 0.0;
    double rms = 0.0;
    int clipCount = 0;
};

/// Accumulates one channel into running peak / sum-of-squares / clip counters.
///
/// Written to vectorise: float accumulators and a branchless clip count, so the
/// compiler can run this eight lanes wide under AVX2 instead of one sample at a
/// time. The sum stays in float, which costs a little precision over a block --
/// far below the 0.1 dB the meter is rounded to before it is even sent.
inline void AccumulateChannelStats(const float* samples, int numSamples, float& peak, float& sumSquares, int& clipCount)
{
    float localPeak = peak;
    float localSum = sumSquares;
    int localClips = clipCount;

    for (int i = 0; i < numSamples; ++i)
    {
        const float value = samples[i];
        const float absValue = std::abs(value);
        localPeak = std::max(localPeak, absValue);
        localSum += value * value;
        localClips += (absValue > 1.0f) ? 1 : 0;
    }

    peak = localPeak;
    sumSquares = localSum;
    clipCount = localClips;
}

inline LevelStats ComputeLevelStats(const float* left, const float* right, int numSamples)
{
    LevelStats stats;

    if (numSamples <= 0)
    {
        return stats;
    }

    float peak = 0.0f;
    float sumSquares = 0.0f;
    int clipCount = 0;
    std::size_t sampleCount = 0;

    if (left)
    {
        AccumulateChannelStats(left, numSamples, peak, sumSquares, clipCount);
        sampleCount += static_cast<std::size_t>(numSamples);
    }

    if (right)
    {
        AccumulateChannelStats(right, numSamples, peak, sumSquares, clipCount);
        sampleCount += static_cast<std::size_t>(numSamples);
    }

    stats.peak = static_cast<double>(peak);
    stats.clipCount = clipCount;

    if (sampleCount > 0)
    {
        stats.rms = std::sqrt(static_cast<double>(sumSquares) / static_cast<double>(sampleCount));
    }

    return stats;
}

/// Node types whose output can be stereo even when their input is not.
inline bool NodeMayProduceStereo(const std::string& type, const std::string& category)
{
    if (type == kNodeTypeInput || type == kNodeTypeOutput || type == kNodeTypeSplitter || type == kNodeTypeMixer)
    {
        return false;
    }

    return category == "mod" || category == "delay" || category == "reverb";
}

inline bool IsNamNodeType(const std::string& type)
{
    return type == EffectGuids::kAmpNam || type == EffectGuids::kAmpNamOptimized || type == EffectGuids::kAmpNamBlend ||
           type == EffectGuids::kFxNam || type == "amp_nam" || type == "amp_nam_optimized" || type == "amp_nam_blend" ||
           type == "fx_nam";
}
} // namespace guitarfx::executor_detail
