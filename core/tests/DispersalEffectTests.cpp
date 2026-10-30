/**
 * @file DispersalEffectTests.cpp
 * @brief Tests for the Dispersal effect.
 *
 * These measure what the effect claims to do, rather than only that it makes sound:
 *   - registration, the parameter table, clamping and the factory presets
 *   - it is an all-pass: a noise burst comes out with its energy, a sine at its level
 *   - it delays its centre by what peakDelayMs says, and frequencies away from it far less
 *   - parameters set before the first block are in force at sample 0 (a reference chain)
 *   - a change of Stages crossfades rather than clicks
 *   - Mix 0 is the dry signal exactly
 *   - Stereo 0 keeps the sides identical, Stereo moves them apart, and each side hears only its
 *     own input whether or not the right chain is linked to the left
 *   - sweeping Frequency fast at 256 stages and the highest Focus gains nothing
 *   - a NaN on the input does not stick; odd sample rates stay finite
 *   - and prints what a block costs
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DispersalEffect.h"

namespace
{
using guitarfx::DispersalEffect;
namespace dispersal = guitarfx::dispersal;
using Params = std::vector<std::pair<std::string, double>>;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;

    if (!condition)
    {
        ++gFailures;
    }

    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v, int precision = 3)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

double ToDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

std::unique_ptr<DispersalEffect> MakeDispersal(const Params& params, double sampleRate = kSampleRate)
{
    auto effect = std::make_unique<DispersalEffect>();
    effect->Prepare(sampleRate, kBlockSize);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    return effect;
}

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
};

/// Runs the effect over the two inputs in blocks, calling `beforeBlock(sampleIndex)` first.
template <typename BeforeBlock>
Stereo Run(DispersalEffect& effect, const std::vector<float>& inL, const std::vector<float>& inR, int block,
           BeforeBlock beforeBlock)
{
    Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};

    for (std::size_t start = 0; start < inL.size(); start += static_cast<std::size_t>(block))
    {
        const int count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), inL.size() - start));
        beforeBlock(start);
        float* ins[2] = {const_cast<float*>(inL.data() + start), const_cast<float*>(inR.data() + start)};
        float* outs[2] = {out.left.data() + start, out.right.data() + start};
        effect.Process(ins, outs, count);
    }

    return out;
}

Stereo Run(DispersalEffect& effect, const std::vector<float>& inL, const std::vector<float>& inR,
           int block = kBlockSize)
{
    return Run(effect, inL, inR, block, [](std::size_t) {});
}

/// A mono input, both sides from the same buffer, as a mono guitar reaches the node.
Stereo RunMono(DispersalEffect& effect, const std::vector<float>& input, int block = kBlockSize)
{
    return Run(effect, input, input, block);
}

std::vector<float> Noise(double seconds, double amplitude, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> out(static_cast<std::size_t>(seconds * kSampleRate));

    for (auto& v : out)
    {
        v = static_cast<float>(amplitude) * dist(rng);
    }

    return out;
}

std::vector<float> Sine(double hz, double seconds, double amplitude, double sampleRate = kSampleRate)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sampleRate));
    }

    return out;
}

/// A tone burst under a Gaussian envelope: narrow enough in frequency (sigma 8 Hz) that the
/// chain's group delay is flat across it.
std::vector<float> Burst(double hz, double centreSeconds, double totalSeconds)
{
    constexpr double sigma = 0.02;
    std::vector<float> out(static_cast<std::size_t>(totalSeconds * kSampleRate));

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const double envelope = std::exp(-0.5 * (t - centreSeconds) * (t - centreSeconds) / (sigma * sigma));
        out[i] = static_cast<float>(0.5 * envelope * std::sin(2.0 * kPi * hz * t));
    }

    return out;
}

double Energy(const std::vector<float>& signal, std::size_t from = 0, std::size_t to = 0)
{
    to = to == 0 ? signal.size() : to;
    double sum = 0.0;

    for (std::size_t i = from; i < to; ++i)
    {
        sum += static_cast<double>(signal[i]) * signal[i];
    }

    return sum;
}

double Rms(const std::vector<float>& signal, std::size_t from, std::size_t to)
{
    return std::sqrt(Energy(signal, from, to) / static_cast<double>(to - from));
}

/// Where a signal's energy sits in time, in seconds: its energy-weighted mean time.
double EnergyCentroid(const std::vector<float>& signal)
{
    double weighted = 0.0;
    double total = 0.0;

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double e = static_cast<double>(signal[i]) * signal[i];
        weighted += e * static_cast<double>(i);
        total += e;
    }

    return weighted / std::max(total, 1.0e-30) / kSampleRate;
}

std::vector<float> WithSilence(std::vector<float> signal, double seconds)
{
    signal.resize(signal.size() + static_cast<std::size_t>(seconds * kSampleRate), 0.0f);
    return signal;
}

void TestRegistration()
{
    std::cout << "\nRegistration and parameters" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kDispersal);

    Check(info.has_value(), "registered under its UUID");
    Check(registry.Resolve("dispersal") == guitarfx::EffectGuids::kDispersal, "the dispersal alias resolves to it");
    // Create falls back to a passthrough for an unknown type, so check what it made.
    Check(registry.Create("dispersal")->GetType() == "dispersal", "the registry creates one from the alias");

    if (!info)
    {
        return;
    }

    Check(info->category == "modulation" && !info->requiresTempo, "a modulation effect that needs no tempo");

    bool ordered = info->parameters.size() == dispersal::kParamCount;

    for (std::size_t i = 0; ordered && i < info->parameters.size(); ++i)
    {
        ordered = info->parameters[i].id == dispersal::kParams[i].id;
    }

    Check(ordered, "parameters are declared in table order", std::to_string(info->parameters.size()) + " params");

    const auto& stages = info->parameters[dispersal::kStages];
    Check(stages.taper == guitarfx::ParamTaper::Log && stages.step == 1.0, "Stages is a whole count on a log taper");
    Check(info->parameters[dispersal::kFrequency].taper == guitarfx::ParamTaper::Log &&
              info->parameters[dispersal::kFocus].taper == guitarfx::ParamTaper::Log,
          "Frequency and Focus are on log tapers");

    DispersalEffect effect;
    effect.SetParam("stages", 99999.0);
    Check(effect.GetParam("stages") == 256.0, "Stages clamps to 256");
    effect.SetParam("stages", 37.4);
    Check(effect.GetParam("stages") == 37.0, "and snaps to a whole stage");
    effect.SetParam("frequency", 5.0);
    Check(effect.GetParam("frequency") == 100.0, "Frequency clamps to 100 Hz");
    effect.SetParam("mix", 0.25);
    effect.SetParam("mix", std::nan(""));
    Check(effect.GetParam("mix") == 0.25, "a NaN is ignored");
    Check(effect.KeepsChannelsSeparate() && effect.CanWiden(), "keeps its sides apart, and can widen");

    // 64 stages at 800 Hz and Q 1: 2Q / (pi f0) is 0.796 ms a stage well below Nyquist.
    DispersalEffect defaults;
    defaults.Prepare(kSampleRate, kBlockSize);
    const double peak = defaults.GetParam("peakDelayMs");
    Check(std::abs(peak - 64.0 * 2.0 / (kPi * 800.0) * 1000.0) < 0.2, "peakDelayMs at the defaults is about 51 ms",
          Num(peak, 2) + " ms");
}

void TestFactoryPresets()
{
    std::cout << "\nFactory presets" << std::endl;
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(guitarfx::EffectGuids::kDispersal);

    if (!info)
    {
        Check(false, "type info available");
        return;
    }

    Check(!info->presets.empty() && info->presets.front().isDefault &&
              info->presets.front().id == dispersal::kDefaultPresetId,
          "the first preset is the default");

    bool defaultMatches = true;

    for (const auto& [key, value] : info->presets.front().parameters)
    {
        const std::size_t index = guitarfx::FindParamSpec(dispersal::kParams, key);
        defaultMatches =
            defaultMatches && index < dispersal::kParamCount && dispersal::kParams[index].defaultValue == value;
    }

    Check(defaultMatches, "the default preset holds the declared defaults");

    bool wellFormed = true;
    std::string problem;

    for (const auto& preset : info->presets)
    {
        for (const auto& [key, value] : preset.parameters)
        {
            const std::size_t index = guitarfx::FindParamSpec(dispersal::kParams, key);

            if (index == dispersal::kParamCount || value < dispersal::kParams[index].minValue ||
                value > dispersal::kParams[index].maxValue ||
                guitarfx::NormaliseParamValue(dispersal::kParams[index], value) != value)
            {
                wellFormed = false;
                problem = preset.id + "." + key;
            }
        }

        wellFormed = wellFormed && preset.parameters.size() == dispersal::kParamCount;
    }

    Check(wellFormed, "every preset sets every parameter, in range", problem);
}

void TestEnergyPreserved()
{
    std::cout << "\nAll-pass: a noise burst keeps its energy" << std::endl;
    const auto burst = Noise(0.25, 0.3);

    for (const auto& [stages, hz, focus] : std::vector<std::tuple<double, double, double>>{
             {1.0, 800.0, 1.0}, {64.0, 800.0, 1.0}, {256.0, 3000.0, 0.5}, {64.0, 200.0, 3.0}})
    {
        auto effect = MakeDispersal({{"stages", stages}, {"frequency", hz}, {"focus", focus}});
        const double tail = 3.0 * effect->GetParam("peakDelayMs") / 1000.0 + 0.2;
        const auto input = WithSilence(burst, tail);
        const auto out = RunMono(*effect, input);
        const double ratioDb = 10.0 * std::log10(Energy(out.left) / Energy(input));

        Check(std::abs(ratioDb) < 0.05,
              std::to_string(static_cast<int>(stages)) + " stages at " + Num(hz, 0) + " Hz, Focus " + Num(focus, 1),
              Num(ratioDb, 4) + " dB");
    }
}

void TestSineLevel()
{
    std::cout << "\nAll-pass: a steady sine keeps its level" << std::endl;
    double worst = 0.0;
    std::string where;

    for (const double hz : {50.0, 200.0, 800.0, 3000.0, 12000.0})
    {
        auto effect = MakeDispersal({});
        const auto input = Sine(hz, 1.0, 0.5);
        const auto out = RunMono(*effect, input);

        // An integer number of cycles near the last quarter second.
        const auto window = static_cast<std::size_t>(std::round(std::round(0.25 * hz) * kSampleRate / hz));
        const std::size_t to = input.size();
        const double db = ToDb(Rms(out.left, to - window, to) / Rms(input, to - window, to));

        if (std::abs(db) > std::abs(worst))
        {
            worst = db;
            where = Num(hz, 0) + " Hz";
        }
    }

    Check(std::abs(worst) < 0.05, "50 Hz to 12 kHz, after settling", "worst " + Num(worst, 4) + " dB at " + where);
}

void TestDelayWhereDesigned()
{
    std::cout << "\nEach frequency arrives as late as the design says, the centre latest" << std::endl;

    for (const auto& [stages, hz, focus] :
         std::vector<std::tuple<double, double, double>>{{64.0, 800.0, 1.0}, {128.0, 2000.0, 3.0}})
    {
        auto probe = MakeDispersal({{"stages", stages}, {"frequency", hz}, {"focus", focus}});
        const double designed = probe->GetParam("peakDelayMs");
        const std::string label = std::to_string(static_cast<int>(stages)) + " stages at " + Num(hz, 0) + " Hz";

        auto delayAt = [&](double toneHz) {
            auto effect = MakeDispersal({{"stages", stages}, {"frequency", hz}, {"focus", focus}});
            const auto input = Burst(toneHz, 0.1, 0.2 + 3.0 * designed / 1000.0);
            const auto out = RunMono(*effect, input);
            return (EnergyCentroid(out.left) - EnergyCentroid(input)) * 1000.0;
        };

        const double atCentre = stages * dispersal::StageGroupDelaySeconds(hz, hz, focus, kSampleRate) * 1000.0;
        Check(std::abs(atCentre / designed - 1.0) < 1.0e-9, label + ": peakDelayMs is the group delay at the centre");

        double centreDelay = 0.0;

        for (const double toneHz : {hz, hz / 4.0, hz * 4.0})
        {
            const double measured = delayAt(toneHz);
            const double expected = stages * dispersal::StageGroupDelaySeconds(toneHz, hz, focus, kSampleRate) * 1000.0;
            centreDelay = toneHz == hz ? measured : centreDelay;
            Check(std::abs(measured / expected - 1.0) < 0.03 && measured <= centreDelay,
                  label + ": a burst at " + Num(toneHz, 0) + " Hz",
                  "measured " + Num(measured, 2) + " ms, designed " + Num(expected, 2) + " ms");
        }
    }
}

void TestSnapBeforeFirstBlock()
{
    std::cout << "\nParameters set before the first block are in force at sample 0" << std::endl;

    for (const double stages : {1.0, 3.0})
    {
        auto effect = MakeDispersal({{"stages", stages}, {"frequency", 5000.0}, {"focus", 0.5}});
        std::vector<float> impulse(512, 0.0f);
        impulse[0] = 1.0f;
        const auto out = RunMono(*effect, impulse);

        // The same chain with its coefficients fixed from the start.
        const auto c = dispersal::DesignStage(5000.0, 0.5, kSampleRate);
        std::vector<float> ic1(static_cast<std::size_t>(stages), 0.0f);
        std::vector<float> ic2(ic1.size(), 0.0f);
        double worst = 0.0;

        for (std::size_t i = 0; i < impulse.size(); ++i)
        {
            float x = impulse[i];

            for (std::size_t s = 0; s < ic1.size(); ++s)
            {
                const float v3 = x - ic2[s];
                const float v1 = c.a1 * ic1[s] + c.a2 * v3;
                const float v2 = ic2[s] + c.a2 * ic1[s] + c.a3 * v3;
                ic1[s] = 2.0f * v1 - ic1[s];
                ic2[s] = 2.0f * v2 - ic2[s];
                x = x - c.twoK * v1;
            }

            worst = std::max(worst, static_cast<double>(std::fabs(x - out.left[i])));
        }

        Check(worst < 1.0e-5, std::to_string(static_cast<int>(stages)) + " stage(s) at 5 kHz match a fixed chain",
              "worst " + Num(worst * 1.0e6, 3) + "e-6");
    }
}

void TestStagesCrossfade()
{
    std::cout << "\nA change of Stages crossfades" << std::endl;

    for (const double hz : {220.0, 800.0})
    {
        auto effect = MakeDispersal({});
        const auto input = Sine(hz, 2.0, 0.5);
        const auto out = Run(*effect, input, input, kBlockSize, [&](std::size_t sample) {
            // 64 -> 256 -> 32 -> 1, each held half a second.
            if (sample == 24064)
            {
                effect->SetParam("stages", 256.0);
            }
            else if (sample == 48128)
            {
                effect->SetParam("stages", 32.0);
            }
            else if (sample == 72192)
            {
                effect->SetParam("stages", 1.0);
            }
        });

        // A steady sine moves at most 2 pi f A / fs between samples; a click is a jump past that.
        const double steady = 2.0 * kPi * hz * 0.5 / kSampleRate;
        double jump = 0.0;

        for (std::size_t i = 4800; i < out.left.size(); ++i)
        {
            jump = std::max(jump, static_cast<double>(std::fabs(out.left[i] - out.left[i - 1])));
        }

        Check(jump < 1.5 * steady, Num(hz, 0) + " Hz sine through 64, 256, 32 and 1 stages",
              "largest step " + Num(jump / steady, 3) + "x a steady sine's");
    }
}

void TestMix()
{
    std::cout << "\nMix" << std::endl;
    auto effect = MakeDispersal({{"mix", 0.0}});
    const auto input = Noise(0.2, 0.3);
    const auto out = RunMono(*effect, input);
    Check(out.left == input && out.right == input, "Mix 0 is the dry signal exactly");
}

void TestStereo()
{
    std::cout << "\nStereo" << std::endl;
    const auto input = Noise(3.0, 0.3);

    {
        auto effect = MakeDispersal({});
        const auto out = RunMono(*effect, input);
        Check(out.left == out.right, "Stereo 0 keeps a mono input's sides identical");
    }

    auto effect = MakeDispersal({{"stereo", 7.0}});
    const auto out = Run(*effect, input, input, kBlockSize, [&](std::size_t sample) {
        if (sample == 48128)
        {
            effect->SetParam("stereo", 0.0);
        }
    });

    double apart = 0.0;

    for (std::size_t i = 4800; i < 48000; ++i)
    {
        apart = std::max(apart, static_cast<double>(std::fabs(out.left[i] - out.right[i])));
    }

    Check(apart > 0.05, "Stereo +7 moves the sides apart", "largest difference " + Num(apart, 3));

    const std::size_t lastBlock = out.left.size() - kBlockSize;
    const bool together = std::equal(out.left.begin() + static_cast<std::ptrdiff_t>(lastBlock), out.left.end(),
                                     out.right.begin() + static_cast<std::ptrdiff_t>(lastBlock));
    Check(together, "back at 0, the sides converge and are linked again");

    // The left side's output depends on the left input only, whichever path it ran on: linked
    // to a matching right side, or beside a different one.
    auto linked = MakeDispersal({});
    auto apartRight = MakeDispersal({});
    const auto other = Noise(3.0, 0.3, 99);
    const auto a = RunMono(*linked, input);
    const auto b = Run(*apartRight, input, other);
    double worst = 0.0;

    for (std::size_t i = 0; i < a.left.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(a.left[i] - b.left[i])));
    }

    // Not bit-exact: the two loops may contract to FMA differently.
    Check(worst < 1.0e-5, "the left side ignores the right, linked or not", "worst " + Num(worst * 1.0e6, 3) + "e-6");
}

void TestModulationStable()
{
    std::cout << "\nSweeping Frequency at 256 stages and Focus 6 gains nothing" << std::endl;
    auto effect = MakeDispersal({{"stages", 256.0}, {"focus", 6.0}});
    const auto input = Noise(2.0, 0.3);
    const auto out = Run(*effect, input, input, kBlockSize, [&](std::size_t sample) {
        // A triangle between 100 Hz and 10 kHz, by ratio, twice a second.
        const double phase = std::fmod(static_cast<double>(sample) / kSampleRate * 2.0, 1.0);
        const double tri = phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase;
        effect->SetParam("frequency", 100.0 * std::pow(100.0, tri));
    });

    const bool finite = std::all_of(out.left.begin(), out.left.end(), [](float v) { return guitarfx::IsFinite(v); });
    const double gainDb = ToDb(Rms(out.left, 0, out.left.size()) / Rms(input, 0, input.size()));
    const auto peakIn = *std::max_element(input.begin(), input.end());
    float peakOut = 0.0f;

    for (const float v : out.left)
    {
        peakOut = std::max(peakOut, std::fabs(v));
    }

    Check(finite, "the output stays finite");
    Check(gainDb < 0.5, "no level gained over the sweep", Num(gainDb, 2) + " dB");
    Check(peakOut < 4.0f * peakIn, "no runaway peak", Num(ToDb(peakOut / peakIn), 1) + " dB over the input's peak");
}

void TestNonFiniteRecovery()
{
    std::cout << "\nNon-finite input" << std::endl;
    auto effect = MakeDispersal({});
    auto input = Sine(220.0, 0.5, 0.3);
    input[1000] = std::nanf("");
    const auto out = RunMono(*effect, input);
    const bool recovered =
        std::all_of(out.left.begin() + 2048, out.left.end(), [](float v) { return guitarfx::IsFinite(v); });
    Check(recovered, "a NaN on the input does not stick");
}

void TestSampleRates()
{
    std::cout << "\nOther sample rates" << std::endl;

    for (const double rate : {22050.0, 44100.0, 96000.0, 192000.0})
    {
        auto effect = MakeDispersal({{"stages", 128.0}, {"frequency", 10000.0}, {"stereo", 12.0}}, rate);
        const auto input = Sine(440.0, 0.25, 0.3, rate);
        const auto out = Run(*effect, input, input);
        const bool finite =
            std::all_of(out.left.begin(), out.left.end(), [](float v) { return guitarfx::IsFinite(v); }) &&
            std::all_of(out.right.begin(), out.right.end(), [](float v) { return guitarfx::IsFinite(v); });
        const double peak = effect->GetParam("peakDelayMs");
        Check(finite && guitarfx::IsFinite(peak) && peak > 0.0,
              Num(rate / 1000.0, 2) + " kHz, centre at the top and Stereo +12", "peak delay " + Num(peak, 2) + " ms");
    }
}

void ReportCost()
{
    std::cout << "\nCost per 64-sample stereo block at 48 kHz (informational; meaningful in Release)" << std::endl;
    const auto input = Noise(2.0, 0.3);
    const auto other = Noise(2.0, 0.3, 99);

    for (const auto& [label, stages, mono] :
         std::vector<std::tuple<std::string, double, bool>>{{"64 stages, mono input (linked)", 64.0, true},
                                                            {"64 stages, stereo input", 64.0, false},
                                                            {"256 stages, mono input (linked)", 256.0, true},
                                                            {"256 stages, stereo input", 256.0, false}})
    {
        auto effect = MakeDispersal({{"stages", stages}});
        const auto begin = std::chrono::steady_clock::now();
        Run(*effect, input, mono ? input : other, 64);
        const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - begin;
        const double blocks = static_cast<double>(input.size()) / 64.0;
        std::cout << "  " << label << ": " << Num(elapsed.count() / blocks, 2) << " us" << std::endl;
    }
}
} // namespace

int main()
{
    std::cout << "=== DispersalEffectTests ===" << std::endl;
    guitarfx::RegisterDispersalEffect();

    TestRegistration();
    TestFactoryPresets();
    TestEnergyPreserved();
    TestSineLevel();
    TestDelayWhereDesigned();
    TestSnapBeforeFirstBlock();
    TestStagesCrossfade();
    TestMix();
    TestStereo();
    TestModulationStable();
    TestNonFiniteRecovery();
    TestSampleRates();
    ReportCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
