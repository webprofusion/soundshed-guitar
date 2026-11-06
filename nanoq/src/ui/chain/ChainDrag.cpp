// Dragging in the chain strip: a card moved along the chain, an effect dragged out of the FX
// Library onto it, and model and IR files dropped on it from the desktop.

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

namespace nanoq::ui
{
using guitarfx::uiclient::kInputNodeId;
using guitarfx::uiclient::kOutputNodeId;

guitarfx::uiclient::NodeDropTarget Shell::DropTargetAt(el::point to, const std::string& except)
{
    using namespace guitarfx::uiclient;
    NodeDropTarget target;

    if (!mHitMap || !mState.activePreset)
    {
        return target;
    }

    auto& hits = *mHitMap;
    const auto& graph = mState.activePreset->graph;
    const auto* dragged = except.empty() ? nullptr : hits.FindCard(except);

    // What is under the pointer: another card, else a cable (a band round its line, as it is
    // thin). The output takes the card before it (its cable in), never a place after it.
    for (const auto& card : hits.cards)
    {
        if (card.nodeId == except || !card.bounds.includes(to))
        {
            continue;
        }

        const auto incoming = IncomingEdges(graph, card.nodeId);

        if (card.nodeId == kOutputNodeId || graph.FindNode(card.nodeId) == nullptr ||
            graph.FindNode(card.nodeId)->type == guitarfx::kNodeTypeOutput)
        {
            if (incoming.size() == 1)
            {
                target.kind = NodeDropTarget::Kind::Edge;
                target.edge = incoming.front();
            }

            break;
        }

        target.kind = NodeDropTarget::Kind::Node;
        target.nodeId = card.nodeId;
        target.incoming = incoming;
        target.isLeftOfDragged = dragged != nullptr && (card.bounds.left + card.bounds.right) <
                                                           (dragged->bounds.left + dragged->bounds.right);
        break;
    }

    if (target.kind == NodeDropTarget::Kind::None)
    {
        for (const auto& cable : hits.cables)
        {
            if (cable.bounds.inset(-2.0f, cable.bounds.height() * 0.5f - 18.0f).includes(to))
            {
                target.kind = NodeDropTarget::Kind::Edge;
                target.edge = cable.edge;
                break;
            }
        }
    }

    return target;
}

void Shell::DragCard(const std::string& nodeId, el::point from, el::point to, bool done)
{
    using namespace guitarfx::uiclient;

    if (!mHitMap || !mState.activePreset)
    {
        return;
    }

    auto& hits = *mHitMap;
    const auto& graph = mState.activePreset->graph;
    const auto* dragged = hits.FindCard(nodeId);
    const auto target = DropTargetAt(to, nodeId);

    // Let go over another line of a wrapped chain: a card there spans the pointer's height but
    // not the dragged card's.
    bool anotherLine = false;

    if (dragged != nullptr && (to.y < dragged->bounds.top || to.y > dragged->bounds.bottom))
    {
        for (const auto& card : hits.cards)
        {
            if (to.y >= card.bounds.top && to.y <= card.bounds.bottom)
            {
                anotherLine = true;
                break;
            }
        }
    }

    const auto action = ResolveNodeDrop(nodeId, target, to.x - from.x, to.y - from.y, anotherLine);

    // While dragging, light where it would land.
    hits.hoverNode.clear();
    hits.hoverEdge.reset();

    if (!done)
    {
        if (action.kind == NodeDropAction::Kind::ReorderAfterNode)
        {
            hits.hoverNode = action.targetNodeId;
        }
        else if (action.kind == NodeDropAction::Kind::ReorderToEdge)
        {
            hits.hoverEdge = action.edge;
        }

        mSession.View().refresh();
        return;
    }

    auto& commands = mSession.Commands();

    switch (action.kind)
    {
    case NodeDropAction::Kind::ReorderAfterNode:
        commands.MoveNode(nodeId, action.targetNodeId);
        break;
    case NodeDropAction::Kind::ReorderToEdge:
        commands.MoveNodeToEdge(nodeId, action.edge);
        break;
    case NodeDropAction::Kind::ToggleBypass:

        if (const auto* node = graph.FindNode(nodeId))
        {
            commands.SetNodeBypassed(nodeId, node->enabled);
        }

        break;
    case NodeDropAction::Kind::None:
        break;
    }

    if (action.kind != NodeDropAction::Kind::None)
    {
        NoteChainEdit();
    }

    mSession.View().refresh();
}

void Shell::DragLibraryEffect(const std::string& type, const std::string& name, el::point to, bool done)
{
    using namespace guitarfx::uiclient;

    // Its name follows the pointer until it is let go.
    if (!done)
    {
        if (!mDragGhost)
        {
            mDragGhost = std::make_shared<DragGhost>(mTheme, name);
            mSession.View().add(mDragGhost);
        }

        std::static_pointer_cast<DragGhost>(mDragGhost)->MoveTo(to);
    }
    else if (mDragGhost)
    {
        RemoveLayer(std::move(mDragGhost));
        mDragGhost.reset();
    }

    // Over a card it goes after that card; over a cable, onto it (the output's card means its
    // cable in).
    const auto target = DropTargetAt(to, {});

    if (mHitMap)
    {
        mHitMap->hoverNode.clear();
        mHitMap->hoverEdge.reset();

        if (!done && target.kind == NodeDropTarget::Kind::Node)
        {
            mHitMap->hoverNode = target.nodeId;
        }
        else if (!done && target.kind == NodeDropTarget::Kind::Edge)
        {
            mHitMap->hoverEdge = target.edge;
        }
    }

    if (done && target.kind != NodeDropTarget::Kind::None)
    {
        CloseOverlay();
        RecordFxRecent(mSession.Commands(), mState, FxItemKey(type));
        ExpectAddedNode();

        if (target.kind == NodeDropTarget::Kind::Node)
        {
            mSession.Commands().AddNode(type, target.nodeId);
        }
        else
        {
            mSession.Commands().AddNodeOnEdge(type, target.edge);
        }
    }

    mSession.View().refresh();
}

namespace
{
/// The files in a drop, as paths (Elements hands them over as file:// lines).
std::vector<std::string> DroppedPaths(const el::drop_info& info)
{
    std::vector<std::string> paths;
    const auto it = info.data.find("text/uri-list");

    if (it == info.data.end() || !std::holds_alternative<std::string>(it->second))
    {
        return paths;
    }

    std::istringstream lines(std::get<std::string>(it->second));

    for (std::string line; std::getline(lines, line);)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line.rfind("file://", 0) == 0)
        {
            line = line.substr(7);
        }

