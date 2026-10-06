/**
 * @file HarmonizerEffectTests.cpp
 * @brief The harmonizer's voices, its scale arithmetic, and its safety on the audio thread.
 *
 *   - MusicalScale: every scale in every key steps by its own degrees, an octave is seven steps,
 *     and a note outside the scale moves as its nearest scale note does
 *   - Fixed mode: each voice sounds at its own interval, on a single note and on a chord
 *   - Scale mode: a 3rd up is major or minor as the key has it, note by note along a line, and
 *     moves with Key; nothing sounds before a first note is found
 *   - Clean tracking keeps the old interval off a new note, and still starts the harmony with
 *     its pick; Fast plays the pick at the old interval
 *   - Level, Pan, Dry and the reported latency
 *   - the output is the same in any block size, nothing allocates on the audio thread, and
 *     NaN input or parameters never reach the output
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/MusicalScale.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/HarmonizerEffect.h"
#include "helpers/AudioThreadAllocations.h"
#include "helpers/GuitarPhraseSynth.h"

namespace
{
using guitarfx::HarmonizerEffect;
using guitarfx::test::NoteHz;
using guitarfx::test::PhraseNote;
using Params = std::vector<std::pair<std::string, double>>;
namespace music = guitarfx::music;

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;

int gFailures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

/// Runs `input` (mono, fed to both sides) through a fresh harmonizer with `params`, wet only
/// unless the params say otherwise.
Stereo Render(const std::vector<float>& input, const Params& params, int block = kBlock, double rate = kRate)
{
    HarmonizerEffect fx;
    fx.SetParam("dry", 0.0);

    for (const auto& [key, value] : params)
    {
        fx.SetParam(key, value);
    }

    fx.Prepare(rate, std::max(block, kBlock));
    Stereo out{std::vector<float>(input.size(), 0.0f), std::vector<float>(input.size(), 0.0f)};
    std::vector<float> inL(input);
    std::vector<float> inR(input);

    for (std::size_t start = 0; start < input.size(); start += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - start));
        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {out.left.data() + start, out.right.data() + start};
        fx.Process(ins, outs, n);
    }

    return out;
}

std::vector<float> Mono(const Stereo& s)
{
    std::vector<float> m(s.left.size());

    for (std::size_t i = 0; i < m.size(); ++i)
    {
        m[i] = 0.5f * (s.left[i] + s.right[i]);
    }

    return m;
}

double Rms(const std::vector<float>& x, double fromSeconds, double toSeconds, double rate = kRate)
{
    const auto from = static_cast<std::size_t>(fromSeconds * rate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * rate));
    double sum = 0.0;

    for (std::size_t i = from; i < to; ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
    }

    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

double Peak(const std::vector<float>& x, double fromSeconds, double toSeconds, double rate = kRate)
{
    const auto from = static_cast<std::size_t>(fromSeconds * rate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * rate));
    double peak = 0.0;

    for (std::size_t i = from; i < to; ++i)
    {
        peak = std::max(peak, static_cast<double>(std::abs(x[i])));
    }

    return peak;
}

/// The median pitch a fresh tracker reads in `x` between the two times, in Hz; 0 for none.
double PitchBetween(const std::vector<float>& x, double fromSeconds, double toSeconds, double rate = kRate)
{
    guitarfx::PitchTracker tracker;
    tracker.Prepare(rate);
    std::vector<double> readings;
    std::uint64_t seen = 0;
    const auto from = static_cast<std::size_t>(fromSeconds * rate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * rate));

    for (std::size_t i = 0; i < to; ++i)
    {
        tracker.Push(x[i]);

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();

            if (i >= from && tracker.HasPitch())
            {
                readings.push_back(tracker.RawFrequencyHz());
            }
        }
    }

    if (readings.empty())
    {
        return 0.0;
    }

    std::sort(readings.begin(), readings.end());
    return readings[readings.size() / 2];
}

double Cents(double hz, double expectedHz)
{
    return hz > 0.0 ? 1200.0 * std::log2(hz / expectedHz) : 1.0e9;
}

std::vector<float> Tone(double hz, double seconds, double rate = kRate)
{
    // A few harmonics, so the tracker sees a guitar-like waveform rather than a bare sine.
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double>(i) / rate;
        const double p = 2.0 * 3.14159265358979323846 * hz * t;
        x[i] = static_cast<float>(0.2 * std::sin(p) + 0.08 * std::sin(2.0 * p) + 0.04 * std::sin(3.0 * p));
    }

    return x;
}

/// One picked note every `noteSeconds`, at the MIDI notes given.
std::vector<float> Line(const std::vector<int>& notes, double noteSeconds, double tailSeconds = 0.3)
{
    std::vector<PhraseNote> phrase;

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        PhraseNote note;
        note.start = 0.1 + noteSeconds * static_cast<double>(i);
        note.length = noteSeconds;
        note.hz = NoteHz(notes[i]);
        note.peak = 0.3;
        note.muted = false;
        phrase.push_back(note);
    }

    const double seconds = 0.1 + noteSeconds * static_cast<double>(notes.size()) + tailSeconds;
    return guitarfx::test::RenderPhrase(phrase, seconds, kRate);
}

// ── MusicalScale ────────────────────────────────────────────────────────────

void TestScaleArithmetic()
{
    std::cout << "\nMusicalScale" << std::endl;
    bool octaves = true;
    bool inScale = true;
    bool stepsRise = true;
    bool outsideMovesAsNeighbour = true;

    for (int s = 0; s < static_cast<int>(music::Scale::Count); ++s)
    {
        const auto scale = static_cast<music::Scale>(s);

        for (int key = 0; key < music::kPitchClasses; ++key)
        {
            for (int note = 40; note < 88; ++note)
            {
                const int up = music::ScaleStepShift(note, key, scale, 7);
                const int down = music::ScaleStepShift(note, key, scale, -14);
                octaves = octaves && up == 12 && down == -24;

                if (music::InScale(note, key, scale))
                {
                    int previous = 0;

                    for (int steps = music::kMinScaleSteps; steps <= music::kMaxScaleSteps; ++steps)
                    {
                        const int shift = music::ScaleStepShift(note, key, scale, steps);
                        inScale = inScale && music::InScale(note + shift, key, scale);
                        stepsRise = stepsRise && (steps == music::kMinScaleSteps || shift > previous);
                        previous = shift;
                    }

                    inScale = inScale && music::ScaleStepShift(note, key, scale, 0) == 0;
                }
                else
                {
                    // Every scale offered has its out-of-scale notes a semitone from a scale note.
                    const bool below = music::InScale(note - 1, key, scale);
                    const int neighbour = below ? note - 1 : note + 1;
                    outsideMovesAsNeighbour =
                        outsideMovesAsNeighbour && (below || music::InScale(note + 1, key, scale)) &&
                        music::ScaleStepShift(note, key, scale, 2) == music::ScaleStepShift(neighbour, key, scale, 2);
                }
            }
        }
    }

    Check(octaves, "seven steps are an octave and fourteen down two, in every scale, key and note");
    Check(inScale, "every step from a scale note lands on a scale note; zero steps is unison");
    Check(stepsRise, "more steps is always higher");
    Check(outsideMovesAsNeighbour, "a note outside the scale moves as its nearest scale note (the lower on a tie)");

    // Diatonic thirds in C major, the textbook case.
    const int majorThirds[] = {4, 3, 3, 4, 4, 3, 3};
    const int cMajor[] = {60, 62, 64, 65, 67, 69, 71};
    bool thirds = true;

    for (int i = 0; i < 7; ++i)
    {
        thirds = thirds && music::ScaleStepShift(cMajor[i], 0, music::Scale::Major, 2) == majorThirds[i];
    }

    Check(thirds, "thirds in C major: C E F G major, D E A B minor");
    Check(music::ScaleStepShift(64, 9, music::Scale::HarmonicMinor, 2) == 4,
          "E in A harmonic minor: a major 3rd up to G#");
    // Snapping to the scale: a note 45 cents sharp is still its note, and between two scale notes
    // a semitone apart the margin holds the one being played.
    Check(music::NearestScaleNote(64.45, 0, music::Scale::Major) == 64 &&
              music::NearestScaleNote(65.45, 0, music::Scale::Major) == 65 &&
              music::NearestScaleNote(61.5, 0, music::Scale::Major) == 62 &&
              music::NearestScaleNote(61.0, 0, music::Scale::Major) == 60 &&
              music::NearestScaleNote(42.6, 3, music::Scale::NaturalMinor) == 42,
          "pitches snap to the nearest scale note, the lower on a tie");
    Check(music::FollowScaleNote(64, 64.6, 0, music::Scale::Major, 0.3) == 64 &&
              music::FollowScaleNote(64, 64.7, 0, music::Scale::Major, 0.3) == 65 &&
              music::FollowScaleNote(-1, 64.6, 0, music::Scale::Major, 0.3) == 65,
          "a bend moves to the next scale note only once it is nearer by the margin");
}

// ── Fixed mode ──────────────────────────────────────────────────────────────

void TestFixedIntervals()
{
    std::cout << "\nFixed mode" << std::endl;
    const std::vector<float> tone = Tone(220.0, 1.0);

    for (const double semitones : {-12.0, -5.0, 3.0, 7.0, 12.0, 19.0})
    {
        const auto out = Mono(Render(tone, {{"mode", 1.0}, {"voice1Semitones", semitones}}));
        const double expected = 220.0 * std::exp2(semitones / 12.0);
        const double cents = Cents(PitchBetween(out, 0.3, 0.9), expected);
        Check(std::abs(cents) < 5.0, "a voice " + Num(semitones, 0) + " st from 220 Hz", Num(cents) + " cents");
    }

    // Two voices: the sum carries both pitches. Each voice alone at -6 dB is half the level of 0 dB.
    const auto full = Mono(Render(tone, {{"mode", 1.0}, {"voice1Semitones", 7.0}}));
    const auto half = Mono(Render(tone, {{"mode", 1.0}, {"voice1Semitones", 7.0}, {"voice1Level", -6.0}}));
    const double ratioDb = 20.0 * std::log10(Rms(half, 0.3, 0.9) / Rms(full, 0.3, 0.9));
    Check(std::abs(ratioDb + 6.0) < 0.3, "Level sets the voice's level", Num(ratioDb) + " dB");

    const double inputRms = Rms(tone, 0.3, 0.9);
    const double voiceDb = 20.0 * std::log10(Rms(full, 0.3, 0.9) / inputRms);
    Check(std::abs(voiceDb) < 1.0, "a voice at 0 dB is at the input's level", Num(voiceDb) + " dB");

    // A chord: Fixed mode moves every note. Two notes a fifth apart, shifted a fourth up.
    std::vector<float> chord = Tone(110.0, 1.0);
    const std::vector<float> fifth = Tone(165.0, 1.0);

    for (std::size_t i = 0; i < chord.size(); ++i)
    {
        chord[i] = 0.5f * (chord[i] + fifth[i]);
    }

    // The tracker reads the pair's common period, whichever octave of it; judged against the
    // input's own reading, moved a fourth.
    const auto shiftedChord = Mono(Render(chord, {{"mode", 1.0}, {"voice1Semitones", 5.0}}));
    const double chordDb = 20.0 * std::log10(Rms(shiftedChord, 0.3, 0.9) / Rms(chord, 0.3, 0.9));
    const double cents =
        Cents(PitchBetween(shiftedChord, 0.3, 0.9), PitchBetween(chord, 0.3, 0.9) * std::exp2(5.0 / 12.0));
    const double offOctave = std::abs(cents - 1200.0 * std::round(cents / 1200.0));
    Check(std::abs(chordDb) < 2.0 && offOctave < 10.0, "a chord moves as a whole in Fixed mode",
          Num(chordDb) + " dB, " + Num(offOctave) + " cents off");
}

// ── Scale mode ──────────────────────────────────────────────────────────────

/// The harmony of each note of `notes` (picked every `noteSeconds`) against `expected`, judged in
/// the middle of each note. Returns the worst error in cents.
double WorstLineError(const Stereo& out, const std::vector<int>& expected, double noteSeconds)
{
    const auto mono = Mono(out);
    double worst = 0.0;

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const double start = 0.1 + noteSeconds * static_cast<double>(i);
        const double hz = PitchBetween(mono, start + 0.12, start + noteSeconds - 0.02);
        worst = std::max(worst, std::abs(Cents(hz, NoteHz(expected[i]))));
    }

    return worst;
}

void TestScaleLine()
{
    std::cout << "\nScale mode" << std::endl;
    constexpr double kNote = 0.35;
    const std::vector<int> cMajor = {60, 62, 64, 65, 67, 69, 71, 72};
    const std::vector<float> line = Line(cMajor, kNote);

    for (const double tracking : {0.0, 1.0})
    {
        const std::string name = tracking == 0.0 ? "Clean" : "Fast";
        const auto thirds = Render(line, {{"tracking", tracking}});
        const std::vector<int> expectedThirds = {64, 65, 67, 69, 71, 72, 74, 76};
        const double worst = WorstLineError(thirds, expectedThirds, kNote);
        Check(worst < 15.0, name + ": a 3rd up in C major along C D E F G A B C", Num(worst) + " cents worst");
    }

    // Sixteenths at 120 BPM: every note's harmony still arrives, judged from 50 ms into it.
    {
        constexpr double kSixteenth = 0.125;
        const std::vector<int> run = {64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62};
        const auto out = Mono(Render(Line(run, kSixteenth), {}));
        double worst = 0.0;

        for (std::size_t i = 0; i < run.size(); ++i)
        {
            const double start = 0.1 + kSixteenth * static_cast<double>(i);
            const int expected = run[i] + music::ScaleStepShift(run[i], 0, music::Scale::Major, 2);
            worst = std::max(worst, std::abs(Cents(PitchBetween(out, start + 0.05, start + 0.12), NoteHz(expected))));
        }

        Check(worst < 20.0, "Clean keeps up with sixteenths at 120 BPM", Num(worst) + " cents worst");
    }

    // Legato: hammer-ons and pull-offs have no pick, so nothing ducks; the harmony follows.
    {
        std::vector<PhraseNote> phrase;
        const int notes[] = {69, 71, 72, 71, 69};

        for (int i = 0; i < 5; ++i)
        {
            PhraseNote note;
            note.start = 0.1 + 0.3 * i;
            note.length = 0.3;
            note.hz = NoteHz(notes[i]);
            note.legato = i > 0;
            note.muted = i == 4;
            phrase.push_back(note);
        }

        const auto out = Mono(Render(guitarfx::test::RenderPhrase(phrase, 1.8, kRate), {}));
        double worst = 0.0;

        for (int i = 0; i < 5; ++i)
        {
            const double start = 0.1 + 0.3 * i;
            const int expected = notes[i] + music::ScaleStepShift(notes[i], 0, music::Scale::Major, 2);
            worst = std::max(worst, std::abs(Cents(PitchBetween(out, start + 0.1, start + 0.28), NoteHz(expected))));
        }

        Check(worst < 20.0, "a legato line is followed without a pick", Num(worst) + " cents worst");
    }

    // Two voices at once, and an interval below.
    const auto sixthsBelow = Render(line, {{"voice1Interval", -5.0}});
    const std::vector<int> expectedSixths = {52, 53, 55, 57, 59, 60, 62, 64};
    Check(WorstLineError(sixthsBelow, expectedSixths, kNote) < 15.0, "a 6th down in C major");

    // Key: the same E is a minor 3rd below G in C major, a major 3rd below G# in E major.
    const std::vector<float> e4 = Line({64}, 0.8);
    const double inC = PitchBetween(Mono(Render(e4, {{"key", 0.0}})), 0.3, 0.8);
    const double inE = PitchBetween(Mono(Render(e4, {{"key", 4.0}})), 0.3, 0.8);
    Check(std::abs(Cents(inC, NoteHz(67))) < 15.0 && std::abs(Cents(inE, NoteHz(68))) < 15.0,
          "Key decides the interval: E -> G in C major, E -> G# in E major", Num(inC) + " / " + Num(inE) + " Hz");

    // Nothing before the first note: silence in, silence out, then the first note's harmony.
    const auto first = Mono(Render(Line({64}, 0.5), {}));
    Check(Rms(first, 0.0, 0.1) < 1.0e-6, "silent before the first note", Num(Rms(first, 0.0, 0.1), 8));

    HarmonizerEffect probe;
    probe.Prepare(kRate, kBlock);
    Check(probe.GetParam("note") < 0.0, "no note before any input");
}

void TestTracking()
{
    std::cout << "\nTracking" << std::endl;
    // C4 then D4, a 3rd up in C major: E4 (+4) then F4 (+3). Fast plays the D's pick at +4 (F#4)
    // until the D is known; Clean ducks it and starts F4 from the pick once it is.
    constexpr double kNote = 0.5;
    const std::vector<float> line = Line({60, 62}, kNote);
    const double pick = 0.1 + kNote;
    const auto clean = Mono(Render(line, {{"tracking", 0.0}}));
    const auto fast = Mono(Render(line, {{"tracking", 1.0}}));

    // Just after the pick the Clean voice is ducked; Fast's is playing at the old interval.
    const double cleanEarly = Rms(clean, pick + 0.003, pick + 0.010);
    const double fastEarly = Rms(fast, pick + 0.003, pick + 0.010);
    Check(cleanEarly < 0.25 * fastEarly, "Clean ducks under the new pick",
          Num(20.0 * std::log10(cleanEarly / fastEarly)) + " dB against Fast");

    // When it comes back: the first 1 ms window over a quarter of the note's harmony level, after
    // the old note's harmony has ducked away.
    const double later = Rms(clean, pick + 0.1, pick + 0.2);
    double onset = -1.0;

    for (double t = pick + 0.005; t < pick + 0.1; t += 0.001)
    {
        if (Rms(clean, t, t + 0.001) > 0.25 * later)
        {
            onset = t - pick;
            break;
        }
    }

    Check(onset > 0.008 && onset < 0.045, "Clean's harmony enters 8-45 ms after the pick",
          Num(onset * 1000.0, 1) + " ms");

    // And it enters with its pick: as hard as Fast's, which never stopped playing.
    const double cleanPeak = Peak(clean, pick, pick + 0.08);
    const double fastPeak = Peak(fast, pick, pick + 0.08);
    Check(cleanPeak > 0.7 * fastPeak, "Clean's harmony keeps the pick", Num(cleanPeak / fastPeak) + " of Fast's peak");

    // Across the neck: from the low E up, a semitone change each (a 3rd up in C major moves from
    // major to minor or back on most of them).
    for (const int from : {40, 47, 52, 59, 64, 71, 76})
    {
        const std::vector<float> pair = Line({from, from + 1}, kNote);
        const auto c = Mono(Render(pair, {{"tracking", 0.0}}));
        const auto f = Mono(Render(pair, {{"tracking", 1.0}}));
        const double settled = Rms(c, pick + 0.1, pick + 0.2);
        double enters = -1.0;

        for (double t = pick + 0.005; t < pick + 0.1; t += 0.001)
        {
            if (Rms(c, t, t + 0.001) > 0.25 * settled)
            {
                enters = t - pick;
                break;
            }
        }

        const double ratio = Peak(c, pick, pick + 0.08) / Peak(f, pick, pick + 0.08);
        Check(enters > 0.008 && enters < 0.045 && ratio > 0.6,
              "Clean from MIDI " + std::to_string(from) + " to " + std::to_string(from + 1),
              "enters " + Num(enters * 1000.0, 1) + " ms, pick " + Num(ratio) + " of Fast's");
    }

    // Both settle on F4.
    Check(std::abs(Cents(PitchBetween(clean, pick + 0.15, pick + 0.45), NoteHz(65))) < 15.0 &&
              std::abs(Cents(PitchBetween(fast, pick + 0.15, pick + 0.45), NoteHz(65))) < 15.0,
          "both settle on F4");
}

// ── Output ──────────────────────────────────────────────────────────────────

void TestOutput()
{
    std::cout << "\nOutput" << std::endl;
    const std::vector<float> tone = Tone(196.0, 0.8);

    const auto left = Render(tone, {{"mode", 1.0}, {"voice1Pan", -1.0}});
    Check(Rms(left.right, 0.3, 0.7) < 1.0e-4 && Rms(left.left, 0.3, 0.7) > 0.05,
          "Pan hard left puts the voice on the left only");

    const auto centre = Render(tone, {{"mode", 1.0}});
    const double centreDb = 20.0 * std::log10(Rms(centre.left, 0.3, 0.7));
    const double hardDb = 20.0 * std::log10(Rms(left.left, 0.3, 0.7));
    Check(std::abs(hardDb - centreDb - 3.01) < 0.3, "equal-power pan: hard left is 3 dB over each side at the centre",
          Num(hardDb - centreDb) + " dB");

    const auto dryOnly = Render(tone, {{"mode", 1.0}, {"dry", 1.0}, {"voice1On", 0.0}});
    double maxDiff = 0.0;

    for (std::size_t i = 0; i < tone.size(); ++i)
    {
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(dryOnly.left[i] - tone[i])));
    }

    Check(maxDiff < 1.0e-6, "with every voice off the guitar passes undelayed", Num(maxDiff, 8));

    HarmonizerEffect fx;
    fx.Prepare(kRate, kBlock);
    Check(fx.GetLatencySamples() == 0, "reports no latency: the guitar is not delayed");
    Check(!fx.ProducesStereoOutput(), "centred voices keep a mono input mono");
    fx.SetParam("voice1Pan", 0.4);
    Check(fx.ProducesStereoOutput(), "a voice panned off centre makes it stereo");
    fx.SetParam("voice1On", 0.0);
    Check(!fx.ProducesStereoOutput(), "a panned voice that is off does not");

    // High Cut takes the top off the harmony.
    const auto bright = Mono(Render(Line({64}, 0.6), {}));
    const auto dark = Mono(Render(Line({64}, 0.6), {{"highCut", 1500.0}}));
    Check(Rms(dark, 0.2, 0.6) < Rms(bright, 0.2, 0.6), "High Cut darkens the harmony");
}

// ── Determinism and safety ─────────────────────────────────────────────────

void TestBlockSizeIndependence()
{
    std::cout << "\nBlock sizes" << std::endl;
    const std::vector<float> line = Line({55, 57, 59, 60}, 0.25);
    const Params params = {{"voice2On", 1.0}, {"voice2Pan", 0.5}, {"voice1Delay", 12.0}, {"humanize", 1.0}};
    const auto reference = Render(line, params, 64);
    bool same = true;

    for (const int block : {1, 17, 128, 512})
    {
        const auto other = Render(line, params, block);
        same = same && other.left == reference.left && other.right == reference.right;
    }

    Check(same, "bit-identical in blocks of 1, 17, 64, 128 and 512");
}

void TestRealtimeSafety()
{
    std::cout << "\nAudio thread" << std::endl;
    HarmonizerEffect fx;
    fx.Prepare(kRate, kBlock);

    // Keys built here: a Debug std::string from a literal allocates.
    std::vector<std::string> keys;

    for (const auto& spec : guitarfx::harmonizer::kParams)
    {
        keys.emplace_back(spec.id);
    }

    keys.emplace_back("bpm");
    keys.emplace_back("no-such-key");
    const std::vector<float> line = Line({52, 64, 67}, 0.2);
    std::vector<float> inL(line);
    std::vector<float> inR(line);
    std::vector<float> outL(line.size());
    std::vector<float> outR(line.size());

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        int k = 0;

        for (std::size_t start = 0; start + kBlock <= line.size(); start += kBlock)
        {
            const std::string& key = keys[static_cast<std::size_t>(k++) % keys.size()];
            fx.SetParam(key, static_cast<double>(k % 7) - 2.0);
            float* ins[2] = {inL.data() + start, inR.data() + start};
            float* outs[2] = {outL.data() + start, outR.data() + start};
            fx.Process(ins, outs, kBlock);
        }
    });

    Check(allocations.count == 0, "SetParam and Process allocate nothing",
          audio_thread_allocations::Describe(allocations));

    // NaN parameters are refused, NaN and infinite input never reach the output.
    HarmonizerEffect nan;
    nan.Prepare(kRate, kBlock);
    nan.SetParam("voice2On", 1.0);

    for (const auto& key : keys)
    {
        nan.SetParam(key, std::nan(""));
    }

    std::vector<float> bad(line);
    bad[4000] = std::nanf("");
    bad[9000] = INFINITY;
    bad[9001] = -INFINITY;
    std::vector<float> badR(bad);
    bool finite = true;

    for (std::size_t start = 0; start + kBlock <= bad.size(); start += kBlock)
    {
        float* ins[2] = {bad.data() + start, badR.data() + start};
        float* outs[2] = {outL.data() + start, outR.data() + start};
        nan.Process(ins, outs, kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            finite = finite && guitarfx::IsFinite(outL[start + static_cast<std::size_t>(i)]) &&
                     guitarfx::IsFinite(outR[start + static_cast<std::size_t>(i)]);
        }
    }

    Check(finite, "NaN parameters and NaN or infinite input leave the output finite");

    // Enum labels: one per value from each enum's minimum.
    bool labels = true;

    for (const auto& spec : guitarfx::harmonizer::kParams)
    {
        if (std::string(spec.unit) == "enum")
        {
            labels = labels && spec.labels.size() == static_cast<std::size_t>(spec.maxValue - spec.minValue + 1.0);
        }
    }

    Check(labels, "every enum declares one label per value, from its minimum");
}

/// Prints what a configuration costs per 64-sample block at 48 kHz, on a low-to-high line.
void ReportCostOf(const std::string& name, const Params& params)
{
    const std::vector<float> line = Line({40, 47, 52, 55, 59, 64}, 0.5);
    HarmonizerEffect fx;

    for (const auto& [key, value] : params)
    {
        fx.SetParam(key, value);
    }

    fx.Prepare(kRate, kBlock);
    std::vector<float> inL(line);
    std::vector<float> inR(line);
    std::vector<float> outL(line.size());
    std::vector<float> outR(line.size());
    std::vector<double> times;

    for (std::size_t start = 0; start + kBlock <= line.size(); start += kBlock)
    {
        float* ins[2] = {inL.data() + start, inR.data() + start};
        float* outs[2] = {outL.data() + start, outR.data() + start};
        const auto t0 = std::chrono::steady_clock::now();
        fx.Process(ins, outs, kBlock);
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }

    std::sort(times.begin(), times.end());
    double mean = 0.0;

    for (const double t : times)
    {
        mean += t / static_cast<double>(times.size());
    }

    std::cout << "  " << name << ": " << Num(mean) << " us mean, " << Num(times[times.size() * 99 / 100]) << " us p99, "
              << Num(times.back()) << " us worst" << std::endl;
}

void ReportCost()
{
    std::cout << "\nCost, 64-sample blocks at 48 kHz (meaningful in Release only)" << std::endl;
    const Params four = {{"voice2On", 1.0}, {"voice3On", 1.0}, {"voice4On", 1.0}};
    Params fourHumanised = four;
    fourHumanised.emplace_back("humanize", 0.5);
    Params fourFixed = four;
    fourFixed.emplace_back("mode", 1.0);
    ReportCostOf("one voice, Scale mode", {});
    ReportCostOf("four voices, Scale mode", four);
    ReportCostOf("four voices, Fixed mode", fourFixed);
    ReportCostOf("four voices, Scale mode, Humanize 0.5", fourHumanised);
}

void TestRegistration()
{
    std::cout << "\nRegistration" << std::endl;
    guitarfx::RegisterHarmonizerEffect();
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kHarmonizer);
    Check(static_cast<bool>(info), "registered under its GUID");

    if (info)
    {
        Check(info->parameters.size() == guitarfx::harmonizer::kParamCount, "every parameter declared");
        Check(info->presets.size() >= 5, "ships factory presets", std::to_string(info->presets.size()));
    }
}
} // namespace

int main()
{
    try
    {
        std::cout << "HarmonizerEffect tests" << std::endl;
        TestScaleArithmetic();
        TestFixedIntervals();
        TestScaleLine();
        TestTracking();
        TestOutput();
        TestBlockSizeIndependence();
        TestRealtimeSafety();
        TestRegistration();
        ReportCost();
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << (gFailures == 0 ? "\nAll passed" : "\nFailures: " + std::to_string(gFailures)) << std::endl;
    return gFailures == 0 ? 0 : 1;
}
