#pragma once

// Minimal lock-free single-producer/single-consumer ring buffer.
//
// No JUCE dependency (core/ is JUCE-independent) — this is deliberately a tiny
// std::atomic-based container rather than pulling in a third-party lock-free
// library, since the only user (PracticeToolService) needs exactly this
// shape: one background thread pushes stereo frames, the audio thread pops
// them, and the audio thread must never block or allocate.
//
// Flush semantics: the PRODUCER may invalidate everything currently queued
// (used when a seek/loop/tempo/pitch change makes in-flight rendered audio
// stale) via RequestFlush(). The actual pointer move happens on the CONSUMER
// side the next time Pop() runs, which preserves the single-writer-per-index
// discipline required for this to stay lock-free:
//   - mWriteIndex is written only by the producer.
//   - mReadIndex is written only by the consumer.
//   - mFlushToIndex/mFlushGeneration are written only by the producer and
//     only ever read by the consumer; the generation counter's release/acquire
//     pair publishes mFlushToIndex (and, transitively, everything the
//     producer wrote earlier, including its latest mWriteIndex) safely.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx::util
{

template <typename T>
class SpscRingBuffer
{
public:
    explicit SpscRingBuffer(std::size_t capacity)
        : mCapacity(NextPowerOfTwo(std::max<std::size_t>(capacity, 2)))
        , mMask(mCapacity - 1)
        , mBuffer(mCapacity)
    {
    }

    /// Producer only. Returns the number of elements actually written
    /// (less than count if the buffer is full).
    std::size_t Push(const T* data, std::size_t count)
    {
        const std::size_t writeIdx = mWriteIndex.load(std::memory_order_relaxed);
        const std::size_t readIdx = mReadIndex.load(std::memory_order_acquire);
        const std::size_t used = writeIdx - readIdx;
        const std::size_t freeSpace = (used <= mCapacity) ? (mCapacity - used) : 0;
        const std::size_t n = std::min(count, freeSpace);
        for (std::size_t i = 0; i < n; ++i)
            mBuffer[(writeIdx + i) & mMask] = data[i];
        mWriteIndex.store(writeIdx + n, std::memory_order_release);
        return n;
    }

    /// Consumer only. Returns the number of elements actually read (less than
    /// count on underrun) — remaining destination slots are left untouched,
    /// so the caller fills them with silence.
    std::size_t Pop(T* dest, std::size_t count)
    {
        ApplyPendingFlush();

        const std::size_t readIdx = mReadIndex.load(std::memory_order_relaxed);
        const std::size_t writeIdx = mWriteIndex.load(std::memory_order_acquire);
        const std::size_t available = writeIdx - readIdx;
        const std::size_t n = std::min(count, available);
        for (std::size_t i = 0; i < n; ++i)
            dest[i] = mBuffer[(readIdx + i) & mMask];
        mReadIndex.store(readIdx + n, std::memory_order_release);
        return n;
    }

    /// Producer only. Marks everything currently queued as stale; the
    /// consumer discards it on its next Pop() rather than playing it out.
    /// Non-blocking: just publishes two atomics.
    void RequestFlush()
    {
        const std::size_t writeIdx = mWriteIndex.load(std::memory_order_relaxed);
        mFlushToIndex.store(writeIdx, std::memory_order_relaxed);
        mFlushGeneration.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] std::size_t AvailableToRead() const
    {
        const std::size_t writeIdx = mWriteIndex.load(std::memory_order_acquire);
        const std::size_t readIdx = mReadIndex.load(std::memory_order_acquire);
        return writeIdx - readIdx;
    }

    [[nodiscard]] std::size_t AvailableToWrite() const
    {
        const std::size_t used = AvailableToRead();
        return used <= mCapacity ? (mCapacity - used) : 0;
    }

    [[nodiscard]] std::size_t Capacity() const { return mCapacity; }

private:
    static std::size_t NextPowerOfTwo(std::size_t v)
    {
        std::size_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    // Consumer-side only: pulls in a pending producer-requested flush.
    void ApplyPendingFlush()
    {
        const auto gen = mFlushGeneration.load(std::memory_order_acquire);
        if (gen == mLastSeenFlushGeneration)
            return;
        mLastSeenFlushGeneration = gen;
        const std::size_t flushTo = mFlushToIndex.load(std::memory_order_relaxed);
        // Never move the read index backwards (a flush requested concurrently
        // with normal draining could otherwise re-expose already-read frames).
        const std::size_t current = mReadIndex.load(std::memory_order_relaxed);
        if (flushTo - current <= mCapacity)
            mReadIndex.store(flushTo, std::memory_order_release);
    }

    std::size_t mCapacity;
    std::size_t mMask;
    std::vector<T> mBuffer;

    std::atomic<std::size_t> mWriteIndex{0};
    std::atomic<std::size_t> mReadIndex{0};

    std::atomic<std::uint32_t> mFlushGeneration{0};
    std::atomic<std::size_t> mFlushToIndex{0};
    std::uint32_t mLastSeenFlushGeneration{0}; // consumer-thread-local cache
};

} // namespace guitarfx::util
