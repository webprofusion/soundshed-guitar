/**
 * @file ModulationStereoTests.cpp
 * @brief Holds the executor to the stereo image modulation effects make from a mono input.
 *
 * Chorus and flanger run the right channel's LFO a quarter cycle ahead of the left, the doubler
 * subtracts on the right, tremolo's Pan and the rotary's mics place the signal: each can turn a
 * mono input stereo, so each type declares that it can widen (EffectProcessor::CanWiden), and the
 * executor runs everything after it in stereo from the moment the graph is built. Nothing is
 * decided per block, so turning a Mix or a Depth up from zero is heard at once, with no switch.
 * Phaser, vibe and the wahs move both channels together: they declare they cannot widen, so a
 * following amp keeps its mono path, which is half the cost of the stereo one for a NAM model.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/CompositeEffectProcessor.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using guitarfx::EffectGuids::kAmpBuiltin;
using guitarfx::EffectGuids::kChorus;
using guitarfx::EffectGuids::kDelayDoubler;
using guitarfx::EffectGuids::kFlanger;
using guitarfx::EffectGuids::kOverdrive;
using guitarfx::EffectGuids::kPhaser;
using guitarfx::EffectGuids::kRotary;
using guitarfx::EffectGuids::kTremolo;
using guitarfx::EffectGuids::kVibe;
using guitarfx::EffectGuids::kWah;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 40; // ~213 ms: past the chorus's 15 ms delay plus its 20 ms of depth
constexpr int kWarmupSamples = 4800;
constexpr double kPi = 3.14159265358979323846;

/// Channels this far apart are stereo; a mono-safe effect keeps them within kIdentical.
constexpr double kDistinct = 1.0e-3;
constexpr double kIdentical = 1.0e-6;

constexpr const char* kProbeType = "test_mono_path_probe";

using Params = std::map<std::string, double>;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

/// A mono-capable pass-through that counts which path the executor chose for it.
class MonoPathProbe : public guitarfx::EffectProcessor
{
  public:
    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        ++stereoBlocks;

        for (int ch = 0; ch < 2; ++ch)
        {
            if (outputs[ch] && inputs[ch])
            {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            }
        }
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        ++monoBlocks;

        if (input && output)
        {
            std::copy_n(input, numSamples, output);
        }
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return kProbeType;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

    int monoBlocks = 0;
    int stereoBlocks = 0;
};

void RegisterEffects()
{
    using namespace guitarfx;
    static bool registered = false;

    if (registered)
    {
        return;
    }

    registered = true;
    RegisterAllEffects();

    EffectTypeInfo info;
    info.type = kProbeType;
    info.displayName = "Mono Path Probe";
    info.category = "utility";
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<MonoPathProbe>(); });
}

std::string CategoryOf(const std::string& type)
{
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(type);
    return info ? info->category : std::string{};
}

std::string NameOf(const std::string& type)
{
    const auto info = guitarfx::EffectRegistry::Instance().GetTypeInfo(type);
    return info ? info->displayName : type;
}

/// The same signal on both channels, as a guitar into a mono interface input gives.
std::vector<float> MonoInput(int samples)
{
    std::vector<float> signal(static_cast<size_t>(samples));

    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        signal[static_cast<size_t>(i)] =
            static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * t) + 0.1 * std::sin(2.0 * kPi * 1330.0 * t));
    }

    return signal;
}

double MaxChannelDifference(const std::vector<float>& left, const std::vector<float>& right)
{
    double difference = 0.0;

    for (size_t i = kWarmupSamples; i < left.size() && i < right.size(); ++i)
    {
        difference = std::max(difference, std::abs(static_cast<double>(left[i]) - right[i]));
    }

    return difference;
}

struct Stage
{
    std::string id;
    std::string type;
    Params params;
};

/// Input -> stages -> output, as the UI builds it: each node carries its registry category.
guitarfx::SignalGraph MakeChain(const std::vector<Stage>& stages)
{
    guitarfx::SignalGraph graph;
    graph.nodes.push_back({"in", "input", "", "Input", true});
    std::string previous = "in";

    for (const auto& stage : stages)
    {
        guitarfx::GraphNode node{stage.id, stage.type, CategoryOf(stage.type), NameOf(stage.type), true};
        node.params = stage.params;
        graph.nodes.push_back(std::move(node));
        graph.edges.push_back({previous, stage.id});
        previous = stage.id;
    }

    graph.nodes.push_back({"out", "output", "", "Output", true});
    graph.edges.push_back({previous, "out"});
    return graph;
}

struct ChainRun
{
    std::vector<float> left;
    std::vector<float> right;
    int probeMonoBlocks = 0;
    int probeStereoBlocks = 0;
};

using BeforeBlock = std::function<void(guitarfx::SignalGraphExecutor&, int)>;

/// Runs a mono input through the graph. `beforeBlock` may change a node between blocks.
ChainRun RunGraph(const guitarfx::SignalGraph& graph, const BeforeBlock& beforeBlock = {})
{
    RegisterEffects();

    guitarfx::SignalGraphExecutor executor;
    executor.SetInputLayout(guitarfx::ChannelLayout::Mono); // a guitar on one input
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);

    const auto input = MonoInput(kBlock * kBlocks);
    ChainRun run;
    run.left.resize(input.size());
    run.right.resize(input.size());

    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);

    for (int block = 0; block < kBlocks; ++block)
    {
        if (beforeBlock)
        {
            beforeBlock(executor, block);
        }

        const auto offset = static_cast<size_t>(block) * kBlock;
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inL.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inR.begin());

        float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {outL.data(), outR.data()};
        executor.Process(in, out, kBlock);

        std::copy(outL.begin(), outL.end(), run.left.begin() + static_cast<std::ptrdiff_t>(offset));
        std::copy(outR.begin(), outR.end(), run.right.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    if (const auto* probe = dynamic_cast<const MonoPathProbe*>(executor.GetNodeProcessor("probe")))
    {
        run.probeMonoBlocks = probe->monoBlocks;
        run.probeStereoBlocks = probe->stereoBlocks;
    }

    return run;
}

ChainRun RunChain(const std::vector<Stage>& stages, const BeforeBlock& beforeBlock = {})
{
    return RunGraph(MakeChain(stages), beforeBlock);
}

/// Runs the effect on its own, outside the executor, with the same signal on both inputs.
/// Returns how far apart its two outputs came, and whether its type declares it can widen.
std::pair<double, bool> DirectChannelDifference(const std::string& type, const Params& params)
{
    RegisterEffects();
    auto effect = guitarfx::EffectRegistry::Instance().Create(type);

    if (!effect)
    {
        Check(false, NameOf(type) + ": registry creates it");
        return {0.0, false};
    }

    effect->Prepare(kSampleRate, kBlock);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    const auto input = MonoInput(kBlock * kBlocks);
    std::vector<float> left(input.size()), right(input.size());
    std::vector<float> inL(kBlock), inR(kBlock);

    for (int block = 0; block < kBlocks; ++block)
    {
        const auto offset = static_cast<size_t>(block) * kBlock;
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inL.begin());
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), kBlock, inR.begin());

        float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {left.data() + offset, right.data() + offset};
        effect->Process(in, out, kBlock);
    }

    return {MaxChannelDifference(left, right), effect->CanWiden()};
}

struct ModulationCase
{
    const char* type;
    Params params;
    bool stereo; // whether a mono input should come out with different channels
    const char* label;
};

/// A type that can turn a mono input stereo under some settings declares it can widen; one that
/// never does declares it cannot.
bool ExpectedCanWiden(const std::string& type)
{
    return type == kDelayDoubler || type == kChorus || type == kFlanger || type == kTremolo || type == kRotary;
}

/// Each effect does what it says, and its type declares what it can do.
void TestEffectsDeclareWhatTheyDo()
{
    const std::vector<ModulationCase> cases = {
        {kDelayDoubler, {}, true, "doubler at defaults"},
        {kDelayDoubler, {{"mix", 0.0}}, false, "doubler with Mix at zero"},
        {kDelayDoubler, {{"time", 0.0}}, false, "doubler with Time at zero"},
        {kChorus, {}, true, "chorus at defaults"},
        {kChorus, {{"mix", 0.0}}, false, "chorus with Mix at zero"},
        {kChorus, {{"depth", 0.0}}, false, "chorus with Depth at zero"},
        {kChorus, {{"depth", 20.0}, {"feedback", 0.95}, {"mix", 1.0}}, true, "chorus at full depth"},
        {kFlanger, {}, true, "flanger at defaults"},
        {kFlanger, {{"mix", 0.0}}, false, "flanger with Mix at zero"},
        {kFlanger, {{"depth", 0.0}}, false, "flanger with Depth at zero"},
        {kPhaser, {}, false, "phaser at defaults"},
        {kPhaser, {{"depth", 1.0}, {"feedback", 0.95}, {"mix", 1.0}, {"rate", 8.0}}, false, "phaser pushed hard"},
        {kTremolo, {}, false, "tremolo at defaults"},
        {kTremolo, {{"depth", 1.0}, {"shape", 1.0}, {"rate", 12.0}}, false, "tremolo pushed hard"},
        {kTremolo, {{"mode", 1.0}, {"depth", 1.0}}, false, "tremolo on Harmonic"},
        {kTremolo, {{"mode", 3.0}, {"depth", 1.0}}, false, "tremolo on Slicer"},
        {kTremolo, {{"mode", 2.0}}, true, "tremolo on Pan"},
        {kTremolo, {{"mode", 2.0}, {"depth", 0.0}}, false, "tremolo on Pan with Depth at zero"},
        {kRotary, {}, true, "rotary at defaults"},
        {kRotary, {{"spread", 0.0}}, false, "rotary with Mic Spread at zero"},
        {kRotary, {{"depth", 0.0}}, false, "rotary with Depth at zero"},
        {kRotary, {{"mix", 0.0}}, false, "rotary with Mix at zero"},
        {kVibe, {}, false, "vibe at defaults"},
        {kVibe, {{"mode", 1.0}, {"intensity", 1.0}}, false, "vibe on Vibrato"},
        {kWah, {}, false, "wah at defaults"},
        {kWah, {{"control", 1.0}}, false, "wah on its Auto Wah control"},
    };

    for (const auto& c : cases)
    {
        const auto [difference, declared] = DirectChannelDifference(c.type, c.params);
        const std::string detail = " (channels " + std::to_string(difference) + " apart)";

        if (c.stereo)
        {
            Check(difference > kDistinct, std::string(c.label) + ": a mono input comes out stereo" + detail);
        }
        else
        {
            Check(difference < kIdentical, std::string(c.label) + ": a mono input stays mono" + detail);
        }

        Check(declared == ExpectedCanWiden(c.type), std::string(c.label) + ": the type declares " +
                                                        (ExpectedCanWiden(c.type) ? "it can" : "it cannot") + " widen");
    }
}

/// The bug: a chorus or flanger on a mono input reached the output with identical channels,
/// whether a mono-capable node followed it or nothing did.
void TestChorusAndFlangerReachTheOutputStereo()
{
    for (const char* modulation : {kChorus, kFlanger})
    {
        const std::string name = NameOf(modulation);

        const auto alone = RunChain({{"mod", modulation, {}}});
        Check(MaxChannelDifference(alone.left, alone.right) > kDistinct, name + " -> output keeps its stereo");

        for (const char* follower : {kAmpBuiltin, kOverdrive})
        {
            const auto run = RunChain({{"mod", modulation, {}}, {"next", follower, {}}});
            const double difference = MaxChannelDifference(run.left, run.right);
            Check(difference > kDistinct, name + " -> " + NameOf(follower) + " keeps its stereo (channels " +
                                              std::to_string(difference) + " apart)");
        }

        const auto probed = RunChain({{"mod", modulation, {}}, {"probe", kProbeType, {}}});
        Check(probed.probeStereoBlocks == kBlocks && probed.probeMonoBlocks == 0,
              name + " -> mono-capable node runs its stereo path (mono " + std::to_string(probed.probeMonoBlocks) +
                  ", stereo " + std::to_string(probed.probeStereoBlocks) + ")");
    }
}

/// On a mono rig the output node used to copy the global doubler's left channel over its right: a
/// comb filter on both sides instead of width.
void TestGlobalPostChainDoublerReachesTheOutputStereo()
{
    auto graph = guitarfx::GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    auto doubler = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                [](const guitarfx::GraphNode& node) { return node.type == kDelayDoubler; });

    if (doubler == graph.nodes.end())
    {
        Check(false, "the default global post chain has a doubler");
        return;
    }

    doubler->enabled = true;

    const auto run = RunGraph(graph);
    const double difference = MaxChannelDifference(run.left, run.right);
    Check(difference > kDistinct,
          "global post-chain doubler keeps its stereo (channels " + std::to_string(difference) + " apart)");
}

/// Registers a composite (custom effect) wrapping `stages`.
std::string RegisterComposite(const std::string& id, const std::vector<Stage>& stages)
{
    RegisterEffects();
    auto& registry = guitarfx::EffectRegistry::Instance();

    if (!registry.HasType(id))
    {
        guitarfx::CompositeEffectDefinition definition;
        definition.id = id;
        definition.name = id;
        definition.category = "utility";
        definition.innerGraph = MakeChain(stages);

        guitarfx::EffectTypeInfo info;
        info.type = id;
        info.displayName = id;
        info.category = definition.category;
        registry.Register(info.type, info,
                          [definition]() { return std::make_unique<guitarfx::CompositeEffectProcessor>(definition); });
    }

    return id;
}

/// A chorus inside a composite was merged back to mono by the parent graph, which saw only the
/// composite, and the composite never said its output was stereo.
void TestCompositeKeepsItsInteriorStereo()
{
    const auto chorus = RegisterComposite("test-composite-chorus", {{"mod", kChorus, {}}});

    const auto alone = RunChain({{"composite", chorus, {}}});
    Check(MaxChannelDifference(alone.left, alone.right) > kDistinct, "composite chorus -> output keeps its stereo");

    const auto amped = RunChain({{"composite", chorus, {}}, {"next", kAmpBuiltin, {}}});
    const double difference = MaxChannelDifference(amped.left, amped.right);
    Check(difference > kDistinct, "composite chorus -> " + NameOf(kAmpBuiltin) + " keeps its stereo (channels " +
                                      std::to_string(difference) + " apart)");

    const auto probed = RunChain({{"composite", chorus, {}}, {"probe", kProbeType, {}}});
    Check(probed.probeStereoBlocks == kBlocks && probed.probeMonoBlocks == 0,
          "composite chorus -> mono-capable node runs its stereo path (mono " + std::to_string(probed.probeMonoBlocks) +
              ", stereo " + std::to_string(probed.probeStereoBlocks) + ")");

    // And it widens no more than its interior can.
    const auto phaser = RegisterComposite("test-composite-phaser", {{"mod", kPhaser, {}}});
    const auto mono = RunChain({{"composite", phaser, {}}, {"probe", kProbeType, {}}});
    Check(mono.probeMonoBlocks == kBlocks && mono.probeStereoBlocks == 0,
          "composite phaser leaves the next node mono (mono " + std::to_string(mono.probeMonoBlocks) + ", stereo " +
              std::to_string(mono.probeStereoBlocks) + ")");
}

/// A widening type keeps what follows it stereo even while it is not widening, so turning it up
/// later needs nothing to switch. The channels are still identical: stereo with the same samples.
void TestWideningTypesKeepWhatFollowsStereo()
{
    const std::vector<std::pair<const char*, Params>> settings = {
        {kChorus, {{"mix", 0.0}}},
        {kChorus, {{"depth", 0.0}}},
        {kFlanger, {{"mix", 0.0}}},
        {kFlanger, {{"depth", 0.0}}},
        {kTremolo, {}},
        {kRotary, {{"spread", 0.0}}},
    };

    for (const auto& [type, params] : settings)
    {
        const auto run = RunChain({{"mod", type, params}, {"probe", kProbeType, {}}});
        const std::string what = NameOf(type) + (params.empty() ? "" : " with " + params.begin()->first + " at zero");
        Check(run.probeStereoBlocks == kBlocks && run.probeMonoBlocks == 0,
              what + " keeps the next node stereo (mono " + std::to_string(run.probeMonoBlocks) + ", stereo " +
                  std::to_string(run.probeStereoBlocks) + ")");
        Check(MaxChannelDifference(run.left, run.right) < kIdentical, what + " still puts out identical sides");
    }
}

/// Phaser, vibe and the wah on either control leave a following amp on its mono path.
void TestMonoModulationKeepsTheMonoPath()
{
    const std::vector<std::pair<const char*, Params>> stages = {
        {kPhaser, {}}, {kVibe, {}}, {kWah, {}}, {kWah, {{"control", 1.0}}}};

    for (const auto& [type, params] : stages)
    {
        const auto run = RunChain({{"mod", type, params}, {"probe", kProbeType, {}}});
        Check(run.probeMonoBlocks == kBlocks && run.probeStereoBlocks == 0,
              NameOf(type) + (params.empty() ? "" : " (Auto Wah)") + " leaves the next node mono (mono " +
                  std::to_string(run.probeMonoBlocks) + ", stereo " + std::to_string(run.probeStereoBlocks) + ")");
    }
}

/// Mix automated up from zero is heard in the block it lands in, and the node after the chorus
/// never changes path: it was stereo all along.
void TestAutomatedMixNeverSwitchesThePath()
{
    constexpr int kUpAt = 10;
    int monoBlocks = -1;

    const auto run =
        RunChain({{"mod", kChorus, {{"mix", 0.0}}}, {"probe", kProbeType, {}}},
                 [&](guitarfx::SignalGraphExecutor& executor, int block) {
                     if (const auto* probe = dynamic_cast<const MonoPathProbe*>(executor.GetNodeProcessor("probe")))
                     {
                         monoBlocks = probe->monoBlocks;
                     }

                     if (block == kUpAt)
                     {
                         if (auto* chorus = executor.GetNodeProcessor("mod"))
                         {
                             chorus->SetParam("mix", 0.5);
                         }
                     }
                 });

    Check(monoBlocks == 0 && run.probeStereoBlocks == kBlocks, "the node after the chorus never runs mono");

    const auto upAt = static_cast<size_t>(kUpAt) * kBlock;
    std::vector<float> beforeL(run.left.begin(), run.left.begin() + static_cast<std::ptrdiff_t>(upAt));
    std::vector<float> beforeR(run.right.begin(), run.right.begin() + static_cast<std::ptrdiff_t>(upAt));
    double after = 0.0;

    for (size_t i = upAt + kBlock; i < run.left.size(); ++i)
    {
        after = std::max(after, std::abs(static_cast<double>(run.left[i]) - run.right[i]));
    }

    Check(MaxChannelDifference(beforeL, beforeR) < kIdentical, "with Mix at zero the sides are identical");
    Check(after > kDistinct, "Mix turned up widens at once (channels " + std::to_string(after) + " apart)");
}
} // namespace

int main()
{
    TestEffectsDeclareWhatTheyDo();
    TestChorusAndFlangerReachTheOutputStereo();
    TestGlobalPostChainDoublerReachesTheOutputStereo();
    TestCompositeKeepsItsInteriorStereo();
    TestWideningTypesKeepWhatFollowsStereo();
    TestMonoModulationKeepsTheMonoPath();
    TestAutomatedMixNeverSwitchesThePath();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "ModulationStereoTests passed\n";
    return 0;
}
