#include "dsp/DspReaper.h"

#include <chrono>

namespace guitarfx
{
DspReaper::~DspReaper()
{
    Stop();
}

void DspReaper::Start()
{
    if (mThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = false;
        // Reserve up front so the audio thread's retire path never reallocates.
        mInstances.reserve(kQueueCapacity);
        mExecutors.reserve(kQueueCapacity);
        mMainThreadInstances.reserve(kQueueCapacity);
        mMainThreadExecutors.reserve(kQueueCapacity);
    }

    mThread = std::thread([this] { Loop(); });
}

void DspReaper::Stop()
{
    if (!mThread.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = true;
    }
    mCv.notify_all();
    mThread.join();

    // Destroy anything still queued now that the worker is gone.
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mInstances.clear();
        mExecutors.clear();
    }
    CollectMainThread();
}

void DspReaper::CollectMainThread()
{
    std::vector<std::unique_ptr<PresetInstance>> instances;
    std::vector<std::unique_ptr<SignalGraphExecutor>> executors;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mMainThreadPending.store(false, std::memory_order_relaxed);
        instances.reserve(mMainThreadInstances.size());

        for (auto& inst : mMainThreadInstances)
        {
            instances.push_back(std::move(inst));
        }

        mMainThreadInstances.clear(); // Keep capacity for audio-thread retirement.
        executors.reserve(mMainThreadExecutors.size());

        for (auto& executor : mMainThreadExecutors)
        {
            executors.push_back(std::move(executor));
        }

        mMainThreadExecutors.clear();
    }
    // Hosted editor/plugin destruction is on the message thread, outside both locks.
}

void DspReaper::Loop()
{
    while (true)
    {
        std::vector<std::unique_ptr<PresetInstance>> instances;
        std::vector<std::unique_ptr<SignalGraphExecutor>> executors;

        {
            std::unique_lock<std::mutex> lock(mMutex);
            // Poll rather than relying solely on notification: the audio thread retires
            // finished fade-outs without signalling the condition variable (notify_one is
            // not something we want on the realtime path), so a periodic wake is needed.
            mCv.wait_for(lock, std::chrono::milliseconds(250),
                         [this] { return mQuit || !mInstances.empty() || !mExecutors.empty(); });

            if (mQuit)
            {
                return;
            }

            // Move the work out and clear in place — clear() keeps the reserved capacity the
            // audio thread's allocation-free retire path depends on. Destruction of the moved
            // elements happens below, outside the lock, so a producer never waits on it.
            instances.reserve(mInstances.size());

            for (auto& inst : mInstances)
            {
                instances.push_back(std::move(inst));
            }

            mInstances.clear();

            executors.reserve(mExecutors.size());

            for (auto& exec : mExecutors)
            {
                executors.push_back(std::move(exec));
            }

            mExecutors.clear();
        }

        // instances/executors destruct here, off both the audio and message threads.
    }
}

void DspReaper::Retire(std::unique_ptr<PresetInstance> inst)
{
    if (!inst)
    {
        return;
    }

    Start();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const bool mainThread = inst->executor.AnyNodeRequiresMainThreadLoad();
        (mainThread ? mMainThreadInstances : mInstances).push_back(std::move(inst));

        if (mainThread)
        {
            mMainThreadPending.store(true, std::memory_order_release);
        }
    }
    mCv.notify_one();
}

bool DspReaper::TryRetireRealtime(std::unique_ptr<PresetInstance>& inst)
{
    // Audio thread: never block, never allocate. If the reaper happens to be draining the
    // queue, or the queue is at capacity, leave the instance in place — it is fully faded
    // out by this point, so carrying it for another block costs a silent chain and nothing
    // audible. The next block tries again.
    std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);

    if (!lock.owns_lock())
    {
        return false;
    }

    // Re-evaluate after live graph/config edits too. This query only walks processors;
    // it allocates nothing, and composites propagate their inner thread affinity.
    const bool mainThread = inst->executor.AnyNodeRequiresMainThreadLoad();
    auto& queue = mainThread ? mMainThreadInstances : mInstances;

    if (queue.size() >= queue.capacity())
    {
        return false;
    }

    queue.push_back(std::move(inst));

    if (mainThread)
    {
        mMainThreadPending.store(true, std::memory_order_release);
    }

    return true;
}

void DspReaper::RetireExecutor(std::unique_ptr<SignalGraphExecutor> executor)
{
    if (!executor)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        const bool mainThread = executor->AnyNodeRequiresMainThreadLoad();
        (mainThread ? mMainThreadExecutors : mExecutors).push_back(std::move(executor));

        if (mainThread)
        {
            mMainThreadPending.store(true, std::memory_order_release);
        }
    }
    mCv.notify_one();
}
} // namespace guitarfx
