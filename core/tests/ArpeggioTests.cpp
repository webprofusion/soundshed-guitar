/**
 * ArpeggioTests.cpp - The Scale Guide's arpeggio search (core/src/music/Arpeggios.h): the shapes a
 * guitarist already knows, the rules every run keeps on every tuning, and the view model.
 *
 * `ArpeggioTests --dump <tuning> <key 0-11> <chord> <style> <strings>` prints the runs found, for
 * tuning the search's costs.
 */

#include "music/ScaleGuideView.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace music = guitarfx::music;

namespace
{
using Style = music::ArpeggioStyle;

constexpr int kC = 0;
constexpr int kG = 7;
constexpr int kA = 9;
constexpr int kSweepSpan = 5;

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

const std::string kFlat = music::kFlatSign;

music::Neck NeckFor(const char* tuningId)
{
    const auto* tuning = music::FindTuning(tuningId);
    music::Neck neck;
    neck.strings = tuning->strings;
    neck.instrument = tuning->instrument;
    return neck;
}

/// A run as frets a string, "12 15/14/14/13/12 17", as a player would write it out.
std::string RunText(const music::ArpeggioShape& shape)
{
    std::string text;

    for (std::size_t i = 0; i < shape.notes.size(); ++i)
    {
        const bool sameString = i > 0 && shape.notes[i].string == shape.notes[i - 1].string;
        text += (i == 0 ? "" : sameString ? " " : "/") + std::to_string(shape.notes[i].fret);
    }

    return text;
}

std::vector<music::ArpeggioShape> RunsFor(const char* tuningId, int root, const char* quality, Style style, int strings)
{
    music::ArpeggioSearch search;
    search.rootPitchClass = root;
    search.intervals = music::FindChord(quality)->intervals;
    search.style = style;
    search.strings = strings;
    return music::ArpeggioShapes(NeckFor(tuningId), search);
}

const music::ArpeggioShape* FindRun(const std::vector<music::ArpeggioShape>& runs, const std::string& text)
{
    for (const auto& run : runs)
    {
        if (RunText(run) == text)
        {
            return &run;
        }
    }

    return nullptr;
}

void TestGoldenRuns()
{
    const auto am5 = RunsFor("std", kA, "m", Style::Sweep, 5);
    const auto* root = FindRun(am5, "12 15/14/14/13/12 17");
    Expect(root != nullptr, "Am's five-string sweep, root on the A string");
    Expect(FindRun(am5, "3 7/7/5/5/5 8") != nullptr, "Am from its 3rd");
    Expect(FindRun(am5, "7 12/10/9/10/8 12") != nullptr, "Am from its 5th");

    if (root != nullptr)
    {
        std::string fingers;

        for (const auto& note : root->notes)
        {
            fingers += std::to_string(note.finger);
        }

        Expect(fingers == "1433214", "Am's sweep fingering: " + fingers);
        Expect(root->startTone == 0 && root->fretFrom == 12 && root->fretTo == 17, "root position, frets 12 to 17");
        Expect(root->fromString == 1 && root->toString == 5, "A string to high E");

        std::string strokes;

        for (const auto& step : root->steps)
        {
            strokes += music::PickStrokeId(step.stroke)[0];
        }

        // Down the strings with a hammer on each pair, back up them with a pull-off on each pair.
        Expect(strokes == "dhddddhpuuuup", "Am's pick strokes: " + strokes);
        Expect(root->steps.front().note == 0 && root->steps.back().note == 0, "it ends where it started");
    }

    Expect(FindRun(RunsFor("std", kC, "maj", Style::Sweep, 5), "3 7/5/5/5/3 8") != nullptr, "C's five-string sweep");
    Expect(FindRun(RunsFor("std", kG, "maj", Style::Sweep, 6), "3 7/5/5/4/3/3") != nullptr, "G's six-string sweep");
    Expect(FindRun(RunsFor("std", kA, "m", Style::Sweep, 3), "14/13/12 17") != nullptr, "Am's three-string sweep");
    Expect(FindRun(RunsFor("std", kA, "m", Style::Sweep, 4), "7/5/5/5") != nullptr, "Am across four strings");
    Expect(FindRun(RunsFor("std", kC, "maj9", Style::Sweep, 5), "15/14/12/12/10") != nullptr,
           "Cmaj9 climbs its stack of thirds, a note a string");
    Expect(FindRun(RunsFor("std", kA, "m", Style::Box, 0), "5 8/7/7/5/5/5 8") != nullptr, "Am's box at the 5th fret");
    Expect(FindRun(RunsFor("bass4", kC, "m", Style::Sweep, 4), "8/6/5/5") != nullptr,
           "Cm across a bass's four strings");

    const auto twos = RunsFor("std", kA, "m7", Style::TwoPerString, 6);
    const auto* two = FindRun(twos, "5 8/7 10/7 10/9 12/10 13/12 15");
    Expect(two != nullptr, "Am7 two a string climbs the neck");

    if (two != nullptr)
    {
        for (std::size_t i = 0; i + 1 < two->notes.size(); i += 2)
        {
            Expect(two->notes[i].finger == 1 && two->notes[i + 1].finger == 4, "two a string plays 1 then 4");
        }
    }

    Expect(RunsFor("uke", kC, "maj", Style::Sweep, 4).empty(), "no sweep on a re-entrant tuning");
    Expect(!RunsFor("uke", kC, "maj", Style::Box, 0).empty(), "a box works on one");
    Expect(music::ParseArpeggioStyle("two") == Style::TwoPerString, "style ids round trip");
    Expect(!music::ParseArpeggioStyle("nonsense").has_value(), "an unknown style is refused");
}

void CheckRun(const music::ArpeggioShape& run, const music::Neck& neck, const music::ChordDef& chord, int root,
              Style style, const std::string& label)
{
    const auto text = label + " [" + RunText(run) + "]";
    const int count = static_cast<int>(neck.strings.size());
    int spanFrom = neck.frets;
    int spanTo = 0;
    std::set<int> heard;

    Expect(run.steps.size() == run.notes.size() * 2 - 1, "steps go up and back, the top note once: " + text);

    for (std::size_t n = 0; n < run.notes.size(); ++n)
    {
        const auto& note = run.notes[n];
        const int midi = neck.strings[note.string] + note.fret;
        heard.insert(music::PitchClass(midi - root));

        Expect(note.fret >= 0 && note.fret <= neck.frets, "on the neck: " + text);
        Expect(music::PitchClass(midi - root) == music::PitchClass(chord.intervals[note.tone].semitones),
               "every note is the tone it says: " + text);
        Expect((note.fret == 0) == (note.finger == 0), "a finger for every fretted note: " + text);
        Expect(note.finger >= 0 && note.finger <= 4, "four fingers: " + text);

        if (n > 0)
        {
            const auto& before = run.notes[n - 1];
            Expect(midi > neck.strings[before.string] + before.fret, "it climbs: " + text);

            if (style != Style::Box)
            {
                Expect(note.string == before.string || note.string == before.string + 1, "no string skipped: " + text);
            }

            if (note.string == before.string && before.fret > 0)
            {
                Expect(note.finger >= before.finger, "fingers never cross on a string: " + text);
            }
        }

        if (note.fret > 0)
        {
            spanFrom = std::min(spanFrom, note.fret);
            spanTo = std::max(spanTo, note.fret);
        }
    }

    const int top = std::min(music::kDefaultArpeggioStrings, count);

    if (style == Style::Sweep)
    {
        // An extended chord may sit in a rootless stack of thirds; a chord with more tones than the
        // strings can take leaves one out.
        const bool extended = chord.intervals.back().semitones >= music::kPitchClasses;
        const bool fits = static_cast<int>(chord.intervals.size()) <= top;
        Expect(spanTo - spanFrom <= kSweepSpan, "a sweep stays under the hand: " + text);
        Expect(run.fromString == count - top && run.toString == count - 1, "a sweep plays the top strings: " + text);
        Expect(extended || !fits || heard.size() == chord.intervals.size(), "a sweep plays every tone: " + text);
    }

    if (style == Style::TwoPerString)
    {
        Expect(run.notes.size() == static_cast<std::size_t>(top) * 2, "two notes on each string: " + text);
    }

    if (style == Style::Box)
    {
        Expect(spanTo - spanFrom <= music::kHandFrets, "a box is one hand position: " + text);
    }
}

void TestRulesOnEveryTuning()
{
    for (const auto& tuning : music::Tunings())
    {
        const auto neck = NeckFor(tuning.id.c_str());
        const bool fiveStrings = neck.Ascending() && neck.strings.size() >= 5;

        for (const auto style : {Style::Sweep, Style::TwoPerString, Style::Box})
        {
            for (const auto& chord : music::Chords())
            {
                // A root every few frets: the rest are these shapes moved along the neck.
                for (int root = 0; root < music::kPitchClasses; root += 5)
                {
                    const auto label = tuning.id + " " + chord.id + " " + std::to_string(root) + " " +
                                       std::string(music::ArpeggioStyleId(style));
                    const auto runs = RunsFor(tuning.id.c_str(), root, chord.id.c_str(), style, 0);

                    for (std::size_t r = 0; r < runs.size(); ++r)
                    {
                        Expect(r == 0 || runs[r - 1].fretFrom <= runs[r].fretFrom, "sorted up the neck: " + label);
                        CheckRun(runs[r], neck, chord, root, style, label);
                    }

                    if (style == Style::Sweep && fiveStrings && chord.intervals.size() >= 3)
                    {
                        Expect(!runs.empty(), "a sweep of every chord on every root: " + label);
                    }

                    if (style == Style::Box && chord.intervals.size() >= 3)
                    {
                        Expect(!runs.empty(), "a box of every chord on every tuning: " + label);
                    }
                }
            }
        }
    }

    const auto neck = NeckFor("std");
    Expect(music::ArpeggioStringChoices(neck) == std::vector<int>({3, 4, 5, 6}), "three strings up to all of them");
    Expect(music::ArpeggioStringsFor(neck, 0) == 5, "five strings unless asked");
    Expect(music::ArpeggioStringsFor(neck, 9) == 5, "a string count the neck lacks takes the default");
    Expect(music::ArpeggioStringsFor(NeckFor("bass4"), 0) == 4, "a bass has four to give");
}

void TestView()
{
    nlohmann::json request = {{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"tuning", "std"}, {"position", 1}};
    auto view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["mode"] == "arpeggio" && view["title"] == "Am arpeggio", "the arpeggio's title");
    Expect(view["chord"]["formula"] == "R " + kFlat + "3 5", "its formula");
    Expect(view["arpeggio"]["style"] == "sweep" && view["arpeggio"]["strings"] == 5, "sweep on five strings at first");
    Expect(view["arpeggio"]["stringChoices"].size() == 4, "the string counts on offer");
    Expect(!view.contains("shapes") && !view.contains("positions"), "neither chord shapes nor scale positions");

