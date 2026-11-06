// The chain's undo and redo and its A/B slots: edits settle into history steps
// (uiclient::ChainHistory), and a step or slot is applied back to the active preset.

#include "ui/shell/Shell.h"

#include "uiclient/NodeLabels.h"

#include <algorithm>

namespace nanoq::ui
{
namespace
{
// How long an edit must stay still before it becomes an undo step (a knob drag is one step),
// and how long after a restore the engine's echo of it is not a new edit (history.ts).
constexpr double kHistoryCommitDelay = 0.32;
constexpr double kHistorySettle = 1.5;
} // namespace

void Shell::NoteChainEdit()
{
    mHistoryCommitAt = UiSession::Now() + kHistoryCommitDelay;
}

void Shell::CommitChainHistory()
{
    using namespace guitarfx::uiclient;
    mHistoryCommitAt = 0.0;
    const auto& preset = mState.activePreset;

    if (!preset)
    {
        mHistoryContext.clear();
        return;
    }

    // A different preset or scene starts both stacks over: B holding a chain from a preset since
    // left would overwrite the user's work the moment they switched to it.
    const auto context = mState.activePresetId + "::" + mState.activeSceneId;

    if (context != mHistoryContext || !mHistory.IsSeeded())
    {
        mHistoryContext = context;
        mHistory.Reset(MakeSnapshot(preset->graph, mState.activePresetId, mState.activeSceneId, "Loaded"));
        Mark(kTransport);
        return;
    }

    const auto* current = mHistory.Current();

    if (current != nullptr && current->signature == GraphSignature(preset->graph))
    {
        return;
    }

    auto label = DescribeGraphChange(current != nullptr ? &current->graph : nullptr, preset->graph,
                                     [this](const guitarfx::GraphNode& node) { return NodeDisplayName(mState, node); });
    auto snapshot = MakeSnapshot(preset->graph, mState.activePresetId, mState.activeSceneId, std::move(label));

    // Inside the settle window a graph that differs only in what no node message could carry (a
    // resource ref filled in, an edge normalised) is the engine's echo of the restore.
    const bool echo = UiSession::Now() < mHistorySettleUntil && current != nullptr && [&] {
        const auto changes = DiffNodeStates(current->graph, preset->graph);
        return !changes || changes->empty();
    }();

    if (echo)
    {
        mHistory.AmendCurrent(std::move(snapshot));
    }
    else
    {
        mHistory.Push(std::move(snapshot));
    }

    Mark(kTransport);
}

void Shell::ApplyChainSnapshot(const guitarfx::uiclient::ChainSnapshot& target)
{
    const auto& preset = mState.activePreset;

    if (!preset || target.presetId != mState.activePresetId)
    {
        ShowToast("Chain history unavailable", "The active preset changed", true);
        Mark(kTransport);
        return;
    }

    if (guitarfx::uiclient::GraphSignature(preset->graph) == target.signature)
    {
        Mark(kTransport);
        return;
    }

    // The same chain with other settings goes back as parameter, config and bypass messages;
    // anything else reloads the preset with the target chain, which rebuilds it.
    mHistorySettleUntil = UiSession::Now() + kHistorySettle;
    auto& commands = mSession.Commands();

    if (const auto changes = guitarfx::uiclient::DiffNodeStates(preset->graph, target.graph))
    {
        for (const auto& change : *changes)
        {
            for (const auto& [key, value] : change.params)
            {
                commands.SetNodeParam(change.nodeId, key, value);
            }

            for (const auto& [key, value] : change.config)
            {
                commands.SetNodeConfig(change.nodeId, key, value);
            }

            if (change.bypassed)
            {
                commands.SetNodeBypassed(change.nodeId, *change.bypassed);
            }
        }

        Mark(kValues | kChain | kPage);
        mChainSignature.clear();
    }
    else
    {
        commands.LoadActivePresetWithGraph(target.graph);
    }

    Mark(kTransport);
}

void Shell::UndoChain()
{
    CommitChainHistory();

    if (const auto* target = mHistory.Undo())
    {
        ApplyChainSnapshot(*target);
    }
}

void Shell::RedoChain()
{
    CommitChainHistory();

    if (const auto* target = mHistory.Redo())
    {
        ApplyChainSnapshot(*target);
    }
}

void Shell::SetChainSlot(guitarfx::uiclient::ChainHistory::Slot slot)
{
    CommitChainHistory();

    if (const auto* target = mHistory.SetActiveSlot(slot))
    {
        ApplyChainSnapshot(*target);
    }

    Mark(kTransport);
}

void Shell::CopyChainSlot()
{
    CommitChainHistory();
    const bool fromA = mHistory.ActiveSlot() == guitarfx::uiclient::ChainHistory::Slot::A;

    if (mHistory.CopyActiveToOther())
    {
        ShowToast(std::string("Chain ") + (fromA ? "A copied to B" : "B copied to A"), "", false);
    }

    Mark(kTransport);
}
} // namespace nanoq::ui
