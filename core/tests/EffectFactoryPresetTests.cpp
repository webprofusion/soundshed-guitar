/**
 * @file EffectFactoryPresetTests.cpp
 * @brief Factory preset lists built with FactoryPresetSupport.h hold to its rules.
 *
 * For each effect listed below:
 *   - it ships factory presets, each a named factory preset with its own id
 *   - exactly one is the default, it comes first, and it is the parameter defaults, so a node
 *     added fresh and one set to the default preset agree
 *   - every preset sets every control the effect declares except the player's own, in range,
 *     on an enum's steps, and nothing else, so choosing one never leaves another's settings
 *     behind; a left-out control a preset may still name (a synced delay's division) only
 *     when the condition that makes it the preset's holds, and then it must
 *   - on the demo DI guitar, every preset stays finite, never runs away, and sits within reach
 *     of the default's level
 */

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DemoAudio.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/AutoArpEffect.h"
#include "dsp/effects/BuiltinAmpEffect.h"
#include "dsp/effects/ChorusEffect.h"
#include "dsp/effects/CompressorEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/FlangerEffect.h"
#include "dsp/effects/HarmonizerEffect.h"
#include "dsp/effects/NoiseGateEffect.h"
#include "dsp/effects/ParametricEQEffect.h"
#include "dsp/effects/PitchShiftEffect.h"
#include "dsp/effects/SimpleCabEffect.h"
#include "dsp/effects/SynthSawEffect.h"

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace
{
namespace EffectGuids = guitarfx::EffectGuids;

constexpr int kBlockSize = 128;

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

/// A left-out control a preset may name, and then must: `key` when `whenKey` is `whenValue`.
struct Conditional
{
    std::string key;
    std::string whenKey;
    double whenValue = 0.0;
};

struct Effect
{
    const char* name;
    const char* type;
    std::set<std::string> leaveOut;
    std::vector<Conditional> conditional;
    /// How far from the default's level, in dB, a preset may sit on the DI.
    double levelRangeDb = 6.0;
};

/// A tempo-synced preset names its division; one with Sync off leaves the player's alone.
const Conditional kDivisionWhenSynced{"syncDivision", "syncMode", 1.0};

const std::vector<Effect>& Effects()
{
    static const std::vector<Effect> effects = {
        {"VCA Compressor", EffectGuids::kCompressorVca, {"stereoLink"}, {}},
        {"Opto Compressor", EffectGuids::kCompressorOpto, {"stereoLink"}, {}},
        {"Noise Gate", EffectGuids::kDynamicsGate, {"threshold", "stereoLink"}, {}},
        {"Parametric EQ", EffectGuids::kEqParametric, {}, {}},
        {"Digital Delay", EffectGuids::kDelayDigital, {"syncDivision"}, {kDivisionWhenSynced}},
        {"Chorus", EffectGuids::kChorus, {"syncDivision"}, {kDivisionWhenSynced}},
        {"Flanger", EffectGuids::kFlanger, {"syncDivision"}, {kDivisionWhenSynced}},
        {"Synth Voice", EffectGuids::kSynthSaw, {"outputGain", "gate"}, {}},
        {"Auto Arpeggiator", EffectGuids::kAutoArp, {"pitchMode", "pitchThreshold"}, {}},
        {"Pitch Shift", EffectGuids::kPitchShift, {}, {}},
        // Key is the song's and Lowest Note the guitar's.
        {"Harmonizer", EffectGuids::kHarmonizer, {"key", "lowestNote"}, {}},
        // Output is set, to take out what each preset's tone controls add to the amp's level.
        {"Heavy American", EffectGuids::kAmpBuiltin, {}, {}, 3.0},
        // The bass cab sits lowest on a guitar DI, about 5 dB under the default.
        {"Cybercab", EffectGuids::kCabSimple, {"outputGain"}, {}},
    };
    return effects;
}

const Conditional* FindConditional(const Effect& effect, const std::string& key)
{
    const auto found = std::find_if(effect.conditional.begin(), effect.conditional.end(),
                                    [&key](const Conditional& c) { return c.key == key; });
    return found == effect.conditional.end() ? nullptr : &*found;
}

bool InRangeOnStep(const guitarfx::ParameterDef& def, double value)
{
    const double offset = value - def.minValue;
    const bool onStep = def.step <= 0.0 || std::abs(std::round(offset / def.step) * def.step - offset) < 1e-9;
    return guitarfx::IsFinite(value) && value >= def.minValue && value <= def.maxValue && onStep;
}

void TestWellFormed(const Effect& effect)
{
    std::cout << "\n" << effect.name << ": the preset list" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(effect.type);

    if (!info || info->presets.empty())
    {
        Check(false, "registered, with presets");
        return;
    }

    Check(info->presets.size() >= 5, "ships factory presets", std::to_string(info->presets.size()));

    std::set<std::string> ids;
    bool unique = true;
    int defaults = 0;

    for (const auto& preset : info->presets)
    {
        unique = unique && !preset.id.empty() && !preset.displayName.empty() && preset.isFactory &&
                 ids.insert(preset.id).second;
        defaults += preset.isDefault ? 1 : 0;
    }

    Check(unique, "each is a named factory preset with its own id");
    Check(defaults == 1 && info->presets.front().isDefault, "exactly one starts new nodes, and it comes first",
          std::to_string(defaults) + " defaults");

    std::string problem;
    const auto& first = info->presets.front().parameters;

    for (const auto& def : info->parameters)
    {
        const bool set = first.contains(def.id);

        if (effect.leaveOut.contains(def.id) ? set : (!set || first.at(def.id) != def.defaultValue))
        {
            problem = def.id;
        }
    }

    Check(problem.empty(), "the default preset is the parameter defaults, so a fresh node sounds as before", problem);

    std::map<std::string, const guitarfx::ParameterDef*> declared;

    for (const auto& def : info->parameters)
    {
        declared[def.id] = &def;
    }

    for (const auto& preset : info->presets)
    {
        for (const auto& def : info->parameters)
        {
            const bool set = preset.parameters.contains(def.id);
            bool shouldSet = !effect.leaveOut.contains(def.id);

            if (const auto* conditional = FindConditional(effect, def.id))
            {
                const auto when = preset.parameters.find(conditional->whenKey);
                shouldSet = when != preset.parameters.end() && when->second == conditional->whenValue;
            }

            if (set != shouldSet)
            {
                problem = preset.id + (set ? " sets " : " omits ") + def.id;
            }
            else if (set && !InRangeOnStep(def, preset.parameters.at(def.id)))
            {
                problem = preset.id + " puts " + def.id + " out of range";
            }
        }

        for (const auto& [key, value] : preset.parameters)
        {
            if (!declared.contains(key))
            {
                problem = preset.id + " sets " + key + ", which the effect does not declare";
            }
        }

        const bool ordered = preset.parameterOrder.size() == preset.parameters.size() &&
                             std::all_of(preset.parameterOrder.begin(), preset.parameterOrder.end(),
                                         [&preset](const std::string& key) { return preset.parameters.contains(key); });

        if (!ordered)
        {
            problem = preset.id + "'s parameter order does not list exactly what it sets";
        }
    }

    Check(problem.empty(), "every preset sets every control but the player's own, in range, and nothing else", problem);
}

/// The demo DI guitar as mono. Empty if it does not load, or is not at the 48 kHz the renders
/// below count their samples at.
std::vector<float> LoadGuitar()
{
    double sampleRate = 0.0;
    auto guitar = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, sampleRate);

    if (sampleRate != 48000.0)
    {
        guitar.clear();
    }

    return guitar;
}

