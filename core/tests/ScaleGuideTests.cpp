/**
 * ScaleGuideTests.cpp - The Scale Guide's theory engine (core/src/music): spelling, the
 * catalogue, scale positions, chord shapes and the view model, on every tuning it offers.
 * The native UIs' half (uiclient/ScaleGuide.h) is ScaleGuideUiClientTests.cpp.
 */

#include "MessageHandlerRegistry.h"
#include "controller/ScaleGuideMessages.h"
#include "music/ScaleGuideView.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
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

const std::string kFlat = music::kFlatSign;
const std::string kSharp = music::kSharpSign;

std::string Names(const music::SpelledSet& set)
{
    std::string names;

    for (const auto& note : set.notes)
    {
        names += (names.empty() ? "" : " ") + note.name;
    }

    return names;
}

std::string FretsText(const std::vector<int>& frets)
{
    std::string text;

    for (const int fret : frets)
    {
        text += (text.empty() ? "" : ",") + (fret < 0 ? std::string("x") : std::to_string(fret));
    }

    return text;
}

music::Neck NeckFor(const char* tuningId)
{
    const auto* tuning = music::FindTuning(tuningId);
    music::Neck neck;
    neck.strings = tuning->strings;
    neck.instrument = tuning->instrument;
    return neck;
}

std::vector<music::ChordShape> ShapesFor(const char* tuningId, int root, const char* quality)
{
    music::ChordSearch search;
    search.rootPitchClass = root;
    search.intervals = music::FindChord(quality)->intervals;
    return music::ChordShapes(NeckFor(tuningId), search);
}

const music::ChordShape* FindShape(const std::vector<music::ChordShape>& shapes, const std::vector<int>& frets)
{
    for (const auto& shape : shapes)
    {
        if (shape.frets == frets)
        {
            return &shape;
        }
    }

    return nullptr;
}

std::string ShapesText(const std::vector<music::ChordShape>& shapes)
{
    std::string text;

    for (const auto& shape : shapes)
    {
        text += " [" + FretsText(shape.frets) + "]";
    }

    return text;
}

void TestIntervals()
{
    const auto flatThird = music::ParseInterval("b3");
    Expect(flatThird && flatThird->semitones == 3 && flatThird->label == kFlat + "3", "b3 is three semitones");
    const auto ninth = music::ParseInterval("9");
    Expect(ninth && ninth->semitones == 14 && ninth->degree == 9, "a 9th is fourteen semitones");
    const auto dim7 = music::ParseInterval("bb7");
    Expect(dim7 && dim7->semitones == 9, "a double-flat 7th is nine semitones");
    Expect(music::ParseInterval("1")->label == "R", "the root reads R");
    Expect(music::ParseInterval(kSharp + "4")->semitones == 6, "the UTF-8 sharp parses");
    Expect(!music::ParseInterval("x3") && !music::ParseInterval("14") && !music::ParseInterval(""),
           "junk is not an interval");
    Expect(music::ParseFormula("1 b3 x").empty(), "one bad token empties a formula");
    Expect(music::StepPattern(music::ParseFormula("1 2 3 4 5 6 7")) == "2 2 1 2 2 2 1", "the major steps");
    Expect(music::StepPattern(music::ParseFormula("1 b3 4 5 b7")) == "3 2 2 3 2", "the pentatonic steps");
}

