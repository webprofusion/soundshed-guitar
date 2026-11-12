/**
 * @file ScalePlaybackMessageTests.cpp
 * @brief The Scale Guide's Play as the UI asks for it: the scaleGuide messages (play, playNote,
 * playScale, playChord, stop) through RegisterScaleGuideMessages, rendered at the host's rate,
 * waiting for the metronome's bar where Play should, and reported back as scaleGuidePlayback.
 * What each view sounds and the voice itself are ScalePlaybackTests.cpp.
 */

#include "MessageHandlerRegistry.h"
#include "controller/ScaleGuideMessages.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx;
using nlohmann::json;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << std::endl;
        ++gFailures;
    }
}

double Peak(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    double peak = 0.0;

    for (std::size_t i = from; i < std::min(to, samples.size()); ++i)
    {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }

    return peak;
}

void TestMessages()
{
    guitarfx::MessageHandlerRegistry registry;
    std::vector<json> sent;
    int announced = 0;
    int unannounced = 0;
    int stops = 0;
    double renderedAt = 0.0;
    std::size_t lastLength = 0;

    guitarfx::ScaleGuideAudio audio;
    audio.sampleRate = [] { return 44100.0; };
    audio.tempoBpm = [] { return 120.0; };
    int onBars = 0;
    float lastPeak = 0.0f;
    audio.barSeconds = [] { return 2.0; };
    audio.play = [&](std::vector<float> samples, double rate, bool announce, bool onBar) {
        ++(announce ? announced : unannounced);
        onBars += onBar ? 1 : 0;
        renderedAt = rate;
        lastLength = samples.size();
        lastPeak = Peak(samples, 0, samples.size());
        return onBar ? 0.5 : 0.0;
    };
    audio.stop = [&stops] { ++stops; };
    guitarfx::RegisterScaleGuideMessages(
        registry, [&sent](const std::string& text) { sent.push_back(json::parse(text)); }, audio);

    json play = {{"type", "scaleGuide"},
                 {"action", "play"},
                 {"request", {{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"tuning", "std"}, {"position", 1}}}};
    registry.Dispatch("scaleGuide", play);
    Expect(announced == 1 && lastLength > 44100 && renderedAt == 44100.0, "Play renders at the host's rate");
    Expect(sent.size() == 1 && sent[0]["type"] == "scaleGuidePlayback" && sent[0]["notes"].size() > 5,
           "and says when each note falls");
    Expect(onBars == 1 && sent[0]["delayMs"] == 500.0, "starting on the metronome's bar, and saying how long that is");

    play["request"]["position"] = 0;
    registry.Dispatch("scaleGuide", play);
    Expect(announced == 1 && sent.size() == 1, "the whole neck has nothing to play");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "playNote"}, {"midi", 64}});
    Expect(unannounced == 1 && sent.size() == 1, "a clicked note plays without a report");
    const float full = lastPeak;
    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "playNote"}, {"midi", 64}, {"volume", 0.5}});
    Expect(std::abs(lastPeak - 0.25f * full) < 1e-4f, "at half volume it is a quarter of the level");
    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "playNote"}, {"midi", 64}, {"volume", 7}});
    Expect(lastPeak == full, "and never louder than full");
    unannounced = 1;

    json example = {
        {"type", "scaleGuide"},
        {"action", "playScale"},
        {"request", {{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"tuning", "std"}, {"position", 0}}}};
    registry.Dispatch("scaleGuide", example);
    Expect(announced == 2 && sent.size() == 2 && sent[1]["notes"].size() == 11,
           "a scale's example plays from the first position when the whole neck shows");

    json strum = {{"type", "scaleGuide"},
                  {"action", "playChord"},
                  {"request", {{"mode", "chord"}, {"key", 0}, {"chord", "maj"}, {"tuning", "std"}}},
                  {"frets", {-1, 3, 2, 0, 1, 0}}};
    registry.Dispatch("scaleGuide", strum);
    Expect(announced == 3 && sent.size() == 3 && sent[2]["notes"].size() == 5, "a chord box strums its shape");
    Expect(onBars == 2 && sent[2]["delayMs"] == 0.0, "at once: only Play waits for the bar");

    json whole = {
        {"type", "scaleGuide"},
        {"action", "play"},
        {"request", {{"mode", "arpeggio"}, {"key", 0}, {"progression", "pop"}, {"tuning", "std"}, {"position", 1}}}};
    registry.Dispatch("scaleGuide", whole);
    const auto all = sent.back()["notes"];
    whole["bar"] = 2;
    registry.Dispatch("scaleGuide", whole);
    const auto one = sent.back()["notes"];
    Expect(one.size() < all.size() && one[0]["chord"] == 2 && one[0]["startMs"] == 0.0 && one.back()["chord"] == 2,
           "a bar asked for plays alone, from its first note");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "stop"}});
    Expect(stops == 1, "stop stops");
}
} // namespace

int main()
{
    TestMessages();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScalePlaybackMessageTests passed" << std::endl;
    return 0;
}
