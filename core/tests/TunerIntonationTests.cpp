/**
 * TunerIntonationTests.cpp - The tuner's intonation check as the native UIs have it
 * (uiclient/Intonation.h), held to the web UI's tunerIntonation.ts and its tests
 * (core/ui/tests/tunerIntonation.test.ts): the same fret maths, the same settling and the same
 * advice.
 */

#include "uiclient/Intonation.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx::uiclient::intonation;

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

bool Near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) <= tolerance;
}

constexpr double kLowE = 82.4069;

double Cents(double hz, double c)
{
    return hz * std::pow(2.0, c / 1200.0);
}

/// A plucked note as the tuner reads it: an attack that starts sharp, then a steady sustain.
std::vector<Reading> Pluck(double hz, int sustain = 12, double levelDb = -20.0)
{
    std::vector<Reading> out;

    for (int i = 0; i < kAttackReadings; ++i)
    {
        out.push_back({true, Cents(hz, 30.0 - i * 7.0), levelDb});
    }

    for (int i = 0; i < sustain; ++i)
    {
        out.push_back({true, Cents(hz, i % 2 ? 1.0 : -1.0), levelDb - i * 0.3});
    }

    return out;
}

const Reading kSilence{false, 0.0, std::nullopt};

View Play(IntonationTest& test, const std::vector<Reading>& readings, bool thenSilence = true)
{
    for (const auto& r : readings)
    {
        test.Push(r);
    }

    if (thenSilence)
    {
        test.Push(kSilence);
    }

    return test.GetView();
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines)
{
    std::string out;

    for (const auto& line : lines)
    {
        out += line + " ";
    }

    return out;
}

void TestFretReading()
{
    auto r = ReadFret(kLowE, kLowE * 2.0);
    Expect(r && r->fret == 12 && Near(r->cents, 0.0), "an octave above the open string is the 12th fret");
    Expect(ReadFret(kLowE, kLowE * std::pow(2.0, 5.0 / 12.0))->fret == 5, "five semitones is the 5th fret");

    r = ReadFret(kLowE, Cents(kLowE * 2.0, 7.0));
    Expect(r && r->fret == 12 && Near(r->cents, 7.0) && Near(r->targetFrequency, kLowE * 2.0),
           "how far the fret is out, sharp positive");
    Expect(Near(ReadFret(kLowE, Cents(kLowE * 2.0, -12.0))->cents, -12.0), "flat negative");

    const double open = Cents(kLowE, 18.0);
    Expect(Near(ReadFret(open, Cents(open * 2.0, 3.0))->cents, 3.0),
           "how well the open string was tuned does not matter");

    Expect(!ReadFret(kLowE, kLowE * 0.9), "a note below the open string is no fret");
    Expect(!ReadFret(kLowE, kLowE * std::pow(2.0, 25.0 / 12.0)), "nor is one past the top fret");
    Expect(ReadFret(kLowE, kLowE * 4.0)->fret == 24, "the 24th fret is read");
}

void TestQualityAndAdvice()
{
    Expect(QualityOf(1.5) == Quality::Excellent && QualityOf(-4.0) == Quality::Good &&
               QualityOf(8.0) == Quality::Fair && QualityOf(-15.0) == Quality::Poor,
           "graded by cents either way");

    Expect(Contains(Advice(12, 8.0), "further from the fretboard"), "sharp at the 12th: saddle further away");
    Expect(Contains(Advice(12, -8.0), "closer to the fretboard"), "flat at the 12th: saddle closer");
    Expect(Advice(12, 2.0).empty(), "nothing to say when it is fine");
    Expect(Contains(Advice(1, 9.0), "nut slot") && !Contains(Advice(1, 9.0), "saddle"),
           "sharp near the nut blames the nut, not the saddle");

    const auto nut = Join(StringAdvice({{1, 9.0, Quality::Fair}, {12, 1.0, Quality::Excellent}}));
    Expect(Contains(nut, "Saddle is right") && Contains(nut, "nut slot"), "first frets sharp, 12th fine: the nut");

    const auto saddle = Join(StringAdvice({{2, 8.0, Quality::Fair}, {12, 9.0, Quality::Fair}}));
    Expect(Contains(saddle, "further from the fretboard") && !Contains(saddle, "nut slot"),
           "all sharp alike: the saddle, not the nut");

    const auto nearest =
        StringAdvice({{7, -1.0, Quality::Excellent}, {12, -9.0, Quality::Fair}, {19, 0.0, Quality::Excellent}});
    Expect(nearest.size() == 1 && Contains(nearest[0], "closer to the fretboard"), "the fret nearest the 12th decides");

    Expect(FormatCents(3.24) == "+3.2\xC2\xA2" &&
               FormatCents(-1.0) == "\xE2\x88\x92"
                                    "1.0\xC2\xA2" &&
               FormatCents(0.01) == "\xC2\xB1"
                                    "0.0\xC2\xA2",
           "cents formatted as the web UI does");
}

