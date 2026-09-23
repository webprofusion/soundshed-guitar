#pragma once

#include <atomic>
#include <cstdint>

namespace guitarfx
{
/**
 * The expensive half of a parameter change that SetParam could not make where it ran.
 *
 * SetParam can run on the audio thread: MIDI and DAW automation apply there, under the DSP lock
 * (PluginController::ProcessQueuedMidi). The message thread holds the same lock for anything it
 * calls into the chain, and the audio thread outputs silence for any block that finds it held.
 * So a parameter whose change needs a rebuild that allocates (the IR cab's Normalize and Low
 * Latency, which rebuild its convolvers) is only recorded by SetParam, which then calls
 * NoteRequested(). The message thread finishes it in three steps
 * (PluginController::ApplyDeferredNodeRebuilds):
 *
 *   1. Under the DSP lock, EffectProcessor::TakeDeferredRebuild() hands over the work, with what
 *      it will read copied into it, or nullptr when nothing is waiting.
 *   2. Off the lock, Build() does the expensive part.
 *   3. Under the lock again, EffectProcessor::CommitDeferredRebuild() swaps the result in, in
 *      O(1), unless something rebuilt the effect in between. What it replaces moves into the
 *      work, to be freed after the lock is released.
 */
class DeferredRebuild
{
  public:
    virtual ~DeferredRebuild() = default;

    /// Message thread, off the DSP lock. Reads only what the work copied when it was taken.
    virtual void Build() = 0;

    /// Called by SetParam when it records a change: lock-free and allocation-free, so the audio
    /// thread may call it. Counts requests from every effect in the process, which lets the message
    /// thread tell without taking the DSP lock whether there can be anything to take.
    static void NoteRequested() noexcept
    {
        Requests().fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] static std::uint64_t RequestCount() noexcept
    {
        return Requests().load(std::memory_order_acquire);
    }

  private:
    static std::atomic<std::uint64_t>& Requests() noexcept
    {
        // Constant-initialised, so the first call from the audio thread takes no guard.
        static std::atomic<std::uint64_t> requests{0};
        return requests;
    }
};
} // namespace guitarfx
