/**
 * @file RingModEffectTests.cpp
 * @brief Tests for the ring modulator.
 *
 * These measure what the effect claims to do, rather than only that it makes sound:
 *   - registration, the parameter table, clamping and the factory presets
 *   - a sine carrier turns F into F - Fc and F + Fc, leaving neither F nor Fc behind
 *   - every waveform comes out at the input's RMS, and Mix, Level and Tone do what they say
 *   - the square and triangle carriers alias far less than their naive forms
 *   - the LFO sweeps the carrier by the set number of octaves, and follows the host tempo
 *   - Frequency snaps before the first block and glides after it
 *   - a Waveform change crossfades instead of jumping
 *   - Stereo Spread makes a mono input stereo, and letting it go locks the channels back
 *   - the mono path matches the stereo one sample for sample, in both carrier modes
 *   - Tracking mode puts the carrier on the note played, moved by Interval and Fine, holds it
 *     through silence, and at Interval 0 turns a note into its own harmonics with no DC
 *   - it stays finite at low sample rates, and prints what a block costs
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/RingModEffect.h"
#include "RingModTestSupport.h"

namespace
{
using namespace ring_mod_test;

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kRingMod);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("ring_mod") == guitarfx::EffectGuids::kRingMod, "the ring_mod alias resolves to it");
    // Create falls back to a passthrough for an unknown type, so check what it made.
    Check(registry.Create("ring_mod")->GetType() == "ring_mod", "the registry creates one from the alias");

    if (!info)
    {
        return;
    }

    Check(info->category == "modulation" && info->requiresTempo, "a tempo-aware modulation effect");

    bool ordered = info->parameters.size() == guitarfx::ring_mod::kParamCount;

    for (std::size_t i = 0; ordered && i < info->parameters.size(); ++i)
    {
        ordered = info->parameters[i].id == guitarfx::ring_mod::kParams[i].id;
    }

    Check(ordered, "parameters are declared in table order", std::to_string(info->parameters.size()) + " params");

    // Frequency spans three decades, so its knob travels by ratio, and the catalog says so.
    // Only a tapered parameter carries the field, so an older UI's catalog is unchanged.
    const auto& frequency = info->parameters[guitarfx::ring_mod::kFrequency];
    const auto frequencyJson = guitarfx::controller_detail::SerializeEffectParameter(frequency);
    const auto mixJson =
        guitarfx::controller_detail::SerializeEffectParameter(info->parameters[guitarfx::ring_mod::kMix]);
    Check(frequency.taper == guitarfx::ParamTaper::Log, "Frequency is on a log taper");
    Check(frequencyJson.value("taper", "") == "log", "the catalog sends it as taper: log", frequencyJson.dump());
    Check(!mixJson.contains("taper"), "a linear parameter sends no taper", mixJson.dump());
    Check(std::abs(guitarfx::TaperPositionToValue(frequency.taper, 1.0, 2000.0, 0.5) - std::sqrt(2000.0)) < 1e-9,
          "half its travel is the range's geometric mean, 44.7 Hz");

    RingModEffect ring;
    ring.SetParam("frequency", 99999.0);
    Check(ring.GetParam("frequency") == 2000.0, "Frequency clamps to 2 kHz");
    ring.SetParam("frequency", -5.0);
    Check(ring.GetParam("frequency") == 1.0, "and to 1 Hz");
    ring.SetParam("waveform", 1.4);
    Check(ring.GetParam("waveform") == 1.0, "an enum snaps to a whole choice");
    ring.SetParam("mix", 0.25);
    ring.SetParam("mix", std::nan(""));
    Check(ring.GetParam("mix") == 0.25, "a NaN is ignored");
    ring.SetParam("bpm", 1000.0);
    Check(ring.GetParam("bpm") == 300.0, "bpm is clamped to the tempo range");
}

void TestFactoryPresets()
{
    std::cout << "\nFactory presets" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kRingMod);

    if (!info)
    {
        Check(false, "type info available");
        return;
    }

    Check(!info->presets.empty() && info->presets.front().isDefault &&
              info->presets.front().id == guitarfx::ring_mod::kDefaultPresetId,
          "the first preset is the default");

    // The default preset is the defaults: a new node sounds the same whichever it starts from.
    bool defaultMatches = true;

    for (const auto& [key, value] : info->presets.front().parameters)
    {
        const std::size_t index = guitarfx::FindParamSpec(guitarfx::ring_mod::kParams, key);
        defaultMatches = defaultMatches && index < guitarfx::ring_mod::kParamCount &&
                         guitarfx::ring_mod::kParams[index].defaultValue == value;
    }

    Check(defaultMatches, "the default preset holds the declared defaults");

    bool wellFormed = true;
    bool stable = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        for (const auto& [key, value] : preset.parameters)
        {
            const std::size_t index = guitarfx::FindParamSpec(guitarfx::ring_mod::kParams, key);
            const bool known = index < guitarfx::ring_mod::kParamCount;

            if (!known || value < guitarfx::ring_mod::kParams[index].minValue ||
                value > guitarfx::ring_mod::kParams[index].maxValue)
            {
                wellFormed = false;
                problem = preset.id + "." + key;
            }
        }

        for (const double sampleRate : {44100.0, 48000.0, 96000.0})
        {
            RingModEffect ring;
            ring.Prepare(sampleRate, kBlockSize);

            for (const auto& [key, value] : preset.parameters)
            {
                ring.SetParam(key, value);
            }

            const auto input = Noise(0.5, sampleRate);
            std::vector<float> outL(64), outR(64);

            for (std::size_t start = 0; start + 64 <= input.size() && stable; start += 64)
            {
                // Sweep Frequency across its whole range far faster than a hand would.
                const double t = static_cast<double>(start) / sampleRate;
                ring.SetParam("frequency", 1000.0 + 999.0 * std::sin(2.0 * kPi * 7.0 * t));
                float* inputs[2] = {const_cast<float*>(input.data() + start), const_cast<float*>(input.data() + start)};
                float* outputs[2] = {outL.data(), outR.data()};
                ring.Process(inputs, outputs, 64);

                for (std::size_t i = 0; i < 64; ++i)
                {
                    if (!guitarfx::IsFinite(outL[i]) || !guitarfx::IsFinite(outR[i]) || std::abs(outL[i]) > 8.0f ||
                        std::abs(outR[i]) > 8.0f)
                    {
                        stable = false;
                        problem = preset.id + " at " + Num(sampleRate, 0) + " Hz";
                        break;
                    }
                }
            }
        }
    }

    Check(wellFormed, "every preset names known parameters within their ranges", problem);
    Check(stable, "every preset stays finite and bounded on full-scale noise while sweeping", problem);
}

void TestSidebands()
{
    std::cout << "\nSidebands" << std::endl;
    auto ring = MakeRing({{"frequency", 300.0}, {"mix", 1.0}});
    const auto input = Sines({{1000.0, 0.5}}, 1.25);
    const auto out = RunMono(*ring, input);

    // Each sideband carries half of the input's amplitude, times the sine carrier's sqrt(2).
    const double expected = 0.5 * std::sqrt(2.0) / 2.0;
    const double lower = Amplitude(out.left, 700.0);
    const double upper = Amplitude(out.left, 1300.0);
    Check(std::abs(ToDb(lower / expected)) < 0.1 && std::abs(ToDb(upper / expected)) < 0.1,
          "1 kHz with a 300 Hz carrier becomes 700 Hz and 1300 Hz",
          Num(lower, 4) + " and " + Num(upper, 4) + ", expected " + Num(expected, 4));

    const double input1k = Amplitude(out.left, 1000.0);
    const double carrier = Amplitude(out.left, 300.0);
    Check(ToDb(input1k / 0.5) < -80.0, "the input frequency is gone", Num(ToDb(input1k / 0.5), 1) + " dB");
    Check(ToDb(carrier / 0.5) < -80.0, "the carrier does not leak", Num(ToDb(carrier / 0.5), 1) + " dB");
}

void TestLevelMatch()
{
    std::cout << "\nLevel across waveforms" << std::endl;
    // A 470 Hz carrier keeps every product of these partials, the square's and triangle's odd
    // harmonics included, on a frequency of its own, so their powers simply add.
    const auto input = Sines({{220.0, 0.2}, {330.0, 0.2}, {550.0, 0.2}}, 1.25);
    const std::size_t start = input.size() - static_cast<std::size_t>(kSampleRate);
    const double inputRms = Rms(input, start, static_cast<std::size_t>(kSampleRate));

    for (const char* name : {"Sine", "Triangle", "Square"})
    {
        const double waveform = std::string(name) == "Sine" ? 0.0 : (std::string(name) == "Triangle" ? 1.0 : 2.0);
        auto ring = MakeRing({{"frequency", 470.0}, {"waveform", waveform}});
        const auto out = RunMono(*ring, input);
        const double db = ToDb(Rms(out.left, start, static_cast<std::size_t>(kSampleRate)) / inputRms);
        Check(std::abs(db) < 0.5, std::string(name) + " carrier comes out at the input's RMS", Num(db, 2) + " dB");
    }
}

void TestMixLevelAndTone()
{
    std::cout << "\nMix, Level and Tone" << std::endl;
    const auto input = Sines({{1000.0, 0.5}}, 1.25);

    auto dry = MakeRing({{"frequency", 300.0}, {"mix", 0.0}});
    const auto dryOut = RunMono(*dry, input);
    Check(dryOut.left == input && dryOut.right == input, "Mix 0 passes the input through untouched");

    auto half = MakeRing({{"frequency", 300.0}, {"mix", 0.5}});
    const auto halfOut = RunMono(*half, input);
    const double sideband = 0.5 * std::sqrt(2.0) / 2.0;
    Check(std::abs(ToDb(Amplitude(halfOut.left, 1000.0) / 0.25)) < 0.1 &&
              std::abs(ToDb(Amplitude(halfOut.left, 1300.0) / (0.5 * sideband))) < 0.1,
          "Mix 0.5 is half the dry signal plus half the sidebands");

    auto hot = MakeRing({{"frequency", 300.0}, {"level", 6.0}});
    const auto hotOut = RunMono(*hot, input);
    const double levelDb = ToDb(Amplitude(hotOut.left, 1300.0) / sideband);
    Check(std::abs(levelDb - 6.0) < 0.1, "Level +6 dB raises the wet signal 6 dB", Num(levelDb, 2) + " dB");

    const auto high = Sines({{3000.0, 0.5}}, 1.25);
    auto open = MakeRing({{"frequency", 1000.0}, {"tone", 1.0}});
    auto dark = MakeRing({{"frequency", 1000.0}, {"tone", 0.0}});
    const auto openOut = RunMono(*open, high);
    const auto darkOut = RunMono(*dark, high);
    const double openDb = ToDb(Amplitude(openOut.left, 4000.0) / sideband);
    const double cutDb = ToDb(Amplitude(darkOut.left, 4000.0) / Amplitude(openOut.left, 4000.0));
    Check(std::abs(openDb) < 0.1, "Tone fully open leaves a 4 kHz sideband alone", Num(openDb, 2) + " dB");
    Check(cutDb < -30.0, "Tone at 0 cuts it hard", Num(cutDb, 1) + " dB");
}

/// Power of the harmonics that fold back below 10 kHz, relative to the fundamental's. That is
/// where an alias lands among the sidebands as an audible, inharmonic tone. Two-point BLEP
/// leaves most of what it does not remove within a few kHz of Nyquist instead, where it is
/// far less audible and Tone takes it out.
double AliasBelow10kDb(guitarfx::ring_mod::Waveform waveform, bool bandLimited)
{
    // Not a divisor of 48 kHz, so the aliases fall between the harmonics, on whole hertz.
    constexpr double kFrequency = 2900.0;
    const auto count = static_cast<std::size_t>(kSampleRate);
    std::vector<float> signal(count);
    double phase = 0.0;
    const double dt = kFrequency / kSampleRate;

    for (auto& sample : signal)
    {
        sample = guitarfx::ring_mod::CarrierSample(waveform, phase, bandLimited ? dt : 1.0e-12);
        phase = guitarfx::ring_mod::WrapPhase(phase + dt);
    }

    std::vector<double> seen;
    double alias = 0.0;

    for (int k = 1; k < 400; k += 2)
    {
        const double harmonic = k * kFrequency;
        double folded = std::fmod(harmonic, kSampleRate);
        folded = folded > 0.5 * kSampleRate ? kSampleRate - folded : folded;

        if (harmonic < 0.5 * kSampleRate || folded >= 10000.0 ||
            std::find(seen.begin(), seen.end(), folded) != seen.end())
        {
            continue;
        }

        seen.push_back(folded);
        const double a = Amplitude(signal, folded);
        alias += 0.5 * a * a;
    }

    const double fundamental = Amplitude(signal, kFrequency);
    return 10.0 * std::log10(std::max(alias, 1.0e-30) / (0.5 * fundamental * fundamental));
}

void TestBandLimiting()
{
    std::cout << "\nBand-limited carriers (2.9 kHz at 48 kHz, aliases below 10 kHz)" << std::endl;
    using guitarfx::ring_mod::Waveform;

    const double squareNaive = AliasBelow10kDb(Waveform::Square, false);
    const double square = AliasBelow10kDb(Waveform::Square, true);
    const double triangleNaive = AliasBelow10kDb(Waveform::Triangle, false);
    const double triangle = AliasBelow10kDb(Waveform::Triangle, true);

    Check(square < -50.0 && square < squareNaive - 30.0, "the square's audible aliases are far below a naive one's",
          Num(square, 1) + " dB vs " + Num(squareNaive, 1) + " dB");
    Check(triangle < -70.0 && triangle < triangleNaive - 20.0, "and the triangle's",
          Num(triangle, 1) + " dB vs " + Num(triangleNaive, 1) + " dB");
}

void TestLfo()
{
    std::cout << "\nLFO" << std::endl;
    auto ring = MakeRing({{"frequency", 400.0}, {"lfoDepth", 1.0}, {"lfoRate", 2.0}});
    const std::vector<float> silence(16, 0.0f);
    std::vector<float> outL(16), outR(16);
    double lowest = 1.0e9;
    double highest = 0.0;

    for (int tick = 0; tick < static_cast<int>(kSampleRate) / 16; ++tick)
    {
        float* inputs[2] = {const_cast<float*>(silence.data()), const_cast<float*>(silence.data())};
        float* outputs[2] = {outL.data(), outR.data()};
        ring->Process(inputs, outputs, 16);
        lowest = std::min(lowest, ring->GetParam("carrierFrequency"));
        highest = std::max(highest, ring->GetParam("carrierFrequency"));
    }

    Check(std::abs(lowest - 200.0) < 2.0 && std::abs(highest - 800.0) < 8.0,
          "a one-octave depth sweeps 400 Hz from 200 Hz to 800 Hz", Num(lowest, 1) + " to " + Num(highest, 1) + " Hz");

    ring->SetParam("syncMode", 1.0);
    ring->SetParam("bpm", 120.0);
    ring->SetParam("syncDivision", 4.0);
    Check(std::abs(ring->GetParam("effectiveRate") - 2.0) < 1.0e-9, "tempo sync: 1/4 at 120 bpm is 2 Hz");
    ring->SetParam("syncDivision", 7.0);
    Check(std::abs(ring->GetParam("effectiveRate") - 4.0) < 1.0e-9, "1/8 is 4 Hz");
    ring->SetParam("bpm", 300.0);
    ring->SetParam("syncDivision", 14.0);
    Check(ring->GetParam("effectiveRate") == 20.0, "a division faster than 20 Hz is held there");
}

void TestFrequencySnapAndGlide()
{
    std::cout << "\nFrequency snap and glide" << std::endl;
    auto ring = MakeRing({{"frequency", 1000.0}});
    const std::vector<float> silence(kBlockSize, 0.0f);
    std::vector<float> outL(kBlockSize), outR(kBlockSize);
    float* inputs[2] = {const_cast<float*>(silence.data()), const_cast<float*>(silence.data())};
    float* outputs[2] = {outL.data(), outR.data()};

    ring->Process(inputs, outputs, kBlockSize);
    Check(std::abs(ring->GetParam("carrierFrequency") - 1000.0) < 1.0e-6,
          "a Frequency set before any audio is where the carrier starts");

    ring->SetParam("frequency", 250.0);
    ring->Process(inputs, outputs, kBlockSize);
    const double gliding = ring->GetParam("carrierFrequency");
    Check(gliding > 300.0 && gliding < 900.0, "a later change glides", Num(gliding, 1) + " Hz after 5.3 ms");

    for (int block = 0; block < 60; ++block)
    {
        ring->Process(inputs, outputs, kBlockSize);
    }

    Check(std::abs(ring->GetParam("carrierFrequency") - 250.0) < 1.0e-6, "and settles exactly on the target");
}

void TestWaveformFade()
{
    std::cout << "\nWaveform crossfade" << std::endl;
    const auto input = Sines({{330.0, 0.5}}, 0.5);
    const std::size_t switchAt = 12288; // a block boundary, 256 ms in
    const Params params = {{"frequency", 700.0}};

    auto stays = MakeRing(params);
    auto target = MakeRing(params);
    target->SetParam("waveform", 2.0);
    const auto sine = RunMono(*stays, input);
    const auto square = RunMono(*target, input);

    auto switching = MakeRing(params);
    const std::vector<float> head(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(switchAt));
    const std::vector<float> tail(input.begin() + static_cast<std::ptrdiff_t>(switchAt), input.end());
    RunMono(*switching, head);
    switching->SetParam("waveform", 2.0);
    const auto after = RunMono(*switching, tail);

    Check(std::abs(after.left[0] - sine.left[switchAt]) < 1.0e-6,
          "the first sample after the switch is still the old carrier");

    // Past the 10 ms fade it is the square, whose tone filter state caught up within samples.
    const std::size_t settled = static_cast<std::size_t>(0.02 * kSampleRate);
    float worst = 0.0f;

    for (std::size_t i = settled; i < tail.size(); ++i)
    {
        worst = std::max(worst, std::abs(after.left[i] - square.left[switchAt + i]));
    }

    Check(worst < 1.0e-3f, "20 ms later it is the new carrier", "max difference " + Num(worst, 6));
}

void TestStereoSpread()
{
    std::cout << "\nStereo Spread" << std::endl;
    const auto input = Sines({{330.0, 0.3}, {495.0, 0.2}}, 1.0);
    auto ring = MakeRing({{"frequency", 600.0}, {"lfoDepth", 1.0}, {"lfoRate", 1.5}});

    auto out = RunMono(*ring, input);
    Check(MaxDifference(out.left, out.right) < kChannelTolerance, "without Spread a mono input stays mono",
          "channels within " + Num(MaxDifference(out.left, out.right), 9));
    // Spread can turn a mono input stereo, so the type declares it can widen: a graph keeps what
    // follows a ring modulator in stereo, and turning Spread up is heard at once.
    Check(ring->CanWiden(), "declared able to widen, since Spread does");

    ring->SetParam("spread", 1.0);
    out = RunMono(*ring, input);
    const std::size_t half = out.left.size() / 2;
    double difference = 0.0;

    for (std::size_t i = half; i < out.left.size(); ++i)
    {
        difference = std::max(difference, static_cast<double>(std::abs(out.left[i] - out.right[i])));
    }

    const double balance = ToDb(Rms(out.left, half, half) / Rms(out.right, half, half));
    Check(difference > 0.1, "Spread makes the channels differ", "max difference " + Num(difference, 3));
    Check(std::abs(balance) < 1.0, "at about the same level", Num(balance, 2) + " dB left of right");

    // Spread glides to zero in about 140 ms, then the phase lock takes about 800 ms to converge.
    ring->SetParam("spread", 0.0);
    RunMono(*ring, Sines({{330.0, 0.3}}, 2.0));
    out = RunMono(*ring, Sines({{330.0, 0.3}}, 0.5));
    Check(MaxDifference(out.left, out.right) < kChannelTolerance,
          "letting Spread go locks the right carrier back onto the left");
}

/// A steady note: harmonics 1 to 5, falling off as 1/k.
std::vector<float> Note(double frequency, double seconds)
{
    std::vector<std::pair<double, double>> partials;

    for (int k = 1; k <= 5; ++k)
    {
        partials.emplace_back(k * frequency, 0.25 / k);
    }

    return Sines(partials, seconds);
}

/// Milliseconds of `input`, fed 16 samples at a time, until the carrier is within 20 cents of `hz`.
double MsToReach(RingModEffect& ring, const std::vector<float>& input, double hz)
{
    for (std::size_t start = 0; start + 16 <= input.size(); start += 16)
    {
        Run(ring,
            std::vector<float>(input.begin() + static_cast<std::ptrdiff_t>(start),
                               input.begin() + static_cast<std::ptrdiff_t>(start + 16)),
            std::vector<float>(input.begin() + static_cast<std::ptrdiff_t>(start),
                               input.begin() + static_cast<std::ptrdiff_t>(start + 16)),
            16);

        if (std::abs(1200.0 * std::log2(ring.GetParam("carrierFrequency") / hz)) < 20.0)
        {
            return 1000.0 * static_cast<double>(start + 16) / kSampleRate;
        }
    }

    return 1.0e9;
}

void TestTracking()
{
    std::cout << "\nTracking mode" << std::endl;
    const auto cents = [](double hz, double reference) { return 1200.0 * std::log2(hz / reference); };
    auto ring = MakeRing({{"mode", 1.0}, {"frequency", 500.0}, {"glide", 0.0}});

    RunMono(*ring, std::vector<float>(4800, 0.0f));
    Check(ring->GetParam("carrierFrequency") == 500.0, "before a first note the carrier sits on Frequency");

    const auto out = RunMono(*ring, Note(220.0, 1.25));
    Check(std::abs(cents(ring->GetParam("carrierFrequency"), 220.0)) < 3.0, "then it follows the note played",
          Num(ring->GetParam("carrierFrequency"), 2) + " Hz");

    // At Interval 0 every product lands on a harmonic of the note, and the DC term is filtered.
    const std::size_t second = static_cast<std::size_t>(kSampleRate);
    const double total = Rms(out.left, out.left.size() - second, second);
    double harmonic = 0.0;
    double mean = 0.0;

    for (int k = 1; k <= 12; ++k)
    {
        harmonic += 0.5 * Amplitude(out.left, 220.0 * k) * Amplitude(out.left, 220.0 * k);
    }

    for (std::size_t n = out.left.size() - second; n < out.left.size(); ++n)
    {
        mean += out.left[n] / static_cast<double>(second);
    }

    Check(harmonic / (total * total) > 0.99, "at Interval 0 the output is the note's own harmonics",
          Num(100.0 * harmonic / (total * total), 2) + "% of the power");
    Check(std::abs(mean) < 0.01 * total, "with no DC left in it", "mean " + Num(mean, 6));

    bool intervals = true;
    std::string detail;

    for (const auto& [interval, fine, expected] :
         std::vector<std::tuple<double, double, double>>{{7.0, 0.0, 220.0 * std::exp2(7.0 / 12.0)},
                                                         {-12.0, 0.0, 110.0},
                                                         {12.0, 25.0, 440.0 * std::exp2(0.25 / 12.0)}})
    {
        auto shifted = MakeRing({{"mode", 1.0}, {"interval", interval}, {"fine", fine}, {"glide", 0.0}});
        RunMono(*shifted, Note(220.0, 0.5));
        const double error = cents(shifted->GetParam("carrierFrequency"), expected);
        intervals = intervals && std::abs(error) < 3.0;
        detail += Num(error, 1) + " ";
    }

    Check(intervals, "Interval and Fine move the carrier from the note", "errors " + detail + "cents");

    auto instant = MakeRing({{"mode", 1.0}, {"glide", 0.0}});
    RunMono(*instant, Note(220.0, 0.5));
    const double instantMs = MsToReach(*instant, Note(329.63, 0.3), 329.63);
    auto gliding = MakeRing({{"mode", 1.0}});
    RunMono(*gliding, Note(220.0, 0.5));
    const double glidingMs = MsToReach(*gliding, Note(329.63, 0.3), 329.63);
    Check(instantMs < 50.0, "with Glide 0 a new note is followed within 50 ms", Num(instantMs, 1) + " ms");
    Check(glidingMs > instantMs && glidingMs < 120.0, "the default 15 ms Glide slides there within 120 ms",
          Num(glidingMs, 1) + " ms");

    RunMono(*ring, std::vector<float>(static_cast<std::size_t>(0.5 * kSampleRate), 0.0f));
    Check(std::abs(cents(ring->GetParam("carrierFrequency"), 220.0)) < 3.0, "through silence it holds the note");

    ring->SetParam("mode", 0.0);
    RunMono(*ring, std::vector<float>(static_cast<std::size_t>(0.3 * kSampleRate), 0.0f));
    Check(std::abs(ring->GetParam("carrierFrequency") - 500.0) < 1.0e-6, "back in Fixed it returns to Frequency");
}

void TestLowSampleRate()
{
    std::cout << "\nLow sample rate" << std::endl;
    auto ring = MakeRing({{"frequency", 2000.0}, {"lfoDepth", 3.0}, {"waveform", 2.0}}, 8000.0);
    const auto out = RunMono(*ring, Noise(1.0, 8000.0));
    const bool finite = std::all_of(out.left.begin(), out.left.end(),
                                    [](float v) { return guitarfx::IsFinite(v) && std::abs(v) < 8.0f; });

    Check(ring->GetParam("carrierFrequency") <= 2000.0 + 1.0e-9, "the carrier is held to a quarter of 8 kHz",
          Num(ring->GetParam("carrierFrequency"), 1) + " Hz");
    Check(finite, "and the output stays finite and bounded");
}

void TestNonFiniteRecovery()
{
    std::cout << "\nNon-finite input" << std::endl;

    for (const double mode : {0.0, 1.0})
    {
        auto ring = MakeRing({{"frequency", 440.0}, {"mode", mode}});
        auto input = Sines({{220.0, 0.3}}, 0.5);
        input[1000] = std::nanf("");
        const auto out = RunMono(*ring, input);
        const bool recovered =
            std::all_of(out.left.begin() + 2048, out.left.end(), [](float v) { return guitarfx::IsFinite(v); });

        Check(recovered, std::string("a NaN on the input does not stick") + (mode > 0.0 ? " (Tracking)" : " (Fixed)"));
    }
}

void ReportCost()
{
    std::cout << "\nCost per 64-sample stereo block at 48 kHz (informational)" << std::endl;
    const auto input = Sines({{220.0, 0.3}}, 2.0);

    for (const auto& [label, params] : std::vector<std::pair<std::string, Params>>{
             {"sine, mono-capable", {{"frequency", 440.0}}},
             {"square, LFO, Spread", {{"frequency", 440.0}, {"waveform", 2.0}, {"lfoDepth", 1.0}, {"spread", 1.0}}},
             {"sine, Tracking", {{"mode", 1.0}}}})
    {
        auto ring = MakeRing(params);
        const auto begin = std::chrono::steady_clock::now();
        Run(*ring, input, input, 64);
        const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - begin;
        const double blocks = static_cast<double>(input.size()) / 64.0;
        std::cout << "  " << label << ": " << Num(elapsed.count() / blocks, 2) << " us" << std::endl;
    }
}
} // namespace

int main()
{
    std::cout << "=== RingModEffectTests ===" << std::endl;
    guitarfx::RegisterRingModEffect();

    TestRegistration();
    TestFactoryPresets();
    TestSidebands();
    TestLevelMatch();
    TestMixLevelAndTone();
    TestBandLimiting();
    TestLfo();
    TestFrequencySnapAndGlide();
    TestWaveformFade();
    TestStereoSpread();
    TestTracking();
    TestLowSampleRate();
    TestNonFiniteRecovery();
    ReportCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
