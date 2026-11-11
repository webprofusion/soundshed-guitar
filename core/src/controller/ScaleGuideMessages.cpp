#include "controller/ScaleGuideMessages.h"

#include "MessageHandlerRegistry.h"
#include "music/ScaleGuideView.h"
#include "music/ScalePlayback.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace guitarfx
{
namespace
{
constexpr double kFallbackSampleRate = 48000.0;

double RenderRate(const ScaleGuideAudio& audio)
{
    const double rate = audio.sampleRate();
    return rate > 0.0 ? rate : kFallbackSampleRate;
}

/// The view the payload's request draws, with `position` in place of the request's when given.
nlohmann::json RequestedView(const nlohmann::json& payload, int position = -1)
{
    const auto found = payload.find("request");
    auto request = found != payload.end() && found->is_object() ? *found : nlohmann::json::object();

    if (position >= 0)
    {
        request["position"] = position;
    }

    return music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
}

/// The notes rendered at the level the payload's `volume` asks for, 0 to 1, full without one. The
/// slider is squared, so its middle is a quarter of the level (-12 dB), as a volume control feels.
std::vector<float> Rendered(const std::vector<music::PlayedNote>& notes, double rate, const nlohmann::json& payload)
{
    auto samples = music::RenderPluckedNotes(notes, rate);
    const auto asked = payload.find("volume");
    const double volume =
        asked != payload.end() && asked->is_number() ? std::clamp(asked->get<double>(), 0.0, 1.0) : 1.0;
    const auto gain = static_cast<float>(volume * volume);

    if (gain < 1.0f)
    {
        for (auto& sample : samples)
        {
            sample *= gain;
        }
    }

    return samples;
}

/// Sounds the notes, and tells the UI when each one falls so it can follow along. `onBar` waits for
/// the metronome's bar line.
void PlayNotes(const std::vector<music::PlayedNote>& notes, const ScaleGuideAudio& audio,
               const std::function<void(const std::string&)>& send, bool onBar, const nlohmann::json& payload)
{
    if (notes.empty())
    {
        return;
    }

    const double rate = RenderRate(audio);
    const double delaySec = audio.play(Rendered(notes, rate, payload), rate, true, onBar);

    nlohmann::json timed = nlohmann::json::array();

    for (const auto& note : notes)
    {
        timed.push_back({{"string", note.string},
                         {"fret", note.fret},
                         {"midi", note.midi},
                         {"startMs", note.startSec * 1000.0},
                         {"lengthMs", note.lengthSec * 1000.0},
                         {"step", note.step},
                         {"chord", note.chord}});
    }

    send(nlohmann::json{{"type", "scaleGuidePlayback"}, {"notes", timed}, {"delayMs", delaySec * 1000.0}}.dump());
}
} // namespace

void RegisterScaleGuideMessages(MessageHandlerRegistry& registry, std::function<void(const std::string&)> sendToUI,
                                ScaleGuideAudio audio)
{
    registry.Register(
        "scaleGuide", [send = std::move(sendToUI), audio = std::move(audio)](const nlohmann::json& payload) {
            const std::string action = payload.value("action", "view");

            if (action == "play")
            {
                const auto bar = payload.find("bar");
                const int onlyBar = bar != payload.end() && bar->is_number_integer() ? bar->get<int>() : -1;
                const double barSec = audio.barSeconds ? audio.barSeconds() : 0.0;
                PlayNotes(music::ScaleGuidePlaybackNotes(RequestedView(payload), audio.tempoBpm(), barSec, onlyBar),
                          audio, send, true, payload);
                return;
            }

            if (action == "playScale")
            {
                // The whole neck has no one way through, so its example comes from the first position.
                const auto request = payload.find("request");
                const bool wholeNeck =
                    request == payload.end() || !request->is_object() || request->value("position", 0) <= 0;
                PlayNotes(music::ScaleExampleNotes(RequestedView(payload, wholeNeck ? 1 : -1), audio.tempoBpm()), audio,
                          send, true, payload);
                return;
            }

            if (action == "playChord")
            {
                std::vector<int> frets;

                for (const auto& fret : payload.value("frets", nlohmann::json::array()))
                {
                    frets.push_back(fret.is_number_integer() ? std::clamp(fret.get<int>(), -1, 36) : -1);
                }

                PlayNotes(music::StrummedChord(RequestedView(payload), frets), audio, send, false, payload);
                return;
            }

            if (action == "playNote")
            {
                const int midi = std::clamp(payload.value("midi", 60), 0, 127);
                const double rate = RenderRate(audio);
                audio.play(Rendered({music::SingleNote(midi)}, rate, payload), rate, false, false);
                return;
            }

            if (action == "stop")
            {
                audio.stop();
                return;
            }

            nlohmann::json message;

            if (action == "catalog")
            {
                message = music::ScaleGuideCatalogJson();
                message["type"] = "scaleGuideCatalog";
            }
            else
            {
                const auto request = payload.find("request");
                message["view"] = music::BuildScaleGuideView(
                    music::ParseScaleGuideRequest(request != payload.end() ? *request : nlohmann::json::object()));
                message["type"] = "scaleGuideView";
            }

            if (const auto id = payload.find("requestId"); id != payload.end() && id->is_number_integer())
            {
                message["requestId"] = *id;
            }

            send(message.dump());
        });
}
} // namespace guitarfx