struct Rendered
{
    bool finite = true;
    double peak = 0.0;
    double rmsDb = -200.0;
};

/// 20 s of the DI (from 10 s in, at its own 48 kHz), the first second left out of the level.
Rendered Render(const std::string& type, const std::map<std::string, double>& params, const std::vector<float>& guitar)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    auto effect = registry.Create(type);
    const auto info = registry.GetTypeInfo(type);

    for (const auto& def : info->parameters)
    {
        effect->SetParam(def.id, def.defaultValue);
    }

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    if (info->requiresTempo)
    {
        effect->SetParam("bpm", 120.0);
    }

    effect->Prepare(48000.0, kBlockSize);

    const std::size_t start = 10 * 48000;
    const std::size_t length = std::min<std::size_t>(20 * 48000, guitar.size() - start);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    Rendered r;
    double energy = 0.0;
    std::size_t counted = 0;

    for (std::size_t offset = 0; offset + kBlockSize <= length; offset += kBlockSize)
    {
        std::copy_n(guitar.begin() + static_cast<std::ptrdiff_t>(start + offset), kBlockSize, inL.begin());
        std::copy_n(inL.begin(), kBlockSize, inR.begin());
        effect->Process(inputs, outputs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            if (!guitarfx::IsFinite(outL[i]) || !guitarfx::IsFinite(outR[i]))
            {
                r.finite = false;
                continue;
            }

            r.peak = std::max({r.peak, static_cast<double>(std::abs(outL[i])), static_cast<double>(std::abs(outR[i]))});

            if (offset >= 48000)
            {
                energy += 0.5 * (static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i]);
                ++counted;
            }
        }
    }

    r.rmsDb = 10.0 * std::log10(std::max(energy / static_cast<double>(std::max<std::size_t>(counted, 1)), 1e-20));
    return r;
}

