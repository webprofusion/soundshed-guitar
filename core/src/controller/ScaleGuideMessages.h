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
// note, and `action: "stop"` ends either.

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
    /// A mono signal to play into the input. `announce` as for DemoPreviewService.
    std::function<void(std::vector<float> samples, double sampleRate, bool announce)> play;
    std::function<void()> stop;
};

void RegisterScaleGuideMessages(MessageHandlerRegistry& registry, std::function<void(const std::string&)> sendToUI,
                                ScaleGuideAudio audio);
} // namespace guitarfx
