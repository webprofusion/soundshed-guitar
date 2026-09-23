/**
 * ChainLayout.cpp - Walking a signal graph into a drawable row (see ChainLayout.h).
 */

#include "uiclient/ChainLayout.h"

#include <algorithm>
#include <map>
#include <set>

namespace guitarfx::uiclient
{
namespace
{
struct Graph
{
    std::map<std::string, const GraphNode*> nodeById;
    std::map<std::string, std::vector<GraphEdge>> outgoing;

    explicit Graph(const SignalGraph& graph)
    {
        for (const auto& node : graph.nodes)
        {
            nodeById[node.id] = &node;
        }

        for (const auto& edge : graph.edges)
        {
            if (!edge.from.empty() && !edge.to.empty())
            {
                outgoing[edge.from].push_back(edge);
            }
        }

        // Stable order, as the web UI's sortEdgesByPort.
        for (auto& [id, edges] : outgoing)
        {
            std::sort(edges.begin(), edges.end(), [](const GraphEdge& a, const GraphEdge& b) {
                if (a.fromPort != b.fromPort)
                {
                    return a.fromPort < b.fromPort;
                }

                if (a.toPort != b.toPort)
                {
                    return a.toPort < b.toPort;
                }

                return a.to < b.to;
            });
        }
    }

    [[nodiscard]] const std::vector<GraphEdge>& Out(const std::string& id) const
    {
        static const std::vector<GraphEdge> none;
        const auto it = outgoing.find(id);
        return it == outgoing.end() ? none : it->second;
    }

    [[nodiscard]] const GraphEdge* Primary(const std::string& id) const
    {
        const auto& outs = Out(id);

        if (outs.empty())
        {
            return nullptr;
        }

        for (const auto& edge : outs)
        {
            if (edge.fromPort == 0)
            {
                return &edge;
            }
        }

        return &outs.front();
    }

    [[nodiscard]] const GraphNode* Node(const std::string& id) const
    {
        const auto it = nodeById.find(id);
        return it == nodeById.end() ? nullptr : it->second;
    }

