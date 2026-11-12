/**
 * ScaleGuideUiClientTests.cpp - The Scale Guide's native-UI half (uiclient/ScaleGuide.h), held to
 * the web UI's state.ts, progressions.ts and listen.ts: the same settings, the same request to the
 * engine, the same LISTEN rules and Play's timing. The theory engine is ScaleGuideTests.cpp.
 */

#include "music/ScaleGuideView.h"
#include "uiclient/ScaleGuide.h"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace music = guitarfx::music;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// The Arpeggios tab's settings and request, and a progression's arpeggios.
void TestUiClientArpeggio()
{
    namespace sg = guitarfx::uiclient::scaleguide;
    const auto stored = nlohmann::json{
        {"mode", "arpeggio"}, {"labels", "order"}, {"arpeggioStyle", "two"}, {"arpeggioStrings", 4}, {"chord", "m7"}};
    const auto settings = sg::ParseSettings(stored);
    Expect(settings.mode == "arpeggio" && settings.labels == "order", "the native UIs read the Arpeggios tab");
    Expect(settings.arpeggioStyle == "two" && settings.arpeggioStrings == 4, "its style and strings");

    const auto again = sg::ParseSettings(sg::SettingsJson(settings));
    Expect(again.arpeggioStyle == "two" && again.arpeggioStrings == 4, "saving keeps the web UI's choices");

    const auto request = sg::ViewRequest(settings, {}, 0);
    Expect(request["mode"] == "arpeggio" && request["arpeggioStyle"] == "two" && request["arpeggioStrings"] == 4,
           "the request carries them");
    Expect(!request.contains("progression") && !request.contains("step"), "one chord sends no progression");

    // Arpeggios over a progression: its id, the step, the anchor chord and the slides.
    auto over = settings;
    over.progression = "b:pop";
    over.arpeggioSlides = true;
    const auto whole = sg::ViewRequest(over, {}, 2, {0, 2, 2}, 1);
    Expect(whole["progression"] == "pop" && whole["step"] == 2 && whole["anchorStep"] == 1 &&
               whole["arpeggioSlides"] == true && !whole.contains("previousFrets"),
           "arpeggios take a progression, with the chord the run was chosen for");
    const auto walked = music::BuildScaleGuideView(music::ParseScaleGuideRequest(whole));
    Expect(walked["progressionRun"]["bars"].size() == 4 && walked["progression"]["anchorStep"] == 1,
           "and the engine answers with the whole of it");
    over.mode = "scale";
    Expect(!sg::ViewRequest(over, {}, 2).contains("progression"), "a scale never sends one");

    const auto view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["strings"] == 4 && !view["arpeggio"]["runs"].empty(), "and the engine answers it");

    const auto bad = sg::ParseSettings({{"arpeggioStyle", "nonsense"}, {"arpeggioStrings", 99}});
    Expect(bad.arpeggioStyle == "sweep" && bad.arpeggioStrings == 8, "bad values are made safe");
}

