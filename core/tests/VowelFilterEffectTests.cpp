/**
 * @file VowelFilterEffectTests.cpp
 * @brief Tests for the Vowel Filter.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - at each vowel the response peaks at that vowel's formants and dips between them
 *   - Resonance sharpens the formants; a guitar comes out about as loud as it went in
 *   - LFO sweeps the formants at Rate; Envelope opens them further the harder the note
 *   - a move of the Vowel control mid-note does not click
 *   - Mix, the mono path, non-finite input and odd sample rates
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoFiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/VowelFilterEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::VowelFilterEffect;
namespace vowel = guitarfx::vowel_filter;

const Params kManual = {{"mode", 0.0}, {"vowel", 0.5}, {"resonance", 0.5}, {"mix", 1.0}};

Params With(Params overrides)
{
    Params params = kManual;
    params.insert(params.end(), overrides.begin(), overrides.end());
    return params;
}

/// What the effect does to a sine at `hz`, in dB.
double GainDb(const Params& params, double hz)
{
    VowelFilterEffect effect;
    Configure(effect, params);
    const auto input = Sine(std::round(hz), 0.1, 2.0);
    return ToDb(Amplitude(Run(effect, input, input).left, std::round(hz)) / 0.1);
}

/// Amplitude at `hz` over a tenth of a second starting at `from` seconds.
double Slice(const std::vector<float>& x, double hz, double from)
{
    const auto start = static_cast<std::size_t>(from * kSampleRate);
    const std::size_t count = 4800;
    double re = 0.0;
    double im = 0.0;

    for (std::size_t n = 0; n < count && start + n < x.size(); ++n)
    {
        const double w = 2.0 * kPi * hz * static_cast<double>(n) / kSampleRate;
        re += x[start + n] * std::cos(w);
        im -= x[start + n] * std::sin(w);
    }

    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kVowelFilter);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("vowel_filter") == guitarfx::EffectGuids::kVowelFilter, "the alias resolves");
    Check(registry.Create("vowel_filter")->GetType() == "vowel_filter", "the registry creates one");

    if (!info)
    {
        return;
    }

    const std::vector<std::string> expected = {"mode", "vowel", "depth", "rate", "sensitivity", "resonance", "mix"};
    bool ordered = info->parameters.size() == expected.size();

    for (std::size_t i = 0; ordered && i < expected.size(); ++i)
    {
        ordered = info->parameters[i].id == expected[i];
    }

    Check(ordered && info->category == "modulation", "seven parameters, in order");
    Check(info->presets.size() == 5 && info->presets.front().isDefault, "five factory presets, the first the default");

    VowelFilterEffect probe;
    probe.Prepare(kSampleRate, kBlockSize);

    for (const auto& param : info->parameters)
    {
        Check(std::fabs(probe.GetParam(param.id) - param.defaultValue) < 1.0e-6,
              param.id + " starts at its declared default");
    }

    for (const auto& preset : info->presets)
    {
        bool inRange = preset.parameters.size() == info->parameters.size();

        for (const auto& param : info->parameters)
        {
            const auto it = preset.parameters.find(param.id);
            inRange = inRange && it != preset.parameters.end() && it->second >= param.minValue &&
                      it->second <= param.maxValue;
        }

        Check(inRange, preset.id + " sets every parameter within range");
    }

    probe.SetParam("mode", 9.0);
    probe.SetParam("rate", 99.0);
    probe.SetParam("vowel", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("mode") == 2.0 && probe.GetParam("rate") == 10.0 &&
              std::fabs(probe.GetParam("vowel") - 0.15) < 1.0e-6,
          "clamps, and ignores a non-finite value");
}

void TestFormants()
{
    std::cout << "\nFormants" << std::endl;
    const char* names[] = {"U", "O", "A", "E", "I"};

    for (int v = 0; v < vowel::kVowels; ++v)
    {
        const Params params = With({{"vowel", v / 4.0}, {"resonance", 0.7}});
        const double f1 = vowel::kFormantHz[v][0];
        const double f2 = vowel::kFormantHz[v][1];
        const double first = GainDb(params, f1);
        const double second = GainDb(params, f2);
        // Between the two formants, by ratio, where neither reaches. O's are too close together
        // to leave a gap, so it is checked below them instead.
        const double gapHz = (f2 / f1 > 1.8) ? std::sqrt(f1 * f2) : 0.5 * f1;
        const double gap = GainDb(params, gapHz);
        Check(first - gap > 6.0 && second - gap > 2.0,
              std::string(names[v]) + " peaks at " + Num(f1, 0) + " and " + Num(f2, 0) + " Hz",
              Num(first, 1) + " and " + Num(second, 1) + " dB against " + Num(gap, 1) + " at " + Num(gapHz, 0) + " Hz");
    }

    Check(vowel::FormantHz(0.0, 1) < vowel::FormantHz(0.5, 1) && vowel::FormantHz(0.5, 1) < vowel::FormantHz(0.75, 1) &&
              vowel::FormantHz(0.75, 1) < vowel::FormantHz(1.0, 1),
          "the second formant climbs from A through E to I, so a sweep opens without doubling back");

    const double soft = GainDb(With({{"resonance", 0.0}}), 730.0) - GainDb(With({{"resonance", 0.0}}), 2000.0);
    const double sharp = GainDb(With({{"resonance", 1.0}}), 730.0) - GainDb(With({{"resonance", 1.0}}), 2000.0);
    Check(sharp > soft + 6.0, "Resonance sharpens the formants", Num(soft, 1) + " dB to " + Num(sharp, 1) + " dB");
}

void TestLevel()
{
    std::cout << "\nLevel" << std::endl;
    // A stand-in for a guitar: partials falling 6 dB an octave from 110 Hz.
    std::vector<float> guitar(static_cast<std::size_t>(2.0 * kSampleRate), 0.0f);

    for (int partial = 1; partial <= 40; ++partial)
    {
        const auto tone = Sine(110.0 * partial, 0.2 / partial, 2.0);

        for (std::size_t n = 0; n < guitar.size(); ++n)
        {
            guitar[n] += tone[n];
        }
    }

    for (const double position : {0.0, 0.5, 1.0})
    {
        VowelFilterEffect effect;
        Configure(effect, With({{"vowel", position}}));
        const auto out = Run(effect, guitar, guitar).left;
        const double change = ToDb(Rms(out, 48000) / Rms(guitar, 48000));
        Check(std::fabs(change) < 9.0 && Peak(out) < 1.0,
              "a guitar at Vowel " + Num(position, 1) + " stays within 9 dB of its level and under full scale",
              Num(change, 1) + " dB, peak " + Num(Peak(out), 2));
    }
}

void TestMovement()
{
    std::cout << "\nMovement" << std::endl;
    // Two partials: one at U's second formant, one at I's. Which is louder says where the mouth is.
    std::vector<float> probe = Sine(870.0, 0.1, 6.0);
    const auto high = Sine(2290.0, 0.1, 6.0);

    for (std::size_t n = 0; n < probe.size(); ++n)
    {
        probe[n] += high[n];
    }

    VowelFilterEffect lfo;
    Configure(lfo, {{"mode", 1.0}, {"vowel", 0.5}, {"depth", 1.0}, {"rate", 0.5}, {"resonance", 0.7}});
    const auto swept = Run(lfo, probe, probe).left;
    double lowest = 1.0e9;
    double highest = -1.0e9;

    for (double t = 1.0; t < 5.0; t += 0.1)
    {
        const double tilt = ToDb(Slice(swept, 2290.0, t) / Slice(swept, 870.0, t));
        lowest = std::min(lowest, tilt);
        highest = std::max(highest, tilt);
    }

    Check(highest - lowest > 20.0, "LFO sweeps the mouth from one end to the other",
          Num(lowest, 1) + " to " + Num(highest, 1) + " dB of tilt");

    // At 0.5 Hz the mouth is at the I end a quarter cycle in (0.5 s) and at the U end at 1.5 s.
    const double open = ToDb(Slice(swept, 2290.0, 2.45) / Slice(swept, 870.0, 2.45));
    const double closed = ToDb(Slice(swept, 2290.0, 3.45) / Slice(swept, 870.0, 3.45));
    Check(open > closed + 15.0, "and does so at Rate", Num(open, 1) + " dB open, " + Num(closed, 1) + " dB closed");

    // Envelope: the same note, quiet and loud.
    const auto tilt = [&](double scale) {
        VowelFilterEffect effect;
        Configure(effect, {{"mode", 2.0}, {"vowel", 0.0}, {"depth", 1.0}, {"sensitivity", 0.5}, {"resonance", 0.7}});
        std::vector<float> scaled(probe.size());
        std::transform(probe.begin(), probe.end(), scaled.begin(),
                       [scale](float v) { return static_cast<float>(v * scale); });
        const auto out = Run(effect, scaled, scaled).left;
        return ToDb(Slice(out, 2290.0, 3.0) / Slice(out, 870.0, 3.0));
    };
    const double quiet = tilt(0.02);
    const double loud = tilt(1.0);
    Check(loud > quiet + 15.0, "Envelope opens it further the harder the note",
          Num(quiet, 1) + " dB quiet, " + Num(loud, 1) + " dB loud");

    // A turn of the knob mid-note.
    const auto tone = Sine(196.0, 0.3, 1.0);
    VowelFilterEffect moving;
    Configure(moving, With({{"vowel", 0.2}}));
    std::vector<float> rendered(tone.size());

    for (std::size_t start = 0; start < tone.size(); start += 64)
    {
        if (start == 24000 - 24000 % 64)
        {
            moving.SetParam("vowel", 0.9);
        }

        float* in[2] = {const_cast<float*>(tone.data() + start), const_cast<float*>(tone.data() + start)};
        std::vector<float> right(64);
        float* outPtr[2] = {rendered.data() + start, right.data()};
        moving.Process(in, outPtr, static_cast<int>(std::min<std::size_t>(64, tone.size() - start)));
    }

    double worst = 0.0;

    for (std::size_t n = 24000; n < rendered.size(); ++n)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(rendered[n] - rendered[n - 1])));
    }

    Check(worst < 0.05, "the Vowel control turned mid-note makes no jump", Num(worst, 4));
}

void TestPathsAndRobustness()
{
    std::cout << "\nPaths and robustness" << std::endl;
    const auto input = Noise(1.0, 0.3);
    VowelFilterEffect dry;
    Configure(dry, With({{"mix", 0.0}}));
    Check(Run(dry, input, input).left == input, "Mix at zero passes the input exactly");

    const Params busy = {{"mode", 1.0}, {"rate", 3.0}, {"depth", 1.0}, {"resonance", 0.8}};
    VowelFilterEffect stereo;
    VowelFilterEffect mono;
    Configure(stereo, busy);
    Configure(mono, busy);
    const auto out = Run(stereo, input, input);
    Check(out.left == out.right && !stereo.CanWiden() && stereo.SupportsMonoProcessing(), "a mono input stays mono");
    Check(MaxDifference(RunMono(mono, input), out.left) < 1.0e-6, "the mono path matches the stereo one");

    const std::vector<float> silence(48000, 0.0f);
    VowelFilterEffect quiet;
    Configure(quiet, busy);
    Check(Peak(Run(quiet, silence, silence).left) == 0.0, "silence stays silent");

    auto bad = Sine(440.0, 0.3, 1.0);
    bad[1000] = std::numeric_limits<float>::quiet_NaN();
    bad[2000] = std::numeric_limits<float>::infinity();
    bad[3000] = 1.0e30f;
    VowelFilterEffect effect;
    VowelFilterEffect reference;
    Configure(effect, kManual);
    Configure(reference, kManual);
    const auto recovered = Run(effect, bad, bad).left;
    const auto clean = Sine(440.0, 0.3, 1.0);
    Check(AllFinite(recovered), "a non-finite input never reaches the output");
    Check(std::fabs(ToDb(Rms(recovered, 24000) / Rms(Run(reference, clean, clean).left, 24000))) < 0.5,
          "and it is back to normal half a second later");

    for (const double sampleRate : {8000.0, 22050.0, 192000.0})
    {
        VowelFilterEffect any;
        Configure(any, {{"mode", 1.0}, {"rate", 10.0}, {"depth", 1.0}, {"resonance", 1.0}}, sampleRate);
        const auto noise = Noise(1.0, 0.5, sampleRate);
        const auto rendered = Run(any, noise, noise).left;
        Check(AllFinite(rendered) && Peak(rendered) < 8.0, "stable at " + Num(sampleRate, 0) + " Hz",
              "peak " + Num(Peak(rendered), 2));
    }
}
} // namespace

int main()
{
    std::cout << "=== VowelFilterEffectTests ===" << std::endl;
    guitarfx::RegisterVowelFilterEffect();

    TestRegistration();
    TestFormants();
    TestLevel();
    TestMovement();
    TestPathsAndRobustness();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
