#include "ui/Shell.h"

#include "uiclient/ChainLayout.h"

// The keyboard shortcuts, as the web UI's (docs/user-interface.md, keyboard shortcuts): chain
// undo and redo; Space bypasses the effect whose card was clicked last, or taps the tempo;
// Delete removes that effect (a splitter or mixer goes back to one lane); F2 renames the scene;
// Esc closes the menu or dialog on top. None but Esc reach a text box being typed in (the
// router keeps those keys), and only Esc works while a dialog is open.

namespace nanoq::ui
{
bool Shell::RemoveChainNode(const std::string& nodeId)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr)
    {
        return false;
    }

    auto& commands = mSession.Commands();

    // A split or its mix: the whole split collapses, as the web UI's cross on either does.
    if (guitarfx::uiclient::IsSplitterType(node->type) || guitarfx::uiclient::IsMixerType(node->type))
    {
        const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);

        if (const auto* item = layout.FindParallel(nodeId); item != nullptr && item->collapsible)
        {
            commands.CollapseSplit(item->splitNodeId);
            mSelectedNode.clear();
            mChainFocused = false;
            return true;
        }

        return false;
    }

    if (mSelectedNode == nodeId)
    {
        mSelectedNode.clear();
    }

    mChainFocused = false;
    commands.RemoveNode(nodeId);
    Mark(kChain | kPage);
    return true;
}

namespace
{
bool CommandHeld(int modifiers)
{
#if defined(__APPLE__)
    return (modifiers & (el::mod_command | el::mod_action)) != 0;
#else
    return (modifiers & (el::mod_control | el::mod_action)) != 0;
#endif
}
} // namespace

Ptr Shell::KeyLayer(Ptr layer)
{
    auto router = std::make_shared<KeyRouter>(std::move(layer), true);
    router->onKey = [this](const el::key_info& k) { return HandleKey(k); };
    router->ringColour = [this] { return mTheme.Accent(); };
    return router;
}

void Shell::TapTempo()
{
    if (!mTapTempo)
    {
        mTapTempo = std::make_shared<guitarfx::uiclient::TapTempo>();
    }

    if (const auto bpm = mTapTempo->Tap(UiSession::Now()))
    {
        mSession.Commands().SetMetronomeBpm(guitarfx::uiclient::ClampBpm(*bpm));
    }
}

bool Shell::HandleKey(const el::key_info& k)
{
    using el::key_code;

    if (k.action != el::key_action::press && k.action != el::key_action::repeat)
    {
        return false;
    }

    const bool repeat = k.action == el::key_action::repeat;

    // A key a slot maps, or any while a key learn waits, goes to the engine's mappings first.
    if (RouteAutomationKey(k))
    {
        return true;
    }

    if (k.key == key_code::escape)
    {
        if (mDropdown)
        {
            CloseDropdown();
            return true;
        }

        if (mOverlay)
        {
            CancelOverlay();
            return true;
        }

        return false;
    }

    // Under a menu or dialog the page's shortcuts are off, as the web UI's are under a modal.
    if (mDropdown || mOverlay || !mState.activePreset)
    {
        return false;
    }

    if (CommandHeld(k.modifiers))
    {
        if (k.key == key_code::z)
        {
            if (k.modifiers & el::mod_shift)
            {
                RedoChain();
            }
            else
            {
                UndoChain();
            }

            return true;
        }

        if (k.key == key_code::y)
        {
            RedoChain();
            return true;
        }

        return false;
    }

    if (k.modifiers & (el::mod_alt | el::mod_control | el::mod_super))
    {
        return false;
    }

    const auto* node = mChainFocused ? SelectedNode() : nullptr;
    auto& commands = mSession.Commands();

    switch (k.key)
    {
    case key_code::space: {
        if (repeat)
        {
            return true;
        }

        if (node != nullptr && !guitarfx::uiclient::IsSplitterType(node->type) &&
            !guitarfx::uiclient::IsMixerType(node->type))
        {
            commands.SetNodeBypassed(node->id, node->enabled);
            return true;
        }

        TapTempo();
        return true;
    }

    case key_code::_delete:
    case key_code::backspace: {
        // On the pads, Delete clears the pad playing (asking first), as the web UI's.
        if (mPage == Page::Pads && k.key == key_code::_delete && !repeat)
        {
            ClearSetlistPad(mState.setlistCursor);
            return true;
        }

        if (node == nullptr || repeat)
        {
            return false;
        }

        return RemoveChainNode(node->id);
    }

    case key_code::f2: {
        const auto* scene = mState.ActiveScene();

        if (scene == nullptr || mState.activePreset->scenes.size() < 2)
        {
            return false;
        }

        const std::string id = scene->id;
        OpenPrompt("Rename scene", scene->title, "Rename",
                   [this, id](const std::string& entered) { mSession.Commands().RenameScene(id, entered); });
        return true;
    }

    default:
        return false;
    }
}
} // namespace nanoq::ui