void TestSpelling()
{
    const auto& dorian = music::FindScale("dorian")->intervals;
    Expect(Names(music::SpellBest(3, dorian)) ==
               "E" + kFlat + " F G" + kFlat + " A" + kFlat + " B" + kFlat + " C D" + kFlat,
           "E-flat Dorian is spelled with flats, one letter a degree");
    Expect(Names(music::SpellBest(6, music::FindScale("major")->intervals)) ==
               "F" + kSharp + " G" + kSharp + " A" + kSharp + " B C" + kSharp + " D" + kSharp + " E" + kSharp,
           "F-sharp major keeps E-sharp (a tie goes to sharps)");
    Expect(music::SpellBest(1, music::FindScale("major")->intervals).notes.front().name == "D" + kFlat,
           "D-flat major reads better than C-sharp");
    Expect(Names(music::SpellBest(9, music::FindScale("harmin")->intervals)) == "A B C D E F G" + kSharp,
           "A harmonic minor has G-sharp");
    Expect(Names(music::SpellChord(0, *music::FindChord("dim7")).notes) == "C E" + kFlat + " G" + kFlat + " A",
           "a double flat is written as its plain note");
    Expect(music::SpellChord(11, *music::FindChord("m7b5")).name == "Bm7" + kFlat + "5",
           "a half-diminished chord's name");
    Expect(music::SpellChord(10, *music::FindChord("m7")).name == "B" + kFlat + "m7",
           "B-flat minor 7 reads better than A-sharp");
}

void TestRoman()
{
    const auto flatSeven = music::ParseRoman("bVII");
    Expect(flatSeven && flatSeven->offset == 10 && flatSeven->degree == 7 && flatSeven->quality == "maj",
           "bVII is a major chord a tone below the key");
    Expect(music::ParseRoman("ii7")->quality == "m7", "ii7 is minor 7");
    Expect(music::ParseRoman("V7")->quality == "7", "V7 is dominant");
    Expect(music::ParseRoman("ii\xC3\xB8"
                             "7")
                   ->quality == "m7b5",
           "ii-half-diminished");
    Expect(music::ParseRoman("vii\xC2\xB0")->quality == "dim", "vii-diminished");
    Expect(music::ParseRoman("I5")->quality == "5", "I5 is a power chord");
    Expect(music::ParseRoman("IVmaj7")->quality == "maj7", "IVmaj7");
    Expect(!music::ParseRoman("Vi") && !music::ParseRoman("VIII") && !music::ParseRoman("I13"), "junk numerals fail");

    for (const char* token : {"I", "ii7", "bVII", "V7", "iv", "bVI7", "Imaj7", "I5", "bII5"})
    {
        const auto chord = music::ParseRoman(token);
        const std::string back = chord ? music::RomanNumeral(*chord) : "";
        Expect(music::ParseRoman(back) && music::ParseRoman(back)->offset == chord->offset &&
                   music::ParseRoman(back)->quality == chord->quality,
               std::string("a numeral reads back the same: ") + token + " -> " + back);
    }

    const auto rock = music::ChordsInKey(3, music::FindProgression("classicrock")->chords);
    Expect(rock[0].name == "E" + kFlat && rock[1].name == "D" + kFlat && rock[2].name == "A" + kFlat,
           "bVII in E-flat is D-flat");
    const auto blues = music::ChordsInKey(4, music::FindProgression("blues12")->chords);
    Expect(blues[0].name == "E7" && blues[4].name == "A7" && blues[8].name == "B7", "a 12-bar in E");
    const auto pop = music::ChordsInKey(1, music::FindProgression("pop")->chords);
    Expect(pop[0].name == "D" + kFlat && pop[2].name == "B" + kFlat + "m", "C-sharp/D-flat pop reads as D-flat");
    const auto fSharp = music::ChordsInKey(6, music::FindProgression("pop")->chords);
    Expect(fSharp[0].name == "F" + kSharp, "a tie on F-sharp stays sharp");

    const auto triads = music::DiatonicTriads(music::SpellBest(0, music::FindScale("major")->intervals));
    std::string names;
    std::string numerals;

    for (const auto& chord : triads)
    {
        names += chord.name + " ";
        numerals += chord.numeral + " ";
    }

    Expect(names == "C Dm Em F G Am Bdim ", "C major's chords: " + names);
    Expect(numerals == "I ii iii IV V vi vii\xC2\xB0 ", "C major's numerals: " + numerals);

    numerals.clear();

    for (const auto& chord : music::DiatonicTriads(music::SpellBest(9, music::FindScale("minor")->intervals)))
    {
        numerals += chord.numeral + " ";
    }

    Expect(numerals == "i ii\xC2\xB0 " + kFlat + "III iv v " + kFlat + "VI " + kFlat + "VII ",
           "A minor's numerals: " + numerals);
}

