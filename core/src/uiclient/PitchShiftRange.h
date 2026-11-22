#pragma once

/**
 * PitchShiftRange.h - Pitch Shift's Semitones knob follows the node's own settings, as the web
 * UI's signalPath/paramsPanel/pitchShiftRange.ts has it. It spans Range Min to Range Max, the
 * interval an expression pedal sweeps, and moves in whole semitones only while Snap to Semitone
 * is on. The engine applies the same rules (dsp/effects/PitchShiftEffect.h), so the knob shows
 * what is heard.
 */

#include <map>
#include <string>

namespace guitarfx::uiclient
{
struct SemitoneRange
{
    double min = -12.0;
    double max = 12.0;
    /// 0 leaves the knob continuous.
    double step = 0.0;
};

/// Pitch Shift, by its id or its legacy name.
[[nodiscard]] bool IsPitchShiftType(const std::string& type);

/// The settings that change what the Semitones knob allows: Snap to Semitone, Range Min, Range Max.
[[nodiscard]] bool IsPitchShiftRangeSetting(const std::string& key);

/// The Semitones knob's span and step for a node's settings. Crossed bounds read as one range, as
/// the engine reads them.
[[nodiscard]] SemitoneRange SemitoneKnobRange(const std::map<std::string, double>& params);

/// The other settings that have to move once `changedKey` has been set: the bounds stay a
/// semitone apart, the one just set winning unless it is pinned at the end of the scale, and the
/// semitone setting stays inside them, on a whole semitone while snapped (halves rounded away
/// from zero, as the engine's std::round). Empty when nothing else moves.
[[nodiscard]] std::map<std::string, double> ReconcilePitchShiftParams(const std::map<std::string, double>& params,
                                                                     const std::string& changedKey);
} // namespace guitarfx::uiclient
