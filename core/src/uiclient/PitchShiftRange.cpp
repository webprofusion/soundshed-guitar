#include "uiclient/PitchShiftRange.h"

#include "dsp/EffectGuids.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::uiclient
{
namespace
{
constexpr double kHardMin = -12.0;
constexpr double kHardMax = 12.0;
/// A pedal needs something to sweep, so the bounds are kept at least this far apart.
constexpr double kMinRangeWidth = 1.0;

double ReadParam(const std::map<std::string, double>& params, const char* key, double fallback)
{
    const auto it = params.find(key);
    return it != params.end() && std::isfinite(it->second) ? it->second : fallback;
}

bool IsSnapped(const std::map<std::string, double>& params)
{
    return ReadParam(params, "stepMode", 1.0) >= 0.5;
}

struct Bounds
{
    double min;
    double max;
};

Bounds ReadBounds(const std::map<std::string, double>& params)
{
    return {std::clamp(ReadParam(params, "minSemitones", kHardMin), kHardMin, kHardMax),
            std::clamp(ReadParam(params, "maxSemitones", kHardMax), kHardMin, kHardMax)};
}
} // namespace

bool IsPitchShiftType(const std::string& type)
{
    return type == EffectGuids::kPitchShift || type == "pitch_shift";
}

bool IsPitchShiftRangeSetting(const std::string& key)
{
    return key == "stepMode" || key == "minSemitones" || key == "maxSemitones";
}

SemitoneRange SemitoneKnobRange(const std::map<std::string, double>& params)
{
    const auto bounds = ReadBounds(params);
    return {std::min(bounds.min, bounds.max), std::max(bounds.min, bounds.max), IsSnapped(params) ? 1.0 : 0.0};
}

std::map<std::string, double> ReconcilePitchShiftParams(const std::map<std::string, double>& params,
                                                        const std::string& changedKey)
{
    const auto bounds = ReadBounds(params);
    double min = bounds.min;
    double max = bounds.max;

    if (changedKey == "minSemitones" && max - min < kMinRangeWidth)
    {
        max = std::min(kHardMax, min + kMinRangeWidth);
        min = std::min(min, max - kMinRangeWidth);
    }
    else if (changedKey == "maxSemitones" && max - min < kMinRangeWidth)
    {
        min = std::max(kHardMin, max - kMinRangeWidth);
        max = std::max(max, min + kMinRangeWidth);
    }

    std::map<std::string, double> updates;

    if (min != bounds.min)
    {
        updates["minSemitones"] = min;
    }

    if (max != bounds.max)
    {
        updates["maxSemitones"] = max;
    }

    const double current = ReadParam(params, "semitones", 0.0);
    const double requested = IsSnapped(params) ? std::round(current) : current;
    const double semitones = std::clamp(requested, std::min(min, max), std::max(min, max));

    if (semitones != current)
    {
        updates["semitones"] = semitones;
    }

    return updates;
}
} // namespace guitarfx::uiclient