void TestPresetsRun(const Effect& effect, const std::vector<float>& guitar)
{
    std::cout << "\n" << effect.name << ": every preset on the DI guitar" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(effect.type);

    if (!info || info->presets.empty() || guitar.size() < 31 * 48000)
    {
        Check(false, "has presets and the DI to run them on");
        return;
    }

    const auto reference = Render(effect.type, info->presets.front().parameters, guitar);
    std::string problem;

    for (const auto& preset : info->presets)
    {
        const auto r = Render(effect.type, preset.parameters, guitar);
        const double levelDb = r.rmsDb - reference.rmsDb;
        std::cout << "    " << preset.displayName << ": " << std::round(levelDb * 10.0) / 10.0
                  << " dB against the default, peak " << std::round(20.0 * std::log10(r.peak) * 10.0) / 10.0 << " dBFS"
                  << std::endl;

        // A gross mistake, not a voicing: output that is not finite, runs away (the DI peaks
        // at -1 dBFS), or lands far from the default's level.
        if (!r.finite || r.peak > 4.0 || std::abs(levelDb) > effect.levelRangeDb)
        {
            problem = preset.id;
        }
    }

    Check(problem.empty(),
          "each stays finite, never runs away, and stays within " +
              std::to_string(static_cast<int>(effect.levelRangeDb)) + " dB of the default's level",
          problem);
}
} // namespace

int main()
{
    std::cout << "=== EffectFactoryPresetTests ===" << std::endl;
    guitarfx::RegisterCompressorEffects();
    guitarfx::RegisterNoiseGateEffect();
    guitarfx::RegisterParametricEQEffect();
    guitarfx::RegisterDelayEffect();
    guitarfx::RegisterChorusEffect();
    guitarfx::RegisterFlangerEffect();
    guitarfx::RegisterSynthSawEffect();
    guitarfx::RegisterAutoArpEffect();
    guitarfx::RegisterPitchShiftEffect();
    guitarfx::RegisterHarmonizerEffect();
    guitarfx::RegisterBuiltinAmpEffect();
    guitarfx::RegisterSimpleCabEffect();

    const auto guitar = LoadGuitar();

    for (const auto& effect : Effects())
    {
        TestWellFormed(effect);
        TestPresetsRun(effect, guitar);
    }

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
