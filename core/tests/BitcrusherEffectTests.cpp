/**
 * @file BitcrusherEffectTests.cpp
 * @brief Tests for the bitcrusher.
 *
 *   - registration, the parameter table, clamping and the factory presets
 *   - at 16 bits and no downsampling it is transparent
 *   - Bits leaves exactly 2^bits levels, and Downsample holds each sample that long
 *   - Mix at zero passes the input untouched
 *   - the output is never louder than the input, at any setting and sample rate
 *   - silence stays silent, and the mono path matches the stereo one
 *   - non-finite input never reaches the output
 */

#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "LoFiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BitcrusherEffect.h"

namespace
{
using namespace lofi_test;
using guitarfx::BitcrusherEffect;

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kBitcrusher);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("bitcrusher") == guitarfx::EffectGuids::kBitcrusher, "the bitcrusher alias resolves");
    Check(registry.Create("bitcrusher")->GetType() == "bitcrusher", "the registry creates one from the alias");

    if (!info)
    {
        return;
    }

    Check(info->category == "drive", "listed with the drives");
    Check(info->parameters.size() == 3 && info->parameters[0].id == "bits" && info->parameters[1].id == "downsample" &&
              info->parameters[2].id == "mix",
          "bits, downsample, mix");
    Check(info->presets.size() == 4 && info->presets.front().isDefault && info->presets.front().id == "subtle-grit",
          "four factory presets, the first the default");

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

    BitcrusherEffect effect;
    Configure(effect, {{"bits", 99.0}, {"downsample", 0.0}, {"mix", 2.0}});
    Check(effect.GetParam("bits") == 16.0 && effect.GetParam("downsample") == 1.0 && effect.GetParam("mix") == 1.0,
          "values clamp to their ranges");
    effect.SetParam("bits", std::numeric_limits<double>::quiet_NaN());
    Check(effect.GetParam("bits") == 16.0, "a non-finite value is ignored");
}

void TestTransparency()
{
    std::cout << "\nTransparency" << std::endl;
    const auto input = Sine(440.0, 0.5, 1.0);

    BitcrusherEffect full;
    Configure(full, {{"bits", 16.0}, {"downsample", 1.0}});
    const auto out = Run(full, input, input);
    const double errorDb = ToDb(MaxDifference(out.left, input));
    Check(errorDb < -90.0, "16 bits, no downsampling: the input, to the last bit", Num(errorDb, 1) + " dB");

    BitcrusherEffect dry;
    Configure(dry, {{"bits", 2.0}, {"downsample", 30.0}, {"mix", 0.0}});
    const auto dryOut = Run(dry, input, input);
    Check(dryOut.left == input && dryOut.right == input, "Mix at zero passes the input exactly");
}

void TestQuantiser()
{
    std::cout << "\nBits" << std::endl;
    std::vector<float> ramp(20001);

    for (std::size_t n = 0; n < ramp.size(); ++n)
    {
        ramp[n] = -1.0f + 2.0f * static_cast<float>(n) / static_cast<float>(ramp.size() - 1);
    }

    for (const int bits : {1, 2, 4, 8})
    {
        BitcrusherEffect effect;
        Configure(effect, {{"bits", static_cast<double>(bits)}, {"downsample", 1.0}});
        const auto out = Run(effect, ramp, ramp);
        const std::set<float> levels(out.left.begin(), out.left.end());
        Check(levels.size() == static_cast<std::size_t>(1 << bits),
              std::to_string(bits) + " bits leaves " + std::to_string(1 << bits) + " levels",
              std::to_string(levels.size()));
    }

    BitcrusherEffect effect;
    Configure(effect, {{"bits", 3.0}, {"downsample", 1.0}});
    const std::vector<float> silence(4800, 0.0f);
    const auto quiet = Run(effect, silence, silence);
    Check(Peak(quiet.left) == 0.0, "silence stays silent");
}

