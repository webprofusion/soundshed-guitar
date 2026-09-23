/**
 * @file BiquadNyquistTests.cpp
 * @brief Fixed-frequency tone filters stay stable at every sample rate a host can ask for.
 *
 * The RBJ cookbook formulas assume the filter frequency is below Nyquist. Past it sin(w0)
 * turns negative, the bandwidth term alpha with it, and the poles leave the unit circle: the
 * output reaches infinity within a few hundred samples. The NAM amp's 6.3 kHz presence bell
 * does exactly that at 8 kHz, which clap-validator's process-varying-sample-rates test caught
 * as a -inf sample in the plugin's output.
 *
 * This drives each effect's fixed-frequency filters with white noise at the sample rates that
 * test uses, plus the low standard rates hosts offer, with the tone controls at both extremes.
 * The finite test goes through FiniteCheck.h, so it still means something in the fast-math
 * Release build.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"
#include "dsp/effects/SimpleCabEffect.h"
#include "dsp/effects/WahEffect.h"

namespace
{
using namespace guitarfx;

// clap-validator's process-varying-sample-rates list, then the low rates real hosts offer.
constexpr double kSampleRates[] = {8000.0,   22050.0,   44100.0,   48000.0,   88200.0,   96000.0, 192000.0, 384000.0,
                                   768000.0, 1234.5678, 12345.678, 45678.901, 123456.78, 11025.0, 16000.0,  32000.0};

// Unit-amplitude noise through at most +10 dB of EQ stays within a few units. An unstable
// filter passes this bound within a few hundred samples, long before it reaches infinity.
constexpr float kMaxMagnitude = 1000.0f;

constexpr int kBlockSize = 512;

struct NoiseSource
{
    std::uint32_t state = 0x9E3779B9u;

    float Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;
    }
};

// A second of audio, and never fewer than 8192 samples at the tiny fractional rates.
int SampleCountFor(double sampleRate)
{
    return std::max(8192, static_cast<int>(sampleRate));
}

std::string RateLabel(double sampleRate)
{
    std::ostringstream out;
    out << sampleRate << " Hz";
    return out.str();
}

// A test and a separate message: this runs on every sample, and in Debug even an empty
// std::string allocates its iterator-debugging proxy.
bool IsBadSample(float y)
{
    return !(IsFinite(y) && std::abs(y) <= kMaxMagnitude);
}

std::string DescribeBadSample(float y, int index)
{
    std::ostringstream out;
    out << "sample " << index << " is " << y;
    return out.str();
}

int gFailures = 0;

void Fail(const std::string& what)
{
    std::cout << "  FAIL  " << what << "\n";
    ++gFailures;
}

void ReportIfClean(int failuresBefore)
{
    if (gFailures == failuresBefore)
    {
        std::cout << "  PASS\n";
    }
}

// ── NAM amp tone stack ────────────────────────────────────────────────────

struct ToneFilterCase
{
    const char* name;
    void (*configure)(AmpToneBiquad& filter, double gainDb, double sampleRate);
};

// The filters and frequencies OptimizedNAMAmpEffect::UpdateToneStack sets up, plus the
// post-model DC blocker. OptimizedNAMFXEffect inherits all of them.
const ToneFilterCase kAmpToneFilters[] = {
    {"bass low shelf 100 Hz",
     [](AmpToneBiquad& f, double gainDb, double sampleRate) { f.SetLowShelf(kAmpToneBassHz, gainDb, sampleRate); }},
    {"mid bell 500 Hz", [](AmpToneBiquad& f, double gainDb,
                           double sampleRate) { f.SetPeaking(kAmpToneMidHz, gainDb, kAmpToneMidQ, sampleRate); }},
    {"treble high shelf 3.2 kHz",
     [](AmpToneBiquad& f, double gainDb, double sampleRate) { f.SetHighShelf(kAmpToneTrebleHz, gainDb, sampleRate); }},
    {"presence bell 6.3 kHz",
     [](AmpToneBiquad& f, double gainDb, double sampleRate) {
         f.SetPeaking(kAmpTonePresenceHz, gainDb, kAmpTonePresenceQ, sampleRate);
     }},
    {"post-model DC blocker",
     [](AmpToneBiquad& f, double /*gainDb*/, double sampleRate) {
         f.SetHighPass(kNamPostDcBlockerFrequencyHz, kNamPostDcBlockerQ, sampleRate);
     }},
};

