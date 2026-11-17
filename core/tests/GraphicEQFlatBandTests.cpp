/**
 * @file GraphicEQFlatBandTests.cpp
 * @brief The Graphic EQ skips bands at 0 dB without changing a sample of what it plays.
 *
 * A band at 0 dB used to run as a pass-through filter, so ten flat bands cost ten filters per
 * sample per side. Now it is skipped and its state follows the signal. Every check compares the
 * effect with the old way of doing it, kept here: each sample through every switched-on band in
 * turn, a flat one included. They must agree bit for bit, including when a band moves to or from
 * 0 dB mid-stream, at block sizes that don't divide evenly.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/GraphicEQEffect.h"

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBands = GraphicEQEffect::kMaxBands;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

/// The Graphic EQ as it was: the same coefficients, each sample through every switched-on band.
struct Reference
{
    struct Band
    {
        double gainDb = 0.0, frequencyHz = 1000.0, q = 1.0;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
    };

    std::array<Band, kBands> bands{};

    void Design(Band& band) const
    {
        const double maxFrequency = std::max(20.0, std::min(20000.0, kSampleRate * 0.49));
        const double designFrequencyHz = std::min(band.frequencyHz, maxFrequency);

        if (std::abs(band.gainDb) < 0.001)
        {
            band.b0 = 1.0f;
            band.b1 = band.b2 = band.a1 = band.a2 = 0.0f;
            return;
        }

        const double amplitude = std::pow(10.0, band.gainDb / 40.0);
        const double w0 = 2.0 * 3.14159265358979323846 * designFrequencyHz / kSampleRate;
        const double alpha = std::sin(w0) / (2.0 * band.q);
        const double a0 = 1.0 + alpha / amplitude;
        band.b0 = static_cast<float>((1.0 + alpha * amplitude) / a0);
        band.b1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
        band.b2 = static_cast<float>((1.0 - alpha * amplitude) / a0);
        band.a1 = band.b1;
        band.a2 = static_cast<float>((1.0 - alpha / amplitude) / a0);
    }

    void Set(int index, double gainDb, double frequencyHz, double q)
    {
        auto& band = bands[static_cast<size_t>(index)];
        band.gainDb = gainDb;
        band.frequencyHz = frequencyHz;
        band.q = q;
        Design(band);
    }

    float Run(float input, int channel)
    {
        float signal = input;

        for (auto& band : bands)
        {
            const float clean = IsFinite(signal) ? signal : 0.0f;
            float out = band.b0 * clean + band.b1 * band.x1[channel] + band.b2 * band.x2[channel] -
                        band.a1 * band.y1[channel] - band.a2 * band.y2[channel];

            if (!IsFinite(out))
            {
                band.x1[channel] = band.x2[channel] = band.y1[channel] = band.y2[channel] = 0.0f;
                out = clean;
            }
            else
            {
                band.x2[channel] = band.x1[channel];
                band.x1[channel] = clean;
                band.y2[channel] = band.y1[channel];
                band.y1[channel] = out;
            }

            signal = out;
        }

        return signal;
    }
};

struct Rig
{
    GraphicEQEffect eq;
    Reference reference;
    std::mt19937 rng{11};
    size_t blockCursor = 0;

    Rig()
    {
        eq.Prepare(kSampleRate, 512);
        eq.SetParam("bandCount", kBands);

        for (int i = 0; i < kBands; ++i)
        {
            const std::string prefix = "band" + std::to_string(i + 1);
            const double hz = GraphicEQEffect::kDefaultFrequencies[static_cast<size_t>(i)];
            eq.SetParam(prefix + "Enabled", 1.0);
            eq.SetParam(prefix + "Freq", hz);
            eq.SetParam(prefix + "Q", 1.4);
            eq.SetParam(prefix + "Gain", 0.0);
            reference.Set(i, 0.0, hz, 1.4);
        }
    }

    void SetGain(int index, double gainDb)
    {
        eq.SetParam("band" + std::to_string(index + 1) + "Gain", gainDb);
        auto& band = reference.bands[static_cast<size_t>(index)];
        reference.Set(index, gainDb, band.frequencyHz, band.q);
    }

    /// `samples` of noise in blocks of 1, 37, 64 and 300 in turn; false at the first difference.
    bool Run(int samples)
    {
        static constexpr int kSizes[] = {1, 37, 64, 300};
        std::uniform_real_distribution<float> noise(-0.8f, 0.8f);
        std::vector<float> inL(512), inR(512), outL(512), outR(512);
        bool same = true;

        while (samples > 0)
        {
            const int n = std::min(samples, kSizes[blockCursor++ % 4]);

            for (int i = 0; i < n; ++i)
            {
                inL[static_cast<size_t>(i)] = noise(rng);
                inR[static_cast<size_t>(i)] = noise(rng);
            }

            float* ins[2] = {inL.data(), inR.data()};
            float* outs[2] = {outL.data(), outR.data()};
            eq.Process(ins, outs, n);

            for (int i = 0; i < n; ++i)
            {
                const float wantL = reference.Run(inL[static_cast<size_t>(i)], 0);
                const float wantR = reference.Run(inR[static_cast<size_t>(i)], 1);
                same = same && std::memcmp(&wantL, &outL[static_cast<size_t>(i)], sizeof(float)) == 0 &&
                       std::memcmp(&wantR, &outR[static_cast<size_t>(i)], sizeof(float)) == 0;
            }

            samples -= n;
        }

        return same;
    }
};

void TestMatchesTheOldWay()
{
    Rig rig;
    Check(rig.Run(4000), "all bands flat: the signal through untouched");

    rig.SetGain(2, 6.0);
    rig.SetGain(7, -4.5);
    Check(rig.Run(4000), "two bands set, eight flat: the same as running all ten");

    // A flat band comes in mid-stream: its history is the signal's, as if it had run all along.
    rig.SetGain(4, 9.0);
    rig.SetGain(9, -12.0);
    Check(rig.Run(4000), "flat bands raised mid-stream carry on from the signal");

    // And set bands go flat, one of them between two that stay set.
    rig.SetGain(2, 0.0);
    rig.SetGain(4, 0.0);
    Check(rig.Run(4000), "bands brought back to 0 dB");

    rig.SetGain(2, -3.0);
    rig.SetGain(4, 3.0);
    rig.SetGain(0, 18.0);
    Check(rig.Run(4000), "and raised again");
}

void TestInPlace()
{
    GraphicEQEffect inPlace, apart;

    for (auto* eq : {&inPlace, &apart})
    {
        eq->Prepare(kSampleRate, 256);
        eq->SetParam("band2Gain", 6.0);
        eq->SetParam("band4Gain", -6.0);
    }

    std::mt19937 rng(3);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> l(256), r(256), outL(256), outR(256);

    for (auto& sample : l)
    {
        sample = noise(rng);
    }

    for (auto& sample : r)
    {
        sample = noise(rng);
    }

    const std::vector<float> dryL(l);
    float* apartIn[2] = {l.data(), r.data()};
    float* apartOut[2] = {outL.data(), outR.data()};
    apart.Process(apartIn, apartOut, 256);
    float* both[2] = {l.data(), r.data()};
    inPlace.Process(both, both, 256);
    Check(l == outL && r == outR, "processing in place gives the same as into separate buffers");

    // A mono input fed as the same buffer to both sides, out to two others.
    std::vector<float> mono(dryL), monoOutL(256), monoOutR(256);
    GraphicEQEffect shared;
    shared.Prepare(kSampleRate, 256);
    shared.SetParam("band2Gain", 6.0);
    shared.SetParam("band4Gain", -6.0);
    float* sharedIn[2] = {mono.data(), mono.data()};
    float* sharedOut[2] = {monoOutL.data(), monoOutR.data()};
    shared.Process(sharedIn, sharedOut, 256);
    Check(monoOutL == monoOutR && monoOutL == outL, "one buffer read by both sides comes out the same on each");
}

void ReportCost()
{
    GraphicEQEffect eq;
    eq.Prepare(kSampleRate, 64);
    eq.SetParam("bandCount", kBands);
    std::vector<float> l(64, 0.1f), r(64, 0.1f);
    float* io[2] = {l.data(), r.data()};

    const auto time = [&] {
        double best = 1.0e9;

        for (int round = 0; round < 5; ++round)
        {
            const auto start = std::chrono::steady_clock::now();

            for (int b = 0; b < 2000; ++b)
            {
                eq.Process(io, io, 64);
            }

            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }

        return best;
    };

    const double flat = time();
    eq.SetParam("band3Gain", 4.0);
    eq.SetParam("band8Gain", -4.0);
    const double two = time();

    for (int i = 1; i <= kBands; ++i)
    {
        eq.SetParam("band" + std::to_string(i) + "Gain", i % 2 ? 3.0 : -3.0);
    }

    const double all = time();
    std::cout << "10 bands, time relative to all set: flat " << flat / all << ", two set " << two / all << "\n";
    Check(two < all * 0.6, "two bands set cost clearly less than ten");
}
} // namespace

int main()
{
    TestMatchesTheOldWay();
    TestInPlace();
    ReportCost();
    std::cout << (gFailures == 0 ? "GraphicEQFlatBandTests PASSED\n" : "GraphicEQFlatBandTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
