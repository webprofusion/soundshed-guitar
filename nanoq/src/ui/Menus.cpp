// The shell's menus: the preset menu from the top bar, and the menu on a chain card.

#include "ui/Shell.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <chrono>
#include <functional>

namespace nanoq::ui
{
using guitarfx::uiclient::ChainItem;
using guitarfx::uiclient::kInputNodeId;
using guitarfx::uiclient::kOutputNodeId;

std::string Shell::MoveTarget(const std::string& nodeId, int direction) const
{
    if (!mState.activePreset)
    {
        return {};
    }

    const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);

    // A lane is what runs in series: the main row, or one branch of a parallel block. Each
    // slot has an anchor, the node whose output feeds what follows it: a node is its own
    // anchor, a parallel block's is where it joins. "Move after the anchor" is the engine's
    // reorder.
    struct LaneSlot
    {
        std::string anchor;
        std::string nodeId;
    };

    const auto moveIn = [&](const std::vector<LaneSlot>& lane, const std::string& head,
                            bool headIsSplit) -> std::string {
        for (std::size_t i = 0; i < lane.size(); ++i)
        {
            if (lane[i].nodeId != nodeId)
            {
                continue;
            }

            if (direction > 0)
            {
                return i + 1 < lane.size() ? lane[i + 1].anchor : std::string();
            }

            if (i == 0)
            {
                return {};
            }

            if (i >= 2)
            {
                return lane[i - 2].anchor;
            }

            // To the front of a branch would need the splitter's branch edge by port, which
            // the reorder request does not name.
            return headIsSplit ? std::string() : head;
        }

        return {};
    };

    // A row of items (the main row, or a branch) is a lane; a split nested in a branch is a slot
    // of that branch's lane, and its own branches lanes of their own.
    std::function<std::string(const std::vector<ChainItem>&, const std::string&, bool)> search =
        [&](const std::vector<ChainItem>& row, const std::string& head, bool headIsSplit) -> std::string {
        std::vector<LaneSlot> lane;

        for (const auto& item : row)
        {
            if (item.kind == ChainItem::Kind::Node)
            {
                lane.push_back({item.nodeId, item.nodeId});
                continue;
            }

            lane.push_back({item.joinNodeId, {}});

            for (const auto& branch : item.branches)
            {
                if (auto target = search(branch.items, item.splitNodeId, true); !target.empty())
                {
                    return target;
                }
            }
        }

        return moveIn(lane, head, headIsSplit);
    };

    return search(layout.items, kInputNodeId, false);
}

std::string Shell::LastNodeBeforeOutput() const
{
    if (!mState.activePreset)
    {
        return kInputNodeId;
    }

    const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);

    if (layout.items.empty())
    {
        return kInputNodeId;
    }

    const auto& last = layout.items.back();
    return last.kind == ChainItem::Kind::Node ? last.nodeId : last.joinNodeId;
}

