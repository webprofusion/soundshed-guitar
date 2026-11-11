/**
 * @file AcousticSimEffectTests.cpp
 * @brief Tests for the Acoustic effect.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - Body puts in two resonances where the chosen size has them, and a bigger body sits lower
 *   - Top brings the top end up; either control takes the pickup's low mids out
 *   - the processed response matches the designed one
 *   - it is level-matched: a guitar is about as loud through it as without it, at any setting
 *   - Mix at zero passes the input; the mono path matches the stereo one; channels stay apart
 *   - non-finite input never reaches the output, and it is stable at low sample rates
 */

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "LoFiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/AcousticSimEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::AcousticSimEffect;
namespace sim = guitarfx::acoustic_sim;

/// What the effect does to a sine at `hz`, in dB.
double GainDb(const Params& params, double hz)
{
    AcousticSimEffect effect;
    Configure(effect, params);
    const auto input = Sine(hz, 0.1, 2.0);
    return ToDb(Amplitude(Run(effect, input, input).left, hz) / 0.1);
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kAcousticSim);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("acoustic_sim") == guitarfx::EffectGuids::kAcousticSim, "the alias resolves");
    Check(registry.Create("acoustic_sim")->GetType() == "acoustic_sim", "the registry creates one");

    if (!info)
    {
        return;
    }

    const std::vector<std::string> expected = {"size", "body", "top", "level", "mix"};
    bool ordered = info->parameters.size() == expected.size();

    for (std::size_t i = 0; ordered && i < expected.size(); ++i)
    {
        ordered = info->parameters[i].id == expected[i];
    }

    Check(ordered && info->category == "eq" && info->displayName == "Acoustic", "five parameters, in order");
    Check(info->presets.size() == 5 && info->presets.front().isDefault, "five factory presets, the first the default");

    AcousticSimEffect probe;
    probe.Prepare(kSampleRate, kBlockSize);

    for (const auto& param : info->parameters)
    {
        Check(std::fabs(probe.GetParam(param.id) - param.defaultValue) < 1.0e-6,
              param.id + " starts at its declared default");
    }

    for (const auto& preset : info->presets)
    {
        bool inRange = true;

        for (const auto& [key, value] : preset.parameters)
        {
            const auto* param = registry.FindParameter(info->type, key);
            inRange = inRange && param != nullptr && value >= param->minValue && value <= param->maxValue;
        }

        Check(inRange && !preset.parameters.contains("level"), preset.id + " is within range and leaves Level alone");
    }

    probe.SetParam("size", 7.0);
    probe.SetParam("level", -40.0);
    probe.SetParam("body", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("size") == 2.0 && probe.GetParam("level") == -12.0 &&
              std::fabs(probe.GetParam("body") - 0.6) < 1.0e-6,
          "clamps, and ignores a non-finite value");
}

void TestBody()
{
    std::cout << "\nBody" << std::endl;

    for (int size = 0; size < 3; ++size)
    {
        const auto& box = sim::kBodies[static_cast<std::size_t>(size)];
        // The shape alone, before the level trim (which differs between the two settings):
        // TestMatchesDesign holds what is processed to this design.
        const auto with = sim::Design(size, 1.0, 0.0, kSampleRate);
        const auto without = sim::Design(size, 0.0, 0.0, kSampleRate);
        const double air =
            sim::MagnitudeDb(with, box.airHz, kSampleRate) - sim::MagnitudeDb(without, box.airHz, kSampleRate);
        const double top =
            sim::MagnitudeDb(with, box.topHz, kSampleRate) - sim::MagnitudeDb(without, box.topHz, kSampleRate);
        Check(air > 6.0 && top > 4.0,
              guitarfx::acoustic_sim::BodyLabels()[static_cast<std::size_t>(size)] + " resonates at " +
                  Num(box.airHz, 0) + " and " + Num(box.topHz, 0) + " Hz",
              "+" + Num(air, 1) + " dB, +" + Num(top, 1) + " dB");
    }

    Check(sim::kBodies[0].airHz > sim::kBodies[1].airHz && sim::kBodies[1].airHz > sim::kBodies[2].airHz &&
              sim::kBodies[0].topHz > sim::kBodies[1].topHz && sim::kBodies[1].topHz > sim::kBodies[2].topHz,
          "a bigger body resonates lower");
}

