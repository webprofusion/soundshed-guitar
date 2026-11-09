#pragma once

// ScalePlayback — sounding what the Scale Guide shows (docs/plans/scale-guide.md, Play).
//
// The chosen scale position, chord shape or arpeggio run becomes a list of timed notes, and a
// plucked-string voice renders them to a mono guitar signal at DI level. The controller plays that
// into the input, so it goes through the preset like a guitar would. All of this runs on the message
// thread: the audio thread only ever copies the finished buffer.

#include <nlohmann/json.hpp>

#include <vector>

namespace guitarfx::music
{
struct PlayedNote
{
    int midi = 0;
    int string = 0;
    int fret = 0;
    double startSec = 0.0;
    double lengthSec = 0.0;
    /// A hammer-on or pull-off: the fretting hand sounds it, so it starts softer and rounder.
    bool legato = false;
};

/// What the view's chosen position plays: a run's steps as eighth notes at `bpm`, or a chord shape
/// strummed once. Empty with no position chosen.
[[nodiscard]] std::vector<PlayedNote> ScaleGuidePlaybackNotes(const nlohmann::json& view, double bpm);

/// A short example of the view's scale: one octave from the lowest root in its chosen position's run
/// up to the next root and back down, alternate picked. Empty outside Scales mode or with no run.
[[nodiscard]] std::vector<PlayedNote> ScaleExampleNotes(const nlohmann::json& view, double bpm);

/// A chord shape strummed once, low string first: one fret per string of the view's neck, -1 for a
/// string not played.
[[nodiscard]] std::vector<PlayedNote> StrummedChord(const nlohmann::json& view, const std::vector<int>& frets);

/// One note left ringing, for a click on the neck.
[[nodiscard]] PlayedNote SingleNote(int midi);

/// The notes as a mono guitar signal with its loudest peak at DI level. Rendered sample for sample
/// the same every time, so a test can pin it.
[[nodiscard]] std::vector<float> RenderPluckedNotes(const std::vector<PlayedNote>& notes, double sampleRate);
} // namespace guitarfx::music