    const auto runs = view["arpeggio"]["runs"];
    Expect(runs.size() >= 3 && view["position"] == 1, "several runs, the first chosen");
    Expect(view["arpeggio"]["advice"].is_string() && view["arpeggio"]["empty"].is_string(), "the page's words");

    int numbered = 0;

    for (const auto& dot : view["dots"])
    {
        Expect(dot.contains("order") == dot["inPosition"].get<bool>(), "only the run's dots are numbered");
        numbered += dot.contains("order") ? 1 : 0;
    }

    Expect(numbered == static_cast<int>(runs[0]["notes"].size()), "a dot for each note of the run");
    Expect(runs[0]["notes"][0].contains("finger") && runs[0]["steps"][0]["stroke"] == "down", "fingers and strokes");
    Expect(runs[0]["name"] == "From the " + kFlat + "3" && runs[0]["start"] == kFlat + "3", "named by where it starts");

    request = {{"mode", "arpeggio"}, {"key", 9},  {"chord", "m"},           {"tuning", "std"},
               {"position", 2},      {"capo", 2}, {"arpeggioStyle", "box"}, {"arpeggioStrings", 3}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["style"] == "box" && view["arpeggio"]["strings"] == 3, "the chosen style and strings");
    bool behindCapo = false;

    for (const auto& run : view["arpeggio"]["runs"])
    {
        for (const auto& note : run["notes"])
        {
            behindCapo = behindCapo || note["fret"].get<int>() < 2;
        }
    }

