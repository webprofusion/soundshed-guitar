/**
 * @file StackSampler.h
 * @brief In-process call-stack sampler for profiling a busy thread on Windows.
 *
 * Suspends a target thread on a timer and unwinds its stack, so a profile can be
 * taken without the elevation that ETW/WPR CPU sampling requires. Raw addresses are
 * collected while the thread is suspended and resolved to names only afterwards --
 * DbgHelp takes locks and allocates, and doing either against a thread you are
 * holding suspended deadlocks the moment that thread owns the same lock.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
#endif

namespace guitarfx::profiling
{
inline constexpr int kMaxStackDepth = 64;

/// Self and inclusive sample counts for one function.
struct SampleTotals
{
    std::uint64_t self = 0;
    std::uint64_t inclusive = 0;
};

/// Immediate callers of a matched function, by sample count.
struct CallerBreakdown
{
    std::string callee;
    std::uint64_t total = 0;
    std::vector<std::pair<std::string, std::uint64_t>> callers;
};

#if defined(_WIN32)

class StackSampler
{
  public:
    StackSampler(HANDLE targetThread, std::uint64_t intervalUs) : mTargetThread(targetThread), mIntervalUs(intervalUs)
    {
    }

    void Start()
    {
        mRunning.store(true, std::memory_order_release);
        mThread = std::thread([this]() { Run(); });
    }

    void Stop()
    {
        mRunning.store(false, std::memory_order_release);

        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    [[nodiscard]] const std::vector<std::vector<DWORD64>>& Stacks() const
    {
        return mStacks;
    }

  private:
    void Run()
    {
        // The sampler runs above the DSP thread so a busy machine cannot starve it.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        const double ticksPerUs = static_cast<double>(freq.QuadPart) / 1.0e6;

        LARGE_INTEGER next{};
        QueryPerformanceCounter(&next);

        std::vector<DWORD64> frames;
        frames.reserve(kMaxStackDepth);

        while (mRunning.load(std::memory_order_acquire))
        {
            next.QuadPart += static_cast<LONGLONG>(ticksPerUs * static_cast<double>(mIntervalUs));

            LARGE_INTEGER now{};

            for (;;)
            {
                QueryPerformanceCounter(&now);

                if (now.QuadPart >= next.QuadPart)
                {
                    break;
                }

                const double remainingUs = static_cast<double>(next.QuadPart - now.QuadPart) / ticksPerUs;

                if (remainingUs > 1500.0)
                {
                    Sleep(1);
                }
                else
                {
                    YieldProcessor();
                }

                if (!mRunning.load(std::memory_order_acquire))
                {
                    return;
                }
            }

            frames.clear();
            CaptureOnce(frames);

            if (!frames.empty())
            {
                mStacks.push_back(frames);
            }
        }
    }

    void CaptureOnce(std::vector<DWORD64>& frames)
    {
        if (SuspendThread(mTargetThread) == static_cast<DWORD>(-1))
        {
            return;
        }

        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;

        if (GetThreadContext(mTargetThread, &context) != 0)
        {
            // Unwind with RtlVirtualUnwind rather than StackWalk64. StackWalk64 goes
            // through DbgHelp, which allocates -- and allocating here deadlocks outright
            // whenever the thread we just suspended happens to hold the heap lock. The
            // Rtl* pair only reads the module's .pdata, so this loop never allocates.
            for (int depth = 0; depth < kMaxStackDepth; ++depth)
            {
                if (context.Rip == 0)
                {
                    break;
                }

                frames.push_back(context.Rip);

                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION functionEntry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);

                if (functionEntry == nullptr)
                {
                    // A leaf function has no unwind data: its return address is at RSP.
                    if (context.Rsp == 0)
                    {
                        break;
                    }

                    const auto returnAddress = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rip = returnAddress;
                    context.Rsp += 8;
                    continue;
                }

                const DWORD64 previousRsp = context.Rsp;
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, functionEntry, &context, &handlerData,
                                 &establisherFrame, nullptr);

                // A frame that does not pop the stack means the unwind is not making
                // progress; stop rather than spin on the same address.
                if (context.Rsp <= previousRsp)
                {
                    break;
                }
            }
        }

        ResumeThread(mTargetThread);
    }

    HANDLE mTargetThread;
    std::uint64_t mIntervalUs;
    std::atomic<bool> mRunning{false};
    std::thread mThread;
    std::vector<std::vector<DWORD64>> mStacks;
};

inline std::string ResolveSymbol(HANDLE process, DWORD64 address, std::unordered_map<DWORD64, std::string>& cache)
{
    if (const auto it = cache.find(address); it != cache.end())
    {
        return it->second;
    }

    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    std::string name;
    DWORD64 displacement = 0;

    if (SymFromAddr(process, address, &displacement, symbol) != 0)
    {
        name = symbol->Name;
    }

    if (name.empty())
    {
        IMAGEHLP_MODULE64 moduleInfo{};
        moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

        if (SymGetModuleInfo64(process, address, &moduleInfo) != 0)
        {
            name = std::string(moduleInfo.ModuleName) + "!<unknown>";
        }
        else
        {
            name = "<unknown>";
        }
    }

    cache.emplace(address, name);
    return name;
}

#endif // _WIN32
} // namespace guitarfx::profiling
