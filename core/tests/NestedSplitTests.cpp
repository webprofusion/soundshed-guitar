/**
 * @file NestedSplitTests.cpp
 * @brief A split inside a branch of another split: edited on its own, and run as its branch feeds it.
 *
 *   - the inner split grows and shrinks on its own mixer, leaving the outer split alone
 *   - the outer split counts a branch holding the inner split as in use
 *   - collapsing the inner split moves its one used branch into the outer branch, on the outer
 *     mixer's input for that branch
 *   - in Copy, nested splitters sound exactly like plain passthrough splits
 *   - Switch picks one path through both levels, and moving the outer switch silences the inner
 *     split's branches
 *   - Stereo inside Stereo: the inner split gets the one channel its branch was given
 *
 * SplitBranchTests and SplitterRoutingTests cover one level; ChainLayout's nested walk is in
 * UiClientTests and the web UI's in nestedSplitRender.test.ts.
 */

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "GuitarPhraseSynth.h"
#include "controller/internal/SplitBranchSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

namespace
{
using guitarfx::ChannelLayout;
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
using guitarfx::SignalGraphExecutor;
namespace split = guitarfx::controller_detail;

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += ok ? 0 : 1;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

GraphNode Node(const std::string& id, const std::string& type)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    return node;
}

GraphEdge Edge(const std::string& from, const std::string& to, int fromPort = 0, int toPort = 0)
{
    GraphEdge edge;
    edge.from = from;
    edge.to = to;
    edge.fromPort = fromPort;
    edge.toPort = toPort;
    return edge;
}

bool HasEdge(const SignalGraph& graph, const std::string& from, const std::string& to, int toPort = -1)
{
    for (const auto& edge : graph.edges)
    {
        if (edge.from == from && edge.to == to && (toPort < 0 || edge.toPort == toPort))
        {
            return true;
        }
    }

    return false;
}

// ── Editing ───────────────────────────────────────────────────────────────────────────────

/// split → [ | | amp → innerSplit → [ | cab ] → innerMix ] → mix → output.
SignalGraph NestedInThirdBranch()
{
    using namespace guitarfx::EffectGuids;
    SignalGraph graph;
    graph.nodes = {Node("__input__", "input"),    Node("split", kSplitter),    Node("amp", "amp"),
                   Node("innerSplit", kSplitter), Node("cab", "cab"),          Node("innerMix", kMixer),
                   Node("mix", kMixer),           Node("__output__", "output")};
    graph.edges = {Edge("__input__", "split"),   Edge("split", "mix", 0, 0),    Edge("split", "mix", 1, 1),
                   Edge("split", "amp", 2),      Edge("amp", "innerSplit"),     Edge("innerSplit", "innerMix", 0, 0),
                   Edge("innerSplit", "cab", 1), Edge("cab", "innerMix", 0, 1), Edge("innerMix", "mix", 0, 2),
                   Edge("mix", "__output__")};
    return graph;
}

void TestEditing()
{
    std::cout << "\nEditing" << std::endl;

    auto grown = NestedInThirdBranch();
    Check(split::SetSplitBranchCount(grown, "innerSplit", 3).changed &&
              split::CountSplitBranches(grown, "innerSplit") == 3 && HasEdge(grown, "innerSplit", "innerMix", 2),
          "the inner split grows a branch on its own mixer");
    Check(split::CountSplitBranches(grown, "split") == 3 && split::FindSplitJoinMixerId(grown, "split") == "mix",
          "and the outer split is untouched");

    auto shrunk = NestedInThirdBranch();
    Check(split::SetSplitBranchCount(shrunk, "split", 2).changed && split::CountSplitBranches(shrunk, "split") == 2 &&
              HasEdge(shrunk, "split", "amp") && shrunk.FindNode("innerSplit") != nullptr,
          "the outer split drops an empty branch, keeping the one with the inner split");
    const auto beforeRefusal = shrunk;
    const auto refused = split::SetSplitBranchCount(shrunk, "split", 1);
    Check(!refused.changed && shrunk == beforeRefusal, "and goes no lower");

    auto collapsed = NestedInThirdBranch();
    Check(split::CollapseSplit(collapsed, "innerSplit", "innerMix").changed &&
              collapsed.FindNode("innerSplit") == nullptr && collapsed.FindNode("innerMix") == nullptr &&
              HasEdge(collapsed, "amp", "cab") && HasEdge(collapsed, "cab", "mix", 2),
          "the inner split collapses into its branch: amp → cab on the outer mixer's third input");
    Check(split::FindSplitJoinMixerId(collapsed, "split") == "mix" &&
              split::CountSplitBranches(collapsed, "split") == 3,
          "the outer split is still whole");
}

// ── Running ───────────────────────────────────────────────────────────────────────────────

