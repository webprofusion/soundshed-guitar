/**
 * @file PitchTrackerRealGuitarTests.cpp
 * @brief The real-time pitch tracker (dsp/PitchTracker.h) on the DI guitar recording: the
 * decimated tracker agrees frame by frame with brute-force full-rate YIN run by the tracker's own
 * rules, finds a pitch in most frames YIN does (less the dying notes it skips on purpose), and is
 * never an octave out. The synthetic tones are PitchTrackerTests.cpp.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "DemoAudio.h"
#include "dsp/BiquadDesign.h"
#include "dsp/PitchTracker.h"

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace
{
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

/// Brute-force full-rate YIN with the tracker's own rules, over the newest samples before `end`.
double ReferenceYin(const std::vector<float>& signal, std::size_t end, double sampleRate)
{
    const int maxLag = static_cast<int>(std::ceil(sampleRate / PitchTracker::kMinHz)) + 1;
    const int minLag = std::max(2, static_cast<int>(sampleRate / PitchTracker::kMaxHz));
    const int size = 2 * maxLag + 2;

    if (end < static_cast<std::size_t>(size))
    {
        return 0.0;
    }

    const float* w = signal.data() + end - size;
    const int base = size - maxLag;
    double energy = 0.0;

    for (int j = base; j < size; ++j)
    {
        energy += static_cast<double>(w[j]) * w[j];
    }

    if (energy < 0.0017783 * 0.0017783 * maxLag)
    {
        return 0.0;
    }

    std::vector<double> normalised(static_cast<std::size_t>(maxLag + 2), 1.0);
    double running = 0.0;

    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        double sum = 0.0;

        for (int j = base; j < size; ++j)
        {
            const double delta = static_cast<double>(w[j]) - w[j - tau];
            sum += delta * delta;
        }

        running += sum;
        normalised[static_cast<std::size_t>(tau)] = running > 0.0 ? sum * tau / running : 1.0;
    }

    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (normalised[static_cast<std::size_t>(tau)] < 0.2)
        {
            while (tau < maxLag &&
                   normalised[static_cast<std::size_t>(tau + 1)] < normalised[static_cast<std::size_t>(tau)])
            {
                ++tau;
            }

            // The tracker's octave check: a shallow first dip gives way to a markedly deeper one
            // at twice its lag.
            if (normalised[static_cast<std::size_t>(tau)] > 0.05 && 2 * tau + 1 <= maxLag)
            {
                int twice = 2 * tau - 1;

                for (int other = 2 * tau; other <= 2 * tau + 1; ++other)
                {
                    if (normalised[static_cast<std::size_t>(other)] < normalised[static_cast<std::size_t>(twice)])
                    {
                        twice = other;
                    }
                }

                if (normalised[static_cast<std::size_t>(tau)] - normalised[static_cast<std::size_t>(twice)] > 0.05)
                {
                    tau = twice;
                }
            }

            const double left = normalised[static_cast<std::size_t>(tau - 1)];
            const double middle = normalised[static_cast<std::size_t>(tau)];
            const double right = normalised[static_cast<std::size_t>(tau + 1)];
            const double curvature = left - 2.0 * middle + right;
            const double offset =
                std::abs(curvature) > 1.0e-12 ? std::clamp(0.5 * (left - right) / curvature, -1.0, 1.0) : 0.0;
            const double frequency = sampleRate / (tau + offset);
            return (frequency >= PitchTracker::kMinHz && frequency <= PitchTracker::kMaxHz) ? frequency : 0.0;
        }
    }

    return 0.0;
}

void TestRealGuitar()
{
    std::cout << "\nDI guitar recording against full-rate YIN" << std::endl;
    double sampleRate = 0.0;
    auto guitar = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, sampleRate);

    // The clip is part of the repo: not finding it is a broken test, not a reason to skip.
    if (guitar.empty())
    {
        Check(false, "the demo DI guitar loads", guitarfx::test::kDemoDiGuitar);
        return;
    }

    guitar.resize(std::min(guitar.size(), static_cast<std::size_t>(40.0 * sampleRate)));

    // The reference hears what the tracker hears: the same 2 kHz low-pass.
    std::vector<float> filtered(guitar.size());
    const auto low0 = guitarfx::biquad::LowPass(2000.0, guitarfx::biquad::ButterworthSectionQ(4, 0), sampleRate);
    const auto low1 = guitarfx::biquad::LowPass(2000.0, guitarfx::biquad::ButterworthSectionQ(4, 1), sampleRate);
    guitarfx::biquad::State state0;
    guitarfx::biquad::State state1;

    for (std::size_t n = 0; n < guitar.size(); ++n)
    {
        filtered[n] = static_cast<float>(state1.Process(low1, state0.Process(low0, guitar[n])));
    }

    PitchTracker tracker;
    tracker.Prepare(sampleRate);
    std::uint64_t seen = 0;
    int compared = 0;
    int trackerFound = 0;
    int referenceFound = 0;
    int both = 0;
    int within10 = 0;
    int octaves = 0;

    for (std::size_t n = 0; n < guitar.size(); ++n)
    {
        tracker.Push(guitar[n]);

        if (tracker.DetectionCount() == seen || (tracker.DetectionCount() % 4) != 0)
        {
            seen = tracker.DetectionCount();
            continue;
        }

        seen = tracker.DetectionCount();
        const double reference = ReferenceYin(filtered, n + 1, sampleRate);
        const double estimate = tracker.RawFrequencyHz();
        ++compared;
        trackerFound += estimate > 0.0 ? 1 : 0;
        referenceFound += reference > 0.0 ? 1 : 0;

        if (estimate > 0.0 && reference > 0.0)
        {
            ++both;
            const double difference = std::abs(Cents(estimate, reference));
            within10 += difference < 10.0 ? 1 : 0;
            octaves += std::abs(difference - 1200.0) < 60.0 ? 1 : 0;
        }
    }

    // The tracker deliberately skips frames where a note is dying away (the newest quarter of the
    // window under half the power of the same span a whole number of periods earlier), which YIN
    // would read out; this staccato recording has a good many, and the fall after a pick attack
    // counts too. It should never report a pitch YIN does not see.
    const double agreement = both > 0 ? 100.0 * within10 / both : 0.0;
    Check(both > compared / 5, "the recording has plenty of pitched frames to compare",
          std::to_string(both) + " of " + std::to_string(compared));
    Check(trackerFound >= referenceFound * 8 / 10 && trackerFound <= referenceFound + compared / 50,
          "the tracker finds a pitch in most frames YIN does, less the dying notes",
          std::to_string(trackerFound) + " vs " + std::to_string(referenceFound));
    Check(agreement > 99.0 && octaves == 0, "and within 10 cents of it, never an octave out",
          Num(agreement) + "%, " + std::to_string(octaves) + " octave errors");
}
} // namespace

int main()
{
    std::cout << "=== PitchTrackerRealGuitarTests ===" << std::endl;
    TestRealGuitar();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