    Expect(!behindCapo, "no run behind a capo");

    request = {{"mode", "arpeggio"},     {"key", 9},     {"chord", "m"}, {"tuning", "std"},
               {"arpeggioStyle", "two"}, {"position", 1}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["strings"] == 4 && view["arpeggio"]["stringChoices"].size() == 2,
           "two notes a string stops a triad at the strings it can reach");
    Expect(!view["arpeggio"]["runs"].empty() && view["position"] == 1, "so the default count still has runs");

    request["arpeggioStyle"] = "box";
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["strings"] == 5 && view["arpeggio"]["stringChoices"].size() == 4,
           "a box ignores the count");

    request = {{"mode", "arpeggio"}, {"key", 0}, {"chord", "maj"}, {"tuning", "uke"}, {"position", 3}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["runs"].empty() && view["position"] == 0, "no sweep on a ukulele: the whole neck instead");
    Expect(!view["dots"].empty(), "its chord tones still show");
    Expect(view["arpeggio"]["stringChoices"].size() == 2, "and the string counts stay on offer");

    request = {{"mode", "arpeggio"}, {"arpeggioStyle", "nonsense"}, {"arpeggioStrings", 99}, {"chord", "nonsense"}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["arpeggio"]["style"] == "sweep" && view["arpeggio"]["strings"] == 5, "bad choices take the defaults");

    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest({{"mode", "nonsense"}}));
    Expect(view["mode"] == "scale", "an unknown mode is the scale view");
}

int Dump(const char* tuning, int key, const char* quality, const char* style, int strings)
{
    if (music::FindTuning(tuning) == nullptr || music::FindChord(quality) == nullptr)
    {
        std::cerr << "unknown tuning or chord" << std::endl;
        return 2;
    }

    const auto chosen = music::ParseArpeggioStyle(style).value_or(Style::Sweep);

    for (const auto& run : RunsFor(tuning, key, quality, chosen, strings))
    {
        std::cout << "tone " << run.startTone << " frets " << run.fretFrom << "-" << run.fretTo << ": " << RunText(run)
                  << "  fingers ";

        for (const auto& note : run.notes)
        {
            std::cout << note.finger;
        }

        std::cout << std::endl;
    }

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 7 && std::string(argv[1]) == "--dump")
    {
        return Dump(argv[2], std::atoi(argv[3]), argv[4], argv[5], std::atoi(argv[6]));
    }

    TestGoldenRuns();
    TestRulesOnEveryTuning();
    TestView();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ArpeggioTests passed" << std::endl;
    return 0;
}
