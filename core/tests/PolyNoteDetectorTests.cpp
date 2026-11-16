/**
 * @file PolyNoteDetectorTests.cpp
 * @brief Tests for the polyphonic note detector (dsp/PolyNoteDetector.h), its spectrum
 *        (dsp/PolySpectrum.h) and the note model it ships with (dsp/PolyNoteModel.h).
 *
 *   - the spectrum places a tone on its own third of a semitone in every phase-placed window, at
 *     its level, at every sample rate; silence reads the floor
 *   - single notes from the low E to the top of the neck are found as themselves and stop when muted
 *   - double stops, power chords and open chords come out as their notes, each started once
 *   - notes left ringing under new ones stay sounding; a chord strummed twice starts twice
 *   - silence, digital or a noise floor, makes no notes, and ends the ones that were sounding
 *   - the threshold and the lowest note rule notes out
 *   - the notes are the same at every sample rate and for every block size, and after a Reset
 *   - following pitch, a note is bent to its own pitch as soon as it starts, and a bend is
 *     reported as it goes; not following, pitches are whole notes
 *
 * The notes here are synthetic strings (GuitarPhraseSynth.h) summed. The model was trained on
 * recordings and never saw one of them, so this is also a check that it has learned notes and not
 * one guitar. Scores on annotated playing are in docs/fx-library.md.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dsp/PolyNoteDetector.h"
#include "dsp/PolyNoteModel.h"
#include "dsp/PolySpectrum.h"
#include "GuitarPhraseSynth.h"

namespace
{
using guitarfx::PolyNoteDetector;
using guitarfx::PolySpectrum;
using guitarfx::test::NoteHz;
using guitarfx::test::PhraseNote;
using guitarfx::test::RenderPhrase;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

struct Strum
{
    std::vector<int> notes;
    double start = 0.2;
    double length = 0.5;   ///< until the strings are muted
    double spread = 0.005; ///< between one string and the next
    double peak = 0.25;
    bool muted = true;
};

/// One synthetic string per note, summed: a chord strummed low string first.
std::vector<float> Render(const std::vector<Strum>& strums, double seconds, double rate)
{
    std::vector<float> audio(static_cast<std::size_t>(seconds * rate), 0.0f);
    std::uint32_t seed = 11;

    for (const auto& strum : strums)
    {
        for (std::size_t s = 0; s < strum.notes.size(); ++s)
        {
            PhraseNote note;
            note.start = strum.start + static_cast<double>(s) * strum.spread;
            note.length = strum.length;
            note.hz = NoteHz(strum.notes[s]);
            note.peak = strum.peak;
            note.muted = strum.muted;
            const auto string = RenderPhrase({note}, seconds, rate, seed++);

            for (std::size_t n = 0; n < audio.size(); ++n)
            {
                audio[n] += string[n];
            }
        }
    }

    return audio;
}

struct Heard
{
    bool start = false;
    int note = 0;
    double time = 0.0; ///< seconds
    float velocity = 0.0f;
};

struct Options
{
    int block = 64;
    double thresholdDb = -50.0;
    int lowestNote = PolyNoteDetector::kLowestNote;
};

std::vector<Heard> Detect(PolyNoteDetector& detector, const std::vector<float>& audio, double rate, int block = 64)
{
    std::vector<Heard> heard;

    for (std::size_t start = 0; start < audio.size(); start += static_cast<std::size_t>(block))
    {
        const int count =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), audio.size() - start));
        detector.Process(audio.data() + start, count, [&](const PolyNoteDetector::Event& event) {
            heard.push_back({event.type == PolyNoteDetector::Event::Type::Start, event.note,
                             (static_cast<double>(start) + event.sampleOffset) / rate, event.velocity});
        });
    }

    return heard;
}

std::vector<Heard> Detect(const std::vector<float>& audio, double rate, const Options& options = {})
{
    PolyNoteDetector detector;
    detector.Prepare(rate, guitarfx::poly_note_model::Model());
    detector.SetThresholdDb(options.thresholdDb);
    detector.SetLowestNote(options.lowestNote);
    return Detect(detector, audio, rate, options.block);
}

/// When each note first started, in seconds.
std::map<int, double> FirstStarts(const std::vector<Heard>& heard)
{
    std::map<int, double> starts;

    for (const auto& h : heard)
    {
        if (h.start)
        {
            starts.emplace(h.note, h.time);
        }
    }

    return starts;
}

int StartCount(const std::vector<Heard>& heard, int note)
{
    return static_cast<int>(
        std::count_if(heard.begin(), heard.end(), [note](const Heard& h) { return h.start && h.note == note; }));
}

std::string Describe(const std::vector<Heard>& heard)
{
    std::string text;

    for (const auto& h : heard)
    {
        text += (text.empty() ? "" : " ") + std::string(h.start ? "+" : "-") + std::to_string(h.note) + "@" +
                std::to_string(static_cast<int>(std::lround(1000.0 * h.time)));
    }

    return text;
}

struct ChordResult
{
    int found = 0;
    int extra = 0;
    double slowest = 0.0; ///< seconds from a string's pick to its note
    std::string text;
};

/// How a strum at `strum.start` came out: its notes found within `within` seconds of their picks,
/// and the notes started that are not in it.
ChordResult Judge(const std::vector<Heard>& heard, const Strum& strum, double within = 0.25)
{
    ChordResult result;
    const auto starts = FirstStarts(heard);

    for (std::size_t s = 0; s < strum.notes.size(); ++s)
    {
        const double pick = strum.start + static_cast<double>(s) * strum.spread;

        if (const auto it = starts.find(strum.notes[s]); it != starts.end() && it->second - pick < within)
        {
            ++result.found;
            result.slowest = std::max(result.slowest, it->second - pick);
        }
    }

    for (const auto& [note, time] : starts)
    {
        if (std::find(strum.notes.begin(), strum.notes.end(), note) == strum.notes.end())
        {
            ++result.extra;
        }
    }

    result.text = std::to_string(result.found) + " of " + std::to_string(strum.notes.size()) + ", " +
                  std::to_string(result.extra) + " extra, slowest " +
                  std::to_string(static_cast<int>(std::lround(1000.0 * result.slowest))) + " ms: " + Describe(heard);
    return result;
}

void TestSpectrum()
{
    std::cout << "\nSpectrum" << std::endl;
    constexpr double kPi = 3.14159265358979323846;

    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        bool placed = true;
        bool levelled = true;
        std::string detail;

        for (int midi : {40, 52, 64, 76, 88})
        {
            PolySpectrum spectrum;
            spectrum.Prepare(rate);
            const double hz = NoteHz(midi);

            for (int n = 0; n < static_cast<int>(0.5 * rate); ++n)
            {
                spectrum.Push(static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * n / rate)));
            }

            const int expected = (midi - PolySpectrum::kLowestMidi) * PolySpectrum::kBinsPerSemitone;

            // The three longest windows hold at least a period and a half of the low E; the
            // shortest does from the E above.
            for (int window = 0; window < (midi >= 52 ? PolySpectrum::kWindows : PolySpectrum::kWindows - 1); ++window)
            {
                const float* grid = spectrum.Frame() + (PolySpectrum::kWindows + window) * PolySpectrum::kBins;
                int best = 0;

                for (int k = 1; k < PolySpectrum::kBins; ++k)
                {
                    best = grid[k] > grid[best] ? k : best;
                }

                const bool here = std::abs(best - expected) <= 1;
                // log10 of an amplitude of 0.5, where the window holds four periods or more: with
                // fewer, some of the tone's energy lands beside it.
                const bool whole = PolySpectrum::kWindowSeconds[static_cast<std::size_t>(window)] * hz >= 4.0;
                const bool level = !whole || std::abs(grid[best] + 0.301f) < 0.06f;

                if (!here || !level)
                {
                    detail += " note " + std::to_string(midi) + " window " + std::to_string(window) + ": bin " +
                              std::to_string(best) + " for " + std::to_string(expected) + ", " +
                              std::to_string(grid[best]);
                }

                placed = placed && here;
                levelled = levelled && level;
            }
        }

        Check(placed,
              "a tone lands on its own third of a semitone in every window at " +
                  std::to_string(static_cast<int>(rate)) + " Hz",
              detail);
        Check(levelled, "and reads its level there", detail);
    }

    PolySpectrum quiet;
    quiet.Prepare(48000.0);
    int frames = 0;

    for (int n = 0; n < 48000; ++n)
    {
        frames += quiet.Push(0.0f) ? 1 : 0;
    }

    const bool floored = std::all_of(quiet.Frame(), quiet.Frame() + PolySpectrum::kFrameSize,
                                     [](float v) { return v == PolySpectrum::kFloor; });
    Check(floored, "silence reads the floor in every bin");
    Check(quiet.HopSamples() == 256 && frames == 48000 / 256, "a frame every 256 samples at 48 kHz",
          std::to_string(frames) + " frames in a second");
}

void TestSingleNotes()
{
    std::cout << "\nSingle notes" << std::endl;
    const double rate = 48000.0;

    for (int midi : {40, 45, 52, 59, 64, 76, 81})
    {
        Strum strum;
        strum.notes = {midi};
        const auto heard = Detect(Render({strum}, 1.0, rate), rate);
        const auto result = Judge(heard, strum);
        const auto starts = FirstStarts(heard);
        const auto own = starts.find(midi);
        const bool stopped = std::any_of(heard.begin(), heard.end(), [&](const Heard& h) {
            return !h.start && h.note == midi && h.time > strum.start + strum.length &&
                   h.time < strum.start + strum.length + 0.2;
        });
        Check(result.found == 1 && own != starts.end() && own->second - strum.start < 0.08,
              "note " + std::to_string(midi) + " is found within 80 ms", result.text);
        Check(result.extra == 0, "with no other note beside it", result.text);
        Check(stopped, "and stops within 200 ms of being muted", result.text);
    }
}

void TestChords()
{
    std::cout << "\nChords" << std::endl;
    const double rate = 48000.0;

    struct Case
    {
        const char* name;
        std::vector<int> notes;
        int mayMiss;
        int mayAdd;
    };

    const std::vector<Case> cases = {
        {"a third, C4 E4", {60, 64}, 0, 0},
        {"a third low down, G3 B3", {55, 59}, 0, 0},
        {"an octave, E3 E4", {52, 64}, 0, 0},
        {"a power chord, A2 E3 A3", {45, 52, 57}, 0, 1},
        {"a high triad, C5 E5 G5", {72, 76, 79}, 0, 0},
        {"open D", {50, 57, 62, 66}, 0, 1},
        {"open A minor", {45, 52, 57, 60, 64}, 1, 1},
        {"open C", {48, 52, 55, 60, 64}, 1, 1},
        {"open E", {40, 47, 52, 56, 59, 64}, 1, 1},
        {"open G", {43, 47, 50, 55, 59, 67}, 1, 1},
        {"an F barre", {41, 48, 53, 57, 60, 65}, 1, 1},
    };

    int found = 0;
    int total = 0;
    int extra = 0;

    for (const auto& c : cases)
    {
        Strum strum;
        strum.notes = c.notes;
        const auto heard = Detect(Render({strum}, 1.0, rate), rate);
        const auto result = Judge(heard, strum);
        found += result.found;
        total += static_cast<int>(c.notes.size());
        extra += result.extra;
        Check(result.found >= static_cast<int>(c.notes.size()) - c.mayMiss && result.extra <= c.mayAdd,
              std::string(c.name) + " comes out as its notes", result.text);

        bool once = true;

        for (int note : c.notes)
        {
            once = once && StartCount(heard, note) <= 1;
        }

        Check(once, "each started once", result.text);
    }

    Check(found >= total - 3 && extra <= 6, "over all of them, nearly every note and few stray ones",
          std::to_string(found) + " of " + std::to_string(total) + " found, " + std::to_string(extra) + " extra");
}

void TestRingingAndRestrum()
{
    std::cout << "\nRinging notes and re-strums" << std::endl;
    const double rate = 48000.0;

    // An arpeggio left ringing: each note picked while the ones before it sound on.
    std::vector<Strum> arpeggio;
    const std::vector<int> notes = {45, 52, 57, 60, 64};

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        Strum pick;
        pick.notes = {notes[i]};
        pick.start = 0.2 + 0.2 * static_cast<double>(i);
        pick.length = 2.0;
        pick.muted = false;
        arpeggio.push_back(pick);
    }

    PolyNoteDetector detector;
    detector.Prepare(rate, guitarfx::poly_note_model::Model());
    const auto audio = Render(arpeggio, 1.3, rate);
    const auto heard = Detect(detector, audio, rate);
    int inOrder = 0;
    int stray = 0;

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        // A start of this note as it is picked: it may also have started before, as an overtone of
        // a lower string, which counts against it below.
        inOrder += std::any_of(heard.begin(), heard.end(),
                               [&](const Heard& h) {
                                   return h.start && h.note == notes[i] && h.time > arpeggio[i].start - 0.01 &&
                                          h.time < arpeggio[i].start + 0.1;
                               })
                       ? 1
                       : 0;
    }

    for (const auto& h : heard)
    {
        const auto at = std::find(notes.begin(), notes.end(), h.note);
        const bool picked =
            at != notes.end() && h.time > arpeggio[static_cast<std::size_t>(at - notes.begin())].start - 0.01;
        stray += (h.start && !picked) ? 1 : 0;
    }

    Check(inOrder >= static_cast<int>(notes.size()) - 1, "an arpeggio's notes start as they are picked",
          std::to_string(inOrder) + " of " + std::to_string(notes.size()) + ": " + Describe(heard));
    Check(stray <= 2, "with few notes started before their string is picked", std::to_string(stray) + " stray");
    Check(detector.SoundingCount() >= static_cast<int>(notes.size()) - 1 && detector.IsSounding(notes.front()),
          "and the ones left ringing are still sounding at the end",
          std::to_string(detector.SoundingCount()) + " sounding");

    // The same chord strummed twice, ringing on between.
    Strum first;
    first.notes = {52, 59, 64};
    first.muted = false;
    first.length = 0.5;
    Strum second = first;
    second.start = 0.6;
    const auto twice = Detect(Render({first, second}, 1.0, rate), rate);
    int restarted = 0;

    for (int note : first.notes)
    {
        restarted += StartCount(twice, note) >= 2 ? 1 : 0;
    }

    Check(restarted >= 2, "a chord strummed again while it rings starts its notes again",
          std::to_string(restarted) + " of 3: " + Describe(twice));
}

void TestSilence()
{
    std::cout << "\nSilence" << std::endl;
    const double rate = 48000.0;
    const std::vector<float> nothing(static_cast<std::size_t>(0.5 * rate), 0.0f);
    Check(Detect(nothing, rate).empty(), "digital silence makes no notes");

    std::vector<float> hiss(static_cast<std::size_t>(0.5 * rate));
    std::uint32_t state = 12345;

    for (float& v : hiss)
    {
        state = state * 1664525u + 1013904223u;
        v = 0.0003f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f); // about -75 dBFS RMS
    }

    Check(Detect(hiss, rate).empty(), "nor does a noise floor");

    // A chord, then digital silence: everything stops.
    Strum strum;
    strum.notes = {52, 59, 64};
    strum.muted = false;
    strum.length = 2.0;
    auto audio = Render({strum}, 0.6, rate);
    audio.resize(static_cast<std::size_t>(0.9 * rate), 0.0f);
    PolyNoteDetector detector;
    detector.Prepare(rate, guitarfx::poly_note_model::Model());
    detector.SetThresholdDb(-50.0);
    const auto heard = Detect(detector, audio, rate);
    const bool allOff = detector.SoundingCount() == 0 && !detector.IsSounding(52);
    int starts = 0;
    int stops = 0;

    for (const auto& h : heard)
    {
        (h.start ? starts : stops) += 1;
    }

    Check(starts >= 3 && allOff && starts == stops, "a chord cut to silence stops every note it started",
          Describe(heard));
}

void TestThresholdAndLowestNote()
{
    std::cout << "\nThreshold and lowest note" << std::endl;
    const double rate = 48000.0;
    Strum quiet;
    quiet.notes = {52, 59, 64};
    quiet.peak = 0.004; // about -48 dBFS a string
    const auto audio = Render({quiet}, 0.9, rate);
    Options loud;
    loud.thresholdDb = -30.0;
    Check(Detect(audio, rate, loud).empty(), "a chord under the threshold makes no notes");
    Options soft;
    soft.thresholdDb = -60.0;
    const auto result = Judge(Detect(audio, rate, soft), quiet);
    Check(result.found >= 2, "and with the threshold under it, it is found, 40 dB down", result.text);

    Strum low;
    low.notes = {40, 52};
    low.spread = 0.0;
    Options sevenFret;
    sevenFret.lowestNote = 45;
    const auto heard = Detect(Render({low}, 0.9, rate), rate, sevenFret);
    Check(StartCount(heard, 40) == 0 && StartCount(heard, 52) >= 1, "a note below the lowest note never starts",
          Describe(heard));
}

void TestRatesBlocksAndReset()
{
    std::cout << "\nSample rates, block sizes and Reset" << std::endl;
    Strum strum;
    strum.notes = {52, 59, 64};

    for (double rate : {44100.0, 96000.0, 192000.0})
    {
        const auto result = Judge(Detect(Render({strum}, 0.8, rate), rate), strum);
        Check(result.found == 3 && result.extra <= 1,
              "the chord is found at " + std::to_string(static_cast<int>(rate)) + " Hz", result.text);
    }

    const double rate = 48000.0;
    const auto audio = Render({strum}, 0.9, rate);
    const std::string reference = Describe(Detect(audio, rate));
    bool same = true;

    for (int block : {1, 17, 1024})
    {
        Options options;
        options.block = block;
        same = same && Describe(Detect(audio, rate, options)) == reference;
    }

    Check(same && !reference.empty(), "the notes and their times are the same for every block size", reference);

    PolyNoteDetector detector;
    detector.Prepare(rate, guitarfx::poly_note_model::Model());
    detector.SetThresholdDb(Options{}.thresholdDb);
    const std::string first = Describe(Detect(detector, audio, rate));
    // Stopped mid-chord: nothing is left sounding, and the same audio comes out the same again.
    (void)Detect(detector, std::vector<float>(audio.begin(), audio.begin() + audio.size() / 2), rate);
    detector.Reset();
    Check(detector.SoundingCount() == 0, "Reset leaves nothing sounding");
    Check(Describe(Detect(detector, audio, rate)) == first && first == reference,
          "and what follows it is what a fresh detector hears");
}

struct Pitched
{
    PolyNoteDetector::Event::Type type;
    int note = 0;
    double pitch = 0.0;
};

std::vector<Pitched> Follow(const PhraseNote& played, bool follow)
{
    const double rate = 48000.0;
    const auto audio = RenderPhrase({played}, 1.2, rate);
    PolyNoteDetector detector;
    detector.Prepare(rate, guitarfx::poly_note_model::Model());
    detector.SetThresholdDb(-50.0);
    detector.SetFollowPitch(follow);
    std::vector<Pitched> events;

    for (std::size_t start = 0; start + 64 <= audio.size(); start += 64)
    {
        detector.Process(audio.data() + start, 64, [&](const PolyNoteDetector::Event& event) {
            events.push_back({event.type, event.note, event.pitch});
        });
    }

    return events;
}

void TestFollowPitch()
{
    std::cout << "\nFollowing pitch" << std::endl;
    using Type = PolyNoteDetector::Event::Type;

    // An A3 played 25 cents sharp.
    PhraseNote sharp;
    sharp.start = 0.2;
    sharp.length = 0.7;
    sharp.hz = NoteHz(57.25);
    const auto followed = Follow(sharp, true);
    const auto plain = Follow(sharp, false);
    const auto start =
        std::find_if(followed.begin(), followed.end(), [](const Pitched& e) { return e.type == Type::Start; });
    const bool plainWhole =
        std::all_of(plain.begin(), plain.end(), [](const Pitched& e) { return e.type != Type::Bend; }) &&
        std::any_of(plain.begin(), plain.end(),
                    [](const Pitched& e) { return e.type == Type::Start && e.pitch == 57.0; });
    // The start is read from a window the note has only just come into; the next frames settle it.
    const auto bend = std::find_if(start, followed.end(), [](const Pitched& e) { return e.type == Type::Bend; });
    Check(start != followed.end() && start->note == 57 && start->pitch >= 57.0 && start->pitch < 57.3 &&
              bend != followed.end() && std::abs(bend->pitch - 57.25) < 0.05,
          "a note 25 cents sharp starts as itself, and is bent to its own pitch",
          start == followed.end()
              ? "no start"
              : std::to_string(start->pitch) + ", then " +
                    (bend == followed.end() ? std::string("no bend") : std::to_string(bend->pitch)));
    Check(plainWhole, "not following, it starts at the whole note and nothing is bent");

    // An A3 curled up 40 cents.
    PhraseNote curl = sharp;
    curl.hz = NoteHz(57.0);
    curl.bendSemitones = 0.4;
    curl.bendStart = 0.2;
    curl.bendTime = 0.1;
    const auto curled = Follow(curl, true);
    int starts = 0;
    double last = 0.0;
    int bends = 0;

    for (const auto& e : curled)
    {
        starts += e.type == Type::Start ? 1 : 0;

        if (e.type == Type::Bend && e.note == 57)
        {
            ++bends;
            last = e.pitch;
        }
    }

    Check(starts == 1 && bends > 0 && std::abs(last - 57.4) < 0.06, "a curl is one note, bent to where it went",
          std::to_string(starts) + " starts, " + std::to_string(bends) + " bends, last at " + std::to_string(last));
}
} // namespace

int main()
{
    std::cout << "Polyphonic note detector tests" << std::endl;
    TestSpectrum();
    TestSingleNotes();
    TestChords();
    TestRingingAndRestrum();
    TestSilence();
    TestThresholdAndLowestNote();
    TestRatesBlocksAndReset();
    TestFollowPitch();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