void TestCatalogue()
{
    std::set<std::string> ids;

    for (const auto& scale : music::Scales())
    {
        Expect(!scale.intervals.empty(), "scale formula parses: " + scale.id);
        Expect(ids.insert("s:" + scale.id).second, "scale ids are unique: " + scale.id);
        Expect(scale.boxParent.empty() || music::FindScale(scale.boxParent) != nullptr, "box parent exists");

        for (std::size_t i = 1; i < scale.intervals.size() && !scale.arpeggio; ++i)
        {
            Expect(scale.intervals[i].semitones > scale.intervals[i - 1].semitones, "scale ascends: " + scale.id);
        }
    }

    Expect(music::FindScale("dblharm")->intervals.size() == 7, "double harmonic has seven notes");

    for (const auto& chord : music::Chords())
    {
        Expect(chord.intervals.size() >= 2, "chord formula parses: " + chord.id);
        Expect(ids.insert("c:" + chord.id).second, "chord ids are unique: " + chord.id);
    }

    Expect(music::FindChord("9")->intervals.back().semitones == 14, "a 9 chord's 9th is a major 9th");

    for (const auto& progression : music::Progressions())
    {
        Expect(!progression.chords.empty(), "progression parses: " + progression.id);

        for (const auto& chord : progression.chords)
        {
            Expect(music::FindChord(chord.quality) != nullptr, "progression chord exists: " + progression.id);
        }
    }

    for (const auto& tuning : music::Tunings())
    {
        const auto count = tuning.strings.size();
        Expect(count >= 4 && count <= 8, "4 to 8 strings: " + tuning.id);
        Expect(ids.insert("t:" + tuning.id).second, "tuning ids are unique: " + tuning.id);
    }

    Expect(music::FindTuning("uke")->strings[0] > music::FindTuning("uke")->strings[1], "the ukulele is re-entrant");
}

int CountOnString(const music::ScalePosition& position, int string)
{
    int count = 0;

    for (const auto& cell : position.cells)
    {
        count += cell.string == string ? 1 : 0;
    }

    return count;
}

void TestScalePositions()
{
    const auto std6 = NeckFor("std");
    const auto em = music::ScalePositions(std6, *music::FindScale("minpent"), 4);
    const std::vector<music::FretPos> open = {{0, 0}, {0, 3}, {1, 0}, {1, 2}, {2, 0}, {2, 2},
                                              {3, 0}, {3, 2}, {4, 0}, {4, 3}, {5, 0}, {5, 3}};
    Expect(em.size() == 5 && em[0].cells == open, "E minor pentatonic box 1 is the open box");
    const auto am = music::ScalePositions(std6, *music::FindScale("minpent"), 9);
    Expect(am[0].fretFrom == 5 && am[0].fretTo == 8 && am[0].cells.size() == 12, "A minor pentatonic box 1 at 5");

    for (int s = 0; s < 6; ++s)
    {
        Expect(CountOnString(am[0], s) == 2, "two notes a string in a pentatonic box");
    }

    for (const auto& box : music::ScalePositions(std6, *music::FindScale("major"), 7))
    {
        for (int s = 0; s < 6; ++s)
        {
            Expect(CountOnString(box, s) == 3, "three notes a string in a major box");
        }
    }

    const auto threeNps = music::ScalePositions(std6, *music::FindScale("minpent"), 9, 3);
    Expect(CountOnString(threeNps[0], 0) == 3, "a three-a-string pentatonic");

    const auto blues = music::ScalePositions(std6, *music::FindScale("blues"), 9);
    Expect(blues.size() == 5 && blues[0].cells.size() > 12, "the blues box adds the flat five to the pentatonic");

    const auto uke = music::ScalePositions(NeckFor("uke"), *music::FindScale("major"), 0);
    Expect(!uke.empty() && uke[0].kind == "window", "a re-entrant neck gets hand-position windows");

    // Every position of every scale on every tuning in every key: only scale notes, on the neck.
    for (const auto& tuning : music::Tunings())
    {
        const auto neck = NeckFor(tuning.id.c_str());

        for (const auto& scale : music::Scales())
        {
            for (int key = 0; key < 12; ++key)
            {
                const auto positions = music::ScalePositions(neck, scale, key);
                const auto spelled = music::SpellBest(key, scale.intervals);
                std::set<int> pcs;

                for (const auto& note : spelled.notes)
                {
                    pcs.insert(note.pitchClass);
                }

                Expect(!positions.empty(), "positions exist: " + tuning.id + " " + scale.id);

                for (const auto& position : positions)
                {
                    Expect(!position.cells.empty(), "a position has notes: " + tuning.id + " " + scale.id);

                    for (const auto& cell : position.cells)
                    {
                        const bool inScale = pcs.count(music::PitchClass(neck.strings[cell.string] + cell.fret)) > 0;
                        Expect(inScale && cell.fret >= 0 && cell.fret <= neck.frets,
                               "a position holds scale notes only: " + tuning.id + " " + scale.id);
                    }
                }
            }
        }
    }
}

