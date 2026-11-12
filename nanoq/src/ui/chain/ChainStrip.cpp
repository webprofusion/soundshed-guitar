// The chain strip: the active preset's signal chain drawn as the web UI draws it
// (core/ui/ts/signalPath.ts, css/signal-path/): the input, each effect as a card with its icon,
// the output, cables between them with a + on each to insert an effect there, and parallel
// branches stacked between a split and the mixer that joins them. When the chain is wider than
// the window it wraps onto more lines, joined by a cable that runs back under each line
// (signalPath/chainRow.ts), instead of scrolling sideways.
//
// A tap selects a card; the power button in its corner (shown on hover, and while it is off)
// bypasses it; a double tap opens the effect picker to replace it; a right click opens its menu.

#include "ui/shell/Shell.h"

#include "ui/chain/ChainWires.h"

#include "uiclient/ChainDropRules.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/FxMarks.h"
#include "uiclient/NodeLabels.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <variant>

namespace nanoq::ui
{
using guitarfx::uiclient::ChainItem;
using guitarfx::uiclient::kInputNodeId;

namespace
{
/// A card's signal light from the telemetry feed: `levels` is its node's reading, or null for the
/// input and output, which have their own.
NodeCard::Light CardLight(const guitarfx::uiclient::ClientState& state, const NodeCard& card,
                          const std::string& levelsKey)
{
    const auto lightFor = [](const guitarfx::uiclient::LevelReading& reading) {
        if (reading.clipped)
        {
            return NodeCard::Light::Clip;
        }

        if (reading.channelCount > 0 && reading.peakDb > -60.0)
        {
            return NodeCard::Light::Live;
        }

        return NodeCard::Light::Idle;
    };
    const auto& telemetry = state.telemetry;

    if (card.BoundaryKind() == NodeCard::Boundary::Input)
    {
        return lightFor(telemetry.input);
    }

    if (card.BoundaryKind() == NodeCard::Boundary::Output)
    {
        return lightFor(telemetry.output);
    }

    if (!card.Enabled())
    {
        return NodeCard::Light::None;
    }

    const auto it = telemetry.nodes.find(levelsKey);
    return it != telemetry.nodes.end() ? lightFor(it->second) : NodeCard::Light::Idle;
}
} // namespace

using guitarfx::uiclient::kOutputNodeId;

namespace
{
/// One unit the chain wraps by: a cable and what it leads into (as chainRow.ts's segments).
struct Segment
{
    std::vector<Ptr> parts;
    float width = 0.0f;
    float height = 0.0f; // past the row's own, for a parallel block
};

/// A node class's accent on the chain (css/signal-path/nodes.css, .signal-node.amp and the rest):
/// not the library's category colours, which are brighter.
el::color NodeAccent(const std::string& nodeClass)
{
    static const std::map<std::string, std::uint32_t> kAccents{
        {"amp", 0xf0a24c},        {"cab", 0xaeb9d0},   {"eq", 0x61b4d6},     {"dynamics", 0xcf7575},
        {"modulation", 0x8c79db}, {"delay", 0x64c2aa}, {"reverb", 0x7fa4ee}, {"utility", 0x97a1b4}};
    const auto it = kAccents.find(nodeClass);
    return Theme::Argb(0xff000000u | (it != kAccents.end() ? it->second : 0x97a1b4u));
}
} // namespace

std::string Shell::ChainSignature() const
{
    if (!mState.activePreset)
    {
        return {};
    }

    // The width decides where lines wrap, so a resize past a break redraws; the stage's height
    // (the looper strip's coming and going too) whether the chain scrolls, and the split its cap.
    std::string s = mSelectedNode + "|" + mState.activePresetId + "|" + mState.activeSceneId + "|" +
                    std::to_string(static_cast<int>(ViewSize().x / 16.0f)) + "x" +
                    std::to_string(static_cast<int>(StageHeight() / 24.0f)) + (Split() ? "s" : "t") +
                    (mWide ? "w" : "");

    for (const auto& node : mState.activePreset->graph.nodes)
    {
        s += "|" + node.id + ":" + node.type + ":" + (node.enabled ? "1" : "0") + ":" +
             guitarfx::uiclient::NodeDisplayName(mState, node) + ":" +
             guitarfx::uiclient::NodeArchitectureBadge(mState, node);
    }

    for (const auto& edge : mState.activePreset->graph.edges)
    {
        s += "|" + edge.from + ">" + edge.to + "#" + std::to_string(edge.fromPort);
    }

    for (const auto& missing : mState.missingNodeResources)
    {
        s += "|!" + missing;
    }

    return s + "|" + MixerSignature() + (IconChain() ? "|icons" : "");
}

bool Shell::IconChain() const
{
    // Tiles are chosen by dragging the bar under a split chain; staged, the chain is cards.
    if (!Split())
    {
        return false;
    }

    if (mChainIcons >= 0)
    {
        return mChainIcons == 1;
    }

    const auto native = mState.uiSettings.find("native");
    return native != mState.uiSettings.end() && native->is_object() && native->value("chainIcons", false);
}

Ptr Shell::BuildChain()
{
    using namespace guitarfx::uiclient;

    mHitMap = std::make_shared<chain::HitMap>();

    if (!mState.activePreset)
    {
        return VGap(0);
    }

    if (mMixTab && MultiRigShown())
    {
        return BuildInlineMixer();
    }

    const auto& graph = mState.activePreset->graph;
    const auto layout = BuildChainLayout(graph);
    const bool icons = IconChain();
    const float rowHeight = icons ? kIconRowHeight : kCardHeight;

    // The input and output are nodes of the graph (their trim is edited like a parameter); a
    // graph that lacks one still gets its card, under the layout's own id.
    auto boundaryId = [&graph](bool input) {
        for (const auto& node : graph.nodes)
        {
            if (node.type == (input ? guitarfx::kNodeTypeInput : guitarfx::kNodeTypeOutput) ||
                node.id == (input ? kInputNodeId : kOutputNodeId))
            {
                return node.id;
            }
        }

        return std::string(input ? kInputNodeId : kOutputNodeId);
    };
    const auto inputId = boundaryId(true);
    const auto outputId = boundaryId(false);

    auto isMissing = [this](const std::string& nodeId) {
        return std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(), nodeId) !=
               mState.missingNodeResources.end();
    };

