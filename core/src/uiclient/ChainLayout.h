#pragma once

/**
 * ChainLayout.h - The signal graph as a row of things to draw.
 *
 * A preset's graph is a DAG; a chain view draws a row. This walks the graph from the input
 * the way the web UI's renderGraphSignalPath does (core/ui/ts/signalPath.ts): follow each
 * node's primary edge (port 0 first), and where a node fans out, find the first node every
 * branch reaches again and draw the branches stacked between the two. Nested splits are not
 * drawn, as in the web UI; a branch stops at the inner split node.
 */

#include "presets/PresetTypes.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
struct ChainBranch
{
    int port = 0;
    std::vector<std::string> nodeIds;
};

struct ChainItem
{
    enum class Kind
    {
        Node,
        Parallel
    };

    Kind kind = Kind::Node;

    /// Kind::Node: the node drawn here.
    std::string nodeId;

    /// Kind::Parallel: where the signal splits and where it comes back together, and what
    /// runs on each branch in between (the join itself is not in any branch).
    std::string splitNodeId;
    std::string joinNodeId;
    std::vector<ChainBranch> branches;

    /// A splitter/mixer pair the user can collapse back to one lane.
    bool collapsible = false;
};

struct ChainLayout
{
    /// Left to right between the input and the output, which are not items.
    std::vector<ChainItem> items;

    /// Every node that is drawn, in drawing order: the items with each parallel block's
    /// branches flattened, top branch first.
    [[nodiscard]] std::vector<std::string> DrawnNodeIds() const;

    /// The node drawn just before `nodeId` on the main row or its branch, or the input.
    [[nodiscard]] std::string NodeBefore(const std::string& nodeId) const;
};

inline constexpr const char* kInputNodeId = "__input__";
inline constexpr const char* kOutputNodeId = "__output__";

[[nodiscard]] ChainLayout BuildChainLayout(const SignalGraph& graph);

[[nodiscard]] bool IsSplitterType(const std::string& type);
[[nodiscard]] bool IsMixerType(const std::string& type);
[[nodiscard]] bool IsBoundaryNode(const GraphNode& node);
} // namespace guitarfx::uiclient