void TestNamAmpToneStack()
{
    std::cout << "Test: NAM amp tone stack stays stable at every sample rate\n";
    const int failuresBefore = gFailures;

    for (const auto& filterCase : kAmpToneFilters)
    {
        for (const double gainDb : {-10.0, 10.0})
        {
            for (const double sampleRate : kSampleRates)
            {
                AmpToneBiquad filter;
                filterCase.configure(filter, gainDb, sampleRate);
                filter.Reset();

                NoiseSource noise;
                const int count = SampleCountFor(sampleRate);

                for (int i = 0; i < count; ++i)
                {
                    if (const float y = filter.Process(noise.Next()); IsBadSample(y))
                    {
                        std::ostringstream what;
                        what << filterCase.name << " at " << (gainDb > 0.0 ? "+" : "") << gainDb << " dB, "
                             << RateLabel(sampleRate) << ": " << DescribeBadSample(y, i);
                        Fail(what.str());
                        break;
                    }
                }
            }
        }
    }

    ReportIfClean(failuresBefore);
}

// ── Whole effects ─────────────────────────────────────────────────────────

using ParamSet = std::vector<std::pair<const char*, double>>;

// Runs a second of stereo noise through the effect. Also fails when the output never differs
// from the input: an effect that bypassed itself would pass the stability check and prove
// nothing.
void RunEffect(const char* effectName, EffectProcessor& effect, const ParamSet& params, const char* paramsLabel,
               double sampleRate)
{
    for (const auto& [key, value] : params)
    {
        effect.SetParam(key, value);
    }

    effect.Prepare(sampleRate, kBlockSize);
    effect.Reset();

    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    NoiseSource noise;
    int remaining = SampleCountFor(sampleRate);
    int processed = 0;
    bool changedSignal = false;

    while (remaining > 0)
    {
        const int n = std::min(kBlockSize, remaining);

        for (int i = 0; i < n; ++i)
        {
            inL[static_cast<size_t>(i)] = noise.Next();
            inR[static_cast<size_t>(i)] = noise.Next();
        }

        effect.Process(inputs, outputs, n);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < n; ++i)
            {
                const float y = outputs[ch][i];

                if (IsBadSample(y))
                {
                    std::ostringstream what;
                    what << effectName << " (" << paramsLabel << "), " << RateLabel(sampleRate) << ", channel " << ch
                         << ": " << DescribeBadSample(y, processed + i);
                    Fail(what.str());
                    return;
                }

                changedSignal = changedSignal || (y != inputs[ch][i]);
            }
        }

        processed += n;
        remaining -= n;
    }

    if (!changedSignal)
    {
        std::ostringstream what;
        what << effectName << " (" << paramsLabel << "), " << RateLabel(sampleRate)
             << ": output equals input, so the effect did not process and the check proves nothing";
        Fail(what.str());
    }
}