void TestTopAndPickupCut()
{
    std::cout << "\nTop and pickup cut" << std::endl;
    const Params flat = {{"body", 0.0}, {"top", 0.0}};
    const Params bright = {{"body", 0.0}, {"top", 1.0}};
    const Params boxy = {{"body", 1.0}, {"top", 0.0}};

    // With both controls down only the low cut and the air low-pass are left.
    Check(std::fabs(GainDb(flat, 1000.0) - GainDb(flat, 400.0)) < 0.3, "both controls down is flat through the mids");
    const double lift =
        (GainDb(bright, 8000.0) - GainDb(bright, 1000.0)) - (GainDb(flat, 8000.0) - GainDb(flat, 1000.0));
    Check(lift > 10.0, "Top lifts 8 kHz well clear of 1 kHz", "+" + Num(lift, 1) + " dB");
    const double cutByTop =
        (GainDb(bright, 700.0) - GainDb(bright, 150.0)) - (GainDb(flat, 700.0) - GainDb(flat, 150.0));
    const double cutByBody = (GainDb(boxy, 700.0) - GainDb(boxy, 400.0)) - (GainDb(flat, 700.0) - GainDb(flat, 400.0));
    Check(cutByTop < -4.0 && cutByBody < -1.0, "either control takes the pickup's 700 Hz out",
          Num(cutByTop, 1) + " dB by Top, " + Num(cutByBody, 1) + " dB against 400 Hz by Body");
    Check(GainDb(bright, 40.0) - GainDb(bright, 150.0) < -6.0, "nothing is left under the body",
          Num(GainDb(bright, 40.0) - GainDb(bright, 150.0), 1) + " dB at 40 Hz against 150 Hz");
}

void TestMatchesDesign()
{
    std::cout << "\nDesign" << std::endl;
    const auto sections = sim::Design(1, 0.6, 0.6, kSampleRate);
    const double trim = sim::LevelTrimDb(sections, kSampleRate);
    double worst = 0.0;

    for (const double hz : {100.0, 195.0, 400.0, 700.0, 1500.0, 3200.0, 6000.0, 10000.0})
    {
        worst = std::max(worst, std::fabs(GainDb({}, hz) - (sim::MagnitudeDb(sections, hz, kSampleRate) + trim)));
    }

    Check(worst < 0.1, "what it does to a sine is what was designed", "worst " + Num(worst, 3) + " dB");
}

void TestLevelMatched()
{
    std::cout << "\nLevel" << std::endl;
    // A stand-in for a guitar's spectrum: partials falling 6 dB an octave from 110 Hz.
    std::vector<float> guitar(static_cast<std::size_t>(2.0 * kSampleRate), 0.0f);

    for (int partial = 1; partial <= 40; ++partial)
    {
        const double hz = 110.0 * partial;
        const auto tone = Sine(hz, 0.2 / partial, 2.0);

        for (std::size_t n = 0; n < guitar.size(); ++n)
        {
            guitar[n] += tone[n];
        }
    }

    for (const auto& [label, params] :
         std::vector<std::pair<std::string, Params>>{{"the default", {}},
                                                     {"all body", {{"body", 1.0}, {"top", 0.0}}},
                                                     {"all top", {{"body", 0.0}, {"top", 1.0}, {"size", 0.0}}},
                                                     {"everything up", {{"body", 1.0}, {"top", 1.0}, {"size", 2.0}}}})
    {
        AcousticSimEffect effect;
        Configure(effect, params);
        const double change = ToDb(Rms(Run(effect, guitar, guitar).left, 48000) / Rms(guitar, 48000));
        Check(std::fabs(change) < 4.0, label + " leaves a guitar within 4 dB of where it was", Num(change, 2) + " dB");
    }

    AcousticSimEffect louder;
    Configure(louder, {{"level", 6.0}});
    AcousticSimEffect reference;
    Configure(reference, {});
    const auto tone = Sine(1000.0, 0.1, 1.0);
    const double step = ToDb(Rms(Run(louder, tone, tone).left, 24000) / Rms(Run(reference, tone, tone).left, 24000));
    Check(std::fabs(step - 6.0) < 0.05, "Level is in decibels", Num(step, 2) + " dB for +6");
}