void TestDownsample()
{
    std::cout << "\nDownsample" << std::endl;
    const auto input = Noise(0.5, 0.5);

    for (const int hold : {1, 8, 50})
    {
        BitcrusherEffect effect;
        Configure(effect, {{"bits", 16.0}, {"downsample", static_cast<double>(hold)}});
        const auto out = Run(effect, input, input);
        bool held = true;

        for (std::size_t n = 0; n < out.left.size(); ++n)
        {
            const std::size_t anchor = n - n % static_cast<std::size_t>(hold);
            held = held && out.left[n] == out.left[anchor];
        }

        std::size_t changes = 0;

        for (std::size_t n = 1; n < out.left.size(); ++n)
        {
            changes += out.left[n] != out.left[n - 1] ? 1 : 0;
        }

        const std::size_t expected = (out.left.size() - 1) / static_cast<std::size_t>(hold);
        Check(held && changes + 2 >= expected && changes <= expected,
              "a hold of " + std::to_string(hold) + " changes once every " + std::to_string(hold) + " samples",
              std::to_string(changes) + " changes");
    }

    // A fractional hold averages out to its length.
    BitcrusherEffect fractional;
    Configure(fractional, {{"bits", 16.0}, {"downsample", 2.5}});
    const auto out = Run(fractional, input, input);
    std::size_t changes = 0;

    for (std::size_t n = 1; n < out.left.size(); ++n)
    {
        changes += out.left[n] != out.left[n - 1] ? 1 : 0;
    }

    const double average = static_cast<double>(out.left.size()) / static_cast<double>(changes);
    Check(std::fabs(average - 2.5) < 0.05, "a hold of 2.5 averages 2.5 samples", Num(average));
}

void TestNeverLouder()
{
    std::cout << "\nLevel" << std::endl;

    for (const double sampleRate : {44100.0, 192000.0})
    {
        for (const auto& [bits, hold] : std::vector<std::pair<double, double>>{{1.0, 50.0}, {1.5, 1.0}, {4.0, 7.3}})
        {
            const auto input = Noise(0.5, 0.5, sampleRate);
            BitcrusherEffect effect;
            Configure(effect, {{"bits", bits}, {"downsample", hold}}, sampleRate);
            const auto out = Run(effect, input, input);
            Check(AllFinite(out.left) && Peak(out.left) <= Peak(input),
                  Num(bits, 1) + " bits, hold " + Num(hold, 1) + " at " + Num(sampleRate, 0) + " Hz: no louder than in",
                  Num(ToDb(Peak(out.left) / Peak(input)), 2) + " dB");
        }
    }

    // Over full scale is clipped to it rather than wrapped.
    BitcrusherEffect effect;
    Configure(effect, {{"bits", 8.0}, {"downsample", 1.0}});
    const std::vector<float> hot(480, 3.0f);
    Check(Peak(Run(effect, hot, hot).left) <= 1.0, "an over-range input clips to full scale");
}

void TestMonoMatchesStereo()
{
    std::cout << "\nMono path" << std::endl;
    const auto input = Noise(0.5, 0.4);
    BitcrusherEffect stereo;
    BitcrusherEffect mono;
    Configure(stereo, {{"bits", 6.0}, {"downsample", 3.7}, {"mix", 0.8}});
    Configure(mono, {{"bits", 6.0}, {"downsample", 3.7}, {"mix", 0.8}});
    Check(mono.SupportsMonoProcessing() && !mono.CanWiden() && mono.KeepsChannelsSeparate(),
          "mono-capable, never widens, keeps its channels apart");
    const auto out = Run(stereo, input, input);
    Check(RunMono(mono, input) == out.left && out.left == out.right, "mono matches each stereo channel exactly");
}

void TestNonFinite()
{
    std::cout << "\nNon-finite input" << std::endl;
    auto input = Sine(440.0, 0.5, 0.2);
    input[100] = std::numeric_limits<float>::quiet_NaN();
    input[200] = std::numeric_limits<float>::infinity();
    input[300] = -std::numeric_limits<float>::infinity();
    input[400] = 1.0e30f;

    BitcrusherEffect effect;
    Configure(effect, {{"bits", 8.0}, {"downsample", 4.0}, {"mix", 0.5}});
    const auto out = Run(effect, input, input);
    Check(AllFinite(out.left) && AllFinite(out.right), "never reaches the output");
}
} // namespace

int main()
{
    std::cout << "=== BitcrusherEffectTests ===" << std::endl;
    guitarfx::RegisterBitcrusherEffect();

    TestRegistration();
    TestTransparency();
    TestQuantiser();
    TestDownsample();
    TestNeverLouder();
    TestMonoMatchesStereo();
    TestNonFinite();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
