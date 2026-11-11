/**
 * @file StutterEffectTests.cpp
 * @brief Tests for the Stutter effect.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - Chance at zero passes the input untouched; at one it repeats every other step
 *   - a stutter plays the step that was just heard, Repeats times, each a Division long
 *   - Reverse plays the slice backwards
 *   - the grid follows tempo and Division
 *   - nothing clicks at a slice's edges, and it is the same every time
 *   - Mix, the mono path, non-finite input and odd sample rates
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
#include "dsp/effects/StutterEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::StutterEffect;

// An eighth note at 120 bpm: a quarter of a second.
constexpr int kStep = 12000;
constexpr int kEdge = 96; // the 2 ms a slice's ends are faded over

/// A signal whose every sample says where it came from: a slow ramp, so a repeated slice is
/// recognisable by value and a reversed one by its slope.
std::vector<float> Ramp(double seconds)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * kSampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        signal[n] = 0.1f + 0.8f * static_cast<float>(n) / static_cast<float>(signal.size());
    }

    return signal;
}

std::vector<float> Play(const Params& params, const std::vector<float>& input, int block = kBlockSize)
{
    StutterEffect effect;
    Configure(effect, params);
    return Run(effect, input, input, block).left;
}

/// Whether out[at .. at+length) is in[from .. from+length), away from the faded edges.
bool Matches(const std::vector<float>& out, std::size_t at, const std::vector<float>& in, std::size_t from,
             bool reversed = false)
{
    for (int k = kEdge + 8; k < kStep - kEdge - 8; k += 97)
    {
        const std::size_t source =
            reversed ? from + static_cast<std::size_t>(kStep - 1 - k) : from + static_cast<std::size_t>(k);

        if (std::fabs(out[at + static_cast<std::size_t>(k)] - in[source]) > 1.0e-6f)
        {
            return false;
        }
    }

    return true;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kStutter);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("stutter") == guitarfx::EffectGuids::kStutter, "the alias resolves");
    Check(registry.Create("stutter")->GetType() == "stutter", "the registry creates one");

    if (!info)
    {
        return;
    }

    const std::vector<std::string> expected = {"division", "chance", "repeats", "reverse", "mix"};
    bool ordered = info->parameters.size() == expected.size();

    for (std::size_t i = 0; ordered && i < expected.size(); ++i)
    {
        ordered = info->parameters[i].id == expected[i];
    }

    Check(ordered && info->requiresTempo && info->category == "delay",
          "five parameters, in order, and it follows tempo");
    Check(info->presets.size() == 5 && info->presets.front().isDefault, "five factory presets, the first the default");

    StutterEffect probe;
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

    probe.SetParam("repeats", 99.0);
    probe.SetParam("chance", std::numeric_limits<double>::quiet_NaN());
    Check(probe.GetParam("repeats") == 16.0 && std::fabs(probe.GetParam("chance") - 0.25) < 1.0e-6,
          "clamps, and ignores a non-finite value");
}

void TestChance()
{
    std::cout << "\nChance" << std::endl;
    const auto input = Ramp(4.0);
    Check(Play({{"chance", 0.0}}, input) == input, "at zero the input passes untouched");

    // At one: a live step, then that step again, then live, then again.
    const auto out = Play({{"chance", 1.0}, {"repeats", 1.0}}, input);
    bool pattern = true;

    for (int step = 0; step < 12; ++step)
    {
        const auto at = static_cast<std::size_t>(step * kStep);
        const bool live = step % 2 == 0;
        pattern = pattern && Matches(out, at, input, live ? at : at - kStep);
    }

    Check(pattern, "at one every live step is followed by its repeat");
}

void TestRepeats()
{
    std::cout << "\nRepeats" << std::endl;
    const auto input = Ramp(6.0);
    const auto out = Play({{"chance", 1.0}, {"repeats", 3.0}}, input);
    // Step 0 live, steps 1 to 3 repeat it, step 4 live again, steps 5 to 7 repeat step 4.
    bool ok = Matches(out, 0, input, 0);

    for (int repeat = 1; repeat <= 3; ++repeat)
    {
        ok = ok && Matches(out, static_cast<std::size_t>(repeat * kStep), input, 0);
        ok = ok && Matches(out, static_cast<std::size_t>((4 + repeat) * kStep), input, 4 * kStep);
    }

    Check(ok && Matches(out, 4 * kStep, input, 4 * kStep),
          "a stutter is the step just heard, Repeats times, then live again");

    const auto backwards = Play({{"chance", 1.0}, {"repeats", 2.0}, {"reverse", 1.0}}, input);
    Check(Matches(backwards, kStep, input, 0, true) && Matches(backwards, 2 * kStep, input, 0, true),
          "Reverse plays the slice backwards");
    Check(Matches(Play({{"chance", 1.0}, {"repeats", 2.0}, {"reverse", 0.0}}, input), kStep, input, 0),
          "and at zero never does");
}

void TestGrid()
{
    std::cout << "\nGrid" << std::endl;
    const auto input = Ramp(6.0);

    // A sixteenth at 120 bpm is 6000 samples; an eighth at 60 bpm is 24000.
    for (const auto& [label, params, step] : std::vector<std::tuple<std::string, Params, int>>{
             {"a sixteenth at 120 bpm", {{"chance", 1.0}, {"repeats", 1.0}, {"division", 10.0}}, 6000},
             {"an eighth at 60 bpm", {{"chance", 1.0}, {"repeats", 1.0}, {"bpm", 60.0}}, 24000}})
    {
        const auto out = Play(params, input);
        bool ok = true;

        // The middle of the second step is the middle of the first, played again.
        for (int k = step / 4; k < 3 * step / 4; k += 53)
        {
            ok =
                ok && std::fabs(out[static_cast<std::size_t>(step + k)] - input[static_cast<std::size_t>(k)]) < 1.0e-6f;
        }

        Check(ok, "the step is " + label, std::to_string(step) + " samples");
    }
}

void TestNoClicks()
{
    std::cout << "\nEdges" << std::endl;
    const auto tone = Sine(220.0, 0.5, 6.0);
    const double toneStep = 0.5 * 2.0 * kPi * 220.0 / kSampleRate;

    for (const auto& [label, params] : std::vector<std::pair<std::string, Params>>{
             {"forward repeats", {{"chance", 1.0}, {"repeats", 3.0}}},
             {"reversed repeats", {{"chance", 1.0}, {"repeats", 3.0}, {"reverse", 1.0}}},
             {"fast random stutters", {{"chance", 0.5}, {"repeats", 2.0}, {"division", 13.0}, {"reverse", 0.5}}}})
    {
        const auto out = Play(params, tone);
        double worst = 0.0;

        for (std::size_t n = 1; n < out.size(); ++n)
        {
            worst = std::max(worst, static_cast<double>(std::fabs(out[n] - out[n - 1])));
        }

        // A raised-cosine fade over 2 ms adds at most the tone's own slope again.
        Check(worst < 2.5 * toneStep, label + " never jump", Num(worst / toneStep, 2) + "x the tone's own step");
        Check(Peak(out) <= 0.5 + 1.0e-6, label + " are never louder than the input");
    }
}

void TestSteadyAndPaths()
{
    std::cout << "\nRepeatability and paths" << std::endl;
    const auto input = Noise(4.0, 0.4);
    const Params busy = {{"chance", 0.5}, {"repeats", 2.0}, {"reverse", 0.3}, {"division", 10.0}};
    const auto first = Play(busy, input);
    Check(first == Play(busy, input), "the same settings stutter in the same places");
    Check(first == Play(busy, input, 64) && first == Play(busy, input, 1000), "whatever the block size");
    Check(first != input, "and it does stutter");

    StutterEffect stereo;
    StutterEffect mono;
    Configure(stereo, busy);
    Configure(mono, busy);
    const auto out = Run(stereo, input, input);
    Check(out.left == out.right && !stereo.CanWiden() && stereo.SupportsMonoProcessing(), "a mono input stays mono");
    // To rounding, not bit for bit: the compiler may vectorise the one-channel loop differently.
    Check(MaxDifference(RunMono(mono, input), out.left) < 1.0e-6, "the mono path matches the stereo one");

    const auto half = Play({{"chance", 1.0}, {"repeats", 1.0}, {"mix", 0.0}}, input);
    Check(half == input, "Mix at zero passes the input exactly");
}

void TestRobustness()
{
    std::cout << "\nRobustness" << std::endl;
    auto input = Sine(440.0, 0.3, 3.0);
    std::fill(input.begin() + 6000, input.begin() + 6200, std::numeric_limits<float>::quiet_NaN());
    input[9000] = std::numeric_limits<float>::infinity();
    Check(AllFinite(Play({{"chance", 1.0}, {"repeats", 4.0}}, input)), "a non-finite input never reaches the output");

    for (const double sampleRate : {8000.0, 44100.0, 192000.0})
    {
        StutterEffect effect;
        Configure(effect, {{"chance", 1.0}, {"repeats", 16.0}, {"division", 0.0}, {"bpm", 30.0}, {"reverse", 0.5}},
                  sampleRate);
        const auto noise = Noise(12.0, 0.5, sampleRate);
        const auto out = Run(effect, noise, noise).left;
        Check(AllFinite(out) && Peak(out) <= 0.5 + 1.0e-6,
              "a whole note at 30 bpm is held to what it can keep, at " + Num(sampleRate, 0) + " Hz");
    }
}
} // namespace

int main()
{
    std::cout << "=== StutterEffectTests ===" << std::endl;
    guitarfx::RegisterStutterEffect();

    TestRegistration();
    TestChance();
    TestRepeats();
    TestGrid();
    TestNoClicks();
    TestSteadyAndPaths();
    TestRobustness();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