void TestNearest()
{
    Expect(Nearest(kLowE).name == "E2" && Nearest(440.0).name == "A4", "notes named with their octave");
    Expect(Near(Nearest(Cents(440.0, 20.0)).cents, 20.0), "and how far from them");
}

void TestSettler()
{
    NoteSettler settler;
    const auto readings = Pluck(kLowE);
    int first = -1;
    std::optional<SteadyNote> last;

    for (std::size_t i = 0; i < readings.size(); ++i)
    {
        last = settler.Push(readings[i]);

        if (last && first < 0)
        {
            first = static_cast<int>(i);
        }
    }

    Expect(first == kAttackReadings + static_cast<int>(kSettleReadings) - 1,
           "nothing until the attack is past and the sustain has settled");
    Expect(last && Near(1200.0 * std::log2(last->frequency / kLowE), 0.0, 0.1), "the sustain is measured");

    Expect(!settler.Push({true, Cents(kLowE, 25.0), -10.0}), "a louder reading of the same note is a new pluck");

    NoteSettler wobble;
    bool any = false;

    for (int i = 0; i < 30; ++i)
    {
        any = any || wobble.Push({true, Cents(kLowE, std::sin(i) * 25.0), -20.0}).has_value();
    }

    Expect(!any, "a wobbling pitch never settles");
}

void TestIntonationTest()
{
    {
        IntonationTest test;
        Expect(!test.SetOpenString(), "no open string before a note is heard");
        Play(test, Pluck(Cents(kLowE, 4.0)));
        test.Push(kSilence);
        Expect(test.GetView().candidate && test.GetView().candidate->name == "E2", "the last steady note is offered");
        Expect(test.SetOpenString(), "and taken after it has died away");
        Expect(test.GetView().hasString && test.GetView().string.name == "E2", "the string being checked");
    }

    {
        IntonationTest test;
        const double open = Cents(kLowE, 4.0);
        Play(test, Pluck(open));
        test.SetOpenString();

        auto view = Play(test, Pluck(Cents(open * 2.0, 6.0)));
        Expect(view.measurement && view.measurement->fret == 12 && Near(view.measurement->cents, 6.0, 0.5) &&
                   view.measurement->quality == Quality::Fair,
               "a fret measured against the open string");

        view = Play(test, Pluck(open * std::pow(2.0, 5.0 / 12.0)));
        Expect(view.strings.size() == 1 && view.strings[0].frets.size() == 2 && view.strings[0].frets[0].fret == 5 &&
                   view.strings[0].frets[1].fret == 12,
               "frets recorded, lowest first");

        view = Play(test, Pluck(open * 2.0));
        Expect(view.strings[0].frets[1].quality == Quality::Excellent, "the same fret again replaces the old reading");

        const auto readout = test.Readout(Cents(open * 2.0, -3.0));
        Expect(readout && readout->fret == 12 && Near(readout->cents, -3.0), "the live readout is the fret");
    }

    {
        IntonationTest test;
        Play(test, Pluck(110.0));
        test.SetOpenString();
        const auto view = Play(test, Pluck(kLowE), false);
        Expect(view.offString && !view.measurement, "a note off the string being checked is flagged");
    }

    {
        IntonationTest test;
        Play(test, Pluck(110.0));
        test.SetOpenString();
        Play(test, Pluck(220.0));
        test.NewString();
        Expect(!test.GetView().hasString, "a new string goes back to setting the open string");
        Play(test, Pluck(kLowE));
        test.SetOpenString();
        Play(test, Pluck(kLowE * 2.0));
        auto view = test.GetView();
        Expect(view.strings.size() == 2 && view.strings[0].name == "E2" && view.strings[1].name == "A2",
               "strings kept apart, lowest first");

        test.NewString();
        Play(test, Pluck(Cents(110.0, -3.0)));
        test.SetOpenString();
        view = test.GetView();
        Expect(view.strings.size() == 2 && view.strings[1].frets.empty() && view.strings[0].frets.size() == 1,
               "setting a string again starts its frets over");

        test.Clear();
        Expect(test.GetView().strings.empty() && !test.GetView().hasString, "clear forgets every string");
    }
}
} // namespace

int main()
{
    TestFretReading();
    TestQualityAndAdvice();
    TestNearest();
    TestSettler();
    TestIntonationTest();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "TunerIntonationTests passed" << std::endl;
    return 0;
}
