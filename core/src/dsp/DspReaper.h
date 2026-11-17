#pragma once

#include "dsp/PresetInstance.h"
#include "dsp/SignalGraphExecutor.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace guitarfx
{
/// Deferred destruction for the chains the mixer retires.
///
/// Destroying a chain frees NAM models, convolver partition tables and node buffers.
/// Doing that on the message thread while holding the DSP lock stalls the audio thread
/// (which try_locks and outputs silence on failure), so retired preset instances and
/// global-chain executors are moved onto a queue and destroyed on a background thread
/// instead. Moving is cheap and allocation-free as long as the queue has spare capacity,
/// which lets the audio thread retire finished fade-outs itself via a try_lock.
///
/// A chain holding a hosted plugin or anything else that must be torn down on the message
/// thread (SignalGraphExecutor::AnyNodeRequiresMainThreadLoad) goes to a second queue, which
/// only CollectMainThread() empties.
///
/// Neither copyable nor movable: its thread is bound to this address, and a mixer that is
/// moved keeps its own reaper.
class DspReaper
{
  public:
    DspReaper() = default;
    ~DspReaper();
    DspReaper(const DspReaper&) = delete;
    DspReaper& operator=(const DspReaper&) = delete;
    DspReaper(DspReaper&&) = delete;
    DspReaper& operator=(DspReaper&&) = delete;

    /// Brings the thread up and reserves the queues. Idempotent. Call it before any swap
    /// can happen, so retiring never has to spawn a thread while the DSP lock is held.
    void Start();

    /// Joins the thread and destroys everything still queued, including the message-thread
    /// queue — so call it on the message thread.
    void Stop();

    /// Message thread: always succeeds, may allocate, wakes the thread. Starts it if needed.
    void Retire(std::unique_ptr<PresetInstance> inst);

    /// Audio thread: never blocks, never allocates. False if the caller should keep the
    /// instance (it is already silent) and try again on the next block — the thread is
    /// draining the queue, or the queue is at capacity.
    [[nodiscard]] bool TryRetireRealtime(std::unique_ptr<PresetInstance>& inst);

    /// Message thread, under the DSP lock: queues an executor the caller has just replaced.
    /// Its worker threads go with it, so nothing is joined under the lock.
    void RetireExecutor(std::unique_ptr<SignalGraphExecutor> executor);

    /// Message thread only, without the DSP lock. Hosted processors (including composites)
    /// must be destroyed here rather than on the reaper thread, which may wait on their
    /// editor UI.
    void CollectMainThread();

    /// Whether the message-thread queue holds anything, so a caller polling at a timer's rate
    /// collects only when there is something to collect. Any thread.
    [[nodiscard]] bool HasMainThreadWork() const noexcept
    {
        return mMainThreadPending.load(std::memory_order_acquire);
    }

    /// Capacity the queues are reserved to, and so how many retirements the audio thread
    /// can make between two passes of the reaper thread.
    static constexpr std::size_t kQueueCapacity = 16;

  private:
    void Loop();

    std::vector<std::unique_ptr<PresetInstance>> mInstances;
    std::vector<std::unique_ptr<SignalGraphExecutor>> mExecutors;
    std::vector<std::unique_ptr<PresetInstance>> mMainThreadInstances;
    std::vector<std::unique_ptr<SignalGraphExecutor>> mMainThreadExecutors;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::thread mThread;
    bool mQuit = false;
    std::atomic<bool> mMainThreadPending{false};
};
} // namespace guitarfx
