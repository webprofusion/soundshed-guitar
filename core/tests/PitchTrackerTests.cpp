/**
 * @file PitchTrackerTests.cpp
 * @brief Tests for the real-time pitch tracker (dsp/PitchTracker.h).
 *
 *   - sines, sawtooths and plucked tones with a strong second harmonic, 46 Hz to 1.48 kHz, read
 *     within a few cents at 22.05 to 192 kHz; up to 2 kHz too, and nothing above rather than
 *     an octave low
 *   - steady sawtooths and plucks from 46 to 110 Hz find their pitch on nearly every detection,
 *     though a quarter of the window is shorter than their period
 *   - a note change is followed within 50 ms, vibrato is followed continuously, and a note's
 *     pitch is held through silence however the stop falls between two detections
 *   - a note whose octave partial is far louder than its fundamental still reads as itself
 *   - with fast note changes on, a new note is followed sooner and a steady one reads the same
 *   - white noise never produces a pitch, and a NaN on the input does not stick
 *
 * The DI guitar recording against brute-force full-rate YIN is PitchTrackerRealGuitarTests.cpp.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/PitchTracker.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

using guitarfx::PitchTracker;

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

std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

double Cents(double frequency, double reference)
{
    return 1200.0 * std::log2(frequency / reference);
}

enum class Kind
{
    Sine,
    Saw,
    Pluck
};

/// A test tone at `frequency`. The pluck's second harmonic is louder than its fundamental and its
/// upper harmonics decay faster, which is what tempts a tracker an octave high.
std::vector<float> Tone(double frequency, double seconds, double sampleRate, Kind kind, double vibratoCents = 0.0)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));
    double phase = 0.0;

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;
        const double f = frequency * std::exp2(vibratoCents / 1200.0 * std::sin(2.0 * kPi * 5.0 * t));
        double value = 0.0;

        if (kind == Kind::Sine)
        {
            value = std::sin(phase);
        }
        else if (kind == Kind::Saw)
        {
            for (int k = 1; k * frequency < 0.5 * sampleRate; ++k)
            {
                value += 0.5 * std::sin(k * phase) / k;
            }
        }
        else
        {
            constexpr double kAmplitudes[] = {1.0, 1.4, 0.9, 0.6, 0.4, 0.3, 0.2, 0.15};

            for (int k = 1; k <= 8 && k * frequency < 0.5 * sampleRate; ++k)
            {
                value += 0.25 * kAmplitudes[k - 1] * std::exp(-1.5 * k * t) * std::sin(k * phase + 0.3 * k);
            }
        }

        signal[n] = static_cast<float>(0.3 * value);
        phase += 2.0 * kPi * f / sampleRate;
    }

    return signal;
}

void TestAccuracy()
{
    std::cout << "\nAccuracy after 0.4 s" << std::endl;
    constexpr double kFrequencies[] = {46.2, 55.0, 82.41, 110.0, 196.0, 329.63, 659.26, 1000.0, 1318.5, 1480.0};
    double worst = 0.0;
    std::string where;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        for (const Kind kind : {Kind::Sine, Kind::Saw, Kind::Pluck})
        {
            for (const double frequency : kFrequencies)
            {
                PitchTracker tracker;
                tracker.Prepare(sampleRate);
                const auto tone = Tone(frequency, 0.4, sampleRate, kind);
                tracker.Process(tone.data(), static_cast<int>(tone.size()));
                const double error =
                    tracker.FrequencyHz() > 0.0 ? std::abs(Cents(tracker.FrequencyHz(), frequency)) : 1.0e9;

                if (error > worst)
                {
                    worst = error;
                    where = Num(frequency, 1) + " Hz at " + Num(sampleRate, 0);
                }
            }
        }
    }

    Check(worst < 3.0, "every tone from 46 Hz to 1.48 kHz reads within 3 cents",
          "worst " + Num(worst) + " cents, " + where);

    bool otherRates = true;

    for (const double sampleRate : {22050.0, 32000.0, 88200.0, 192000.0})
    {
        PitchTracker tracker;
        tracker.Prepare(sampleRate);
        const auto tone = Tone(110.0, 0.4, sampleRate, Kind::Pluck);
        tracker.Process(tone.data(), static_cast<int>(tone.size()));
        otherRates = otherRates && tracker.FrequencyHz() > 0.0 && std::abs(Cents(tracker.FrequencyHz(), 110.0)) < 3.0;
    }

    Check(otherRates, "and at 22.05, 32, 88.2 and 192 kHz");

    // Up to 2 kHz, where a guitar's natural harmonics reach; above it, nothing rather than the
    // subharmonic an octave down that the first dip inside the range would give.
    double worstHigh = 0.0;
    bool nothingAbove = true;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        for (const Kind kind : {Kind::Sine, Kind::Saw, Kind::Pluck})
        {
            for (const double frequency : {1568.0, 1648.0, 1760.0, 1975.5, 2200.0})
            {
                PitchTracker tracker;
                tracker.Prepare(sampleRate);
                const auto tone = Tone(frequency, 0.4, sampleRate, kind);
                tracker.Process(tone.data(), static_cast<int>(tone.size()));

                if (frequency > PitchTracker::kMaxHz)
                {
                    nothingAbove = nothingAbove && tracker.FrequencyHz() == 0.0;
                    continue;
                }

                worstHigh = std::max(
                    worstHigh, tracker.FrequencyHz() > 0.0 ? std::abs(Cents(tracker.FrequencyHz(), frequency)) : 1.0e9);
            }
        }
    }

    Check(worstHigh < 3.0 && nothingAbove, "1.57 to 1.98 kHz within 3 cents, and 2.2 kHz not an octave low",
          "worst " + Num(worstHigh) + " cents");
}

/// Of the detections from 0.1 s into `tone`, the percentage that found a pitch.
double FoundPercent(const std::vector<float>& tone, double sampleRate, double lowestHz = PitchTracker::kMinHz,
                    double fromSeconds = 0.1)
{
    PitchTracker tracker;
    tracker.SetLowestFrequency(lowestHz);
    tracker.Prepare(sampleRate);
    std::uint64_t seen = 0;
    int detections = 0;
    int found = 0;

    for (std::size_t n = 0; n < tone.size(); ++n)
    {
        tracker.Push(tone[n]);

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();

            if (n >= static_cast<std::size_t>(fromSeconds * sampleRate))
            {
                ++detections;
                found += tracker.HasPitch() ? 1 : 0;
            }
        }
    }

    return detections > 0 ? 100.0 * found / detections : 0.0;
}

void TestLowNotes()
{
    std::cout << "\nLow notes" << std::endl;

    // Below about 90 Hz the newest quarter of the window, which judges whether a note is stopping,
    // is shorter than a period. A sawtooth's or a pluck's power is uneven across its period, and
    // judged against the window's mean power a steady 46 Hz tone read as stopping on a quarter
    // of its detections.
    double worst = 100.0;
    std::string where;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        for (const Kind kind : {Kind::Saw, Kind::Pluck})
        {
            for (const double frequency : {46.2, 55.0, 65.41, 82.41, 110.0})
            {
                const double found = FoundPercent(Tone(frequency, 0.6, sampleRate, kind), sampleRate);

                if (found < worst)
                {
                    worst = found;
                    where = std::string(kind == Kind::Saw ? "saw " : "pluck ") + Num(frequency, 1) + " Hz at " +
                            Num(sampleRate, 0);
                }
            }
        }
    }

    Check(worst >= 95.0, "sawtooths and plucks from 46 to 110 Hz find their pitch on at least 95% of detections",
          "worst " + Num(worst, 1) + "%" + (where.empty() ? "" : ", " + where));
}

/// A bass's low strings, which the tuner reads with the range taken down to kBassLowestHz.
void TestBassRange()
{
    std::cout << "\nBass range" << std::endl;
    double worst = 100.0;
    double worstCents = 0.0;
    std::string where;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        // An extended-range bass's low C#, a semitone flat of it, a seven-string's low F#, a
        // five-string's low B and a four-string's low E.
        for (const double frequency : {17.32, 16.35, 23.12, 30.87, 41.2})
        {
            const auto tone = Tone(frequency, 0.6, sampleRate, Kind::Pluck);
            // From once the window, 125 ms at the lowest, has filled and the note been confirmed.
            const double found = FoundPercent(tone, sampleRate, PitchTracker::kBassLowestHz, 0.2);
            PitchTracker tracker;
            tracker.SetLowestFrequency(PitchTracker::kBassLowestHz);
            tracker.Prepare(sampleRate);
            tracker.Process(tone.data(), static_cast<int>(tone.size()));
            const double cents =
                tracker.FrequencyHz() > 0.0 ? std::abs(Cents(tracker.FrequencyHz(), frequency)) : 1.0e9;
            worstCents = std::max(worstCents, cents);

            if (found < worst)
            {
                worst = found;
                where = Num(frequency, 2) + " Hz at " + Num(sampleRate, 0);
            }
        }
    }

    Check(worst >= 95.0,
          "plucks down to a semitone under a bass's low C# find their pitch on at least 95% of detections",
          "worst " + Num(worst, 1) + "%" + (where.empty() ? "" : ", " + where));
    Check(worstCents < 2.0, "and read within 2 cents", "worst " + Num(worstCents, 2) + " cents");
    Check(FoundPercent(Tone(30.87, 0.6, 48000.0, Kind::Pluck), 48000.0) < 5.0 &&
              FoundPercent(Tone(17.32, 0.6, 48000.0, Kind::Pluck), 48000.0) < 5.0,
          "left at its default range the tracker does not reach them");

    PitchTracker tracker;
    tracker.SetLowestFrequency(1.0);
    Check(tracker.LowestFrequencyHz() == PitchTracker::kBassLowestHz, "a lower value is held to kBassLowestHz");

    // The effects set their range after Prepare(), and must keep the guitar's buffers and window.
    PitchTracker effect;
    effect.Prepare(48000.0);
    effect.SetLowestFrequency(PitchTracker::kBassLowestHz);
    Check(effect.LowestFrequencyHz() == PitchTracker::kMinHz,
          "after Prepare() the range goes no lower than its buffers were sized for");
    effect.SetLowestFrequency(78.0);
    effect.Prepare(48000.0);
    effect.SetLowestFrequency(1.0);
    Check(effect.LowestFrequencyHz() == PitchTracker::kMinHz, "nor does it from a narrowed range");
}

/// Samples from the start of `second` until the tracker reads it within 20 cents.
int SamplesToFollow(PitchTracker& tracker, const std::vector<float>& second, double frequency)
{
    for (std::size_t n = 0; n < second.size(); ++n)
    {
        tracker.Push(second[n]);

        if (tracker.FrequencyHz() > 0.0 && std::abs(Cents(tracker.FrequencyHz(), frequency)) < 20.0)
        {
            return static_cast<int>(n);
        }
    }

    return -1;
}

void TestResponse()
{
    std::cout << "\nResponse" << std::endl;
    constexpr double kSampleRate = 48000.0;
    constexpr double kChanges[][2] = {
        {110.0, 164.81}, {82.41, 329.63}, {659.26, 82.41}, {392.0, 196.0}, {1318.5, 110.0}};
    int slowest = 0;

    for (const auto& change : kChanges)
    {
        PitchTracker tracker;
        tracker.Prepare(kSampleRate);
        const auto first = Tone(change[0], 0.5, kSampleRate, Kind::Pluck);
        tracker.Process(first.data(), static_cast<int>(first.size()));
        const int samples = SamplesToFollow(tracker, Tone(change[1], 0.3, kSampleRate, Kind::Pluck), change[1]);
        slowest = samples < 0 ? 1 << 30 : std::max(slowest, samples);
    }

    Check(slowest < 0.05 * kSampleRate, "a note change is followed within 50 ms",
          "slowest " + Num(slowest / 48.0, 1) + " ms");

    PitchTracker fresh;
    fresh.Prepare(kSampleRate);
    const int firstNote = SamplesToFollow(fresh, Tone(196.0, 0.3, kSampleRate, Kind::Pluck), 196.0);
    Check(firstNote >= 0 && firstNote < 0.04 * kSampleRate, "a first note within 40 ms",
          Num(firstNote / 48.0, 1) + " ms");

    // 50 cents of vibrato at 5 Hz, either way: the tracked pitch should cover most of it.
    PitchTracker vibrato;
    vibrato.Prepare(kSampleRate);
    const auto wobble = Tone(220.0, 1.5, kSampleRate, Kind::Saw, 50.0);
    double lowest = 1.0e9;
    double highest = -1.0e9;

    for (std::size_t n = 0; n < wobble.size(); ++n)
    {
        vibrato.Push(wobble[n]);

        if (n > wobble.size() / 3 && vibrato.FrequencyHz() > 0.0)
        {
            lowest = std::min(lowest, Cents(vibrato.FrequencyHz(), 220.0));
            highest = std::max(highest, Cents(vibrato.FrequencyHz(), 220.0));
        }
    }

    Check(highest - lowest > 80.0 && highest - lowest < 105.0, "vibrato of +/-50 cents is followed",
          Num(lowest, 1) + " to " + Num(highest, 1) + " cents");

    // A note stopping, abruptly or over a release, then silence: the pitch is held, and held
    // right, rather than read off the window as the note dies away. 0.3 s is a detection at
    // 48 kHz, so the stop is moved across the 5 ms to the next: a window ending in even half a
    // millisecond of silence reads a low note several cents out, and a stop just after a
    // detection leaves one such estimate accepted before the next sees the note stopping.
    double worstHeld = 0.0;
    std::string whereHeld;
    bool quiet = true;

    for (const Kind kind : {Kind::Saw, Kind::Pluck})
    {
        for (const double frequency : {46.2, 65.41, 82.41, 146.83, 329.63, 659.26})
        {
            const auto tone = Tone(frequency, 0.7, kSampleRate, kind);

            for (const double releaseMs : {0.0, 5.0, 10.0, 20.0, 40.0})
            {
                for (const double offsetMs : {0.0, 1.0, 2.0, 3.0, 4.0})
                {
                    PitchTracker held;
                    held.Prepare(kSampleRate);
                    auto note = tone;
                    const double stop = 0.3 + offsetMs / 1000.0;

                    for (auto n = static_cast<std::size_t>(stop * kSampleRate); n < note.size(); ++n)
                    {
                        const double t = static_cast<double>(n) / kSampleRate - stop;
                        note[n] *= releaseMs > 0.0 ? static_cast<float>(std::exp(-1000.0 * t / releaseMs)) : 0.0f;
                    }

                    held.Process(note.data(), static_cast<int>(note.size()));
                    const double error = std::abs(Cents(held.FrequencyHz(), frequency));
                    quiet = quiet && !held.HasPitch();

                    if (!(error <= worstHeld))
                    {
                        worstHeld = error;
                        whereHeld = std::string(kind == Kind::Saw ? "saw " : "pluck ") + Num(frequency, 1) + " Hz, " +
                                    Num(releaseMs, 0) + " ms release, " + Num(offsetMs, 0) + " ms after a detection";
                    }
                }
            }
        }
    }

    Check(worstHeld < 4.0 && quiet,
          "after a note stops, abruptly or over a 5-40 ms release, anywhere between detections, its pitch is held",
          "worst " + Num(worstHeld) + " cents, " + whereHeld);
}

/// A steady tone from its partials' amplitudes, first the fundamental's.
std::vector<float> Partials(double frequency, double seconds, double sampleRate,
                            std::initializer_list<double> amplitudes)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;
        double value = 0.0;
        int k = 1;

        for (const double amplitude : amplitudes)
        {
            value += amplitude * std::sin(2.0 * kPi * k * frequency * t + 0.3 * k);
            ++k;
        }

        signal[n] = static_cast<float>(0.3 * value);
    }

    return signal;
}

/// A note whose octave partial is 14 dB over its fundamental nearly repeats at half its period:
/// YIN's first dip under the threshold is there, an octave high. On the GuitarSet solos that was
/// a fifth of all the time Guitar to MIDI spent on a wrong pitch, mostly notes from E3 to D#4.
void TestWeakFundamental()
{
    std::cout << "\nWeak fundamental" << std::endl;
    int found = 0;
    int right = 0;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        for (const double frequency : {82.41, 110.0, 146.83, 196.0, 261.63, 392.0, 523.25})
        {
            PitchTracker tracker;
            tracker.Prepare(sampleRate);
            const auto tone = Partials(frequency, 0.5, sampleRate, {0.2, 1.0, 0.1});
            std::uint64_t seen = 0;

            for (std::size_t n = 0; n < tone.size(); ++n)
            {
                tracker.Push(tone[n]);

                // Past the first 50 ms: until the window holds two periods, only the octave shows.
                if (tracker.DetectionCount() != seen && n > static_cast<std::size_t>(0.05 * sampleRate))
                {
                    seen = tracker.DetectionCount();
                    found += tracker.HasPitch() ? 1 : 0;
                    right +=
                        (tracker.HasPitch() && std::abs(Cents(tracker.RawFrequencyHz(), frequency)) < 20.0) ? 1 : 0;
                }
            }
        }
    }

    Check(found > 1500 && right == found, "a note with a far louder octave partial reads as itself, not an octave up",
          std::to_string(right) + " of " + std::to_string(found) + " detections");
}

/// SetFastNoteChanges(): a new note is read off the newest audio alone and looked for four times
/// as often, so sooner, and a steady one is left to the long window, so as before.
void TestFastNoteChanges()
{
    std::cout << "\nFast note changes" << std::endl;
    constexpr double kSampleRate = 48000.0;
    constexpr double kChanges[][2] = {{110.0, 164.81}, {82.41, 329.63}, {659.26, 82.41}, {392.0, 196.0},
                                      {1318.5, 110.0}, {196.0, 220.0},  {329.63, 392.0}, {246.94, 196.0}};
    int sooner = 0;
    int later = 0;
    int slowest = 0;

    for (const auto& change : kChanges)
    {
        int samples[2] = {};

        for (int fast = 0; fast < 2; ++fast)
        {
            PitchTracker tracker;
            tracker.Prepare(kSampleRate);
            tracker.SetLowestFrequency(70.0);
            tracker.SetFastNoteChanges(fast != 0);
            const auto first = Tone(change[0], 0.5, kSampleRate, Kind::Pluck);
            tracker.Process(first.data(), static_cast<int>(first.size()));
            samples[fast] = SamplesToFollow(tracker, Tone(change[1], 0.3, kSampleRate, Kind::Pluck), change[1]);
        }

        sooner += (samples[1] >= 0 && samples[1] < samples[0]) ? 1 : 0;
        later += (samples[1] < 0 || samples[1] > samples[0]) ? 1 : 0;
        slowest = samples[1] < 0 ? 1 << 30 : std::max(slowest, samples[1]);
    }

    Check(later == 0 && sooner >= 4, "a note change is followed sooner, and never later",
          std::to_string(sooner) + " of " + std::to_string(std::size(kChanges)) + " sooner, " + std::to_string(later) +
              " later");
    Check(slowest <= 0.025 * kSampleRate, "within 25 ms for a guitar in drop D",
          "slowest " + Num(slowest / 48.0, 1) + " ms");

    // A steady note is read by the long window alone. Its detections fall at other instants,
    // since the search at its start runs four times as often and leaves the 5 ms grid elsewhere,
    // so the two agree to a fraction of a cent rather than to the bit.
    double worstCents = 0.0;
    int plainFound = 0;
    int fastFound = 0;

    for (const double frequency : {46.0, 82.41, 110.0, 196.0, 440.0, 987.77, 1480.0})
    {
        for (const Kind kind : {Kind::Sine, Kind::Saw, Kind::Pluck})
        {
            PitchTracker plain;
            PitchTracker fast;
            plain.Prepare(kSampleRate);
            fast.Prepare(kSampleRate);
            fast.SetFastNoteChanges(true);
            const auto tone = Tone(frequency, 1.0, kSampleRate, kind);
            std::uint64_t plainSeen = 0;
            std::uint64_t fastSeen = 0;

            for (std::size_t n = 0; n < tone.size(); ++n)
            {
                plain.Push(tone[n]);
                fast.Push(tone[n]);

                // Past the note's start, which is the part it is there to change.
                if (n <= static_cast<std::size_t>(0.1 * kSampleRate))
                {
                    plainSeen = plain.DetectionCount();
                    fastSeen = fast.DetectionCount();
                    continue;
                }

                if (plain.FrequencyHz() > 0.0 && fast.FrequencyHz() > 0.0)
                {
                    worstCents = std::max(worstCents, std::abs(Cents(fast.FrequencyHz(), plain.FrequencyHz())));
                }

                if (plain.DetectionCount() != plainSeen)
                {
                    plainSeen = plain.DetectionCount();
                    plainFound += plain.HasPitch() ? 1 : 0;
                }

                if (fast.DetectionCount() != fastSeen)
                {
                    fastSeen = fast.DetectionCount();
                    fastFound += fast.HasPitch() ? 1 : 0;
                }
            }
        }
    }

    Check(worstCents < 1.0 && fastFound >= plainFound, "a steady note reads within a cent of what it does with it off",
          "worst " + Num(worstCents) + " cents, found on " + std::to_string(fastFound) + " detections against " +
              std::to_string(plainFound));
}

void TestRejection()
{
    std::cout << "\nNoise and bad input" << std::endl;
    PitchTracker tracker;
    tracker.Prepare(48000.0);
    std::uint32_t seed = 1u;
    int found = 0;
    std::uint64_t seen = 0;

    for (int n = 0; n < 48000 * 3; ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        tracker.Push(static_cast<float>(0.3 * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0)));

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();
            found += tracker.HasPitch() ? 1 : 0;
        }
    }

    Check(found == 0 && tracker.FrequencyHz() == 0.0, "three seconds of white noise produce no pitch",
          std::to_string(found) + " detections found one");

    PitchTracker poisoned;
    poisoned.Prepare(48000.0);
    auto tone = Tone(220.0, 0.6, 48000.0, Kind::Saw);
    tone[4800] = std::nanf("");
    poisoned.Process(tone.data(), static_cast<int>(tone.size()));
    Check(std::abs(Cents(poisoned.FrequencyHz(), 220.0)) < 3.0, "a NaN on the input does not stick");
}

void ReportCost()
{
    std::cout << "\nCost per 64-sample block at 48 kHz (informational)" << std::endl;
    const auto tone = Tone(82.41, 3.0, 48000.0, Kind::Pluck);
    PitchTracker tracker;
    tracker.Prepare(48000.0);
    const auto begin = std::chrono::steady_clock::now();

    for (std::size_t start = 0; start + 64 <= tone.size(); start += 64)
    {
        tracker.Process(tone.data() + start, 64);
    }

    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - begin;
    std::cout << "  mean " << Num(elapsed.count() / (tone.size() / 64.0)) << " us" << std::endl;
}
} // namespace

int main()
{
    std::cout << "=== PitchTrackerTests ===" << std::endl;

    TestAccuracy();
    TestLowNotes();
    TestBassRange();
    TestResponse();
    TestWeakFundamental();
    TestFastNoteChanges();
    TestRejection();
    ReportCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