void TestPaths()
{
    std::cout << "\nPaths" << std::endl;
    const auto input = Noise(1.0, 0.3);
    AcousticSimEffect dry;
    Configure(dry, {{"mix", 0.0}});
    Check(Run(dry, input, input).left == input, "Mix at zero passes the input exactly");

    AcousticSimEffect stereo;
    AcousticSimEffect mono;
    Configure(stereo, {});
    Configure(mono, {});
    const auto out = Run(stereo, input, input);
    Check(out.left == out.right && !stereo.CanWiden() && stereo.KeepsChannelsSeparate(), "a mono input stays mono");
    Check(MaxDifference(RunMono(mono, input), out.left) < 1.0e-6, "the mono path matches the stereo one");

    AcousticSimEffect apart;
    Configure(apart, {});
    const std::vector<float> silence(input.size(), 0.0f);
    Check(Peak(Run(apart, input, silence).right) == 0.0, "one side never reaches the other");

    // A setting changed while a note rings does not jump.
    AcousticSimEffect moving;
    Configure(moving, {{"body", 0.2}});
    const auto tone = Sine(196.0, 0.3, 1.0);
    std::vector<float> rendered(tone.size());

    for (std::size_t start = 0; start < tone.size(); start += 64)
    {
        if (start == 24000 - 24000 % 64)
        {
            moving.SetParam("body", 0.25);
        }

        float* in[2] = {const_cast<float*>(tone.data() + start), const_cast<float*>(tone.data() + start)};
        std::vector<float> right(64);
        float* outPtr[2] = {rendered.data() + start, right.data()};
        moving.Process(in, outPtr, static_cast<int>(std::min<std::size_t>(64, tone.size() - start)));
    }

    double worst = 0.0;

    for (std::size_t n = 1; n < rendered.size(); ++n)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(rendered[n] - rendered[n - 1])));
    }

    Check(worst < 0.05, "a small move of a control mid-note makes no jump", Num(worst, 4));
}

void TestRobustness()
{
    std::cout << "\nRobustness" << std::endl;
    auto input = Sine(440.0, 0.3, 1.0);
    input[1000] = std::numeric_limits<float>::quiet_NaN();
    input[2000] = std::numeric_limits<float>::infinity();
    input[3000] = 1.0e30f;
    AcousticSimEffect effect;
    Configure(effect, {{"body", 1.0}, {"top", 1.0}});
    const auto out = Run(effect, input, input).left;
    AcousticSimEffect reference;
    Configure(reference, {{"body", 1.0}, {"top", 1.0}});
    const auto clean = Sine(440.0, 0.3, 1.0);
    Check(AllFinite(out), "a non-finite input never reaches the output");
    Check(std::fabs(ToDb(Rms(out, 24000) / Rms(Run(reference, clean, clean).left, 24000))) < 0.5,
          "and it is back to normal half a second later");

    for (const double sampleRate : {8000.0, 22050.0, 192000.0})
    {
        AcousticSimEffect any;
        Configure(any, {{"body", 1.0}, {"top", 1.0}}, sampleRate);
        const auto noise = Noise(1.0, 0.5, sampleRate);
        const auto rendered = Run(any, noise, noise).left;
        Check(AllFinite(rendered) && Peak(rendered) < 8.0, "stable at " + Num(sampleRate, 0) + " Hz",
              "peak " + Num(Peak(rendered), 2));
    }
}
} // namespace

int main()
{
    std::cout << "=== AcousticSimEffectTests ===" << std::endl;
    guitarfx::RegisterAcousticSimEffect();

    TestRegistration();
    TestBody();
    TestTopAndPickupCut();
    TestMatchesDesign();
    TestLevelMatched();
    TestPaths();
    TestRobustness();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
