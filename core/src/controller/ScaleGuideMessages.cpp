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

/// Sounds the notes, and tells the UI when each one falls so it can follow along.
void PlayNotes(const std::vector<music::PlayedNote>& notes, const ScaleGuideAudio& audio,
               const std::function<void(const std::string&)>& send)
{
    if (notes.empty())
    {
        return;
    }

    const double rate = RenderRate(audio);
    audio.play(music::RenderPluckedNotes(notes, rate), rate, true);

    nlohmann::json timed = nlohmann::json::array();

    for (const auto& note : notes)
    {
        timed.push_back({{"string", note.string},
                         {"fret", note.fret},
                         {"midi", note.midi},
                         {"startMs", note.startSec * 1000.0},
                         {"lengthMs", note.lengthSec * 1000.0}});
    }

    send(nlohmann::json{{"type", "scaleGuidePlayback"}, {"notes", timed}}.dump());
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
                PlayNotes(music::ScaleGuidePlaybackNotes(RequestedView(payload), audio.tempoBpm()), audio, send);
                return;
            }

            if (action == "playScale")
            {
                // The whole neck has no one way through, so its example comes from the first position.
                const auto request = payload.find("request");
                const bool wholeNeck =
                    request == payload.end() || !request->is_object() || request->value("position", 0) <= 0;
                PlayNotes(music::ScaleExampleNotes(RequestedView(payload, wholeNeck ? 1 : -1), audio.tempoBpm()), audio,
                          send);
                return;
            }

            if (action == "playChord")
            {
                std::vector<int> frets;

                for (const auto& fret : payload.value("frets", nlohmann::json::array()))
                {
                    frets.push_back(fret.is_number_integer() ? std::clamp(fret.get<int>(), -1, 36) : -1);
                }

                PlayNotes(music::StrummedChord(RequestedView(payload), frets), audio, send);
                return;
            }

            if (action == "playNote")
            {
                const int midi = std::clamp(payload.value("midi", 60), 0, 127);
                const double rate = RenderRate(audio);
                audio.play(music::RenderPluckedNotes({music::SingleNote(midi)}, rate), rate, false);
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