void ExpectShape(const char* tuning, int root, const char* quality, const std::vector<int>& frets,
                 const std::vector<int>& fingers, const std::string& caged = {})
{
    const auto shapes = ShapesFor(tuning, root, quality);
    const auto* shape = FindShape(shapes, frets);
    const std::string what = std::string(tuning) + " " + std::to_string(root) + quality + " [" + FretsText(frets) + "]";
    Expect(shape != nullptr, "shape found: " + what + " in" + ShapesText(shapes));

    if (shape != nullptr)
    {
        Expect(shape->fingers == fingers, "fingers: " + what + " got " + FretsText(shape->fingers));
        Expect(caged.empty() || shape->caged == caged, "CAGED name: " + what + " got " + shape->caged);
    }
}

void TestGoldenShapes()
{
    ExpectShape("std", 4, "maj", {0, 2, 2, 1, 0, 0}, {0, 2, 3, 1, 0, 0}, "E");
    ExpectShape("std", 0, "maj", {-1, 3, 2, 0, 1, 0}, {0, 3, 2, 0, 1, 0}, "C");
    ExpectShape("std", 7, "maj", {3, 2, 0, 0, 0, 3}, {2, 1, 0, 0, 0, 3}, "G");
    ExpectShape("std", 9, "maj", {-1, 0, 2, 2, 2, 0}, {0, 0, 1, 2, 3, 0}, "A");
    ExpectShape("std", 2, "maj", {-1, -1, 0, 2, 3, 2}, {0, 0, 0, 1, 3, 2}, "D");
    ExpectShape("std", 4, "m", {0, 2, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0});
    ExpectShape("std", 11, "7", {-1, 2, 1, 2, 0, 2}, {0, 2, 1, 3, 0, 4});

    const auto f = ShapesFor("std", 5, "maj");
    const auto* barre = FindShape(f, {1, 3, 3, 2, 1, 1});
    Expect(barre != nullptr && barre->fingers == std::vector<int>({1, 3, 4, 2, 1, 1}) && barre->barres.size() == 1 &&
               barre->barres[0].fret == 1 && barre->barres[0].fromString == 0 && barre->barres[0].toString == 5,
           "F is the E-shape barre at 1:" + ShapesText(f));

    const auto bb = ShapesFor("std", 10, "maj");
    const auto* aShape = FindShape(bb, {-1, 1, 3, 3, 3, 1});
    Expect(aShape != nullptr && aShape->barres.size() == 2,
           "B-flat's A shape: a barre and a small barre:" + ShapesText(bb));

    ExpectShape("uke", 0, "maj", {0, 0, 0, 3}, {0, 0, 0, 1});
    ExpectShape("uke", 7, "maj", {0, 2, 3, 2}, {0, 1, 3, 2});
    ExpectShape("uke", 9, "m", {2, 0, 0, 0}, {1, 0, 0, 0});
    ExpectShape("uke", 5, "maj", {2, 0, 1, 0}, {2, 0, 1, 0});

    const auto bass = ShapesFor("bass4", 4, "maj");
    Expect(FindShape(bass, {0, 2, 2, -1}) != nullptr || FindShape(bass, {0, 2, -1, -1}) != nullptr,
           "bass E is root and fifth:" + ShapesText(bass));
}

