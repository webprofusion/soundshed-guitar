/**
 * @file NoteTrackerTests.cpp
 * @brief Tests for real-time note detection (dsp/NoteTracker.h), and the pitch tracker's
 *        narrower range that it relies on (PitchTracker::SetLowestFrequency).
 *
 * Against synthetic guitar lines whose notes are known exactly (helpers/GuitarPhraseSynth.h), at
 * 44.1 to 192 kHz:
 *   - single notes from silence, E2 to E6 at three strengths, are all found at the right pitch
 *     with nothing extra, within 35 ms of the pick
 *   - a note re-picked while it still rings is found every time, at eighths and sixteenths
 *   - a melody picked over ringing notes, and a staccato one, come out note for note, with no
 *     note read from the window straddling two of them
 *   - hammer-ons and pull-offs come out legato, bends and vibrato as glides of one note
 *   - a harder pick gives a higher velocity; a note under the threshold is not a note
 *   - a muted note stops promptly, a ringing one does not, and noise or silence makes nothing
 *   - the events do not depend on the block size, and a NaN on the input does not stick
 * and on the DI guitar recording, no note is so short, or so quickly corrected, that it looks
 * like a mistake. On the two single-note demo riffs every note is played, and starts within a
 * detection or two of the pitch tracker first reading it.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "DemoAudio.h"
#include "GuitarPhraseSynth.h"
#include "dsp/NoteTracker.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/PitchTracker.h"

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace
{
using guitarfx::NoteTracker;
using guitarfx::PitchTracker;
using guitarfx::test::MidiNote;
using guitarfx::test::NoteHz;
using guitarfx::test::PhraseNote;
using guitarfx::test::RenderPhrase;
using Type = NoteTracker::Event::Type;

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

std::string Num(double v, int precision = 1)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};
constexpr double kDropD = 70.0; ///< Guitar to MIDI's default lowest note

struct Timed
{
    Type type = Type::Start;
    double seconds = 0.0;
    double pitch = 0.0;
    float velocity = 0.0f;
    std::size_t sample = 0;
};

std::vector<Timed> Run(const std::vector<float>& x, double sampleRate, double lowestHz = kDropD, int block = 64,
                       double thresholdDb = NoteTracker::kDefaultThresholdDb)
{
    NoteTracker tracker;
    tracker.SetThresholdDb(thresholdDb);
    tracker.Prepare(sampleRate);
    tracker.SetLowestFrequency(lowestHz);
    std::vector<Timed> events;

    for (std::size_t start = 0; start < x.size(); start += static_cast<std::size_t>(block))
    {
        const int count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - start));
        tracker.Process(x.data() + start, count, [&](const NoteTracker::Event& e) {
            const std::size_t sample = start + static_cast<std::size_t>(e.sampleOffset);
            events.push_back({e.type, static_cast<double>(sample) / sampleRate, e.pitch, e.velocity, sample});
        });
    }

    return events;
}

bool IsNoteStart(const Timed& e)
{
    return e.type == Type::Start || e.type == Type::Legato;
}

struct Score
{
    int notes = 0;
    int found = 0;
    int extra = 0;
    double worstLatencyMs = 0.0;
    double worstCents = 0.0;
};

/// Each played note must be matched by a Start or Legato at its pitch between its own start and
/// the next note's; anything left over is extra.
Score ScorePhrase(const std::vector<PhraseNote>& notes, const std::vector<Timed>& events)
{
    Score score;
    std::vector<bool> used(events.size(), false);

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        const double truth = MidiNote(note.hz);
        const double end = (i + 1 < notes.size()) ? notes[i + 1].start : note.start + note.length;
        ++score.notes;

        for (std::size_t e = 0; e < events.size(); ++e)
        {
            const auto& event = events[e];

            if (used[e] || !IsNoteStart(event) || event.seconds < note.start || event.seconds > end + 0.001)
            {
                continue;
            }

            if (std::lround(event.pitch) == std::lround(truth))
            {
                used[e] = true;
                ++score.found;
                score.worstLatencyMs = std::max(score.worstLatencyMs, 1000.0 * (event.seconds - note.start));
                score.worstCents = std::max(score.worstCents, std::abs(100.0 * (event.pitch - truth)));
                break;
            }
        }
    }

    for (std::size_t e = 0; e < events.size(); ++e)
    {
        score.extra += (!used[e] && IsNoteStart(events[e])) ? 1 : 0;
    }

    return score;
}

std::string Describe(const Score& s)
{
    return std::to_string(s.found) + "/" + std::to_string(s.notes) + " found, " + std::to_string(s.extra) +
           " extra, worst " + Num(s.worstLatencyMs) + " ms, " + Num(s.worstCents) + " cents";
}

std::vector<PhraseNote> SingleNotes()
{
    std::vector<PhraseNote> notes;
    double t = 0.2;

    for (int midi = 40; midi <= 88; midi += 3)
    {
        for (double peak : {0.05, 0.2, 0.6})
        {
            PhraseNote n;
            n.start = t;
            n.length = 0.35;
            n.hz = NoteHz(midi);
            n.peak = peak;
            notes.push_back(n);
            t += 0.55;
        }
    }

    return notes;
}

std::vector<PhraseNote> Repicks(double spacing)
{
    std::vector<PhraseNote> notes;
    double t = 0.2;

    for (int midi : {40, 45, 52, 57, 64, 76})
    {
        for (int r = 0; r < 6; ++r)
        {
            PhraseNote n;
            n.start = t;
            n.length = spacing;
            n.hz = NoteHz(midi);
            n.peak = 0.25;
            n.muted = false;
            notes.push_back(n);
            t += spacing;
        }
    }

    notes.back().muted = true;
    return notes;
}

std::vector<PhraseNote> Melody(bool staccato)
{
    std::vector<PhraseNote> notes;
    double t = 0.2;
    int midi = staccato ? 45 : 52;
    const std::vector<int> ringing = {2, 2, 1, 2, -5, 7, -12, 12, 3, -1, -2, 5, -7, 4, -3, 1, -1, 9, -9, 2, -2};
    const std::vector<int> short_ = {3, 2, 2, 3, 2, -2, -3, -2, -2, 5, 7, -12, 12, -7};

    for (int rep = 0; rep < 3; ++rep)
    {
        for (int step : staccato ? short_ : ringing)
        {
            PhraseNote n;
            n.start = t;
            n.length = staccato ? 0.1 : 0.18;
            n.hz = NoteHz(midi);
            n.peak = 0.25;
            n.muted = staccato;
            notes.push_back(n);
            t += staccato ? 0.15 : 0.18;
            midi += step;
        }
    }

    notes.back().muted = true;
    return notes;
}

double EndOf(const std::vector<PhraseNote>& notes)
{
    return notes.back().start + notes.back().length + 0.5;
}

void TestPhrases()
{
    std::cout << "\nKnown phrases, 44.1 to 192 kHz" << std::endl;

    for (const double rate : kRates)
    {
        const std::string at = " at " + Num(rate / 1000.0) + " kHz";

        const auto single = SingleNotes();
        const Score a = ScorePhrase(single, Run(RenderPhrase(single, EndOf(single), rate), rate));
        Check(a.found == a.notes && a.extra == 0 && a.worstLatencyMs <= 35.0 && a.worstCents < 30.0,
              "single notes from silence" + at, Describe(a));

        for (const double spacing : {0.25, 0.125})
        {
            const auto repicks = Repicks(spacing);
            const Score b = ScorePhrase(repicks, Run(RenderPhrase(repicks, EndOf(repicks), rate), rate));
            Check(b.found == b.notes && b.extra == 0,
                  std::string(spacing > 0.2 ? "eighths" : "sixteenths") + " re-picked while ringing" + at, Describe(b));
        }

        for (const bool staccato : {false, true})
        {
            const auto melody = Melody(staccato);
            const Score c = ScorePhrase(melody, Run(RenderPhrase(melody, EndOf(melody), rate), rate));
            Check(c.found == c.notes && c.extra == 0 && c.worstLatencyMs <= 30.0 && c.worstCents < 20.0,
                  std::string(staccato ? "a staccato melody" : "a melody picked over ringing notes") + at, Describe(c));
        }
    }
}

void TestLegatoAndBends()
{
    std::cout << "\nHammer-ons, pull-offs and bends" << std::endl;
    const double rate = 48000.0;

    std::vector<PhraseNote> legato;
    double t = 0.2;

    for (int midi : {45, 52, 57, 64, 69})
    {
        PhraseNote pick;
        pick.start = t;
        pick.length = 0.15;
        pick.hz = NoteHz(midi);
        pick.peak = 0.3;
        pick.muted = false;
        legato.push_back(pick);
        PhraseNote hammer = pick;
        hammer.start = t + 0.15;
        hammer.hz = NoteHz(midi + 2);
        hammer.legato = true;
        legato.push_back(hammer);
        PhraseNote pull = hammer;
        pull.start = t + 0.30;
        pull.hz = NoteHz(midi);
        pull.muted = true;
        legato.push_back(pull);
        t += 0.7;
    }

    const auto legatoEvents = Run(RenderPhrase(legato, EndOf(legato), rate), rate);
    const Score d = ScorePhrase(legato, legatoEvents);
    const auto starts =
        std::count_if(legatoEvents.begin(), legatoEvents.end(), [](const Timed& e) { return e.type == Type::Start; });
    Check(d.found == d.notes && d.extra == 0, "every hammer-on and pull-off is found", Describe(d));
    Check(starts == 5, "and only the picks start notes; the rest move them legato",
          std::to_string(starts) + " starts for 5 picks");

    std::vector<PhraseNote> bends;
    t = 0.2;

    for (int midi : {52, 59, 64, 71})
    {
        PhraseNote bend;
        bend.start = t;
        bend.length = 0.8;
        bend.hz = NoteHz(midi);
        bend.peak = 0.3;
        bend.bendSemitones = 2.0;
        bend.bendStart = 0.1;
        bend.bendTime = 0.15;
        bends.push_back(bend);
        PhraseNote vibrato = bend;
        vibrato.start = t + 1.0;
        vibrato.bendSemitones = 0.0;
        vibrato.vibratoCents = 40.0;
        bends.push_back(vibrato);
        t += 2.0;
    }

    const auto bendEvents = Run(RenderPhrase(bends, EndOf(bends), rate), rate);
    int bendStarts = 0;
    int bendLegatos = 0;

    for (const auto& e : bendEvents)
    {
        bendStarts += e.type == Type::Start ? 1 : 0;
        bendLegatos += e.type == Type::Legato ? 1 : 0;
    }

    double worstTop = 0.0;
    double widestVibrato = 0.0;

    for (std::size_t i = 0; i < bends.size(); i += 2)
    {
        const double base = MidiNote(bends[i].hz);
        double last = 0.0;
        double low = 1.0e9;
        double high = -1.0e9;

        for (const auto& e : bendEvents)
        {
            if (e.type == Type::Glide && e.seconds > bends[i].start && e.seconds < bends[i].start + 0.8)
            {
                last = e.pitch;
            }

            if (e.type == Type::Glide && e.seconds > bends[i + 1].start + 0.1 && e.seconds < bends[i + 1].start + 0.7)
            {
                low = std::min(low, e.pitch);
                high = std::max(high, e.pitch);
            }
        }

        worstTop = std::max(worstTop, std::abs(last - (base + 2.0)));
        widestVibrato = std::max(widestVibrato, high - low);
    }

    Check(bendStarts == 8 && bendLegatos == 0, "a bend or vibrato stays one note",
          std::to_string(bendStarts) + " starts, " + std::to_string(bendLegatos) + " legato");
    Check(worstTop < 0.05, "a whole-step bend glides all the way up",
          "worst " + Num(100.0 * worstTop) + " cents off +2");
    Check(widestVibrato > 0.6 && widestVibrato < 0.95, "vibrato of +/-40 cents is followed",
          Num(100.0 * widestVibrato) + " cents peak to peak");
}

void TestVelocityAndThreshold()
{
    std::cout << "\nVelocity and threshold" << std::endl;
    const double rate = 48000.0;
    std::vector<PhraseNote> notes;

    for (int i = 0; i < 4; ++i)
    {
        PhraseNote n;
        n.start = 0.2 + 0.5 * i;
        n.length = 0.3;
        n.hz = NoteHz(57);
        n.peak = 0.02 * std::pow(3.0, i); // 0.02, 0.06, 0.18, 0.54
        notes.push_back(n);
    }

    const auto events = Run(RenderPhrase(notes, EndOf(notes), rate), rate);
    std::vector<float> velocities;

    for (const auto& e : events)
    {
        if (e.type == Type::Start)
        {
            velocities.push_back(e.velocity);
        }
    }

    const bool rising = velocities.size() == 4 && std::is_sorted(velocities.begin(), velocities.end()) &&
                        velocities.front() < velocities.back() - 0.3f;
    Check(rising, "a harder pick gives a higher velocity",
          velocities.size() == 4 ? Num(velocities[0], 2) + " " + Num(velocities[1], 2) + " " + Num(velocities[2], 2) +
                                       " " + Num(velocities[3], 2)
                                 : std::to_string(velocities.size()) + " notes");

    PhraseNote quiet;
    quiet.start = 0.2;
    quiet.length = 0.4;
    quiet.hz = NoteHz(57);
    quiet.peak = 0.006; // peaks around -44 dBFS
    const auto quietAudio = RenderPhrase({quiet}, 1.0, rate);
    Check(Run(quietAudio, rate, kDropD, 64, -35.0).empty(), "a note under the threshold is not a note");
    const auto lower = Run(quietAudio, rate, kDropD, 64, -50.0);
    Check(!lower.empty() && lower.front().type == Type::Start && std::lround(lower.front().pitch) == 57,
          "and a lower threshold finds it");
}

void TestStops()
{
    std::cout << "\nStops" << std::endl;
    const double rate = 48000.0;

    PhraseNote muted;
    muted.start = 0.2;
    muted.length = 0.3;
    muted.hz = NoteHz(45);
    muted.peak = 0.3;
    const auto mutedEvents = Run(RenderPhrase({muted}, 1.2, rate), rate);
    const auto stop =
        std::find_if(mutedEvents.begin(), mutedEvents.end(), [](const Timed& e) { return e.type == Type::Stop; });
    const double stopMs = stop != mutedEvents.end() ? 1000.0 * (stop->seconds - 0.5) : 1.0e9;
    Check(stopMs > 0.0 && stopMs < 60.0, "a muted low note stops soon after the mute", Num(stopMs) + " ms");

    PhraseNote ringing = muted;
    ringing.muted = false;
    const auto ringingEvents = Run(RenderPhrase({ringing}, 1.2, rate), rate);
    const bool stillSounding =
        std::none_of(ringingEvents.begin(), ringingEvents.end(), [](const Timed& e) { return e.type == Type::Stop; });
    Check(stillSounding, "a note left ringing keeps sounding");

    std::vector<float> noise(static_cast<std::size_t>(3.0 * rate), 0.0f);
    std::uint32_t seed = 7;

    for (std::size_t n = static_cast<std::size_t>(rate); n < noise.size(); ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        noise[n] = 0.01f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }

    Check(Run(noise, rate).empty(), "silence, then noise at -40 dBFS, makes no notes");
}

void TestBlockSizeAndNaN()
{
    std::cout << "\nBlock size and non-finite input" << std::endl;
    const double rate = 48000.0;
    const auto melody = Melody(false);
    const auto audio = RenderPhrase(melody, EndOf(melody), rate);
    const auto small = Run(audio, rate, kDropD, 16);
    const auto large = Run(audio, rate, kDropD, 4096);
    bool same = small.size() == large.size();

    for (std::size_t i = 0; same && i < small.size(); ++i)
    {
        same = small[i].type == large[i].type && small[i].sample == large[i].sample && small[i].pitch == large[i].pitch;
    }

    Check(same, "the same events at the same samples in blocks of 16 and 4096",
          std::to_string(small.size()) + " events");

    PhraseNote first;
    first.start = 0.2;
    first.length = 0.3;
    first.hz = NoteHz(52);
    first.peak = 0.3;
    PhraseNote second = first;
    second.start = 1.0;
    second.hz = NoteHz(57);
    auto poisoned = RenderPhrase({first, second}, 1.6, rate);

    for (std::size_t n = static_cast<std::size_t>(0.3 * rate); n < static_cast<std::size_t>(0.3 * rate) + 64; ++n)
    {
        poisoned[n] = std::numeric_limits<float>::quiet_NaN();
    }

    const auto events = Run(poisoned, rate);
    const bool recovered = std::any_of(events.begin(), events.end(), [](const Timed& e) {
        return e.type == Type::Start && e.seconds > 1.0 && std::lround(e.pitch) == 57;
    });
    Check(recovered, "a burst of NaN mid-note does not stop the next note being found");
}

void TestLowestFrequency()
{
    std::cout << "\nThe pitch tracker's lowest frequency" << std::endl;
    PitchTracker full;
    full.Prepare(48000.0);
    PitchTracker standard;
    standard.SetLowestFrequency(78.0);
    standard.Prepare(48000.0);
    const double fullWindow = full.HalfWindowSeconds();
    const double standardWindow = standard.HalfWindowSeconds();
    Check(full.LowestFrequencyHz() == PitchTracker::kMinHz && std::abs(fullWindow - 1.0 / PitchTracker::kMinHz) < 0.001,
          "left alone, the tracker keeps its full range and window", Num(1000.0 * fullWindow, 2) + " ms");
    Check(standardWindow < 0.6 * fullWindow, "narrowed to a low E, its window is under 60% as long",
          Num(1000.0 * standardWindow, 2) + " ms");

    // An E2 read on the narrowed tracker, then the range changed under it: a reset, no pitch held.
    PhraseNote e2;
    e2.start = 0.05;
    e2.length = 1.0;
    e2.hz = NoteHz(40);
    e2.peak = 0.3;
    const auto audio = RenderPhrase({e2}, 0.6, 48000.0);
    standard.Process(audio.data(), static_cast<int>(audio.size()));
    full.Process(audio.data(), static_cast<int>(audio.size()));
    const double cents = (standard.FrequencyHz() > 0.0 && full.FrequencyHz() > 0.0)
                             ? 1200.0 * std::log2(standard.FrequencyHz() / full.FrequencyHz())
                             : 1.0e9;
    Check(std::abs(cents) < 1.0, "and reads a low E as the full range does", Num(cents, 2) + " cents apart");
    standard.SetLowestFrequency(58.0);
    Check(standard.FrequencyHz() == 0.0 && !standard.HasPitch(), "changing the range starts the tracker again");
    standard.SetLowestFrequency(1.0e6);
    Check(standard.LowestFrequencyHz() == PitchTracker::kMaxLowestHz, "an out-of-range value is clamped");
}

void TestRealGuitar()
{
    std::cout << "\nDI guitar recording" << std::endl;
    double sampleRate = 0.0;
    const auto guitar = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, sampleRate);

    // The clip is part of the repo: not finding it is a broken test, not a reason to skip.
    if (guitar.empty())
    {
        Check(false, "the demo DI guitar loads", guitarfx::test::kDemoDiGuitar);
        return;
    }

    const auto events = Run(guitar, sampleRate);
    int notes = 0;
    int picks = 0;
    int tooShort = 0;
    int corrected = 0;
    double pickedAt = -1.0; ///< the sounding note's pick
    double movedAt = -1.0;  ///< its pick or its latest legato move
    double notePitch = 0.0;

    for (const auto& e : events)
    {
        if (IsNoteStart(e))
        {
            // A note replaced by another within 30 ms, with no pick between, was most likely wrong.
            if (e.type == Type::Legato && movedAt >= 0.0 && e.seconds - movedAt < 0.03 &&
                std::lround(e.pitch) != std::lround(notePitch))
            {
                ++corrected;
            }

            ++notes;
            picks += e.type == Type::Start ? 1 : 0;
            pickedAt = e.type == Type::Start ? e.seconds : pickedAt;
            movedAt = e.seconds;
            notePitch = e.pitch;
        }
        else if (e.type == Type::Stop && pickedAt >= 0.0)
        {
            tooShort += (e.seconds - pickedAt < 0.03) ? 1 : 0;
            pickedAt = -1.0;
            movedAt = -1.0;
        }
    }

    Check(notes > 200, "the riff comes out as plenty of notes", std::to_string(notes) + " notes");
    Check(tooShort == 0, "no picked note under 30 ms", std::to_string(tooShort) + " of " + std::to_string(picks));
    // The riff is mostly power chords, which a monophonic tracker resolves as one of their notes,
    // not always the same one; this bounds how often that shows as a note swapped at once.
    Check(corrected <= notes / 40, "and few corrected straight away",
          std::to_string(corrected) + " of " + std::to_string(notes));
}

/// A pick in a real recording and the note it played, read offline as the median of the pitch
/// tracker's own estimates from 40 ms after the pick to the next pick, where they agree within a
/// semitone (10th to 90th percentile); and the first estimate on that note, which is as early as
/// anything built on the tracker can know it.
struct RiffNote
{
    std::size_t pick = 0;
    std::size_t end = 0;
    double pitch = 0.0; ///< 0 for no single pitch: a chord, a mute, a scrape
    std::size_t firstRead = 0;
};

std::vector<RiffNote> ReadRiffOffline(const std::vector<float>& x, double sampleRate)
{
    guitarfx::PickAttackDetector picks;
    picks.Prepare(sampleRate);
    PitchTracker tracker;
    tracker.Prepare(sampleRate);
    tracker.SetLowestFrequency(kDropD);
    std::vector<std::size_t> attacks;
    std::vector<std::pair<std::size_t, double>> estimates;
    std::uint64_t seen = 0;

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        if (picks.Process(x[i]))
        {
            attacks.push_back(i);
        }

        tracker.Push(x[i]);

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();

            if (tracker.HasPitch() && tracker.RawFrequencyHz() > 0.0)
            {
                estimates.emplace_back(i, NoteTracker::MidiFromHz(tracker.RawFrequencyHz()));
            }
        }
    }

    attacks.push_back(x.size());
    const auto settle = static_cast<std::size_t>(0.04 * sampleRate);
    std::vector<RiffNote> notes;

    for (std::size_t a = 0; a + 1 < attacks.size(); ++a)
    {
        RiffNote note;
        note.pick = attacks[a];
        note.end = attacks[a + 1];
        std::vector<double> settled;

        for (const auto& [at, pitch] : estimates)
        {
            if (at >= note.pick + settle && at < note.end)
            {
                settled.push_back(pitch);
            }
        }

        std::sort(settled.begin(), settled.end());

        if (settled.size() >= 3 && settled[settled.size() * 9 / 10] - settled[settled.size() / 10] < 1.0)
        {
            note.pitch = settled[settled.size() / 2];

            for (const auto& [at, pitch] : estimates)
            {
                if (at >= note.pick && at < note.end && std::abs(pitch - note.pitch) < 0.6)
                {
                    note.firstRead = at;
                    break;
                }
            }
        }

        notes.push_back(note);
    }

    return notes;
}

/// Guitar Riff 01 and 02 are low single notes (Eb2 to C3), picked hard, between muted chugs. After
/// those picks no lag of the tracker's dips under YIN's threshold for about 50 ms, so a note starts
/// late, whatever this does; what this owes is to start it one detection after the tracker can
/// first read it, and to find every one.
void TestRealRiffs()
{
    std::cout << "\nSingle-note riffs" << std::endl;

    for (const char* file : {"guitar-riff-01.wav", "guitar-riff-02.wav"})
    {
        double sampleRate = 0.0;
        const auto guitar = guitarfx::test::LoadDemoClipMono(file, sampleRate);

        if (guitar.empty())
        {
            Check(false, std::string("the demo clip loads"), file);
            continue;
        }

        const auto truth = ReadRiffOffline(guitar, sampleRate);
        const auto events = Run(guitar, sampleRate);
        const double msPerSample = 1000.0 / sampleRate;
        std::vector<double> afterPick;
        std::vector<double> afterTracker;
        int pitched = 0;
        int missed = 0;

        for (const auto& note : truth)
        {
            if (note.pitch == 0.0)
            {
                continue;
            }

            ++pitched;
            double sounding = 0.0; // the tracker's note at the pick, then as each event moves it
            std::size_t next = 0;

            while (next < events.size() && events[next].sample <= note.pick)
            {
                sounding = events[next].type == Type::Stop ? 0.0 : events[next].pitch;
                ++next;
            }

            // A re-pick of the note already sounding starts nothing new; it only has to carry on.
            const bool alreadyOn = sounding > 0.0 && std::abs(sounding - note.pitch) < 0.5;
            bool found = alreadyOn;

            for (; !found && next < events.size() && events[next].sample < note.end; ++next)
            {
                sounding = events[next].type == Type::Stop ? 0.0 : events[next].pitch;

                if (sounding > 0.0 && std::abs(sounding - note.pitch) < 0.5)
                {
                    found = true;
                    afterPick.push_back(static_cast<double>(events[next].sample - note.pick) * msPerSample);
                    afterTracker.push_back(
                        (static_cast<double>(events[next].sample) - static_cast<double>(note.firstRead)) * msPerSample);
                }
            }

            missed += found ? 0 : 1;
        }

        std::sort(afterPick.begin(), afterPick.end());
        std::sort(afterTracker.begin(), afterTracker.end());
        const auto median = [](const std::vector<double>& v) { return v.empty() ? 1.0e9 : v[v.size() / 2]; };
        const auto worst = [](const std::vector<double>& v) { return v.empty() ? 1.0e9 : v.back(); };
        const std::string name(file);
        Check(pitched >= 8 && missed == 0, name + ": every single note is played",
              std::to_string(pitched - missed) + " of " + std::to_string(pitched));
        Check(median(afterTracker) <= 10.0 && worst(afterTracker) <= 15.0,
              name + ": a new note starts within a detection or two of the tracker first reading it",
              Num(median(afterTracker)) + " ms median, " + Num(worst(afterTracker)) + " ms worst");
        Check(median(afterPick) <= 65.0 && worst(afterPick) <= 100.0, name + ": and so within 100 ms of its pick",
              Num(median(afterPick)) + " ms median, " + Num(worst(afterPick)) + " ms worst");
    }
}
} // namespace

int main()
{
    std::cout << "Note tracker tests" << std::endl;
    TestPhrases();
    TestLegatoAndBends();
    TestVelocityAndThreshold();
    TestStops();
    TestBlockSizeAndNaN();
    TestLowestFrequency();
    TestRealGuitar();
    TestRealRiffs();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
