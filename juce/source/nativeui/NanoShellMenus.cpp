#include "nativeui/NanoShell.h"

#include "nativeui/views/PresetsPage.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"

namespace soundshed::nano
{
namespace
{
using guitarfx::uiclient::ChainItem;
using guitarfx::uiclient::kInputNodeId;
using guitarfx::uiclient::kOutputNodeId;

juce::String text (const std::string& utf8)
{
    return juce::String::fromUTF8 (utf8.c_str());
}

/// A menu action that does nothing if the shell has gone by the time it is chosen.
template <typename Fn>
std::function<void()> guarded (juce::Component* shell, Fn fn)
{
    return [safe = juce::Component::SafePointer<juce::Component> (shell), fn] {
        if (safe != nullptr)
            fn();
    };
}
} // namespace

std::string NanoShell::moveTarget (const std::string& nodeId, int direction) const
{
    const auto& preset = context.state().activePreset;

    if (! preset)
        return {};

    const auto layout = guitarfx::uiclient::BuildChainLayout (preset->graph);

    // A lane is what runs in series: the main row, or one branch of a parallel block. Each
    // slot has an anchor, the node whose output feeds what follows it: a node is its own
    // anchor, a parallel block's is where it joins. "Move after the anchor" is the engine's
    // reorder.
    struct Slot
    {
        std::string anchor;
        std::string nodeId;
    };

    const auto moveIn = [&] (const std::vector<Slot>& lane, const std::string& head, bool headIsSplit) -> std::string {
        for (std::size_t i = 0; i < lane.size(); ++i)
        {
            if (lane[i].nodeId != nodeId)
                continue;

            if (direction > 0)
                return i + 1 < lane.size() ? lane[i + 1].anchor : std::string();

            if (i == 0)
                return {};

            if (i >= 2)
                return lane[i - 2].anchor;

            // To the front of a branch would need the splitter's branch edge by port, which
            // the reorder request does not name; the chain page's drag can do that.
            return headIsSplit ? std::string() : head;
        }

        return {};
    };

    std::vector<Slot> mainLane;

    for (const auto& item : layout.items)
    {
        if (item.kind == ChainItem::Kind::Node)
        {
            mainLane.push_back ({ item.nodeId, item.nodeId });
            continue;
        }

        mainLane.push_back ({ item.joinNodeId, {} });

        for (const auto& branch : item.branches)
        {
            std::vector<Slot> lane;

            for (const auto& id : branch.nodeIds)
                lane.push_back ({ id, id });

            if (auto target = moveIn (lane, item.splitNodeId, true); ! target.empty())
                return target;
        }
    }

    return moveIn (mainLane, kInputNodeId, false);
}

void NanoShell::moveNode (const std::string& nodeId, int direction)
{
    if (const auto target = moveTarget (nodeId, direction); ! target.empty())
        context.commands.MoveNode (nodeId, target);
}

void NanoShell::showNodeMenu (const std::string& nodeId, juce::Component* target)
{
    const auto& state = context.state();

    if (! state.activePreset)
        return;

    const auto& graph = state.activePreset->graph;
    const auto* node = graph.FindNode (nodeId);
    juce::PopupMenu menu;

    const bool boundary = nodeId == kInputNodeId || nodeId == kOutputNodeId || (node != nullptr && guitarfx::uiclient::IsBoundaryNode (*node));

    if (boundary)
    {
        const bool isInput = nodeId == kInputNodeId || (node != nullptr && node->type == guitarfx::kNodeTypeInput);

        if (isInput)
            menu.addItem ("Add effect at the start...", guarded (this, [this, nodeId] { openEffectPicker (nodeId); }));

        menu.addItem ("Input and output...", guarded (this, [this] { openControls(); }));
        context.showMenu (menu, target);
        return;
    }

    if (node == nullptr)
        return;

    // A splitter or mixer stands for its parallel block.
    if (guitarfx::uiclient::IsSplitterType (node->type) || guitarfx::uiclient::IsMixerType (node->type))
    {
        const auto layout = guitarfx::uiclient::BuildChainLayout (graph);

        for (const auto& item : layout.items)
        {
            if (item.kind == ChainItem::Kind::Parallel && (item.splitNodeId == nodeId || item.joinNodeId == nodeId))
            {
                menu.addItem ("Back to one lane", item.collapsible,
                              false, guarded (this, [this, splitter = item.splitNodeId] { context.commands.CollapseSplit (splitter); }));

                if (! item.joinNodeId.empty())
                    menu.addItem ("Add effect after the blend...", guarded (this, [this, join = item.joinNodeId] { openEffectPicker (join); }));
            }
        }

        context.showMenu (menu, target);
        return;
    }

    const auto name = text (guitarfx::uiclient::NodeDisplayName (state, *node));
    menu.addSectionHeader (name);
    menu.addItem ("Show controls", guarded (this, [this, nodeId] { selectNode (nodeId, true); }));
    menu.addItem (node->enabled ? "Turn off" : "Turn on",
                  guarded (this, [this, nodeId, bypass = node->enabled] { context.commands.SetNodeBypassed (nodeId, bypass); }));
    menu.addSeparator();
    menu.addItem ("Move earlier", ! moveTarget (nodeId, -1).empty(), false, guarded (this, [this, nodeId] { moveNode (nodeId, -1); }));
    menu.addItem ("Move later", ! moveTarget (nodeId, 1).empty(), false, guarded (this, [this, nodeId] { moveNode (nodeId, 1); }));
    menu.addItem ("Add effect after...", guarded (this, [this, nodeId] { openEffectPicker (nodeId); }));
    menu.addSeparator();
    menu.addItem ("Remove...", guarded (this, [this, nodeId, name] {
                      actions.confirm ("Remove effect", "Remove " + name + " from this preset?", "Remove", [this, nodeId] {
                          if (selectedNodeId == nodeId)
                              selectedNodeId.clear();

                          context.commands.RemoveNode (nodeId);
                      });
                  }));

    context.showMenu (menu, target);
}

void NanoShell::saveActivePreset (bool asNew)
{
    const auto& state = context.state();

    if (! state.activePreset)
        return;

    const auto* summary = state.FindPresetSummary (state.activePresetId);
    const bool userPreset = summary != nullptr && summary->source == "user";

    // A factory preset is never overwritten: saving it saves a copy, as the web UI does.
    if (! asNew && userPreset)
    {
        context.commands.SavePreset();
        return;
    }

    const auto name = text (state.activePreset->name);
    actions.promptText (asNew ? "Save as a new preset" : "Save preset", userPreset ? name + " copy" : name, [this] (const juce::String& entered) {
        const auto newName = entered.trim();

        if (newName.isEmpty())
            return;

        const auto& preset = context.state().activePreset;
        context.commands.SavePresetAs (newName.toStdString(), preset ? preset->category : std::string(),
                                       preset ? preset->description : std::string());
    });
}

void NanoShell::showMoreMenu()
{
    const auto& state = context.state();
    const auto* summary = state.FindPresetSummary (state.activePresetId);
    const bool havePreset = state.activePreset.has_value();
    const bool userPreset = summary != nullptr && summary->source == "user";
    juce::PopupMenu menu;

    menu.addItem ("Save", havePreset, false, guarded (this, [this] { saveActivePreset (false); }));
    menu.addItem ("Save as new preset...", havePreset, false, guarded (this, [this] { saveActivePreset (true); }));
    menu.addItem ("Rename...", havePreset && userPreset, false, guarded (this, [this] {
                      const auto& preset = context.state().activePreset;

                      if (! preset)
                          return;

                      actions.promptText ("Rename preset", text (preset->name), [this] (const juce::String& entered) {
                          const auto& current = context.state().activePreset;

                          if (current && entered.trim().isNotEmpty())
                              context.commands.RenamePreset (entered.trim().toStdString(), current->category, current->description);
                      });
                  }));
    menu.addItem ("New preset", guarded (this, [this] {
                      if (context.state().activePresetDirty)
                          actions.confirm ("Unsaved changes", "Start a new preset and lose the changes to this one?", "New preset",
                                           [this] { context.commands.NewPreset(); });
                      else
                          context.commands.NewPreset();
                  }));
    menu.addItem ("Add scene", havePreset, false, guarded (this, [this] { context.commands.AddScene(); }));
    menu.addItem ("Delete preset...", havePreset && userPreset, false, guarded (this, [this] {
                      PresetsPage::deleteWithConfirm (context, actions, context.state().activePresetId);
                  }));
    menu.addSeparator();
    menu.addItem ("Input and output...", guarded (this, [this] { openControls(); }));
    menu.addItem ("Metronome...", guarded (this, [this] { openMetronome(); }));
    menu.addItem ("Settings", guarded (this, [this] { showPage (Page::Settings); }));

    context.showMenu (menu, nullptr);
}
} // namespace soundshed::nano