    [[nodiscard]] bool IsSplitPoint(const std::string& id) const
    {
        return Out(id).size() >= 2;
    }
};

std::vector<std::string> WalkBranch(const Graph& graph, const std::string& start)
{
    std::vector<std::string> path;
    std::set<std::string> visited;
    std::string current = start;

    for (int guard = 0; !current.empty() && !visited.contains(current) && guard < 500; ++guard)
    {
        visited.insert(current);

        if (current == kOutputNodeId)
        {
            break;
        }

        path.push_back(current);
        const auto* edge = graph.Primary(current);

        if (edge == nullptr)
        {
            break;
        }

        current = edge->to;
    }

    return path;
}

/// The first node downstream of a split that every branch reaches again.
std::string FindJoin(const Graph& graph, const std::string& splitId)
{
    const auto& outs = graph.Out(splitId);

    if (outs.size() < 2)
    {
        return {};
    }

    std::vector<std::vector<std::string>> paths;

    for (const auto& edge : outs)
    {
        if (!edge.to.empty() && edge.to != kOutputNodeId)
        {
            paths.push_back(WalkBranch(graph, edge.to));
        }
    }

    if (paths.size() < 2)
    {
        return {};
    }

    std::set<std::string> candidates(paths.front().begin(), paths.front().end());

    for (std::size_t i = 1; i < paths.size(); ++i)
    {
        const std::set<std::string> other(paths[i].begin(), paths[i].end());
        std::erase_if(candidates, [&other](const std::string& id) { return !other.contains(id); });
    }

    candidates.erase(splitId);

    for (const auto& id : paths.front())
    {
        if (candidates.contains(id))
        {
            return id;
        }
    }

    return {};
}

bool BuildParallel(const Graph& graph, const std::string& splitId, ChainItem& item)
{
    const auto joinId = FindJoin(graph, splitId);

    if (joinId.empty())
    {
        return false;
    }

    item.kind = ChainItem::Kind::Parallel;
    item.splitNodeId = splitId;
    item.joinNodeId = joinId;

    const auto* split = graph.Node(splitId);
    const auto* join = graph.Node(joinId);
    item.collapsible = split != nullptr && join != nullptr && IsSplitterType(split->type) && IsMixerType(join->type);

    for (const auto& first : graph.Out(splitId))
    {
        ChainBranch branch;
        branch.port = first.fromPort;
        const GraphEdge* edge = &first;

        for (int guard = 0; edge != nullptr && guard < 200; ++guard)
        {
            if (edge->to == joinId)
            {
                break;
            }

            const auto* node = graph.Node(edge->to);

            if (node == nullptr)
            {
                break;
            }

            branch.nodeIds.push_back(node->id);

            // Nested splits are not drawn; the branch stops at the inner split.
            if (IsSplitterType(node->type) || graph.IsSplitPoint(node->id))
            {
                break;
            }

            edge = graph.Primary(node->id);
        }

        item.branches.push_back(std::move(branch));
    }

    return true;
}
} // namespace

bool IsSplitterType(const std::string& type)
{
    return type == "splitter" || type == kNodeTypeSplitter;
}

bool IsMixerType(const std::string& type)
{
    return type == "mixer" || type == kNodeTypeMixer;
}

bool IsBoundaryNode(const GraphNode& node)
{
    return node.id == kInputNodeId || node.id == kOutputNodeId || node.type == kNodeTypeInput ||
           node.type == kNodeTypeOutput;
}

ChainLayout BuildChainLayout(const SignalGraph& signalGraph)
{
    const Graph graph(signalGraph);
    ChainLayout layout;
    std::set<std::string> visited;
    std::string current = kInputNodeId;

    for (int guard = 0; guard < 500; ++guard)
    {
        if (current != kInputNodeId)
        {
            if (visited.contains(current))
            {
                break;
            }

            visited.insert(current);
            const auto* node = graph.Node(current);

            if (node != nullptr && (IsSplitterType(node->type) || graph.IsSplitPoint(current)))
            {
                ChainItem parallel;

                if (BuildParallel(graph, current, parallel))
                {
                    // The split node is folded into the parallel block (its glyph), and the
                    // join is drawn after it as a node of its own, as the web UI does.
                    if (!layout.items.empty() && layout.items.back().kind == ChainItem::Kind::Node &&
                        layout.items.back().nodeId == current)
                    {
                        layout.items.pop_back();
                    }

                    current = parallel.joinNodeId;
                    layout.items.push_back(std::move(parallel));

                    ChainItem join;
                    join.nodeId = current;
                    layout.items.push_back(std::move(join));
                    continue;
                }
            }
        }

        const auto* edge = graph.Primary(current);

        if (edge == nullptr || edge->to == kOutputNodeId)
        {
            break;
        }

        const auto* next = graph.Node(edge->to);

        if (next == nullptr)
        {
            break;
        }

        ChainItem item;
        item.nodeId = next->id;
        layout.items.push_back(std::move(item));
        current = next->id;
    }

    return layout;
}

std::vector<std::string> ChainLayout::DrawnNodeIds() const
{
    std::vector<std::string> ids;

    for (const auto& item : items)
    {
        if (item.kind == ChainItem::Kind::Node)
        {
            ids.push_back(item.nodeId);
            continue;
        }

        for (const auto& branch : item.branches)
        {
            ids.insert(ids.end(), branch.nodeIds.begin(), branch.nodeIds.end());
        }
    }

    return ids;
}

std::string ChainLayout::NodeBefore(const std::string& nodeId) const
{
    std::string previous = kInputNodeId;

    for (const auto& item : items)
    {
        if (item.kind == ChainItem::Kind::Node)
        {
            if (item.nodeId == nodeId)
            {
                return previous;
            }

            previous = item.nodeId;
            continue;
        }

        for (const auto& branch : item.branches)
        {
            std::string branchPrevious = item.splitNodeId;

            for (const auto& id : branch.nodeIds)
            {
                if (id == nodeId)
                {
                    return branchPrevious;
                }

                branchPrevious = id;
            }
        }

        previous = item.splitNodeId;
    }

    return previous;
}
} // namespace guitarfx::uiclient