void TestShapeRules()
{
    long long shapes = 0;
    const auto started = std::chrono::steady_clock::now();

    for (const auto& tuning : music::Tunings())
    {
        const auto neck = NeckFor(tuning.id.c_str());
        const int strings = static_cast<int>(neck.strings.size());

        for (const auto& chord : music::Chords())
        {
            for (int key = 0; key < 12; ++key)
            {
                music::ChordSearch search;
                search.rootPitchClass = key;
                search.intervals = chord.intervals;
                const auto found = music::ChordShapes(neck, search);
                const std::string what = tuning.id + " " + std::to_string(key) + chord.id;
                Expect(static_cast<int>(found.size()) <= search.maxShapes, "at most six shapes: " + what);

                for (const auto& shape : found)
                {
                    ++shapes;
                    int sounding = 0;
                    int bass = 1000;
                    int lowest = 99;
                    int highest = 0;
                    std::set<int> heard;
                    bool gap = false;
                    bool ended = false;

                    for (int s = 0; s < strings; ++s)
                    {
                        const int fret = shape.frets[s];

                        if (fret < 0)
                        {
                            ended = sounding > 0;
                            continue;
                        }

                        gap = gap || ended;
                        ++sounding;
                        heard.insert(music::PitchClass(neck.strings[s] + fret));
                        bass = std::min(bass, neck.strings[s] + fret);

                        if (fret > 0)
                        {
                            lowest = std::min(lowest, fret);
                            highest = std::max(highest, fret);
                        }

                        Expect((fret == 0) == (shape.fingers[s] == 0), "a fretted note has a finger: " + what);
                        Expect(shape.fingers[s] <= music::kMaxFingers, "four fingers at most: " + what);
                    }

                    Expect(!gap, "no muted string inside the chord: " + what + " [" + FretsText(shape.frets) + "]");
                    Expect(highest == 0 || highest - lowest <= 3, "a four-fret stretch at most: " + what);

                    for (const auto& interval : chord.intervals)
                    {
                        const int pc = music::PitchClass(key + interval.semitones);
                        const bool optional =
                            (interval.degree == 5 && interval.semitones == 7 && chord.intervals.size() >= 4) ||
                            neck.instrument == music::Instrument::Bass;
                        Expect(optional || heard.count(pc) > 0, "every tone sounds: " + what);
                        Expect(heard.count(music::PitchClass(key)) > 0, "the root sounds: " + what);
                    }

                    if (neck.instrument != music::Instrument::Ukulele)
                    {
                        Expect(music::PitchClass(bass) == key, "the root is lowest: " + what);
                    }
                    else
                    {
                        Expect(sounding == strings, "a ukulele sounds every string: " + what);
                    }

                    if (neck.instrument == music::Instrument::Bass)
                    {
                        Expect(sounding >= 2 && sounding <= 3, "a bass shape is two or three strings: " + what);
                    }
                }
            }
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "searched " << shapes << " shapes on every tuning, chord and key in " << elapsed << " ms" << std::endl;

    // Common chords everywhere: a guitar or ukulele always has a major and minor shape.
    for (const char* tuning : {"std", "dropd", "eb", "dadgad", "openg", "b7", "fsharp8", "uke", "ukebaritone"})
    {
        for (int key = 0; key < 12; ++key)
        {
            for (const char* quality : {"maj", "m", "7"})
            {
                Expect(!ShapesFor(tuning, key, quality).empty(),
                       std::string("shapes exist: ") + tuning + " " + std::to_string(key) + quality);
            }
        }
    }

    // An extended-range guitar keeps a shape on its low strings.
    for (const char* tuning : {"b7", "fsharp8"})
    {
        const int extra = static_cast<int>(music::FindTuning(tuning)->strings.size()) - 6;

        for (int key = 0; key < 12; ++key)
        {
            const auto found = ShapesFor(tuning, key, "maj");
            bool low = false;

            for (const auto& shape : found)
            {
                low = low || (shape.FirstSoundingString() >= 0 && shape.FirstSoundingString() < extra);
            }

            Expect(low, std::string("a low-string shape: ") + tuning + " " + std::to_string(key) + ShapesText(found));
        }
    }
}

void TestVoiceLeading()
{
    const auto a = ShapesFor("std", 9, "maj");
    music::ChordShape fifthFret;
    fifthFret.frets = {5, 7, 7, 6, 5, 5};
    const int nearest = music::NearestShape(a, fifthFret);
    Expect(nearest >= 0 && a[nearest].position >= 3 && a[nearest].position <= 7,
           "after a chord at the 5th fret the next stays near it: [" +
               (nearest >= 0 ? FretsText(a[nearest].frets) : std::string()) + "]");
    Expect(music::NearestShape({}, fifthFret) == -1, "no shapes, no nearest");
}

void TestView()
{
    nlohmann::json request = {{"mode", "scale"}, {"key", 4}, {"scale", "minpent"}, {"tuning", "std"}, {"position", 1}};
    auto view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["title"] == "E Minor Pentatonic", "the scale's title");
    Expect(view["strings"].size() == 6 && view["strings"][0]["name"] == "E", "the strings, low first");
    Expect(view["positions"].size() == 5 && view["position"] == 1, "five positions, the first chosen");
    Expect(view["scale"]["steps"] == "3 2 2 3 2", "the steps");

    bool rootDot = false;
    int inPosition = 0;

    for (const auto& dot : view["dots"])
    {
        rootDot = rootDot || (dot["string"] == 0 && dot["fret"] == 0 && dot["root"] == true && dot["name"] == "E");
        inPosition += dot["inPosition"].get<bool>() ? 1 : 0;
    }

    Expect(rootDot, "the open low E is a root dot");
    Expect(inPosition == 12, "box 1's twelve notes are in position");

    request = {{"mode", "chord"}, {"key", 4}, {"progression", "blues12"}, {"step", 8}, {"tuning", "std"}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["title"] == "B7" && view["progression"]["next"] == "A7", "a 12-bar's ninth bar is B7, then A7");
    Expect(view["progression"]["chords"][4]["numeral"] == "IV7", "the steps carry numerals");
    Expect(!view["shapes"].empty(), "a progression chord has shapes");
    Expect(view["progression"]["chords"].size() == 12, "every chord of the progression is listed");

    for (const auto& chord : view["progression"]["chords"])
    {
        Expect(chord["shape"].is_object(), "every chord in the strip has a shape: " + chord["name"].get<std::string>());
    }

    // The strip is what stepping gives: the next chord's shape is the one nearest this one.
    const auto stripNext = view["progression"]["chords"][9]["shape"]["frets"];
    const auto showing = view["shapes"][view["position"].get<int>() > 0 ? view["position"].get<int>() - 1 : 0];
    request = {{"mode", "chord"}, {"key", 4},        {"progression", "blues12"},
               {"step", 9},       {"tuning", "std"}, {"previousFrets", showing["frets"]}};
    const auto stepped = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(stepped["shapes"][stepped["position"].get<int>() - 1]["frets"] == stripNext,
           "stepping on lands on the strip's next shape");

    request = {{"mode", "chord"}, {"key", 0}, {"chord", "maj"}, {"tuning", "std"}, {"capo", 2}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    bool belowCapo = false;

    for (const auto& dot : view["dots"])
    {
        belowCapo = belowCapo || dot["fret"].get<int>() < 2;
    }

    for (const auto& shape : view["shapes"])
    {
        for (const auto& fret : shape["frets"])
        {
            belowCapo = belowCapo || (fret.get<int>() >= 0 && fret.get<int>() < 2);
        }
    }

    Expect(view["capo"] == 2 && !belowCapo, "nothing is drawn behind a capo");

    request = {{"mode", "chord"}, {"key", 9}, {"chord", "maj"}, {"previousFrets", {5, 7, 7, 6, 5, 5}}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["position"].get<int>() > 0, "a previous shape picks the nearest one");

    request = {{"mode", "chord"}, {"key", 7}, {"progressionChords", {"I", "bVII", "IV"}}, {"progressionName", "Mine"}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["progression"]["id"] == "custom" && view["progression"]["chords"][1]["name"] == "F",
           "a player's own progression");

    request = {{"tuning", "custom"}, {"instrument", "bass"}, {"strings", {28, 33, 38, 43, 48}}, {"frets", 99}};
    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
    Expect(view["strings"].size() == 5 && view["instrument"] == "bass" && view["frets"] == music::kMaxFrets,
           "a custom tuning, frets clamped");

    view = music::BuildScaleGuideView(music::ParseScaleGuideRequest(nlohmann::json::array()));
    Expect(view["mode"] == "scale" && !view["dots"].empty(), "a malformed request falls back to the defaults");

    const auto catalogue = music::ScaleGuideCatalogJson();
    Expect(catalogue["keys"].size() == 12 && catalogue["keys"][1] == "C" + kSharp + "/D" + kFlat, "the keys");
    Expect(catalogue["tunings"].size() == music::Tunings().size(), "every tuning listed");
}

void TestMessages()
{
    guitarfx::MessageHandlerRegistry registry;
    std::vector<nlohmann::json> sent;
    guitarfx::RegisterScaleGuideMessages(
        registry, [&sent](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); }, {});

    Expect(registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "catalog"}}), "scaleGuide is handled");
    Expect(sent.size() == 1 && sent[0]["type"] == "scaleGuideCatalog" && sent[0]["scales"].size() > 40,
           "the catalogue answers scaleGuideCatalog");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"},
                                     {"action", "view"},
                                     {"requestId", 7},
                                     {"request", {{"mode", "chord"}, {"key", 9}, {"chord", "m"}}}});
    Expect(sent.size() == 2 && sent[1]["type"] == "scaleGuideView" && sent[1]["requestId"] == 7 &&
               sent[1]["view"]["title"] == "Am",
           "a view answers scaleGuideView with the request's id");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}});
    Expect(sent.size() == 3 && sent[2]["type"] == "scaleGuideView" && !sent[2].contains("requestId") &&
               sent[2]["view"]["mode"] == "scale",
           "no action or request draws the default scale");
}

/// `--dump <tuning> <key 0-11> <chord>`: the shapes found, with fingers, for tuning the search.
int Dump(const char* tuning, int key, const char* quality)
{
    if (music::FindTuning(tuning) == nullptr || music::FindChord(quality) == nullptr)
    {
        std::cerr << "unknown tuning or chord" << std::endl;
        return 2;
    }

    for (const auto& shape : ShapesFor(tuning, key, quality))
    {
        std::cout << "[" << FretsText(shape.frets) << "] fingers " << FretsText(shape.fingers) << " score "
                  << shape.score << (shape.barres.empty() ? "" : " barre") << (shape.caged.empty() ? "" : " ")
                  << shape.caged << std::endl;
    }

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 5 && std::string(argv[1]) == "--dump")
    {
        return Dump(argv[2], std::atoi(argv[3]), argv[4]);
    }

    TestIntervals();
    TestSpelling();
    TestRoman();
    TestCatalogue();
    TestScalePositions();
    TestGoldenShapes();
    TestShapeRules();
    TestVoiceLeading();
    TestView();
    TestMessages();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScaleGuideTests passed" << std::endl;
    return 0;
}
