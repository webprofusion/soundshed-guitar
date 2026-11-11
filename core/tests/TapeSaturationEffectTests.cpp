/**
 * @file TapeSaturationEffectTests.cpp
 * @brief Tests for the Tape effect.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - with every stage at rest the output is the input, with no latency
 *   - Wow bends the pitch by a tape's worth, late by no more than 5 ms, and leaves no echo
 *   - Drive adds odd harmonics at about the level it came in, without audible aliasing
 *   - Tone takes the top off; Hiss is quiet and absent at zero; Dropout dips and recovers
 *   - a stage switched in mid-stream fades in rather than clicks
 *   - the mono path matches the stereo one, and a mono input stays mono
 *   - non-finite input never reaches the output, and the effect recovers from it
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "LoFiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/TapeSaturationEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::TapeSaturationEffect;

/// Everything off: the settings each test turns one thing up from.
const Params kAtRest = {{"drive", 0.0},   {"wow", 0.0},  {"flutter", 0.0}, {"hiss", 0.0},
                        {"dropout", 0.0}, {"tone", 1.0}, {"mix", 1.0}};

Params With(Params overrides)
{
    Params params = kAtRest;
    params.insert(params.end(), overrides.begin(), overrides.end());
    return params;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kTapeSaturation);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("tape_saturation") == guitarfx::EffectGuids::kTapeSaturation, "the alias resolves");
    Check(registry.Create("tape_saturation")->GetType() == "tape_saturation", "the registry creates one");

    if (!info)
    {
        return;
    }

    const std::vector<std::string> expected = {"drive", "wow", "flutter", "hiss", "dropout", "tone", "mix"};
    bool ordered = info->parameters.size() == expected.size();

    for (std::size_t i = 0; ordered && i < expected.size(); ++i)
    {
        ordered = info->parameters[i].id == expected[i];
    }

    Check(ordered && info->category == "drive" && info->displayName == "Tape", "seven parameters, in order");
    Check(info->presets.size() == 5 && info->presets.front().isDefault && info->presets.front().id == "clean-reel",
          "five factory presets, the first the default");

    TapeSaturationEffect probe;
    probe.Prepare(kSampleRate, kBlockSize);

    for (const auto& param : info->parameters)
    {
        Check(probe.GetParam(param.id) == static_cast<double>(static_cast<float>(param.defaultValue)),
              param.id + " starts at its declared default");
    }

    for (const auto& preset : info->presets)
    {
        bool inRange = preset.parameters.size() == info->parameters.size();

        for (const auto& [key, value] : preset.parameters)
        {
            inRange = inRange && value >= 0.0 && value <= 1.0;
        }

        Check(inRange, preset.id + " sets every parameter within range");
    }

    probe.SetParam("drive", 7.0);
    probe.SetParam("wow", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("drive") == 1.0 && probe.GetParam("wow") == 0.25, "clamps, and ignores a non-finite value");
}

void TestAtRestIsTransparent()
{
    std::cout << "\nAt rest" << std::endl;
    TapeSaturationEffect effect;
    Configure(effect, kAtRest);
    const auto input = Noise(0.5, 0.5);
    const auto out = Run(effect, input, input);
    Check(out.left == input && out.right == input, "every stage off: the input, sample for sample, no latency");
    Check(effect.GetLatencySamples() == 0 && !effect.CanWiden() && effect.SupportsMonoProcessing(),
          "reports no latency, never widens, has a mono path");

    // Saturation only is still on time: an impulse peaks where it went in.
    TapeSaturationEffect driven;
    Configure(driven, With({{"drive", 0.6}}));
    std::vector<float> impulse(4800, 0.0f);
    impulse[1000] = 0.5f;
    const auto hit = Run(driven, impulse, impulse).left;
    const auto peak = static_cast<std::size_t>(
        std::max_element(hit.begin(), hit.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
        hit.begin());
    // The oversampler's filters are minimum phase: a handful of samples, which it does not report.
    Check(peak >= 1000 && peak <= 1012, "saturation alone is late by under a quarter millisecond",
          std::to_string(peak - 1000) + " samples");
}

/// Frequency of `x` between each pair of rising zero crossings after `from`, lowest and highest.
std::pair<double, double> FrequencyRange(const std::vector<float>& x, std::size_t from)
{
    double lowest = 1.0e9;
    double highest = 0.0;
    double previous = -1.0;
    std::vector<double> crossings;

    for (std::size_t n = from + 1; n < x.size(); ++n)
    {
        if (x[n - 1] < 0.0f && x[n] >= 0.0f)
        {
            crossings.push_back(static_cast<double>(n - 1) + x[n - 1] / (x[n - 1] - x[n]));
        }
    }

    // Over twenty cycles at a time, which is short against wow and averages the rounding out.
    for (std::size_t i = 20; i < crossings.size(); i += 20)
    {
        const double frequency = 20.0 * kSampleRate / (crossings[i] - crossings[i - 20]);
        lowest = std::min(lowest, frequency);
        highest = std::max(highest, frequency);
        previous = frequency;
    }

    (void)previous;
    return {lowest, highest};
}

void TestWowAndFlutter()
{
    std::cout << "\nWow and flutter" << std::endl;
    const auto tone = Sine(1000.0, 0.5, 8.0);

    TapeSaturationEffect wow;
    Configure(wow, With({{"wow", 1.0}}));
    const auto [low, high] = FrequencyRange(Run(wow, tone, tone).left, 4800);
    const double deviation = std::max(1000.0 - low, high - 1000.0) / 1000.0;
    Check(deviation >= 0.005 && deviation <= 0.025, "full Wow bends the pitch by 0.5 to 2.5 percent",
          Num(deviation * 100.0, 2) + " %");

    TapeSaturationEffect steady;
    Configure(steady, kAtRest);
    const auto [steadyLow, steadyHigh] = FrequencyRange(Run(steady, tone, tone).left, 4800);
    Check(steadyHigh - steadyLow < 0.01, "at zero the pitch does not move", Num(steadyHigh - steadyLow, 4) + " Hz");

    for (const auto& [label, params] : std::vector<std::pair<std::string, Params>>{
             {"full Wow", With({{"wow", 1.0}})}, {"full Wow and Flutter", With({{"wow", 1.0}, {"flutter", 1.0}})}})
    {
        TapeSaturationEffect effect;
        Configure(effect, params);
        std::vector<float> impulse(static_cast<std::size_t>(kSampleRate), 0.0f);
        const std::size_t at = 24000;
        impulse[at] = 1.0f;
        const auto out = Run(effect, impulse, impulse).left;
        const auto peak = static_cast<std::size_t>(
            std::max_element(out.begin(), out.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
            out.begin());
        const double lateMs = 1000.0 * static_cast<double>(peak - at) / kSampleRate;
        Check(peak >= at && lateMs <= 5.0, label + ": late by no more than 5 ms", Num(lateMs, 2) + " ms");

        double total = 0.0;
        double tail = 0.0;

        for (std::size_t n = at; n < out.size(); ++n)
        {
            const double energy = static_cast<double>(out[n]) * out[n];
            total += energy;
            tail += (n > at + 480) ? energy : 0.0;
        }

        Check(10.0 * std::log10(std::max(tail / total, 1.0e-20)) < -60.0, label + ": no echo after 10 ms");
    }
}

void TestDrive()
{
    std::cout << "\nDrive" << std::endl;
    const auto note = Sine(220.0, 0.25, 2.0);

    TapeSaturationEffect effect;
    Configure(effect, With({{"drive", 1.0}}));
    const auto out = Run(effect, note, note).left;
    const double third = ToDb(Amplitude(out, 660.0) / Amplitude(out, 220.0));
    const double second = ToDb(Amplitude(out, 440.0) / Amplitude(out, 220.0));
    const double level = ToDb(Rms(out, 48000) / Rms(note, 48000));
    Check(third > -30.0, "full Drive: a third harmonic above -30 dB", Num(third, 1) + " dB");
    Check(second < -60.0, "symmetric: no second harmonic", Num(second, 1) + " dB");
    Check(std::fabs(level) < 3.0, "comes out within 3 dB of how it went in", Num(level, 2) + " dB");

    for (const double amplitude : {0.05, 0.25, 0.7})
    {
        const auto in = Sine(220.0, amplitude, 2.0);
        TapeSaturationEffect half;
        Configure(half, With({{"drive", 0.5}}));
        const double change = ToDb(Rms(Run(half, in, in).left, 48000) / Rms(in, 48000));
        Check(change > -6.0 && change < 6.0, "half Drive at " + Num(ToDb(amplitude), 0) + " dBFS stays within 6 dB",
              Num(change, 2) + " dB");
    }

    // A hot high note: whatever is not a harmonic of it folded back from above Nyquist.
    const auto high = Sine(5000.0, 0.5, 2.0);
    TapeSaturationEffect hot;
    Configure(hot, With({{"drive", 1.0}}));
    const auto driven = Run(hot, high, high).left;
    const double fundamental = Amplitude(driven, 5000.0);
    double worst = 0.0;

    for (double frequency = 100.0; frequency < 20000.0; frequency += 100.0)
    {
        if (std::fmod(frequency, 5000.0) != 0.0)
        {
            worst = std::max(worst, Amplitude(driven, frequency));
        }
    }

    // Odd harmonics above 24 kHz fold onto 1, 3, 9, 11, 19 kHz ...: all on this grid.
    Check(ToDb(worst / fundamental) < -60.0, "aliases of a hot 5 kHz note stay under -60 dB",
          Num(ToDb(worst / fundamental), 1) + " dB");
}

void TestTone()
{
    std::cout << "\nTone" << std::endl;

    for (const auto& [tone, cornerHz] : std::vector<std::pair<double, double>>{{0.0, 2500.0}, {0.5, 6325.0}})
    {
        const auto at = Sine(std::round(cornerHz), 0.25, 2.0);
        const auto below = Sine(200.0, 0.25, 2.0);
        TapeSaturationEffect a;
        TapeSaturationEffect b;
        Configure(a, With({{"tone", tone}}));
        Configure(b, With({{"tone", tone}}));
        const double corner = ToDb(Amplitude(Run(a, at, at).left, std::round(cornerHz)) / 0.25);
        const double low = ToDb(Amplitude(Run(b, below, below).left, 200.0) / 0.25);
        Check(std::fabs(corner + 3.0) < 0.5 && std::fabs(low) < 0.2,
              "Tone " + Num(tone, 1) + ": -3 dB at " + Num(cornerHz, 0) + " Hz, flat below",
              Num(corner, 2) + " dB, " + Num(low, 2) + " dB");
    }
}

void TestHissAndDropout()
{
    std::cout << "\nHiss and dropout" << std::endl;
    const std::vector<float> silence(96000, 0.0f);

    TapeSaturationEffect quiet;
    Configure(quiet, With({{"drive", 1.0}, {"wow", 1.0}, {"flutter", 1.0}, {"dropout", 1.0}, {"tone", 0.3}}));
    Check(Peak(Run(quiet, silence, silence).left) == 0.0, "no Hiss, no input: exact silence");

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        TapeSaturationEffect hissy;
        Configure(hissy, With({{"hiss", 1.0}}), sampleRate);
        const std::vector<float> none(static_cast<std::size_t>(2.0 * sampleRate), 0.0f);
        const double rmsDb = ToDb(Rms(Run(hissy, none, none).left, 4800));
        Check(rmsDb > -70.0 && rmsDb < -50.0,
              "full Hiss at " + Num(sampleRate, 0) + " Hz sits between -70 and -50 dBFS", Num(rmsDb, 1) + " dBFS");
    }

    // Hiss is laid over the tape, after the saturation: Drive does not turn it up.
    TapeSaturationEffect drivenHiss;
    Configure(drivenHiss, With({{"hiss", 1.0}, {"drive", 1.0}}));
    Check(ToDb(Rms(Run(drivenHiss, silence, silence).left, 4800)) < -50.0, "Drive does not amplify the hiss");

    const auto steadyTone = Sine(440.0, 0.5, 10.0);
    TapeSaturationEffect worn;
    Configure(worn, With({{"dropout", 1.0}}));
    const auto out = Run(worn, steadyTone, steadyTone).left;
    double lowest = 1.0e9;
    double highest = 0.0;

    // The level over each 10 ms.
    for (std::size_t start = 0; start + 480 <= out.size(); start += 480)
    {
        double sum = 0.0;

        for (std::size_t n = start; n < start + 480; ++n)
        {
            sum += static_cast<double>(out[n]) * out[n];
        }

        const double gain = std::sqrt(sum / 480.0) / (0.5 / std::sqrt(2.0));
        lowest = std::min(lowest, gain);
        highest = std::max(highest, gain);
    }

    Check(lowest < 0.9 && lowest >= 0.4 && highest > 0.99 && highest < 1.01,
          "full Dropout dips, never below 0.4, and comes back to full", Num(lowest, 2) + " .. " + Num(highest, 2));
}

void TestStagesFadeIn()
{
    std::cout << "\nSwitching a stage in" << std::endl;
    const auto tone = Sine(220.0, 0.5, 1.0);
    const double inputStep = 0.5 * 2.0 * kPi * 220.0 / kSampleRate;

    for (const auto& [label, key, value] : std::vector<std::tuple<std::string, std::string, double>>{
             {"Drive", "drive", 1.0}, {"Wow", "wow", 1.0}, {"Tone", "tone", 0.0}})
    {
        TapeSaturationEffect effect;
        Configure(effect, kAtRest);
        std::vector<float> out(tone.size());
        double worst = 0.0;

        for (std::size_t start = 0; start < tone.size(); start += 64)
        {
            if (start == 24000 - 24000 % 64)
            {
                effect.SetParam(key, value);
            }

            float* in[2] = {const_cast<float*>(tone.data() + start), const_cast<float*>(tone.data() + start)};
            std::vector<float> right(64);
            float* outPtr[2] = {out.data() + start, right.data()};
            effect.Process(in, outPtr, static_cast<int>(std::min<std::size_t>(64, tone.size() - start)));
        }

        for (std::size_t n = 1; n < out.size(); ++n)
        {
            worst = std::max(worst, static_cast<double>(std::fabs(out[n] - out[n - 1])));
        }

        Check(worst < 3.0 * inputStep, label + " switched in mid-note does not click",
              Num(worst / inputStep, 2) + "x the input's own step");
    }
}

void TestMonoMatchesStereo()
{
    std::cout << "\nMono path" << std::endl;
    const auto input = Noise(1.0, 0.3);
    const Params busy = {{"drive", 0.7},   {"wow", 0.8},  {"flutter", 0.6}, {"hiss", 0.5},
                         {"dropout", 0.7}, {"tone", 0.4}, {"mix", 0.8}};
    TapeSaturationEffect stereo;
    TapeSaturationEffect mono;
    Configure(stereo, busy);
    Configure(mono, busy);
    const auto out = Run(stereo, input, input);
    Check(out.left == out.right, "a mono input stays mono");
    Check(MaxDifference(RunMono(mono, input), out.left) < 1.0e-5, "the mono path matches the stereo one");

    // In place, as the executor may call it.
    TapeSaturationEffect inPlace;
    Configure(inPlace, busy);
    std::vector<float> buffer = input;

    for (std::size_t start = 0; start < buffer.size(); start += 256)
    {
        float* io[2] = {buffer.data() + start, buffer.data() + start};
        inPlace.Process(io, io, static_cast<int>(std::min<std::size_t>(256, buffer.size() - start)));
    }

    Check(MaxDifference(buffer, out.left) < 1.0e-5, "processing in place gives the same result");
}

void TestNonFiniteRecovery()
{
    std::cout << "\nNon-finite input" << std::endl;
    const Params busy = {{"drive", 0.7},   {"wow", 0.8},  {"flutter", 0.6}, {"hiss", 0.3},
                         {"dropout", 0.5}, {"tone", 0.4}, {"mix", 1.0}};

    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.0e30f})
    {
        auto input = Sine(440.0, 0.3, 2.0);
        std::fill(input.begin() + 1000, input.begin() + 1512, bad);

        TapeSaturationEffect effect;
        TapeSaturationEffect reference;
        Configure(effect, busy);
        Configure(reference, busy);
        const auto out = Run(effect, input, input).left;
        const auto clean = Sine(440.0, 0.3, 2.0);
        const auto expected = Run(reference, clean, clean).left;
        const double after = ToDb(Rms(out, 48000) / Rms(expected, 48000));
        Check(AllFinite(out), "the output stays finite");
        Check(std::fabs(after) < 1.0, "and is back to normal within a second", Num(after, 2) + " dB");
    }
}

void TestSampleRates()
{
    std::cout << "\nSample rates" << std::endl;

    for (const double sampleRate : {8000.0, 44100.0, 96000.0, 192000.0})
    {
        TapeSaturationEffect effect;
        Configure(effect,
                  {{"drive", 1.0},
                   {"wow", 1.0},
                   {"flutter", 1.0},
                   {"hiss", 1.0},
                   {"dropout", 1.0},
                   {"tone", 0.0},
                   {"mix", 1.0}},
                  sampleRate);
        const auto input = Noise(1.0, 0.9, sampleRate);
        const auto out = Run(effect, input, input).left;
        Check(AllFinite(out) && Peak(out) < 2.0, "everything at full is bounded at " + Num(sampleRate, 0) + " Hz",
              "peak " + Num(Peak(out), 2));
    }
}
} // namespace

int main()
{
    std::cout << "=== TapeSaturationEffectTests ===" << std::endl;
    guitarfx::RegisterTapeSaturationEffect();

    TestRegistration();
    TestAtRestIsTransparent();
    TestWowAndFlutter();
    TestDrive();
    TestTone();
    TestHissAndDropout();
    TestStagesFadeIn();
    TestMonoMatchesStereo();
    TestNonFiniteRecovery();
    TestSampleRates();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