void TestSimpleCab()
{
    std::cout << "Test: simple cab filters stay stable at every sample rate\n";
    const int failuresBefore = gFailures;

    // The brightest and darkest corners of the cabinets that bound the voicing table — the
    // original 4x12, the 1x12 with the narrowest resonance and the 4x10 with the highest
    // roll-off — each with a different mic type, a second mic, spread and distance, so every
    // section is pushed as far towards Nyquist as the voicing takes it.
    constexpr std::pair<double, double> kCabinetAndMic[] = {{0.0, 0.0}, {1.0, 1.0}, {4.0, 2.0}};

    for (const auto& [cab, micType] : kCabinetAndMic)
    {
        const double otherMicType = std::fmod(micType + 1.0, 3.0);
        const ParamSet brightest = {{"cabinet", cab},    {"micType", micType},
                                    {"bass", 1.0},       {"presence", 1.0},
                                    {"brightness", 1.0}, {"mids", 1.0},
                                    {"size", 0.0},       {"micPosition", 0.0},
                                    {"mic2Blend", 0.5},  {"mic2Type", otherMicType},
                                    {"spread", 1.0},     {"mix", 1.0}};
        const ParamSet darkest = {{"cabinet", cab},   {"micType", micType},       {"bass", 0.0},
                                  {"presence", 0.0},  {"brightness", 0.0},        {"mids", 0.0},
                                  {"size", 1.0},      {"micPosition", 1.0},       {"micDistance", 1.0},
                                  {"mic2Blend", 1.0}, {"mic2Type", otherMicType}, {"mic2Distance", 0.3},
                                  {"mix", 1.0}};

        for (const double sampleRate : kSampleRates)
        {
            SimpleCabEffect bright;
            RunEffect("simple cab", bright, brightest, "tone controls at maximum", sampleRate);

            SimpleCabEffect dark;
            RunEffect("simple cab", dark, darkest, "tone controls at minimum", sampleRate);
        }
    }

    ReportIfClean(failuresBefore);
}

void TestBuiltinAmp()
{
    std::cout << "Test: built-in amp filters stay stable at every sample rate\n";
    const int failuresBefore = gFailures;

    const ParamSet toneMaximum = {{"bright", 1.0},  {"preEmphasis", 1.0}, {"treble", 1.0},    {"presence", 1.0},
                                  {"contour", 1.0}, {"depth", 1.0},       {"resonance", 1.0}, {"damping", 1.0}};
    const ParamSet toneMinimum = {{"bright", 0.0},  {"preEmphasis", 0.0}, {"treble", 0.0},    {"presence", 0.0},
                                  {"contour", 0.0}, {"depth", 0.0},       {"resonance", 0.0}, {"damping", 0.0}};

    for (const double sampleRate : kSampleRates)
    {
        BuiltinAmpEffect maximum;
        RunEffect("built-in amp", maximum, toneMaximum, "tone controls at maximum", sampleRate);

        BuiltinAmpEffect minimum;
        RunEffect("built-in amp", minimum, toneMinimum, "tone controls at minimum", sampleRate);
    }

    ReportIfClean(failuresBefore);
}

// The wah's sweep reaches 5 kHz, and its treble shelf sits at 2.5 kHz: both past Nyquist at the
// lowest rates, where they have to be clamped.
void TestWah()
{
    std::cout << "Test: wah filters stay stable at every sample rate\n";
    const int failuresBefore = gFailures;

    const ParamSet toeExtreme = {{"position", 1.0}, {"toeFreq", 5000.0}, {"q", 20.0},         {"toeQScale", 2.0},
                                 {"treble", 12.0},  {"lowEnd", 1.0},     {"saturation", 1.0}, {"mix", 1.0}};
    const ParamSet heelExtreme = {{"position", 0.0}, {"heelFreq", 150.0}, {"q", 20.0},     {"treble", -12.0},
                                  {"lowEnd", 0.0},   {"saturation", 0.0}, {"level", 18.0}, {"mix", 1.0}};

    for (const double sampleRate : kSampleRates)
    {
        WahEffect toe;
        RunEffect("wah", toe, toeExtreme, "toe at 5 kHz, Q 40, treble +12 dB", sampleRate);

        WahEffect heel;
        RunEffect("wah", heel, heelExtreme, "heel at 150 Hz, Q 20, level +18 dB", sampleRate);
    }

    ReportIfClean(failuresBefore);
}
} // namespace

int main()
{
    std::cout << "=== BiquadNyquistTests ===\n";

    TestNamAmpToneStack();
    TestSimpleCab();
    TestBuiltinAmp();
    TestWah();

    if (gFailures > 0)
    {
        std::cout << "\n" << gFailures << " case(s) failed\n";
        return 1;
    }

    std::cout << "\nAll cases passed\n";
    return 0;
}
