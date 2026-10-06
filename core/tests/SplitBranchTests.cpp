/**
 * @file SplitBranchTests.cpp
 * @brief A split's branch count: found from the graph, and changed without losing an effect.
 *
 * controller/internal/SplitBranchSupport.h backs `setSplitBranchCount` and lets
 * `collapseSignalPathSplit` find a split's mixer itself. These check:
 *   - the join mixer is found through effects on the branches and past a split nested in one
 *   - growing a split adds empty branches on free mixer inputs, up to the mixer's four
 *   - shrinking removes only empty branches, the last first, and clears that input's params
 *   - every refusal leaves the graph exactly as it was
 */

#include <iostream>
#include <string>

#include "controller/internal/SplitBranchSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/MixerEffect.h"
#include "presets/PresetTypes.h"

namespace
{
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
namespace split = guitarfx::controller_detail;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what)
{
    ++gChecks;

    if (!ok)
    {
        ++gFailures;
        std::cout << "  FAIL: " << what << std::endl;
    }
    else
    {
        std::cout << "  ok: " << what << std::endl;
    }
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

/// Deathcore Three-Amp Blend's shape: gate → split → [amp | amp → cab | amp → cab] → mix → output.
SignalGraph ThreeAmpBlend()
{
    SignalGraph graph;
    graph.nodes = {Node("__input__", "input"),  Node("gate", "gate"),   Node("split", guitarfx::EffectGuids::kSplitter),
                   Node("amp5150", "amp"),      Node("ampUber", "amp"), Node("cabUber", "cab"),
                   Node("ampArchon", "amp"),    Node("cabMesa", "cab"), Node("mix", guitarfx::EffectGuids::kMixer),
                   Node("__output__", "output")};
    graph.edges = {Edge("__input__", "gate"),    Edge("gate", "split"),         Edge("split", "amp5150", 0),
                   Edge("amp5150", "mix", 0, 0), Edge("split", "ampUber", 1),   Edge("ampUber", "cabUber"),
                   Edge("cabUber", "mix", 0, 1), Edge("split", "ampArchon", 2), Edge("ampArchon", "cabMesa"),
                   Edge("cabMesa", "mix", 0, 2), Edge("mix", "__output__")};
    return graph;
}

/// A fresh split as splitSignalPathEdge makes it: two empty branches.
SignalGraph EmptySplit()
{
    SignalGraph graph;
    graph.nodes = {Node("__input__", "input"), Node("split", guitarfx::EffectGuids::kSplitter),
                   Node("mix", guitarfx::EffectGuids::kMixer), Node("__output__", "output")};
    graph.edges = {Edge("__input__", "split"), Edge("split", "mix", 0, 0), Edge("split", "mix", 1, 1),
                   Edge("mix", "__output__")};
    return graph;
}

int EdgesInto(const SignalGraph& graph, const std::string& id, int toPort)
{
    int count = 0;

    for (const auto& edge : graph.edges)
    {
        count += edge.to == id && edge.toPort == toPort ? 1 : 0;
    }

    return count;
}

void TestFindJoin()
{
    std::cout << "\n-- finding the mixer a split joins at --" << std::endl;
    const auto blend = ThreeAmpBlend();
    Check(split::FindSplitJoinMixerId(blend, "split") == "mix", "through effects on every branch");
    Check(split::CountSplitBranches(blend, "split") == 3, "Deathcore Three-Amp Blend has three branches");
    Check(split::FindSplitJoinMixerId(blend, "gate").empty(), "nothing for a node that is not a splitter");

    // Wrap the 5150 branch's amp in a split of its own: its mixer is not the outer join.
    auto nested = blend;
    nested.nodes.push_back(Node("innerSplit", guitarfx::EffectGuids::kSplitter));
    nested.nodes.push_back(Node("innerMix", guitarfx::EffectGuids::kMixer));
    nested.edges.erase(nested.edges.begin() + 2);
    nested.edges.push_back(Edge("split", "innerSplit", 0));
    nested.edges.push_back(Edge("innerSplit", "innerMix", 0, 0));
    nested.edges.push_back(Edge("innerSplit", "innerMix", 1, 1));
    nested.edges.push_back(Edge("innerMix", "amp5150"));
    Check(split::FindSplitJoinMixerId(nested, "split") == "mix", "past a split nested in a branch");
    Check(split::FindSplitJoinMixerId(nested, "innerSplit") == "innerMix", "the nested split's own mixer");

    auto diverged = blend;
    diverged.nodes.push_back(Node("otherMix", guitarfx::EffectGuids::kMixer));
    diverged.edges.back() = Edge("mix", "otherMix");
    diverged.edges[6] = Edge("cabUber", "otherMix", 0, 1);
    Check(split::FindSplitJoinMixerId(diverged, "split").empty(), "nothing when the branches meet at different mixers");
}

void TestGrow()
{
    std::cout << "\n-- adding branches --" << std::endl;
    auto graph = EmptySplit();
    graph.FindNode("mix")->params = {{"level_2", -9.0}, {"pan_2", 0.5}, {"level_0", -3.0}};

    const auto change = split::SetSplitBranchCount(graph, "split", 4);
    Check(change.changed && change.error.empty(), "two to four");
    Check(split::CountSplitBranches(graph, "split") == 4, "the split has four branches");
    Check(EdgesInto(graph, "mix", 2) == 1 && EdgesInto(graph, "mix", 3) == 1, "on the mixer's free inputs 2 and 3");
    Check(split::FindSplitJoinMixerId(graph, "split") == "mix", "still joining at the same mixer");

    const auto& mixParams = graph.FindNode("mix")->params;
    Check(!mixParams.contains("level_2") && !mixParams.contains("pan_2"), "a new branch starts at unity, centred");
    Check(mixParams.contains("level_0"), "an existing branch keeps its level");

    const auto before = graph;
    const auto tooMany = split::SetSplitBranchCount(graph, "split", guitarfx::MixerEffect::kMaxInputs + 1);
    Check(!tooMany.changed && !tooMany.error.empty() && graph == before, "five is refused, the graph untouched");

    const auto same = split::SetSplitBranchCount(graph, "split", 4);
    Check(!same.changed && same.error.empty() && graph == before, "the count it already has is no change");

    // A mixer fed from elsewhere too has fewer inputs to give.
    auto shared = EmptySplit();
    shared.nodes.push_back(Node("side", "gain"));
    shared.edges.push_back(Edge("side", "mix", 0, 2));
    shared.edges.push_back(Edge("side", "mix", 0, 3));
    const auto sharedBefore = shared;
    const auto full = split::SetSplitBranchCount(shared, "split", 3);
    Check(!full.changed && !full.error.empty() && shared == sharedBefore, "refused when the mixer has no free input");
}

void TestShrink()
{
    std::cout << "\n-- removing branches --" << std::endl;
    auto blend = ThreeAmpBlend();
    const auto before = blend;
    const auto occupied = split::SetSplitBranchCount(blend, "split", 2);
    Check(!occupied.changed && !occupied.error.empty() && blend == before,
          "Deathcore Three-Amp Blend cannot drop a branch: every one holds an amp");

    auto graph = ThreeAmpBlend();
    split::SetSplitBranchCount(graph, "split", 4);
    graph.FindNode("mix")->params = {{"level_3", -6.0}, {"mute_3", 1.0}, {"level_1", -2.0}};
    const auto change = split::SetSplitBranchCount(graph, "split", 3);
    Check(change.changed && change.error.empty(), "four back to three");
    Check(split::CountSplitBranches(graph, "split") == 3 && EdgesInto(graph, "mix", 3) == 0,
          "the empty branch went, and its mixer input with it");
    Check(!graph.FindNode("mix")->params.contains("level_3") && !graph.FindNode("mix")->params.contains("mute_3"),
          "that input's level and mute are cleared");
    Check(graph.FindNode("mix")->params.contains("level_1"), "the other inputs keep theirs");
    Check(graph.edges == before.edges, "the wiring is as it was before the branch was added");

    auto empty = EmptySplit();
    split::SetSplitBranchCount(empty, "split", 4);
    split::SetSplitBranchCount(empty, "split", 2);
    Check(split::CountSplitBranches(empty, "split") == 2 && EdgesInto(empty, "mix", 0) == 1 &&
              EdgesInto(empty, "mix", 1) == 1,
          "the last branches go first");

    const auto belowTwo = split::SetSplitBranchCount(empty, "split", 1);
    Check(!belowTwo.changed && !belowTwo.error.empty(), "one branch is refused: that is not a split");
}
} // namespace

int main()
{
    std::cout << "=== SplitBranchTests ===" << std::endl;

    TestFindJoin();
    TestGrow();
    TestShrink();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
