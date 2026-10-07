/**
 * @file DualMonoTests.cpp
 * @brief Holds dual mono to its one rule: nothing crosses between the sides.
 *
 * In a dual-mono graph (SignalGraphExecutor::SetDualMono) a type that keeps its channels apart
 * runs as usual, with its links switched off, and any other gets a second instance that runs the
 * right side. Either way, what reaches the left output must depend on the left input alone, and
 * the right on the right. Every registered type is driven here at its defaults and under random
 * settings, so a type wrongly declared to keep its channels apart, or a second instance that
 * missed a parameter, shows up as one side moving with the other's input.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
using guitarfx::ChannelLayout;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
using guitarfx::SignalGraphExecutor;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 24; // ~130 ms: past every pitch shifter's latency
constexpr int kSettingsPerType = 2;
constexpr double kPi = 3.14159265358979323846;
/// One side moving with the other's input by more than this is a leak.
constexpr double kNoLeak = 1.0e-6;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

class Random
{
  public:
    explicit Random(std::uint32_t seed) : mState(seed)
    {
    }

    double Unit()
    {
        mState ^= mState << 13;
        mState ^= mState >> 17;
        mState ^= mState << 5;
        return static_cast<double>(mState) / 4294967296.0;
    }

  private:
    std::uint32_t mState;
};

std::vector<float> Tone(double hz, double amplitude, std::uint32_t noiseSeed)
{
    Random noise(noiseSeed);
    std::vector<float> signal(static_cast<std::size_t>(kBlock) * kBlocks);

    for (std::size_t i = 0; i < signal.size(); ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        signal[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * t) + 0.02 * (noise.Unit() * 2.0 - 1.0));
    }

    return signal;
}

double RandomValue(const guitarfx::ParameterDef& def, Random& random)
{
    const double value = def.minValue + random.Unit() * (def.maxValue - def.minValue);

    if (def.step > 0.0)
    {
        return std::clamp(def.minValue + std::round((value - def.minValue) / def.step) * def.step, def.minValue,
                          def.maxValue);
    }

    return value;
}

SignalGraph SingleNode(const GraphNode& effect)
{
    SignalGraph graph;
    graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back(effect);
    graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", effect.id, 0, 0, 1.0});
    graph.edges.push_back({effect.id, "out", 0, 0, 1.0});
    return graph;
}

struct Output
{
    std::vector<float> left;
    std::vector<float> right;
};

/// Runs `graph` on the two inputs, dual mono or plain stereo.
Output Run(const SignalGraph& graph, const std::vector<float>& inLeft, const std::vector<float>& inRight, bool dualMono)
{
    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Stereo);
    executor.SetDualMono(dualMono);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);
    executor.SetTempo(120.0);

    Output out{std::vector<float>(inLeft.size()), std::vector<float>(inLeft.size())};
    std::vector<float> l(kBlock), r(kBlock), ol(kBlock), orr(kBlock);

    for (std::size_t offset = 0; offset + kBlock <= inLeft.size(); offset += kBlock)
    {
        std::copy_n(inLeft.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, l.begin());
        std::copy_n(inRight.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, r.begin());
        float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        executor.Process(ins, outs, kBlock);
        std::copy(ol.begin(), ol.end(), out.left.begin() + static_cast<std::ptrdiff_t>(offset));
        std::copy(orr.begin(), orr.end(), out.right.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    return out;
}

double MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const double difference = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));

        if (!(difference <= 1.0e9)) // NaN or worse: a leak by any measure
        {
            return std::numeric_limits<double>::infinity();
        }

        worst = std::max(worst, difference);
    }

    return worst;
}

/// For every registered type: in dual mono, the left output does not move when only the right
/// input changes, and the right does not move when only the left does.
void TestNoTypeLeaksBetweenSides()
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto guitar = Tone(196.0, 0.3, 0x1111u);
    const auto keys = Tone(587.0, 0.25, 0x2222u);
    const auto other = Tone(330.0, 0.2, 0x3333u);
    Random random(0xd0a1d0a1u);
    int types = 0;

    for (const auto& info : registry.GetAllTypes())
    {
        if (info.type == guitarfx::kNodeTypeInput || info.type == guitarfx::kNodeTypeOutput)
        {
            continue;
        }

        ++types;

        for (int setting = 0; setting <= kSettingsPerType; ++setting)
        {
            GraphNode node{"fx", info.type, info.category, info.displayName, true};
            std::string values;

            for (const auto& def : info.parameters)
            {
                const double value = setting == 0 ? def.defaultValue : RandomValue(def, random);
                node.params[def.id] = value;
                values += " " + def.id + "=" + std::to_string(value);
            }

            const auto graph = SingleNode(node);
            const auto both = Run(graph, guitar, keys, true);
            const auto otherRight = Run(graph, guitar, other, true);
            const auto otherLeft = Run(graph, other, keys, true);

            const double rightIntoLeft = MaxDifference(both.left, otherRight.left);
            const double leftIntoRight = MaxDifference(both.right, otherLeft.right);
            Check(rightIntoLeft <= kNoLeak && leftIntoRight <= kNoLeak,
                  info.displayName + " (" + info.type + "): nothing crosses in dual mono; right into left " +
                      std::to_string(rightIntoLeft) + ", left into right " + std::to_string(leftIntoRight) + " with" +
                      values);
        }
    }

    std::cout << types << " types checked for leaks between the sides\n";
}

/// At its defaults, one node fed the same signal on both sides puts out in dual mono exactly what
/// it puts out in stereo: each side is what a mono input of its own would make.
void TestIdenticalSidesMatchStereo()
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto guitar = Tone(196.0, 0.3, 0x4444u);

    for (const auto& info : registry.GetAllTypes())
    {
        if (info.type == guitarfx::kNodeTypeInput || info.type == guitarfx::kNodeTypeOutput)
        {
            continue;
        }

        GraphNode node{"fx", info.type, info.category, info.displayName, true};

        for (const auto& def : info.parameters)
        {
            node.params[def.id] = def.defaultValue;
        }

        const auto graph = SingleNode(node);
        const auto dual = Run(graph, guitar, guitar, true);
        const auto stereo = Run(graph, guitar, guitar, false);
        const double apart = std::max(MaxDifference(dual.left, stereo.left), MaxDifference(dual.right, stereo.right));
        Check(apart <= 1.0e-5,
              info.displayName + ": dual mono matches stereo on identical sides; apart by " + std::to_string(apart));
    }
}

/// A second instance hears every change its primary does: a parameter set while running, by name
/// and through by-type automation, and a bypass. Fed identical sides, a dual-mono graph matches a
/// stereo one only if both of its instances took each change.
void TestSecondInstanceStaysInStep()
{
    using namespace guitarfx::EffectGuids;
    GraphNode reverb{"verb", kReverbRoom, "reverb", "Room", true};
    const auto graph = SingleNode(reverb);
    const auto guitar = Tone(196.0, 0.3, 0x5555u);
    const auto type = guitarfx::EffectRegistry::Instance().Resolve(kReverbRoom);

    SignalGraphExecutor dual;
    dual.SetInputLayout(ChannelLayout::Stereo);
    dual.SetDualMono(true);
    dual.SetGraph(graph);
    dual.Prepare(kSampleRate, kBlock);
    Check(dual.DualMonoSharedNodes().empty(), "a reverb gets a second instance in dual mono");

    SignalGraphExecutor stereo;
    stereo.SetInputLayout(ChannelLayout::Stereo);
    stereo.SetGraph(graph);
    stereo.Prepare(kSampleRate, kBlock);

    std::vector<float> l(kBlock), r(kBlock), dl(kBlock), dr(kBlock), sl(kBlock), sr(kBlock);
    double drift = 0.0;
    int block = 0;

    for (std::size_t offset = 0; offset + kBlock <= guitar.size(); offset += kBlock, ++block)
    {
        for (auto* executor : {&dual, &stereo})
        {
            if (block == 8)
            {
                executor->SetNodeParam("verb", "mix", 0.9);
            }
            else if (block == 14)
            {
                SignalGraphExecutor::SetAutomationTargetParam(executor->FindAutomationTarget(type), "decay", 0.8);
            }
            else if (block == 20)
            {
                executor->SetNodeEnabled("verb", false);
            }
            else if (block == 24)
            {
                executor->SetNodeEnabled("verb", true);
            }

            std::copy_n(guitar.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, l.begin());
            std::copy_n(guitar.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, r.begin());
            float* ins[2] = {l.data(), r.data()};
            float* outs[2] = {executor == &dual ? dl.data() : sl.data(), executor == &dual ? dr.data() : sr.data()};
            executor->Process(ins, outs, kBlock);
        }

        drift = std::max({drift, MaxDifference(dl, sl), MaxDifference(dr, sr)});
    }

    Check(drift <= 1.0e-5, "both instances take every change; dual mono and stereo apart by " + std::to_string(drift));
}

/// A running graph switched to dual mono installs what was staged off the lock; without staging
/// the coupled nodes run shared, and say so.
void TestStagingInstallsSecondInstances()
{
    using namespace guitarfx::EffectGuids;
    GraphNode reverb{"verb", kReverbSpring, "reverb", "Spring", true};
    const auto graph = SingleNode(reverb);

    SignalGraphExecutor unstaged;
    unstaged.SetInputLayout(ChannelLayout::Stereo);
    unstaged.SetGraph(graph);
    unstaged.Prepare(kSampleRate, kBlock);
    unstaged.SetDualMono(true);
    Check(unstaged.DualMonoSharedNodes() == std::vector<std::string>{"verb"},
          "switched on while running without staging, the reverb runs shared and is reported");

    SignalGraphExecutor staged;
    staged.SetInputLayout(ChannelLayout::Stereo);
    staged.SetGraph(graph);
    staged.Prepare(kSampleRate, kBlock);
    staged.StageDualMonoTwins();
    staged.SetDualMono(true);
    Check(staged.DualMonoSharedNodes().empty(), "staged first, the reverb has its second instance");

    // And it keeps the sides apart from the first block.
    const auto guitar = Tone(196.0, 0.3, 0x6666u);
    const auto keys = Tone(587.0, 0.25, 0x7777u);
    const auto silence = std::vector<float>(guitar.size(), 0.0f);
    std::vector<float> l(kBlock), r(kBlock), ol(kBlock), orr(kBlock);
    double leak = 0.0;

    for (std::size_t offset = 0; offset + kBlock <= guitar.size(); offset += kBlock)
    {
        std::copy_n(silence.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, l.begin());
        std::copy_n(keys.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, r.begin());
        float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        staged.Process(ins, outs, kBlock);

        for (const float sample : ol)
        {
            leak = std::max(leak, static_cast<double>(std::fabs(sample)));
        }
    }

    Check(leak <= kNoLeak, "a silent left input stays silent on the left; reached " + std::to_string(leak));
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();

    TestNoTypeLeaksBetweenSides();
    TestIdenticalSidesMatchStereo();
    TestSecondInstanceStaysInStep();
    TestStagingInstallsSecondInstances();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "DualMonoTests passed\n";
    return 0;
}