void Shell::OpenNodeMenu(const std::string& nodeId)
{
    if (!mState.activePreset)
    {
        return;
    }

    auto& commands = mSession.Commands();
    const auto& graph = mState.activePreset->graph;
    const auto* node = graph.FindNode(nodeId);
    const bool boundary = nodeId == kInputNodeId || nodeId == kOutputNodeId ||
                          (node != nullptr && guitarfx::uiclient::IsBoundaryNode(*node));

    std::vector<MenuItem> items;

    if (boundary)
    {
        const bool isInput = nodeId == kInputNodeId || (node != nullptr && node->type == guitarfx::kNodeTypeInput);

        if (isInput)
        {
            items.push_back({"Add effect at the start...", false, false, [this, nodeId] { OpenEffectPicker(nodeId); }});
        }

        OpenMenu(isInput ? "Input" : "Output", std::move(items));
        return;
    }

    if (node == nullptr)
    {
        return;
    }

    // A splitter or mixer stands for its parallel block.
    if (guitarfx::uiclient::IsSplitterType(node->type) || guitarfx::uiclient::IsMixerType(node->type))
    {
        const auto layout = guitarfx::uiclient::BuildChainLayout(graph);

        if (const auto* item = layout.FindParallel(nodeId))
        {
            if (item->collapsible)
            {
                items.push_back({"Back to one lane", false, false,
                                 [&commands, splitter = item->splitNodeId] { commands.CollapseSplit(splitter); }});
            }

            if (!item->joinNodeId.empty())
            {
                items.push_back({"Add effect after the blend...", false, false,
                                 [this, join = item->joinNodeId] { OpenEffectPicker(join); }});
            }
        }

        if (guitarfx::uiclient::IsSplitterType(node->type))
        {
            items.insert(items.begin(), {"Show branches", false, false, [this, nodeId] { SelectNode(nodeId); }});
        }
        else
        {
            items.insert(items.begin(), {"Show branch levels", false, false, [this, nodeId] { SelectNode(nodeId); }});
        }

        OpenMenu("Parallel lanes", std::move(items));
        return;
    }

    const auto name = guitarfx::uiclient::NodeDisplayName(mState, *node);
    items.push_back({"Show controls", false, false, [this, nodeId] { SelectNode(nodeId); }});
    items.push_back({node->enabled ? "Turn off" : "Turn on", false, false,
                     [&commands, nodeId, bypass = node->enabled] { commands.SetNodeBypassed(nodeId, bypass); }});

    if (!MoveTarget(nodeId, -1).empty())
    {
        items.push_back({"Move earlier", false, false, [this, nodeId] {
                             if (const auto t = MoveTarget(nodeId, -1); !t.empty())
                             {
                                 mSession.Commands().MoveNode(nodeId, t);
                             }
                         }});
    }

    if (!MoveTarget(nodeId, 1).empty())
    {
        items.push_back({"Move later", false, false, [this, nodeId] {
                             if (const auto t = MoveTarget(nodeId, 1); !t.empty())
                             {
                                 mSession.Commands().MoveNode(nodeId, t);
                             }
                         }});
    }

    items.push_back({"Replace with...", false, false, [this, nodeId] { OpenEffectPicker({}, nodeId); }});
    items.push_back({"Rename...", false, false, [this, nodeId] { RenameNodeWithPrompt(nodeId); }});

    if (!node->title.empty())
    {
        items.push_back(
            {"Use the automatic name", false, false, [&commands, nodeId] { commands.RenameNode(nodeId, {}); }});
    }

    items.push_back({"Add effect after...", false, false, [this, nodeId] { OpenEffectPicker(nodeId); }});
    items.push_back({"Remove...", false, false, [this, nodeId, name] {
                         OpenConfirm("Remove effect", "Remove " + name + " from this preset?", "Remove",
                                     [this, nodeId] {
                                         if (mSelectedNode == nodeId)
                                         {
                                             mSelectedNode.clear();
                                         }

                                         mSession.Commands().RemoveNode(nodeId);
                                     });
                     }});

    OpenMenu(name, std::move(items));
}

void Shell::SaveActivePreset(bool asNew)
{
    if (!mState.activePreset)
    {
        return;
    }

    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    const bool userPreset = summary != nullptr && summary->source == "user";

    // A factory preset is never overwritten: saving it saves a copy, as the web UI does.
    if (!asNew && userPreset)
    {
        mSession.Commands().SavePreset();
        return;
    }

    OpenPresetDetails(true);
}

void Shell::OpenAddMenu(el::rect anchor)
{
    // As the web UI's .signal-path-add-menu: the FX Library, or a new scene.
    std::vector<MenuItem> items{
        {"Add FX...", false, false, [this] { OpenEffectPicker(LastNodeBeforeOutput()); }},
        {"Add Scene", false, false, [this] { mSession.Commands().AddScene(); }},
    };
    const el::rect left{anchor.right - 150.0f, anchor.top, anchor.right, anchor.bottom};
    OpenDropdown(left, std::move(items), 150.0f);
}

