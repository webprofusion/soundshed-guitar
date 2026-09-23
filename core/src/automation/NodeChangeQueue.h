#pragma once

/**
 * NodeChangeQueue.h — hands what node.* automation changed over to the message thread.
 *
 * MIDI and DAW automation apply slots on the audio thread, and the working copy of the preset and
 * the UI both have to hear what changed. The message thread collects the changes in OnIdle, which
 * only runs while an editor is open, so a plugin automated for an hour with its editor closed must
 * not be holding an hour of events by then. A change therefore replaces any the message thread has
 * not taken yet for the same thing: the same parameter on a node with the same id, or the bypass of
 * the same effect type. That is all the working copy needs, since it keeps only the last value.
 *
 * Realtime rules. Post never allocates, frees or blocks: the cells are part of the queue, and what
 * a change names is held through shared pointers that Post only copies. Take moves them out, so
 * they are released on the message thread. Posts must not overlap one another (every caller holds
 * mDSPMutex), and only one thread takes. A post that finds every cell holding a change for
 * something else is dropped, and counted.
 *
 * Each cell is Free, Posting (a poster has it), Pending (holds a change) or Taking (the message
 * thread has it). A poster only touches a cell it moved to Posting and the taker only one it moved
 * to Taking, both by compare-exchange from the state the other side leaves it in, so neither can
 * read a change the other is writing. A change posted while the taker holds its predecessor lands
 * in another cell; the taker delivers that one after the predecessor, whether later in the same
 * pass or on the next, so the last value is always the last delivered.
 *
 * A poll that finds nothing to take costs one atomic load (HasChanges), so the plugin can check
 * from a timer that runs whether or not an editor is open.
 */

#include "automation/AutomationTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace guitarfx
{
class NodeChangeQueue
{
  public:
    struct Change
    {
        /// The slot's binding: the effect type, and for a parameter its id.
        std::shared_ptr<const NodeAddressBinding> binding;
        /// The node whose parameter was set. Empty for a bypass, which switches every node of the
        /// effect type.
        std::shared_ptr<const std::string> nodeId;
        /// The native value the parameter was set to, or for a bypass 1 enabled and 0 bypassed.
        double value = 0.0;
    };

    /// Records a change of `binding`'s parameter on the node `nodeId`, or with no `nodeId`, of its
    /// effect type's bypass. False, and counted, when there is no cell for it.
    bool Post(const std::shared_ptr<const NodeAddressBinding>& binding,
              const std::shared_ptr<const std::string>* nodeId, double value)
    {
        for (auto& cell : mCells)
        {
            if (!Acquire(cell, kPending, kPosting))
            {
                continue;
            }

            const bool same = Matches(cell.change, *binding, nodeId);

            if (same)
            {
                cell.change.value = value;
            }

            cell.state.store(kPending, std::memory_order_release);

            if (same)
            {
                mPosted.store(true, std::memory_order_release);
                return true;
            }
        }

        for (auto& cell : mCells)
        {
            if (!Acquire(cell, kFree, kPosting))
            {
                continue;
            }

            // The taker left these empty, so assigning them releases nothing.
            cell.change.binding = binding;

            if (nodeId)
            {
                cell.change.nodeId = *nodeId;
            }

            cell.change.value = value;
            cell.state.store(kPending, std::memory_order_release);
            mPosted.store(true, std::memory_order_release);
            return true;
        }

        // Flagged too, so the drop is reported.
        mDropped.fetch_add(1, std::memory_order_relaxed);
        mPosted.store(true, std::memory_order_release);
        return false;
    }

    /// Any thread: whether anything has been posted, or dropped, since the last Take began. A
    /// change posted while Take runs may be taken by it and still leave this set, so the next
    /// Take can find nothing; it never leaves a change behind with this clear.
    [[nodiscard]] bool HasChanges() const
    {
        return mPosted.load(std::memory_order_acquire);
    }

    /// Message thread: hands `take` each change posted since the last call, and forgets it.
    template <typename Fn> void Take(Fn&& take)
    {
        // Cleared first, so a change posted from here on sets it again. An exchange, so that when it
        // reads a poster's flag this pass also sees the cell that poster filled.
        (void)mPosted.exchange(false, std::memory_order_acq_rel);

        for (auto& cell : mCells)
        {
            if (!Acquire(cell, kPending, kTaking))
            {
                continue;
            }

            // Moving leaves the cell's pointers empty, ready for the next poster.
            Change change = std::move(cell.change);
            cell.state.store(kFree, std::memory_order_release);
            take(std::move(change));
        }
    }

    /// Message thread: how many changes were dropped since the last call.
    [[nodiscard]] std::size_t TakeDroppedCount()
    {
        return mDropped.exchange(0, std::memory_order_relaxed);
    }

    /// Changes one queue holds at once, each for a different node parameter or bypassed type.
    static constexpr std::size_t kCapacity = 128;

  private:
    enum State : int
    {
        kFree,
        kPosting,
        kPending,
        kTaking,
    };

    struct Cell
    {
        std::atomic<int> state{kFree};
        Change change;
    };

    static bool Acquire(Cell& cell, int from, int to)
    {
        int expected = from;
        return cell.state.load(std::memory_order_relaxed) == from &&
               cell.state.compare_exchange_strong(expected, to, std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Whether `pending` is a change to the same thing: a node id and param id for a parameter,
    /// the effect type for a bypass. By id rather than by node, since the id is all the working
    /// copy and the UI go by.
    static bool Matches(const Change& pending, const NodeAddressBinding& binding,
                        const std::shared_ptr<const std::string>* nodeId)
    {
        if (!nodeId)
        {
            return !pending.nodeId && pending.binding->effectType == binding.effectType;
        }

        return pending.nodeId && *pending.nodeId == **nodeId && pending.binding->paramId == binding.paramId;
    }

    std::array<Cell, kCapacity> mCells;
    std::atomic<std::size_t> mDropped{0};
    std::atomic<bool> mPosted{false};
};
} // namespace guitarfx
