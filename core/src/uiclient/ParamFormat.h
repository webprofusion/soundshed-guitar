#pragma once

/**
 * ParamFormat.h - How a parameter's value is shown and how a control's travel maps onto it.
 *
 * Formatting follows the web UI's knob (core/ui/ts/signalPath/paramsPanel/paramControls.ts):
 * an enum shows its label, pan shows L/C/R, a log-taper value shows about three significant
 * figures (kHz from 1 kHz up), anything else two decimals with its unit. Travel goes through
 * the engine's own taper (dsp/ParamTaper.h), which is what MIDI and host automation use, so
 * a control here and an expression pedal agree on where the middle is.
 */

#include "uiclient/ClientState.h"

#include <string>

namespace guitarfx::uiclient
{
[[nodiscard]] std::string FormatParamValue(const EffectParamInfo& param, double value);

/// The web UI's formatTaperedValue: about three significant figures, kHz from 1 kHz up.
[[nodiscard]] std::string FormatTaperedValue(double value, const std::string& unit);

[[nodiscard]] double ParamValueToPosition(const EffectParamInfo& param, double value);
[[nodiscard]] double ParamPositionToValue(const EffectParamInfo& param, double position);

/// Clamps to the range and snaps to the parameter's step, if it has one.
[[nodiscard]] double SnapParamValue(const EffectParamInfo& param, double value);

[[nodiscard]] bool IsToggleParam(const EffectParamInfo& param);
[[nodiscard]] bool IsEnumParam(const EffectParamInfo& param);

/// The value a node holds for a parameter, or the parameter's default.
[[nodiscard]] double NodeParamValue(const GraphNode& node, const EffectParamInfo& param);
} // namespace guitarfx::uiclient
