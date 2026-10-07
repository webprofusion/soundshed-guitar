/**
 * ParamFormat.cpp - Parameter value display and control travel (see ParamFormat.h).
 */

#include "uiclient/ParamFormat.h"

#include "dsp/ParamTaper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
std::string Fixed(double value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);

    // JavaScript's toFixed never prints "-0.00".
    if (text.find_first_not_of("-0.") == std::string::npos && text.front() == '-')
    {
        text.erase(0, 1);
    }

    return text;
}

ParamTaper TaperOf(const EffectParamInfo& param)
{
    return param.logTaper ? ParamTaper::Log : ParamTaper::Linear;
}

// An enum steps by its declared step, or by one where it declares none.
double EnumStep(const EffectParamInfo& param)
{
    return param.step > 0.0 ? param.step : 1.0;
}
} // namespace

bool IsToggleParam(const EffectParamInfo& param)
{
    return param.unit == "toggle";
}

bool IsEnumParam(const EffectParamInfo& param)
{
    return param.unit == "enum" && !param.labels.empty();
}

int EnumLabelIndex(const EffectParamInfo& param, double value)
{
    const auto index = std::llround((value - param.minValue) / EnumStep(param));
    return index >= 0 && static_cast<std::size_t>(index) < param.labels.size() ? static_cast<int>(index) : -1;
}

double EnumLabelValue(const EffectParamInfo& param, std::size_t index)
{
    return param.minValue + static_cast<double>(index) * EnumStep(param);
}

std::string FormatTaperedValue(double value, const std::string& unit)
{
    const std::string suffix = !unit.empty() && unit != "amount" ? unit : std::string{};

    // 999.6 Hz would otherwise round to "1000Hz"
    if (suffix == "Hz" && std::abs(value) >= 999.5)
    {
        const double khz = value / 1000.0;
        return Fixed(khz, std::abs(khz) >= 10.0 ? 1 : 2) + "kHz";
    }

    const double magnitude = std::abs(value);
    const int decimals = magnitude >= 100.0 ? 0 : (magnitude >= 10.0 ? 1 : 2);
    return Fixed(value, decimals) + suffix;
}

std::string FormatParamValue(const EffectParamInfo& param, double value)
{
    if (IsToggleParam(param))
    {
        return value >= 0.5 ? "On" : "Off";
    }

    if (IsEnumParam(param))
    {
        const int index = EnumLabelIndex(param, value);
        return index >= 0 ? param.labels[static_cast<std::size_t>(index)] : std::to_string(std::llround(value));
    }

    if (param.unit == "pan")
    {
        if (std::abs(value) < 0.01)
        {
            return "C";
        }

        return (value < 0 ? "L" : "R") + Fixed(std::abs(value * 100.0), 0);
    }

    if (param.logTaper && EffectiveTaper(ParamTaper::Log, param.minValue, param.maxValue) == ParamTaper::Log)
    {
        return FormatTaperedValue(value, param.unit);
    }

    // A parameter that moves in whole steps (a MIDI channel, semitones, a count) shows a whole
    // number, as the web UI's wholeStepLabel (core/ui/ts/paramLabels.ts) does.
    if (param.step >= 1.0 && param.step == std::floor(param.step) && std::abs(value - std::round(value)) <= 1.0e-6)
    {
        return Fixed(std::round(value), 0) + (param.unit == "amount" ? std::string{} : param.unit);
    }

    return Fixed(value, 2) + (param.unit == "amount" ? std::string{} : param.unit);
}

double ParamValueToPosition(const EffectParamInfo& param, double value)
{
    return TaperValueToPosition(TaperOf(param), param.minValue, param.maxValue, value);
}

double ParamPositionToValue(const EffectParamInfo& param, double position)
{
    return TaperPositionToValue(TaperOf(param), param.minValue, param.maxValue, std::clamp(position, 0.0, 1.0));
}

double SnapParamValue(const EffectParamInfo& param, double value)
{
    const double lo = std::min(param.minValue, param.maxValue);
    const double hi = std::max(param.minValue, param.maxValue);
    double clamped = std::clamp(value, lo, hi);

    if (IsEnumParam(param) || IsToggleParam(param))
    {
        clamped = std::round(clamped);
    }
    else if (param.step > 0.0)
    {
        clamped = std::round((clamped - lo) / param.step) * param.step + lo;
    }

    return std::clamp(clamped, lo, hi);
}

double NodeParamValue(const GraphNode& node, const EffectParamInfo& param)
{
    const auto it = node.params.find(param.key);
    return it != node.params.end() ? it->second : param.defaultValue;
}
} // namespace guitarfx::uiclient
