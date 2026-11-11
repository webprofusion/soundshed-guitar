#pragma once

// ScaleGuideMessages — the Scale Guide's one UI message (docs/plans/scale-guide.md).
//
// `scaleGuide` with `action: "catalog"` answers `scaleGuideCatalog`: the scales, chords,
// progressions and tunings for the guide's pickers. With `action: "view"` and a request
// (music/ScaleGuideView.h) it answers `scaleGuideView`, the model the UI draws, with the
// request's `requestId` echoed so the UI can drop an answer a later change has overtaken.
//
// The guide holds no state of its own (the UI keeps its choices in app settings), so this is
// a registration, not a service: there is nothing for the controller to own.
//
// `action: "play"` sounds the request's chosen position through the preset and answers
// `scaleGuidePlayback` with when each note falls, `action: "playNote"` with `midi` sounds one
// note, and `action: "stop"` ends either. With `bar`, play sounds that one chord of a
// progression's arpeggios. Play and a scale's example start on the metronome's next bar line
// while it is clicking, so they are in time with it: `delayMs` in the answer says how long that is.
// Every play action takes `volume`, 0 to 1: the guitar's level going into the preset.

#include <functional>
#include <string>
#include <vector>

namespace guitarfx
{
class MessageHandlerRegistry;

/// How the guide reaches the audio it does not own.
struct ScaleGuideAudio
{
    std::function<double()> sampleRate;
    std::function<double()> tempoBpm;
    /// One bar of the metronome's, in seconds: a progression's chords each start on one.
    std::function<double()> barSeconds;
    /// A mono signal to play into the input. `announce` as for DemoPreviewService. With `onBar` it
    /// waits for the metronome's next bar line, if the metronome is clicking. Returns the wait, in
    /// seconds.
    std::function<double(std::vector<float> samples, double sampleRate, bool announce, bool onBar)> play;
    std::function<void()> stop;
};

void RegisterScaleGuideMessages(MessageHandlerRegistry& registry, std::function<void(const std::string&)> sendToUI,
                                ScaleGuideAudio audio);
} // namespace guitarfx
