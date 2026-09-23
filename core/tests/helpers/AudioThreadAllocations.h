#pragma once

/**
 * Counts what one thread allocates, for tests that check work meant for the audio thread
 * allocates nothing.
 *
 * Replaces the global operator new and delete, so include it in exactly one translation unit of a
 * test executable. Only a thread running inside CountOnAudioThread counts, so nothing the main
 * thread does can land in a measurement.
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <thread>

namespace audio_thread_allocations
{
inline thread_local bool tCountAllocations = false;
inline std::atomic<std::size_t> gAllocations{0};
inline std::atomic<std::size_t> gAllocatedBytes{0};

inline void NoteAllocation(std::size_t size)
{
    if (tCountAllocations)
    {
        gAllocations.fetch_add(1, std::memory_order_relaxed);
        gAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
    }
}

inline void* AlignedAllocate(std::size_t size, std::size_t alignment)
{
#if defined(_MSC_VER)
    return _aligned_malloc(std::max<std::size_t>(size, 1), alignment);
#else
    const std::size_t rounded = (std::max<std::size_t>(size, 1) + alignment - 1) / alignment * alignment;
    return std::aligned_alloc(alignment, rounded);
#endif
}

inline void AlignedFree(void* pointer)
{
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

struct Allocations
{
    std::size_t count = 0;
    std::size_t bytes = 0;
};

/// Runs `work` on a thread of its own, the stand-in for the audio thread, and returns what that
/// thread allocated while it ran.
inline Allocations CountOnAudioThread(const std::function<void()>& work)
{
    Allocations result;
    std::thread audio([&] {
        const std::size_t countBefore = gAllocations.load(std::memory_order_relaxed);
        const std::size_t bytesBefore = gAllocatedBytes.load(std::memory_order_relaxed);
        tCountAllocations = true;
        work();
        tCountAllocations = false;
        result.count = gAllocations.load(std::memory_order_relaxed) - countBefore;
        result.bytes = gAllocatedBytes.load(std::memory_order_relaxed) - bytesBefore;
    });
    audio.join();
    return result;
}

inline std::string Describe(const Allocations& allocations)
{
    return std::to_string(allocations.count) + " allocations, " + std::to_string(allocations.bytes) + " bytes";
}
} // namespace audio_thread_allocations

void* operator new(std::size_t size)
{
    audio_thread_allocations::NoteAllocation(size);

    if (void* pointer = std::malloc(std::max<std::size_t>(size, 1)))
    {
        return pointer;
    }

    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    audio_thread_allocations::NoteAllocation(size);

    if (void* pointer = audio_thread_allocations::AlignedAllocate(size, static_cast<std::size_t>(alignment)))
    {
        return pointer;
    }

    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
    audio_thread_allocations::AlignedFree(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept
{
    audio_thread_allocations::AlignedFree(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    audio_thread_allocations::AlignedFree(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    audio_thread_allocations::AlignedFree(pointer);
}