void Shell::OpenPresetMenu(std::optional<el::rect> anchor)
{
    auto& commands = mSession.Commands();
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    const bool havePreset = mState.activePreset.has_value();
    // What the web UI's toolbar allows (presets/toolbar.ts canModifyPreset).
    const bool canModify = havePreset && guitarfx::uiclient::CanModifyActivePreset(mState);
    const std::string presetId = mState.activePresetId;
    const std::string presetName = havePreset ? mState.activePreset->name : std::string();
    const bool favourite = std::find(mState.presetFavorites.begin(), mState.presetFavorites.end(), presetId) !=
                           mState.presetFavorites.end();

    std::vector<MenuItem> items;

    if (havePreset)
    {
        // Save over it only where the toolbar's Save would; Save As copies any preset.
        if (canModify)
        {
            items.push_back({"Save", false, false, [this] { SaveActivePreset(false); }});
        }

        items.push_back({"Save as new preset...", false, false, [this] { SaveActivePreset(true); }});

        if (canModify)
        {
            items.push_back({"Edit details...", false, false, [this] { OpenPresetDetails(false); }});
        }

        // The web UI's Export preset: a .soundshed.preset through a save dialog.
        items.push_back({"Export preset...", false, false, [&commands, presetId, presetName] {
                             commands.ExportPresetArchive({presetId}, presetName.empty() ? presetId : presetName);
                         }});

        items.push_back({favourite ? "Remove from favourites" : "Add to favourites", false, false,
                         [&commands, presetId, favourite] { commands.SetPresetFavorite(presetId, !favourite); }});
    }

    items.push_back({"Import preset...", false, false, [this] { ImportPresetArchive({}); }});
    items.push_back({"Input and output...", false, false, [this] { OpenControls(); }});
    items.push_back({"New preset", false, false, [this] {
                         if (mState.activePresetDirty)
                         {
                             OpenConfirm("Unsaved changes", "Start a new preset and lose the changes to this one?",
                                         "New preset", [this] { mSession.Commands().NewPreset(); });
                         }
                         else
                         {
                             mSession.Commands().NewPreset();
                         }
                     }});

    if (havePreset)
    {
        items.push_back({"Add scene", false, false, [&commands] { commands.AddScene(); }});

        if (mState.activePreset->scenes.size() > 1)
        {
            items.push_back({"Rename scene...", false, false, [this] {
                                 const auto* scene = mState.ActiveScene();

                                 if (scene == nullptr)
                                 {
                                     return;
                                 }

                                 const std::string id = scene->id;
                                 OpenPrompt("Rename scene", scene->title, "Rename",
                                            [this, id](const std::string& entered) {
                                                mSession.Commands().RenameScene(id, entered);
                                            });
                             }});
            items.push_back({"Remove this scene...", false, false, [this] {
                                 const auto* scene = mState.ActiveScene();

                                 if (scene == nullptr)
                                 {
                                     return;
                                 }

                                 const std::string id = scene->id;
                                 OpenConfirm("Remove scene", "Remove this scene from the preset?", "Remove",
                                             [this, id] { mSession.Commands().RemoveScene(id); });
                             }});
        }

        if (canModify)
        {
            items.push_back({"Delete preset...", false, false, [this, presetId] { DeleteWithConfirm(presetId); }});
        }
    }

    if (anchor)
    {
        OpenDropdown(*anchor, std::move(items), 200.0f);
    }
    else
    {
        OpenMenu("Preset", std::move(items));
    }
}

void Shell::ImportPresetArchive(const std::string& path)
{
    // As the web UI's Import Preset: the engine reads the archive (picking it first when there is
    // no path), installs its presets in a folder named after it, and answers under this id.
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::string requestId = "zip-" + std::to_string(now);
    mSession.Commands().ImportPresetArchiveFile(requestId, path);

    // Told once it is in; a failure is the engine's own error notice.
    mImportWatch = mSession.Client().Subscribe(guitarfx::uiclient::Topic::Tones, [this, requestId] {
        const auto it = mState.toneInstalls.find(requestId);

        if (it == mState.toneInstalls.end() || it->second.status == guitarfx::uiclient::ToneInstall::Status::Installing)
        {
            return;
        }

        if (it->second.status == guitarfx::uiclient::ToneInstall::Status::Installed)
        {
            const auto count = it->second.presetIds.size();
            ShowToast("Preset imported", std::to_string(count) + (count == 1 ? " preset added" : " presets added"),
                      false);
        }

        // Not from inside the callback that holds it.
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
            if (alive.lock())
            {
                mImportWatch = {};
            }
        });
    });
}
} // namespace nanoq::ui