    // A card for a node of the graph; its width is returned too, for the wrap.
    auto makeCard = [this, &graph, &isMissing, &inputId, &outputId, icons](const std::string& nodeId,
                                                                           float& width) -> Ptr {
        NodeCard::Look look;
        const auto* node = graph.FindNode(nodeId);
        const bool input =
            nodeId == inputId || (node != nullptr && IsBoundaryNode(*node) && node->type == guitarfx::kNodeTypeInput);
        const bool output =
            nodeId == outputId || (node != nullptr && IsBoundaryNode(*node) && node->type == guitarfx::kNodeTypeOutput);

        if (input || output)
        {
            look.name = input ? "Input" : "Output";
            look.icon = input ? "guitar" : "output";
            // --node-input and --node-output.
            look.tint = input ? mTheme.Success() : el::rgba(104, 136, 168, 255);
            look.width = kBoundaryWidth;
            look.canBypass = false;
            look.boundary = input ? NodeCard::Boundary::Input : NodeCard::Boundary::Output;
        }
        else if (node != nullptr)
        {
            const auto* type = mState.FindEffectType(node->type);
            const auto category = NodeCategory(mState, *node);
            look.name = NodeDisplayName(mState, *node);
            look.detail = type != nullptr ? type->name : std::string();

            // Renamed, a model or IR node names what it plays under the user's name (signalPath.ts).
            if (!node->title.empty() && type != nullptr && type->requiresResource)
            {
                if (auto resource = NodeResourceName(mState, *node); !resource.empty())
                {
                    look.detail = std::move(resource);
                }
            }

            if (look.detail == look.name)
            {
                look.detail.clear();
            }

            look.badge = NodeArchitectureBadge(mState, *node);
            look.icon = mPresentation.IconFor(node->type, category);
            look.tint = NodeAccent(mPresentation.NodeClass(category));
            look.enabled = node->enabled;
            look.missing = isMissing(nodeId);
            look.canBypass = !IsMixerType(node->type) && !IsSplitterType(node->type);

            // The equipment picture over the card's top edge, wherever the effect has one
            // (signalPath.ts: getEffectVisualizationEquipmentImage).
            if (const auto image = mPresentation.EquipmentImage(node->type, category); !image.empty())
            {
                look.thumbnail = mImages.Find(image);
            }

            look.canRemove = true;

            look.width = CardWidthFor(look.name, look.detail);
        }
        else
        {
            width = 0.0f;
            return HGap(0);
        }

        if (icons)
        {
            look.iconOnly = true;
            look.width = kIconCardSize;
        }

        look.selected = nodeId == mSelectedNode;
        width = look.width;
        const bool canBypass = look.canBypass;
        auto card = std::make_shared<NodeCard>(mTheme, mIcons, std::move(look));
        card->onSelect = [this, nodeId] {
            SelectNode(nodeId);
            mChainFocused = true;
        };
        card->onMenu = [this, nodeId] { OpenNodeMenu(nodeId); };
        // Every card is somewhere to drop; effects can be dragged, the input and output stay put.
        card->Draggable(mHitMap, nodeId);

        if (card->BoundaryKind() == NodeCard::Boundary::None)
        {
            card->onDrag = [this, nodeId](el::point from, el::point to, bool done) {
                DragCard(nodeId, from, to, done);
            };
        }

        if (canBypass)
        {
            card->onToggle = [this, nodeId] {
                if (const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr)
                {
                    mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
                }
            };
            card->onReplace = [this, nodeId] { OpenEffectPicker({}, nodeId); };
        }

        // Posted: removing rebuilds the strip, which would free the card in its own click.
        card->onRemove = [this, nodeId] {
            mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), nodeId] {
                if (alive.lock())
                {
                    RemoveChainNode(nodeId);
                }
            });
        };

        // Its light follows the telemetry feed.
        mLive.Feed(card, [this, key = mState.activePresetId + "::" + nodeId](NodeCard& c) {
            return c.SetLight(CardLight(mState, c, key));
        });
        return card;
    };

    // A cable's +: inserts on the cable's own edge of the graph, as the web UI's connectors do,
    // which is the only way into a parallel branch; after `from` when no edge is found.
    auto plus = [this, &graph](const std::string& from, const std::string& to,
                               int fromPort = -1) -> std::function<void()> {
        for (const auto& edge : graph.edges)
        {
            if (edge.from == from && edge.to == to && (fromPort < 0 || edge.fromPort == fromPort))
            {
                return [this, edge] { OpenEffectPickerOnEdge(edge); };
            }
        }

        return [this, from] { OpenEffectPicker(from); };
    };
    // A cable with its +, which knows its edge of the graph so a dragged card can land on it.
    auto cable = [this, &graph, &plus, rowHeight](const std::string& from, const std::string& to, int fromPort = -1,
                                                  bool stretch = false) {
        auto connector = std::make_shared<Connector>(mTheme, plus(from, to, fromPort), stretch);
        connector->RowHeight(rowHeight);

        for (const auto& edge : graph.edges)
        {
            if (edge.from == from && edge.to == to && (fromPort < 0 || edge.fromPort == fromPort))
            {
                connector->Edge(mHitMap, edge);
                break;
            }
        }

        // Lit with the signal while Animate signal flow is on; the pulses move one spacing every
        // 0.4 s, as the web UI's animation does.
        mLive.Feed(connector, [this, source = connector->Source()](Connector& c) {
            const bool live = !source.empty() && guitarfx::uiclient::IsSignalFlowAnimationEnabled(mState) &&
                              mSignalFlow.IsLive(source);
            return c.SetFlow(live, static_cast<float>(std::fmod(UiSession::Now() / 0.4, 1.0)));
        });
        return connector;
    };

    // ── A parallel block: the split rail, a lane per branch, the join rail. A lane runs as the
    // main row does, so a split nested in a branch is a block inside that lane, and the lane is
    // as tall as its branches ──
    struct Block
    {
        std::vector<Ptr> parts;
        float width = 0.0f;
        float height = 0.0f;
    };

    std::function<Block(const ChainItem&)> parallelBlock;

    const auto laneOf = [&](const ChainItem& item, const ChainBranch& branch) {
        Block lane;
        lane.height = rowHeight;
        std::string after = item.splitNodeId;
        bool joinsBlock = false;

        // The first cable is the splitter's edge for this branch, picked out by its port.
        const auto into = [&](const std::string& to, bool stretch = false) {
            return after == item.splitNodeId ? cable(after, to, branch.port, stretch) : cable(after, to, -1, stretch);
        };

        for (const auto& step : branch.items)
        {
            if (step.kind == ChainItem::Kind::Node)
            {
                // Into a nested block's mixer the cable is drawing only, as on the main row.
                if (joinsBlock)
                {
                    auto drawing = std::make_shared<Connector>(mTheme, std::function<void()>{});
                    drawing->RowHeight(rowHeight);
                    lane.parts.push_back(drawing);
                }
                else
                {
                    lane.parts.push_back(into(step.nodeId));
                }

                float w = 0.0f;
                lane.parts.push_back(makeCard(step.nodeId, w));
                lane.width += kConnectorWidth + w;
                after = step.nodeId;
                joinsBlock = false;
                continue;
            }

            lane.parts.push_back(into(step.splitNodeId));
            auto nested = parallelBlock(step);
            lane.parts.insert(lane.parts.end(), nested.parts.begin(), nested.parts.end());
            lane.width += kConnectorWidth + nested.width;
            lane.height = std::max(lane.height, nested.height);
            joinsBlock = true;
        }

        // The branch runs on to the join, with a + to add at its end (or into it, empty).
        lane.parts.push_back(into(item.joinNodeId, true));
        lane.width += kConnectorWidth;
        return lane;
    };

    parallelBlock = [&](const ChainItem& item) {
        const int laneCount = static_cast<int>(std::max<std::size_t>(1, item.branches.size()));
        std::vector<Ptr> lanes;
        std::vector<float> laneHeights;
        float lanesWidth = 0.0f;

        for (const auto& branch : item.branches)
        {
            auto lane = laneOf(item, branch);
            lanesWidth = std::max(lanesWidth, std::max(lane.width, 2.0f * kConnectorWidth));

            if (!lanes.empty())
            {
                lanes.push_back(VGap(kLaneGap));
            }

            lanes.push_back(FixedHeight(lane.height, Row(lane.parts)));
            laneHeights.push_back(lane.height);
        }

        const auto splitId = item.splitNodeId;
        auto split =
            std::make_shared<Rail>(mTheme, mIcons, laneCount, false, splitId == mSelectedNode, [this, splitId] {
                SelectNode(splitId);
                mChainFocused = true;
            });
        auto join = std::make_shared<Rail>(mTheme, mIcons, laneCount, true, false, std::function<void()>{});
        split->RowHeight(rowHeight);
        join->RowHeight(rowHeight);
        split->LaneHeights(laneHeights);
        join->LaneHeights(laneHeights);

        Block block;
        block.parts = {split, FixedWidth(lanesWidth, Col(lanes)), join};
        block.width = kRailWidth * 2.0f + lanesWidth;
        block.height = static_cast<float>(laneCount - 1) * kLaneGap;

        for (const float h : laneHeights)
        {
            block.height += h;
        }

        return block;
    };

    // ── The segments: the input, then each cable with what it leads into ──
    std::vector<Segment> segments;
    {
        Segment input;
        input.parts.push_back(makeCard(inputId, input.width));
        segments.push_back(std::move(input));
    }

    std::string previous = inputId;
    bool afterParallel = false;

    for (const auto& item : layout.items)
    {
        Segment segment;

        // Into a parallel block's mixer the cable is drawing only: the branches are its inputs.
        if (afterParallel)
        {
            auto drawing = std::make_shared<Connector>(mTheme, std::function<void()>{});
            drawing->RowHeight(rowHeight);
            segment.parts.push_back(drawing);
        }
        else
        {
            segment.parts.push_back(
                cable(previous, item.kind == ChainItem::Kind::Node ? item.nodeId : item.splitNodeId));
        }

        segment.width = kConnectorWidth;
        const bool joinsBlock = afterParallel;
        afterParallel = item.kind == ChainItem::Kind::Parallel;

        if (item.kind == ChainItem::Kind::Node)
        {
            float w = 0.0f;
            segment.parts.push_back(makeCard(item.nodeId, w));
            segment.width += w;
            previous = item.nodeId;

            // The mixer stays on the line of the block it joins (chainRow.ts's segments).
            if (joinsBlock && !segments.empty())
            {
                auto& block = segments.back();
                block.parts.insert(block.parts.end(), segment.parts.begin(), segment.parts.end());
                block.width += segment.width;
            }
            else
            {
                segments.push_back(std::move(segment));
            }

            continue;
        }

        // A parallel block: the split, each branch as a lane, and the join; the mixer that
        // joins them is the next item.
        auto block = parallelBlock(item);
        segment.parts.insert(segment.parts.end(), block.parts.begin(), block.parts.end());
        segment.width += block.width;
        segment.height = block.height;
        // The mixer that joins the branches follows as a node item of its own.
        segments.push_back(std::move(segment));
    }

    {
        Segment output;
        output.parts.push_back(cable(previous, outputId));
        float w = 0.0f;
        output.parts.push_back(makeCard(outputId, w));
        output.width = kConnectorWidth + w;
        segments.push_back(std::move(output));
    }

    // ── Lines: as many segments as fit the width, never fewer than one; one line, scrolling
    // sideways, with the wrap switched off ──
    const float viewWidth = StageWidth();
    const float room = mWrapChain ? std::max(200.0f, viewWidth - 2.0f * kSidePad - 2.0f * kWireEndWidth) : 1e9f;

    struct Line
    {
        std::vector<const Segment*> segments;
        float width = 0.0f;
        float height = 0.0f;
    };

    std::vector<Line> lines(1);

    for (const auto& segment : segments)
    {
        if (!lines.back().segments.empty() && lines.back().width + segment.width > room)
        {
            lines.emplace_back();
        }

        auto& line = lines.back();
        line.segments.push_back(&segment);
        line.width += segment.width;
        line.height = std::max({line.height, segment.height, rowHeight});
    }

    // The lines are centred as one block, as the web UI centres the chain in its stage; each line
    // starts at the same place, with room for the cable in on the left.
    float widest = 0.0f;

    for (const auto& line : lines)
    {
        widest = std::max(widest, line.width);
    }

    const float indent = std::max(kSidePad, (viewWidth - widest - 2.0f * kWireEndWidth) * 0.5f);

    std::vector<Ptr> rows;
    float total = 0.0f;

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        const bool first = i == 0, last = i + 1 == lines.size();
        std::vector<Ptr> row{HGap(indent)};
        row.push_back(first ? HGap(kWireEndWidth)
                            : std::static_pointer_cast<el::element>(std::make_shared<WireEnd>(mTheme, false)));

        for (const auto* segment : line.segments)
        {
            for (const auto& part : segment->parts)
            {
                row.push_back(part);
            }
        }

        if (!last)
        {
            row.push_back(std::make_shared<WireEnd>(mTheme, true));
        }

        row.push_back(Stretch());
        rows.push_back(FixedHeight(line.height, Row(row)));
        total += line.height;

        if (!last)
        {
            const float from = indent + kWireEndWidth + line.width + kWireEndWidth * 0.5f;
            const float to = indent + kWireEndWidth * 0.5f;
            rows.push_back(std::make_shared<ReturnRoute>(mTheme, from, to));
            total += kRouteHeight;
        }
    }

    // The block sits in the middle of the stage, between the scene chips and the add button; it
    // scrolls when taller than that, and sideways when the wrap is off.
    if (Split())
    {
        // The split stage's chain is as tall as its lines, up to a third of the window, so the
        // editor under it keeps the rest; past that it scrolls.
        const float wanted = total + 24.0f;
        // Unless the bar under it was dragged (chainAreaHeight), up to a third of the window;
        // never less than a line, nor so much that the editor has no room.
        // As tiles it is as tall as its lines again: the drag that made them only chose them.
        float limit = mChainAreaLimit;

        if (limit <= 0.0f)
        {
            if (const auto native = mState.uiSettings.find("native");
                native != mState.uiSettings.end() && native->is_object())
            {
                limit = native->value("chainAreaHeight", 0.0f);
            }
        }

        if (icons)
        {
            limit = 0.0f;
        }

        const float floor = rowHeight + 24.0f;
        // Dragged, it leaves the stage 146 px for the rest (the full layout's window less 360 px,
        // as it was, with the bars taken out, so it holds for the compact frame's too).
        const float most = limit > 0.0f ? std::clamp(limit, floor, std::max(floor, StageHeight() - 146.0f))
                                        : std::max(floor, ViewSize().y / 3.0f);
        // Dragged, the chain gets that room whatever it holds (centred in it, or scrolling).
        const float area = limit > 0.0f ? most : std::min(wanted, most);
        mChainAreaHeight = area + SceneRowHeight() + 30.0f; // the scene chips (and rig tabs), the add button
        const auto centred = [area](Ptr block, float height) -> Ptr {
            if (height >= area)
            {
                return FixedHeight(area, block);
            }

            return FixedHeight(area, Col({std::make_shared<el::element>(), FixedHeight(height, block),
                                          std::make_shared<el::element>()}));
        };

        if (!mWrapChain)
        {
            const float lineWidth = 2.0f * indent + 2.0f * kWireEndWidth + widest;
            return centred(HScroll(PadXY(0, 12, FixedWidth(lineWidth, Col(rows)))), wanted);
        }

        if (wanted > area)
        {
            return FixedHeight(area, VScroll(PadXY(0, 12, Col(rows))));
        }

        return centred(PadXY(0, 12, Col(rows)), wanted);
    }

    // Staged: the stage less the scene chips (and rig tabs) and the add button's row under it.
    const float stageHeight = StageHeight() - SceneRowHeight() - 44.0f;

    if (!mWrapChain)
    {
        const float lineWidth = 2.0f * indent + 2.0f * kWireEndWidth + widest;
        auto block = FixedWidth(lineWidth, Col(rows));
        return Col({std::make_shared<el::element>(), FixedHeight(total + 24.0f, HScroll(PadXY(0, 12, block))),
                    std::make_shared<el::element>()});
    }

    if (total + 24.0f > stageHeight)
    {
        return VScroll(PadXY(0, 12, Col(rows)));
    }

    return Col({std::make_shared<el::element>(), FixedHeight(total, Col(rows)), std::make_shared<el::element>()});
}
} // namespace nanoq::ui
