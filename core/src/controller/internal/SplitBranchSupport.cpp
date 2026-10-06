#include "controller/internal/SplitBranchSupport.h"

#include "dsp/EffectGuids.h"
#include "dsp/effects/MixerEffect.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <set>
#include <vector>

namespace guitarfx::controller_detail
{
static_assert(kMaxSplitBranches == MixerEffect::kMaxInputs, "a split has one branch per mixer input");

namespace
{
/// Deep enough for any split a person builds; it only stops a malformed graph recursing forever.
constexpr int kMaxNestedSplits = 16;
/// Longer than any branch, short enough that a cycle ends the walk.
constexpr int kMaxBranchLength = 500;

const GraphEdge* FirstOutgoingEdge(const SignalGraph& graph, const std::string& nodeId)
{
    const GraphEdge* first = nullptr;

    for (const auto& edge : graph.edges)
    {
        if (edge.from == nodeId && (!first || edge.fromPort < first->fromPort))
        {
            first = &edge;
        }
    }

    return first;
}

std::string FindJoin(const SignalGraph& graph, const std::string& splitterId, int depth)
{
    const GraphNode* splitter = graph.FindNode(splitterId);

    if (!splitter || !IsSplitterNode(*splitter) || depth > kMaxNestedSplits)
    {
        return {};
    }

    std::string join;
    bool anyBranch = false;

    for (const auto& branch : graph.edges)
    {
        if (branch.from != splitterId)
        {
            continue;
        }

        anyBranch = true;
        std::string current = branch.to;
        std::string reached;

        for (int step = 0; step < kMaxBranchLength && !current.empty(); ++step)
        {
            const GraphNode* node = graph.FindNode(current);

            if (!node)
            {
                break;
            }

            if (IsMixerNode(*node))
            {
                reached = current;
                break;
            }

            // A split inside a branch: skip to past its own mixer, or that mixer would read as ours.
            const std::string from = IsSplitterNode(*node) ? FindJoin(graph, current, depth + 1) : current;

            if (from.empty())
            {
                break;
            }

            const GraphEdge* next = FirstOutgoingEdge(graph, from);
            current = next ? next->to : std::string{};
        }

        if (reached.empty() || (!join.empty() && reached != join))
        {
            return {};
        }

        join = reached;
    }

    return anyBranch ? join : std::string{};
}

void ClearMixerInputParams(GraphNode& mixer, int port)
{
    const std::string suffix = std::to_string(port);

    for (const char* prefix : {"level_", "pan_", "delay_", "mute_"})
    {
        mixer.params.erase(prefix + suffix);
    }
}
} // namespace

bool IsSplitterNode(const GraphNode& node)
{
    return node.type == EffectGuids::kSplitter || node.type == "splitter";
}

bool IsMixerNode(const GraphNode& node)
{
    return node.type == EffectGuids::kMixer || node.type == "mixer";
}

std::string FindSplitJoinMixerId(const SignalGraph& graph, const std::string& splitterId)
{
    return FindJoin(graph, splitterId, 0);
}

int CountSplitBranches(const SignalGraph& graph, const std::string& splitterId)
{
    return static_cast<int>(std::count_if(graph.edges.begin(), graph.edges.end(),
                                          [&](const GraphEdge& edge) { return edge.from == splitterId; }));
}

SplitBranchChange SetSplitBranchCount(SignalGraph& graph, const std::string& splitterId, int count)
{
    const GraphNode* splitter = graph.FindNode(splitterId);

    if (!splitter || !IsSplitterNode(*splitter))
    {
        return {false, "That node is not a splitter"};
    }

    if (count < kMinSplitBranches || count > kMaxSplitBranches)
    {
        return {false, "A split has " + std::to_string(kMinSplitBranches) + " to " + std::to_string(kMaxSplitBranches) +
                           " branches"};
    }

    const std::string mixerId = FindSplitJoinMixerId(graph, splitterId);
    GraphNode* mixer = mixerId.empty() ? nullptr : graph.FindNode(mixerId);

    if (!mixer)
    {
        return {false, "The split's branches do not meet at one mixer"};
    }

    const int current = CountSplitBranches(graph, splitterId);

    if (count == current)
    {
        return {};
    }

    if (count > current)
    {
        std::set<int> usedInputs;
        int lastBranchPort = -1;

        for (const auto& edge : graph.edges)
        {
            if (edge.to == mixerId)
            {
                usedInputs.insert(edge.toPort);
            }

            if (edge.from == splitterId)
            {
                lastBranchPort = std::max(lastBranchPort, edge.fromPort);
            }
        }

        std::vector<int> freeInputs;

        for (int port = 0; port < MixerEffect::kMaxInputs && static_cast<int>(freeInputs.size()) < count - current;
             ++port)
        {
            if (!usedInputs.contains(port))
            {
                freeInputs.push_back(port);
            }
        }

        if (static_cast<int>(freeInputs.size()) < count - current)
        {
            return {false, "The mixer has no free input for another branch"};
        }

        for (const int input : freeInputs)
        {
            GraphEdge branch;
            branch.from = splitterId;
            branch.to = mixerId;
            branch.fromPort = ++lastBranchPort;
            branch.toPort = input;
            branch.gain = 1.0;
            graph.edges.push_back(branch);
            // A new branch starts at unity, not at whatever an earlier one on this input was set to.
            ClearMixerInputParams(*mixer, input);
        }

        return {true, {}};
    }

    std::vector<const GraphEdge*> emptyBranches;

    for (const auto& edge : graph.edges)
    {
        if (edge.from == splitterId && edge.to == mixerId)
        {
            emptyBranches.push_back(&edge);
        }
    }

    const auto toRemove = static_cast<std::size_t>(current - count);

    if (emptyBranches.size() < toRemove)
    {
        return {false, "Only an empty branch can be removed: take the effects off a branch first"};
    }

    std::sort(emptyBranches.begin(), emptyBranches.end(),
              [](const GraphEdge* a, const GraphEdge* b) { return a->fromPort > b->fromPort; });

    std::vector<GraphEdge> removed;

    for (std::size_t i = 0; i < toRemove; ++i)
    {
        removed.push_back(*emptyBranches[i]);
    }

    graph.edges.erase(std::remove_if(graph.edges.begin(), graph.edges.end(),
                                     [&](const GraphEdge& edge) {
                                         return std::any_of(removed.begin(), removed.end(), [&](const GraphEdge& r) {
                                             return edge.from == r.from && edge.to == r.to &&
                                                    edge.fromPort == r.fromPort && edge.toPort == r.toPort;
                                         });
                                     }),
                      graph.edges.end());

    for (const auto& branch : removed)
    {
        ClearMixerInputParams(*mixer, branch.toPort);
    }

    return {true, {}};
}
} // namespace guitarfx::controller_detail
