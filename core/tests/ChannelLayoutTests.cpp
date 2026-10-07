/**
 * @file ChannelLayoutTests.cpp
 * @brief Holds the executor's channel layout to configuration, never to the audio.
 *
 * Whether a connection is mono or stereo is resolved when a graph is built, from its input layout
 * and the effect types along the path (EffectProcessor::CanWiden). Two things follow, and both are
 * pinned here:
 *
 *  - Every type that declares it cannot widen really never does: identical sides in, identical
 *    sides out, under any settings. A type that secretly widens would let a following mono path
 *    drop its right side, so each one is driven with random settings.
 *  - The layout does not move with the audio: a stereo input keeps its right side through a
 *    silence, and nothing on the left reaches the right.
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
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
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
constexpr int kBlocks = 32; // ~170 ms: past every pitch shifter's latency
constexpr int kSettingsPerType = 5;
constexpr double kPi = 3.14159265358979323846;
/// How far apart an effect's two sides may be and still count as the same signal: -80 dBFS. Not
/// bit-exact, because an effect's two channels can round differently (a pitch shifter's do, by
/// about 1e-5); the executor copies left over right after such a node on a mono connection, so
/// what matters here is only that nothing a listener could hear is being produced.
constexpr double kSameSides = 1.0e-4;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

/// A small deterministic generator, so a failure reproduces.
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

/// A guitar-ish test signal: two partials and a little noise.
std::vector<float> TestSignal(int samples)
{
    Random noise(0x5eed1234u);
    std::vector<float> signal(static_cast<size_t>(samples));

    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        signal[static_cast<size_t>(i)] =
            static_cast<float>(0.3 * std::sin(2.0 * kPi * 196.0 * t) + 0.12 * std::sin(2.0 * kPi * 587.0 * t) +
                               0.02 * (noise.Unit() * 2.0 - 1.0));
    }

    return signal;
}

/// A value for `def` drawn at random, landing on its step for a discrete control.
double RandomValue(const guitarfx::ParameterDef& def, Random& random)
{
    const double value = def.minValue + random.Unit() * (def.maxValue - def.minValue);

    if (def.step > 0.0)
    {
        const double steps = std::round((value - def.minValue) / def.step);
        return std::clamp(def.minValue + steps * def.step, def.minValue, def.maxValue);
    }

    return value;
}

/// The same signal on both inputs, block by block; returns the largest difference between the
/// two outputs, or infinity when an output went non-finite.
double SideDifference(guitarfx::EffectProcessor& effect, const std::vector<float>& signal)
{
    effect.Prepare(kSampleRate, kBlock);
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    double worst = 0.0;

    for (size_t offset = 0; offset + kBlock <= signal.size(); offset += kBlock)
    {
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inL.begin());
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inR.begin());
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        effect.Process(ins, outs, kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            const double difference = std::fabs(static_cast<double>(outL[static_cast<size_t>(i)]) -
                                                static_cast<double>(outR[static_cast<size_t>(i)]));

            // Written so a NaN fails too: no comparison with NaN is true.
            if (!(difference <= 1.0))
            {
                return std::numeric_limits<double>::infinity();
            }

            worst = std::max(worst, difference);
        }
    }

    return worst;
}

/// Every registered type that says it cannot widen keeps identical sides identical, at its
/// defaults and under random settings.
void TestKeepingTypesNeverWiden()
{
    using namespace guitarfx;
    auto& registry = EffectRegistry::Instance();
    const auto signal = TestSignal(kBlock * kBlocks);
    int keeping = 0;
    int widening = 0;
    Random random(0xc0ffee11u);

    for (const auto& info : registry.GetAllTypes())
    {
        auto probe = registry.Create(info.type);

        if (!probe)
        {
            continue;
        }

        if (probe->CanWiden())
        {
            ++widening;
            continue;
        }

        ++keeping;

        for (int setting = 0; setting <= kSettingsPerType; ++setting)
        {
            auto effect = registry.Create(info.type);
            std::string values;

            // Setting 0 is the defaults; the rest draw every parameter at random.
            for (const auto& def : info.parameters)
            {
                const double value = setting == 0 ? def.defaultValue : RandomValue(def, random);
                effect->SetParam(def.id, value);
                values += " " + def.id + "=" + std::to_string(value);
            }

            const double difference = SideDifference(*effect, signal);
            Check(difference <= kSameSides, info.displayName + " (" + info.type +
                                                ") keeps identical sides identical; apart by " +
                                                std::to_string(difference) + " with" + values);
        }
    }

    std::cout << keeping << " types declare they cannot widen, " << widening << " that they can\n";
    Check(keeping > 10 && widening > 10, "the registry has both kinds");
}

/// The same for a NAM amp with a model loaded: its two models must agree on identical input.
void TestNamWithAModelKeepsSidesIdentical()
{
    using namespace guitarfx;
    const std::filesystem::path model = std::filesystem::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" /
                                        "Guitar" / "TimR" / "JCM800 2203 1985" / "JCM800 Hi P6 B8 M4 T7 G6.nam";

    for (const char* type : {EffectGuids::kAmpNamOptimized, EffectGuids::kFxNam})
    {
        auto effect = EffectRegistry::Instance().Create(type);

        if (!effect || !effect->LoadResource(model))
        {
            Check(false, std::string(type) + ": loads the test model");
            continue;
        }

        const double difference = SideDifference(*effect, TestSignal(kBlock * kBlocks));
        Check(!effect->CanWiden() && difference <= kSameSides,
              std::string(type) + " with a model keeps identical sides identical; apart by " +
                  std::to_string(difference));
    }
}

GraphNode Node(const std::string& id, const std::string& type)
{
    return GraphNode{id, type, "", id, true};
}

/// Input -> each type in turn -> output.
SignalGraph Chain(const std::vector<std::pair<std::string, std::string>>& stages)
{
    SignalGraph graph;
    graph.nodes.push_back(Node("in", guitarfx::kNodeTypeInput));
    std::string previous = "in";

    for (const auto& [id, type] : stages)
    {
        graph.nodes.push_back(Node(id, type));
        graph.edges.push_back({previous, id, 0, 0, 1.0});
        previous = id;
    }

    graph.nodes.push_back(Node("out", guitarfx::kNodeTypeOutput));
    graph.edges.push_back({previous, "out", 0, 0, 1.0});
    return graph;
}

/// Each node's channel count after one block.
std::map<std::string, int> ChannelCounts(const SignalGraph& graph, ChannelLayout input)
{
    SignalGraphExecutor executor;
    executor.SetInputLayout(input);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);

    std::vector<float> inL(kBlock, 0.1f), inR(kBlock, 0.1f), outL(kBlock), outR(kBlock);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    executor.Process(ins, outs, kBlock);

    std::map<std::string, int> counts;

    for (const auto& level : executor.GetNodeSignalLevels())
    {
        counts[level.nodeId] = level.channelCount;
    }

    return counts;
}

/// A mono input stays mono until the first type that can widen, and stereo after it; a stereo
/// input is stereo throughout; a mixer, which can pan, widens.
void TestLayoutResolvesFromTypes()
{
    using namespace guitarfx::EffectGuids;

    const auto chain = Chain({{"drive", kOverdrive}, {"amp", kAmpBuiltin}, {"chorus", kChorus}, {"eq", kEqGraphic}});
    const auto mono = ChannelCounts(chain, ChannelLayout::Mono);
    Check(mono.at("in") == 1 && mono.at("drive") == 1 && mono.at("amp") == 1,
          "a mono input is mono up to the first type that can widen");
    Check(mono.at("chorus") == 2 && mono.at("eq") == 2 && mono.at("out") == 2, "and stereo from it on");

    const auto stereo = ChannelCounts(chain, ChannelLayout::Stereo);
    Check(stereo.at("in") == 2 && stereo.at("drive") == 2 && stereo.at("amp") == 2,
          "a stereo input is stereo throughout");

    const auto keeps = ChannelCounts(Chain({{"phaser", kPhaser}, {"amp", kAmpBuiltin}}), ChannelLayout::Mono);
    Check(keeps.at("phaser") == 1 && keeps.at("amp") == 1 && keeps.at("out") == 1,
          "types that cannot widen leave a mono input mono to the output");

    SignalGraph parallel;
    parallel.nodes = {Node("in", guitarfx::kNodeTypeInput),
                      Node("split", guitarfx::kNodeTypeSplitter),
                      Node("a", kAmpBuiltin),
                      Node("b", kOverdrive),
                      Node("mix", guitarfx::kNodeTypeMixer),
                      Node("out", guitarfx::kNodeTypeOutput)};
    parallel.edges = {{"in", "split", 0, 0, 1.0}, {"split", "a", 0, 0, 1.0}, {"split", "b", 1, 0, 1.0},
                      {"a", "mix", 0, 0, 1.0},    {"b", "mix", 0, 1, 1.0},   {"mix", "out", 0, 0, 1.0}};
    const auto split = ChannelCounts(parallel, ChannelLayout::Mono);
    Check(split.at("split") == 1 && split.at("a") == 1 && split.at("b") == 1,
          "both branches of a mono split stay mono");
    Check(split.at("mix") == 2, "a mixer, which can pan, puts out stereo");
}

/// A node set to a mono channel mode folds a stereo input, runs mono and puts out mono; the
/// chain after it is mono again until something widens it.
void TestChannelModeFoldsToMono()
{
    using namespace guitarfx::EffectGuids;

    auto graph = Chain({{"chorus", kChorus}, {"amp", kAmpBuiltin}, {"eq", kEqGraphic}});
    graph.FindNode("amp")->channelMode = guitarfx::kChannelModeMono;
    const auto counts = ChannelCounts(graph, ChannelLayout::Mono);
    Check(counts.at("chorus") == 2, "the chorus widens a mono input");
    Check(counts.at("amp") == 1 && counts.at("eq") == 1 && counts.at("out") == 1,
          "an amp set to mono puts out mono, and the chain after it stays mono");

    // A widening type set to mono has its two sides summed: the sides come out identical.
    auto widener = Chain({{"chorus", kChorus}});
    widener.FindNode("chorus")->channelMode = guitarfx::kChannelModeMono;
    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Stereo);
    executor.SetGraph(widener);
    executor.Prepare(kSampleRate, kBlock);
    const auto signal = TestSignal(kBlock * kBlocks);
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    double apart = 0.0;

    for (size_t offset = 0; offset + kBlock <= signal.size(); offset += kBlock)
    {
        std::copy_n(signal.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inL.begin());
        std::fill(inR.begin(), inR.end(), 0.0f);
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        executor.Process(ins, outs, kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            apart = std::max(apart, std::fabs(static_cast<double>(outL[static_cast<size_t>(i)]) -
                                              static_cast<double>(outR[static_cast<size_t>(i)])));
        }
    }

    Check(!executor.OutputIsStereo() && apart == 0.0,
          "a chorus set to mono sums its sides; apart by " + std::to_string(apart));

    // Mono (left) and Mono (right) take one side of a stereo input.
    for (const auto& [mode, takeLeft] :
         {std::pair{guitarfx::kChannelModeMonoLeft, true}, std::pair{guitarfx::kChannelModeMonoRight, false}})
    {
        auto oneSide = Chain({{"gain", kGain}});
        oneSide.FindNode("gain")->channelMode = mode;
        SignalGraphExecutor sideExecutor;
        sideExecutor.SetInputLayout(ChannelLayout::Stereo);
        sideExecutor.SetGraph(oneSide);
        sideExecutor.Prepare(kSampleRate, kBlock);
        std::vector<float> left(kBlock, 0.25f), right(kBlock, -0.5f), sideL(kBlock), sideR(kBlock);
        float* ins[2] = {left.data(), right.data()};
        float* outs[2] = {sideL.data(), sideR.data()};
        sideExecutor.Process(ins, outs, kBlock);
        const float expected = takeLeft ? 0.25f : -0.5f;
        Check(std::fabs(sideL.back() - expected) < 1e-6f && sideL.back() == sideR.back(),
              std::string(mode) + " takes " + (takeLeft ? "the left" : "the right") + " side on both");
    }
}

/// Changing a running graph's input layout re-resolves it in place.
void TestInputLayoutChangesInPlace()
{
    using namespace guitarfx::EffectGuids;
    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Mono);
    executor.SetGraph(Chain({{"amp", kAmpBuiltin}}));
    executor.Prepare(kSampleRate, kBlock);
    Check(!executor.OutputIsStereo(), "mono in, nothing that widens: mono out");
    executor.SetInputLayout(ChannelLayout::Stereo);
    Check(executor.OutputIsStereo(), "switched to a stereo input, the same graph is stereo out");
    executor.SetInputLayout(ChannelLayout::Mono);
    Check(!executor.OutputIsStereo(), "and back");
}

/// In stereo, a right input that comes and goes changes nothing about the layout, and nothing on
/// the left ever reaches the right: the right output is exactly what the right input alone makes.
void TestStereoNeverReadsTheAudio()
{
    using namespace guitarfx::EffectGuids;
    const auto graph = Chain({{"drive", kOverdrive}, {"amp", kAmpBuiltin}, {"eq", kEqParametric}});

    // Right input: three blocks of a tone, three of silence, and so on.
    std::vector<float> right(static_cast<size_t>(kBlock) * kBlocks, 0.0f);

    for (size_t i = 0; i < right.size(); ++i)
    {
        if ((i / kBlock) % 6 < 3)
        {
            right[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / kSampleRate));
        }
    }

    const auto left = TestSignal(kBlock * kBlocks);
    const std::vector<float> silence(left.size(), 0.0f);

    // Runs the graph on (inL, right) and returns the right output, checking every node's channel
    // count every block on the way.
    const auto runRight = [&](const std::vector<float>& inLeft, bool& countsSteady) {
        SignalGraphExecutor executor;
        executor.SetInputLayout(ChannelLayout::Stereo);
        executor.SetGraph(graph);
        executor.Prepare(kSampleRate, kBlock);
        std::vector<float> outRight(right.size());
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        countsSteady = true;

        for (size_t offset = 0; offset + kBlock <= right.size(); offset += kBlock)
        {
            std::copy_n(inLeft.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inL.begin());
            std::copy_n(right.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inR.begin());
            float* ins[2] = {inL.data(), inR.data()};
            float* outs[2] = {outL.data(), outR.data()};
            executor.Process(ins, outs, kBlock);
            std::copy(outR.begin(), outR.end(), outRight.begin() + static_cast<std::ptrdiff_t>(offset));

            for (const auto& level : executor.GetNodeSignalLevels())
            {
                countsSteady = countsSteady && level.channelCount == 2;
            }
        }

        return outRight;
    };

    bool steadyWithGuitar = false;
    bool steadyAlone = false;
    const auto withGuitar = runRight(left, steadyWithGuitar);
    const auto alone = runRight(silence, steadyAlone);
    double leak = 0.0;

    for (size_t i = 0; i < withGuitar.size(); ++i)
    {
        leak = std::max(leak, std::fabs(static_cast<double>(withGuitar[i]) - alone[i]));
    }

    Check(steadyWithGuitar && steadyAlone, "every node stays stereo while the right input comes and goes");
    Check(leak <= kSameSides, "nothing on the left reaches the right; apart by " + std::to_string(leak));
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();

    TestKeepingTypesNeverWiden();
    TestNamWithAModelKeepsSidesIdentical();
    TestLayoutResolvesFromTypes();
    TestChannelModeFoldsToMono();
    TestInputLayoutChangesInPlace();
    TestStereoNeverReadsTheAudio();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "ChannelLayoutTests passed\n";
    return 0;
}