void TestUiClient()
{
    namespace sg = guitarfx::uiclient::scaleguide;

    const auto defaults = sg::ParseSettings(nullptr);
    Expect(defaults.mode == "scale" && defaults.key == 4 && defaults.scale == "minpent" && !defaults.leftHanded,
           "settings default as the web UI's");
    const auto parsed = sg::ParseSettings({{"key", 15},
                                           {"mode", "piano"},
                                           {"scale", "<x>"},
                                           {"capo", -3},
                                           {"labels", "interval"},
                                           {"progression", "b:pop"}});
    Expect(parsed.key == 11 && parsed.mode == "scale" && parsed.scale == "minpent" && parsed.capo == 0 &&
               parsed.labels == "interval" && parsed.progression == "b:pop",
           "bad settings fall back and numbers clamp");
    Expect(sg::ParseSettings(sg::SettingsJson(parsed)).progression == "b:pop", "settings round-trip");

    nlohmann::json app = {{sg::kProgressionsSetting,
                           {{{"id", "p1"}, {"name", " "}, {"chords", {"I", "bVII", "<script>"}}},
                            {{"id", "p1"}, {"chords", {"I"}}},
                            {{"id", "bad id"}, {"chords", {"I"}}},
                            {{"id", "p2"}, {"chords", nlohmann::json::array()}}}}};
    const auto mine = sg::LoadProgressions(app);
    Expect(mine.size() == 1 && mine[0].chords == std::vector<std::string>({"I", "bVII"}),
           "only well-formed saved progressions load");

    auto settings = defaults;
    settings.mode = "chord";
    settings.progression = "u:p1";
    auto request = sg::ViewRequest(settings, mine, 2, {0, 2, 2, 1, 0, 0});
    Expect(request["progressionChords"] == nlohmann::json({"I", "bVII"}) && request["step"] == 2 &&
               request["previousFrets"].size() == 6,
           "a player's progression goes as numerals, with the step and the shape to lead from");
    settings.progression = "b:blues12";
    request = sg::ViewRequest(settings, mine, 0);
    Expect(request["progression"] == "blues12" && !request.contains("previousFrets"), "a built-in goes by id");
    const auto view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["title"] == "E7", "the request is one the engine answers");

    Expect(sg::ReadingMidi("A", 4) == 69 && sg::ReadingMidi("C#/Db", 3) == 49 && !sg::ReadingMidi("", 3),
           "the tuner's note as MIDI");

    const std::vector<sg::ChordTone> cMajor = {{0, "R"}, {4, "3"}, {7, "5"}};
    std::array<float, 12> chroma{};
    chroma.fill(0.005f);
    chroma[0] = 0.4f;
    chroma[4] = 0.3f;
    chroma[7] = 0.25f;
    Expect(sg::MatchChord(chroma, 0, cMajor, "normal").ok, "a clean C major matches");
    chroma[4] = 0.005f;
    const auto noThird = sg::MatchChord(chroma, 0, cMajor, "normal");
    Expect(!noThird.ok && noThird.missing == std::vector<int>({4}), "a missing third is named");
    chroma[3] = 0.3f;
    Expect(!sg::MatchChord(chroma, 0, cMajor, "easy").ok, "C minor is not C major");

    sg::StrumGate gate;
    int hits = 0;

    for (int i = 0; i < 12; ++i)
    {
        hits += gate.Push(i == 0 ? -20.0 : -22.0 - i * 0.2, true) ? 1 : 0;
    }

    Expect(hits == 1, "one strum counts once");

    sg::StrumGate ringing;
    ringing.Push(-70.0, false);
    ringing.Push(-70.0, false);
    hits = 0;

    for (int i = 0; i < 20; ++i)
    {
        hits += ringing.Push(-20.0 - i * 0.3, true) ? 1 : 0;
    }

    Expect(hits == 1, "a ringing strum does not re-arm on the quiet before it");

    sg::NoteFollower follower;
    Expect(!follower.Push(57) && follower.Push(57) && follower.Current() == 57, "a note shows once read twice");
    follower.Push(60);
    follower.Push(60);

    for (int i = 0; i < sg::NoteFollower::kReleaseReadings; ++i)
    {
        follower.Push(std::nullopt);
    }

    Expect(!follower.Current() && follower.Trail() == std::vector<int>({60, 57}), "silence leaves a trail");

    // Play's notes as the engine times them: what sounds, and the chord to show, at a moment.
    const auto playback = sg::ParsePlayback(nlohmann::json::parse(R"({"type":"scaleGuidePlayback","delayMs":500,
        "notes":[{"string":0,"fret":3,"startMs":0,"lengthMs":200,"step":0,"chord":0},
                 {"string":1,"fret":2,"startMs":200,"lengthMs":200,"step":1,"chord":0},
                 {"string":1,"fret":5,"startMs":400,"lengthMs":200,"step":2,"chord":1}]})"));
    Expect(playback.notes.size() == 3 && playback.delayMs == 500.0 && playback.notes[2].chord == 1,
           "the playback message is read");
    const auto waiting = sg::PlaybackAt(playback, 100.0);
    Expect(waiting.places.empty() && waiting.chord == -1, "nothing shows while Play waits for the bar");
    Expect(sg::PlaybackAt(playback, 430.0).chord == 0, "the first chord shows just ahead of its note");
    const auto second = sg::PlaybackAt(playback, 750.0);
    Expect(second.places == std::vector<std::pair<int, int>>({{1, 2}}) && second.steps == std::vector<int>({1}) &&
               second.chord == 0,
           "the second note sounds alone");
    Expect(sg::PlaybackAt(playback, 830.0).chord == 1, "the next chord shows ahead of its first note");
    const auto ended = sg::PlaybackAt(playback, 1200.0);
    Expect(ended.places.empty() && ended.chord == 1, "nothing is lit after the last note");
    Expect(sg::ParsePlayback(nlohmann::json::array()).notes.empty(), "a malformed message reads as nothing");
}
} // namespace

int main()
{
    TestUiClient();
    TestUiClientArpeggio();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScaleGuideUiClientTests passed" << std::endl;
    return 0;
}
