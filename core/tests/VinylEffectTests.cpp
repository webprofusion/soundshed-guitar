/**
 * @file VinylEffectTests.cpp
 * @brief Tests for the Vinyl effect.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - with everything at zero the output is the input
 *   - Crackle makes pops at about the rate it says, none of them a spike, and none at zero
 *   - Dust is a quiet hush in the upper mids; Wear takes the top end off
 *   - the surface is there with nothing playing, the same every time, and the same on both sides
 *   - non-finite input never reaches the output
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
#include "dsp/effects/VinylEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::VinylEffect;

const Params kClean = {{"crackle", 0.0}, {"dust", 0.0}, {"wear", 0.0}, {"mix", 1.0}};

Params With(Params overrides)
{
    Params params = kClean;
    params.insert(params.end(), overrides.begin(), overrides.end());
    return params;
}

std::vector<float> Surface(const Params& params, double seconds, double sampleRate = kSampleRate)
{
    VinylEffect effect;
    Configure(effect, params, sampleRate);
    const std::vector<float> silence(static_cast<std::size_t>(seconds * sampleRate), 0.0f);
    return Run(effect, silence, silence).left;
}

/// Pops in `x`: runs above `floor` that are at least 2 ms apart.
int CountPops(const std::vector<float>& x, double floor)
{
    int pops = 0;
    std::size_t last = 0;
    bool any = false;

    for (std::size_t n = 0; n < x.size(); ++n)
    {
        if (std::fabs(x[n]) > floor && (!any || n - last > 96))
        {
            ++pops;
            any = true;
        }

        if (std::fabs(x[n]) > floor)
        {
            last = n;
        }
    }

    return pops;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kVinyl);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("vinyl") == guitarfx::EffectGuids::kVinyl, "the alias resolves");
    Check(registry.Create("vinyl")->GetType() == "vinyl", "the registry creates one");

    if (!info)
    {
        return;
    }

    Check(info->parameters.size() == 4 && info->parameters[0].id == "crackle" && info->parameters[1].id == "dust" &&
              info->parameters[2].id == "wear" && info->parameters[3].id == "mix",
          "crackle, dust, wear, mix");
    Check(info->presets.size() == 5 && info->presets.front().isDefault, "five factory presets, the first the default");

    VinylEffect probe;
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

    probe.SetParam("crackle", 4.0);
    probe.SetParam("dust", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("crackle") == 1.0 && probe.GetParam("dust") == static_cast<double>(0.35f),
          "clamps, and ignores a non-finite value");
}

void TestClean()
{
    std::cout << "\nAt zero" << std::endl;
    VinylEffect effect;
    Configure(effect, kClean);
    const auto input = Noise(1.0, 0.5);
    const auto out = Run(effect, input, input);
    Check(out.left == input && out.right == input, "no crackle, dust or wear: the input, sample for sample");
    Check(Peak(Surface(kClean, 2.0)) == 0.0, "and silence stays silent");

    VinylEffect dry;
    Configure(dry, {{"crackle", 1.0}, {"dust", 1.0}, {"wear", 1.0}, {"mix", 0.0}});
    Check(Run(dry, input, input).left == input, "Mix at zero passes the input exactly");
}

void TestCrackle()
{
    std::cout << "\nCrackle" << std::endl;
    const double seconds = 30.0;

    for (const auto& [crackle, rate] : std::vector<std::pair<double, double>>{{1.0, 40.0}, {0.5, 10.0}})
    {
        const auto surface = Surface(With({{"crackle", crackle}}), seconds);
        // Most pops are small (their size is cubed): count everything clear of the floor.
        const double counted = CountPops(surface, 1.0e-5) / seconds;
        Check(counted > 0.5 * rate && counted < 1.2 * rate,
              "Crackle " + Num(crackle, 1) + " pops about " + Num(rate, 0) + " times a second", Num(counted, 1) + "/s");
        const double peakDb = ToDb(Peak(surface));
        Check(peakDb < -24.0 && peakDb > -40.0, "the loudest of them sits between -40 and -24 dBFS",
              Num(peakDb, 1) + " dBFS");
    }

    // A pop is a short ring, not a click: it is over within a few milliseconds.
    const auto surface = Surface(With({{"crackle", 0.3}}), 20.0);
    const auto loudest = static_cast<std::size_t>(
        std::max_element(surface.begin(), surface.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
        surface.begin());
    double after = 0.0;

    for (std::size_t n = loudest + 480; n < std::min(surface.size(), loudest + 600); ++n)
    {
        after = std::max(after, static_cast<double>(std::fabs(surface[n])));
    }

    Check(after < 0.05 * Peak(surface), "a pop has rung out 10 ms later");
}

void TestDustAndWear()
{
    std::cout << "\nDust and wear" << std::endl;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        const double rmsDb = ToDb(Rms(Surface(With({{"dust", 1.0}}), 4.0, sampleRate), 4800));
        Check(rmsDb > -66.0 && rmsDb < -52.0, "full Dust at " + Num(sampleRate, 0) + " Hz sits between -66 and -52 dBFS",
              Num(rmsDb, 1) + " dBFS");
    }

    for (const auto& [wear, cornerHz] : std::vector<std::pair<double, double>>{{1.0, 3500.0}, {0.5, 7483.0}})
    {
        const auto at = Sine(std::round(cornerHz), 0.25, 2.0);
        const auto below = Sine(200.0, 0.25, 2.0);
        VinylEffect a;
        VinylEffect b;
        Configure(a, With({{"wear", wear}}));
        Configure(b, With({{"wear", wear}}));
        const double corner = ToDb(Amplitude(Run(a, at, at).left, std::round(cornerHz)) / 0.25);
        const double low = ToDb(Amplitude(Run(b, below, below).left, 200.0) / 0.25);
        Check(std::fabs(corner + 3.0) < 0.5 && std::fabs(low) < 0.2,
              "Wear " + Num(wear, 1) + ": -3 dB at " + Num(cornerHz, 0) + " Hz, flat below",
              Num(corner, 2) + " dB, " + Num(low, 2) + " dB");
    }
}

void TestSurfaceIsSteady()
{
    std::cout << "\nThe surface" << std::endl;
    const Params worn = {{"crackle", 0.7}, {"dust", 0.6}, {"wear", 0.5}, {"mix", 1.0}};
    Check(Surface(worn, 2.0) == Surface(worn, 2.0), "the same settings give the same record");

    // It does not depend on the signal: the same surface under a note as under silence.
    const auto note = Sine(220.0, 0.3, 2.0);
    VinylEffect effect;
    Configure(effect, With({{"crackle", 0.7}, {"dust", 0.6}}));
    const auto out = Run(effect, note, note);
    std::vector<float> added(note.size());

    for (std::size_t n = 0; n < note.size(); ++n)
    {
        added[n] = out.left[n] - note[n];
    }

    Check(MaxDifference(added, Surface(With({{"crackle", 0.7}, {"dust", 0.6}}), 2.0)) < 1.0e-6,
          "the surface is laid over the signal, whatever the signal is");
    Check(out.left == out.right && !effect.CanWiden() && effect.SupportsMonoProcessing(), "the same on both sides");

    VinylEffect mono;
    Configure(mono, With({{"crackle", 0.7}, {"dust", 0.6}}));
    Check(MaxDifference(RunMono(mono, note), out.left) < 1.0e-6, "the mono path matches the stereo one");
}

void TestNonFinite()
{
    std::cout << "\nNon-finite input" << std::endl;
    auto input = Sine(440.0, 0.3, 1.0);
    input[1000] = std::numeric_limits<float>::quiet_NaN();
    input[2000] = std::numeric_limits<float>::infinity();
    input[3000] = 1.0e30f;
    VinylEffect effect;
    Configure(effect, {{"crackle", 1.0}, {"dust", 1.0}, {"wear", 1.0}, {"mix", 1.0}});
    const auto out = Run(effect, input, input).left;
    Check(AllFinite(out), "never reaches the output");
    Check(std::fabs(ToDb(Rms(out, 24000) / Rms(Sine(440.0, 0.3, 1.0), 24000))) < 1.0,
          "and the effect is back to normal half a second later");
}
} // namespace

int main()
{
    std::cout << "=== VinylEffectTests ===" << std::endl;
    guitarfx::RegisterVinylEffect();

    TestRegistration();
    TestClean();
    TestCrackle();
    TestDustAndWear();
    TestSurfaceIsSteady();
    TestNonFinite();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