        if (!line.empty())
        {
            paths.push_back(line);
        }
    }

    return paths;
}

/// "nam" for a model, "ir" for an impulse response, else empty.
std::string ResourceTypeOfFile(const std::string& path)
{
    const auto ext = Lower(guitarfx::util::PathToUtf8(guitarfx::util::PathFromUtf8(path).extension()));

    if (ext == ".nam")
    {
        return "nam";
    }

    if (ext == ".wav")
    {
        return "ir";
    }

    return {};
}
} // namespace

std::string Shell::FileDropTarget(const std::string& resourceType, el::point where) const
{
    if (resourceType.empty() || !mState.activePreset)
    {
        return {};
    }

    const auto& graph = mState.activePreset->graph;
    const auto takes = [&](const std::string& nodeId) {
        const auto* node = graph.FindNode(nodeId);
        const auto* type = node != nullptr ? mState.FindEffectType(node->type) : nullptr;
        return type != nullptr && type->resourceType == resourceType;
    };

    // The card under the pointer, else the effect whose editor is showing.
    if (mHitMap)
    {
        for (const auto& card : mHitMap->cards)
        {
            if (card.bounds.includes(where))
            {
                return takes(card.nodeId) ? card.nodeId : std::string();
            }
        }
    }

    if (PlayShown() && !mSelectedNode.empty() && takes(mSelectedNode))
    {
        return mSelectedNode;
    }

    return {};
}

void Shell::TrackFileDrop(const el::drop_info& info, bool leaving)
{
    if (!mHitMap)
    {
        return;
    }

    std::string target;

    if (!leaving)
    {
        if (const auto paths = DroppedPaths(info); !paths.empty())
        {
            target = FileDropTarget(ResourceTypeOfFile(paths.front()), info.where);
        }
    }

    if (target != mHitMap->hoverNode)
    {
        mHitMap->hoverNode = target;
        mSession.View().refresh();
    }
}

bool Shell::DropFiles(const el::drop_info& info)
{
    if (mHitMap && !mHitMap->hoverNode.empty())
    {
        mHitMap->hoverNode.clear();
        mSession.View().refresh();
    }

    const auto paths = DroppedPaths(info);

    if (paths.empty())
    {
        return false;
    }

    if (DropPracticeTrack(paths))
    {
        return true;
    }

    const auto& path = paths.front();

    // A preset archive is imported, wherever it lands (presets/archive.ts
    // findDroppedPresetPackCandidate).
    {
        const std::string lower = Lower(path);

        for (const std::string suffix : {".soundshed.preset", ".soundshed.presets"})
        {
            if (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                ImportPresetArchive(path);
                return true;
            }
        }
    }

    const auto resourceType = ResourceTypeOfFile(path);
    const auto nodeId = FileDropTarget(resourceType, info.where);

    if (nodeId.empty())
    {
        if (!resourceType.empty())
        {
            ShowToast("Drop it on a model or IR effect",
                      "A " + std::string(resourceType == "nam" ? "model" : "IR") +
                          " goes into an effect that plays one.",
                      false);
        }

        return !resourceType.empty();
    }

    // Into the library where it is, and into that effect's (first) slot.
    const auto name = guitarfx::util::PathToUtf8(guitarfx::util::PathFromUtf8(path).stem());
    mSession.Commands().SaveLocalLibraryResource(resourceType, path, name, nlohmann::json::object(), nodeId, 0);
    NoteChainEdit();
    return true;
}
} // namespace nanoq::ui