/// in → outer → [ inner → [ p | q ] → innerMix | r ] → outerMix → out
SignalGraph NestedGraph(const std::string& splitType)
{
    using namespace guitarfx::EffectGuids;
    SignalGraph graph;
    graph.nodes = {Node("in", guitarfx::kNodeTypeInput),
                   Node("outer", splitType),
                   Node("inner", splitType),
                   Node("p", kGain),
                   Node("q", kGain),
                   Node("innerMix", guitarfx::kNodeTypeMixer),
                   Node("r", kGain),
                   Node("outerMix", guitarfx::kNodeTypeMixer),
                   Node("out", guitarfx::kNodeTypeOutput)};
    graph.edges = {Edge("in", "outer"),      Edge("outer", "inner", 0, 0),       Edge("inner", "p", 0, 0),
                   Edge("inner", "q", 1, 0), Edge("p", "innerMix", 0, 0),        Edge("q", "innerMix", 0, 1),
                   Edge("outer", "r", 1, 0), Edge("innerMix", "outerMix", 0, 0), Edge("r", "outerMix", 0, 1),
                   Edge("outerMix", "out")};
    return graph;
}

std::vector<float> RunGraph(const SignalGraph& graph, const std::vector<float>& x, ChannelLayout layout)
{
    SignalGraphExecutor executor;
    executor.SetInputLayout(layout);
    executor.SetGraph(graph);
    executor.Prepare(kRate, kBlock);
    std::vector<float> out(x.size() * 2, 0.0f);
    std::vector<float> l(kBlock), r(kBlock), ol(kBlock), orr(kBlock);

    for (std::size_t start = 0; start + kBlock <= x.size(); start += kBlock)
    {
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), kBlock, l.begin());
        std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), kBlock, r.begin());
        float* ins[2] = {l.data(), r.data()};
        float* outs[2] = {ol.data(), orr.data()};
        executor.Process(ins, outs, kBlock);
        std::copy_n(ol.begin(), kBlock, out.begin() + static_cast<std::ptrdiff_t>(2 * start));
        std::copy_n(orr.begin(), kBlock, out.begin() + static_cast<std::ptrdiff_t>(2 * start + kBlock));
    }

    return out;
}

void TestCopyUnchanged()
{
    std::cout << "\nCopy" << std::endl;
    const auto x = guitarfx::test::RenderPhrase({{0.05, 0.4, 110.0, 0.3}, {0.5, 0.4, 330.0, 0.2}}, 1.0, kRate);

    for (const auto layout : {ChannelLayout::Mono, ChannelLayout::Stereo})
    {
        // An unregistered type gets a passthrough: what a splitter was before it had modes.
        const auto routed = RunGraph(NestedGraph(guitarfx::EffectGuids::kSplitter), x, layout);
        const auto plain = RunGraph(NestedGraph("test-passthrough-split"), x, layout);
        Check(routed == plain, std::string(layout == ChannelLayout::Mono ? "mono" : "stereo") +
                                   ": sample-identical to plain passthrough splits");
    }
}

void TestRouting()
{
    std::cout << "\nRouting through both levels" << std::endl;
    auto graph = NestedGraph(guitarfx::EffectGuids::kSplitter);
    graph.FindNode("outer")->params = {{"mode", 2.0}, {"branch", 0.0}};
    graph.FindNode("inner")->params = {{"mode", 2.0}, {"branch", 1.0}};

    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Stereo);
    executor.SetGraph(graph);
    executor.Prepare(kRate, kBlock);
    std::vector<float> l(kBlock, 0.1f), r(kBlock, -0.1f), ol(kBlock), orr(kBlock);
    float* ins[2] = {l.data(), r.data()};
    float* outs[2] = {ol.data(), orr.data()};

    // Each node's peak and channel count after a run of blocks.
    const auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i)
        {
            executor.Process(ins, outs, kBlock);
        }

        std::map<std::string, std::pair<double, int>> found;

        for (const auto& level : executor.GetNodeSignalLevels())
        {
            found[level.nodeId] = {level.peak, level.channelCount};
        }

        return found;
    };

    // Long enough for a 10 ms switch crossfade to finish.
    const int settle = static_cast<int>(std::ceil(0.02 * kRate / kBlock));
    auto heard = run(settle);
    Check(heard["q"].first > 0.05 && heard["p"].first == 0.0 && heard["r"].first == 0.0,
          "Switch on outer branch 1 and inner branch 2: only that path plays",
          "p " + Num(heard["p"].first) + ", q " + Num(heard["q"].first) + ", r " + Num(heard["r"].first));
    // Each mixer's centre pan is equal power, -3 dB a side, so two in a row halve it.
    Check(std::abs(ol.back() - 0.05f) < 1.0e-4f && std::abs(orr.back() + 0.05f) < 1.0e-4f,
          "and reaches the output through both mixers, each at its centre pan",
          Num(ol.back()) + " / " + Num(orr.back()));

    executor.SetNodeParam("outer", "branch", 1.0);
    heard = run(settle);
    Check(heard["r"].first > 0.05 && heard["q"].first == 0.0 && heard["p"].first == 0.0,
          "the outer switch moved to branch 2 silences the whole inner split");

    executor.SetNodeParam("outer", "mode", 5.0);
    executor.SetNodeParam("inner", "mode", 5.0);
    heard = run(2);
    Check(heard["inner"].second == 1 && heard["p"].second == 1 && heard["q"].second == 1 && heard["r"].second == 1,
          "Stereo inside Stereo: the inner split gets one channel and its branches run mono");
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();
    std::cout << "=== Nested split tests ===" << std::endl;

    TestEditing();
    TestCopyUnchanged();
    TestRouting();

    std::cout << "\n" << (gChecks - gFailures) << " of " << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
