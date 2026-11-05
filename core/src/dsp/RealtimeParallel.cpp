#include "dsp/RealtimeParallel.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <optional>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <dispatch/dispatch.h>
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <mach/thread_policy.h>
    #include <pthread.h>
#else
    #include <cerrno>
    #include <pthread.h>
    #include <sched.h>
    #include <semaphore.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    #include <xmmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
    #include <float.h>
#endif

namespace guitarfx
{
namespace rtparallel
{
namespace
{
constexpr int kHeadShift = 0;
constexpr int kCountShift = 8;
constexpr int kGenerationShift = 16;
constexpr std::uint64_t kFieldMask = 0xFF;
static_assert(RealtimeTaskPool::kMaxTasks == static_cast<int>(kFieldMask), "the count and head are 8-bit fields");

constexpr int HeadOf(std::uint64_t word) noexcept
{
    return static_cast<int>((word >> kHeadShift) & kFieldMask);
}

constexpr int CountOf(std::uint64_t word) noexcept
{
    return static_cast<int>((word >> kCountShift) & kFieldMask);
}

// How long a dispatcher spins on the last tasks before it starts yielding its core. Spinning
// is the point on the fast path, where the helpers are running and finish in microseconds.
// But a helper that has not been scheduled cannot be waited out by spinning: the audio
// thread usually holds the DSP lock here, so starving the helper behind it would hang every
// message-thread handler waiting on that lock, not just this block.
constexpr int kSpinsBeforeYield = 10000;

std::atomic<RealtimeTaskPool::ClaimHook> gWorkerClaimHook{nullptr};

#if defined(_WIN32)
int CurrentCpu() noexcept
{
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return number.Group * 64 + number.Number;
}

// Keeps a worker off the physical core its dispatcher runs on. Windows 11 on an 8-core Ryzen woke
// the dual-lane helper on the audio thread's SMT sibling in 70-98% of blocks, paced or back to
// back. There both lanes ran at about half speed while the audio thread spun on the helper, so the
// hand-off saved at most 25% of the block, often nothing, for 1.5-2x the CPU. Narrowing the worker's selected CPU sets
// (Windows 10 and later) to the other cores stopped it. Setting its ideal processor instead left
// it on the sibling in up to half the blocks, and keeping it off the dispatcher's last two cores
// cost more at small blocks than it saved: waking a sleeping core is slower than waking the
// sibling of a busy one. docs/nam-dual-lane-threshold-2026-10-16.md has the numbers.
class CoreAvoidance
{
  public:
    static const CoreAvoidance& Instance()
    {
        static const CoreAvoidance instance;
        return instance;
    }

    // The physical core of a logical processor (CurrentCpu() numbering), or -1.
    [[nodiscard]] int CoreOf(int cpu) const noexcept
    {
        return cpu >= 0 && cpu < static_cast<int>(mCoreOfCpu.size()) ? mCoreOfCpu[static_cast<std::size_t>(cpu)] : -1;
    }

    [[nodiscard]] std::size_t CpuSetCount() const noexcept
    {
        return mCpuSets.size();
    }

    // Narrows the calling thread to the cores other than `avoid`. `scratch` holds CpuSetCount()
    // ids, reserved by the caller so this never allocates. A no-op with one core, or before
    // Windows 10.
    void AvoidOnThisThread(int avoid, std::vector<ULONG>& scratch) const noexcept
    {
        if (mSetSelectedCpuSets == nullptr || scratch.capacity() < mCpuSets.size())
        {
            return;
        }

        scratch.clear();

        for (const auto& [id, core] : mCpuSets)
        {
            if (core != avoid)
            {
                scratch.push_back(id);
            }
        }

        mSetSelectedCpuSets(GetCurrentThread(), scratch.data(), static_cast<ULONG>(scratch.size()));
    }

  private:
    using GetSystemCpuSetInformationFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    using SetThreadSelectedCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

    CoreAvoidance()
    {
        // Looked up rather than linked, so the core still loads where they do not exist.
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const auto getInformation =
            kernel
                ? reinterpret_cast<GetSystemCpuSetInformationFn>(GetProcAddress(kernel, "GetSystemCpuSetInformation"))
                : nullptr;
        const auto setSelected =
            kernel ? reinterpret_cast<SetThreadSelectedCpuSetsFn>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"))
                   : nullptr;
        ULONG length = 0;

        if (getInformation == nullptr || setSelected == nullptr)
        {
            return;
        }

        getInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        std::vector<std::uint8_t> buffer(length);

        if (length == 0 || !getInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length,
                                           &length, GetCurrentProcess(), 0))
        {
            return;
        }

        // Logical processors on one core share its group and CoreIndex.
        std::vector<std::pair<int, int>> coreKeys;

        for (ULONG offset = 0; offset < length;)
        {
            const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            offset += entry->Size;

            if (entry->Type != CpuSetInformation)
            {
                continue;
            }

            const std::pair<int, int> key{entry->CpuSet.Group, entry->CpuSet.CoreIndex};
            auto found = std::find(coreKeys.begin(), coreKeys.end(), key);

            if (found == coreKeys.end())
            {
                found = coreKeys.insert(coreKeys.end(), key);
            }

            const int core = static_cast<int>(found - coreKeys.begin());
            const auto cpu = static_cast<std::size_t>(entry->CpuSet.Group) * 64 + entry->CpuSet.LogicalProcessorIndex;

            if (cpu >= mCoreOfCpu.size())
            {
                mCoreOfCpu.resize(cpu + 1, -1);
            }

            mCoreOfCpu[cpu] = core;
            mCpuSets.emplace_back(entry->CpuSet.Id, core);
        }

        if (coreKeys.size() >= 2) // else there is nowhere else to go
        {
            mSetSelectedCpuSets = setSelected;
        }
    }

    SetThreadSelectedCpuSetsFn mSetSelectedCpuSets = nullptr;
    std::vector<int> mCoreOfCpu;
    std::vector<std::pair<ULONG, int>> mCpuSets; // id, core
};

// One worker's side of it, run on the worker after its tasks are done, so the system call (about
// 0.5 µs) never sits between the audio thread and the end of its block. It places the next wake,
// and runs only when the dispatcher has moved to another core. All Run() adds is reading the
// dispatcher's processor number (about a nanosecond).
class DispatcherCoreAvoider
{
  public:
    DispatcherCoreAvoider()
    {
        mScratch.reserve(CoreAvoidance::Instance().CpuSetCount());
    }

    void Follow(int dispatcherCpu) noexcept
    {
        const auto& avoidance = CoreAvoidance::Instance();
        const int core = avoidance.CoreOf(dispatcherCpu);

        if (core >= 0 && core != mAvoided)
        {
            mAvoided = core;
            avoidance.AvoidOnThisThread(core, mScratch);
        }
    }

  private:
    int mAvoided = -1;
    std::vector<ULONG> mScratch;
};
#endif
} // namespace

void FlushDenormalsOnThisThread() noexcept
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#elif defined(_MSC_VER) && defined(_M_ARM64)
    _control87(_DN_FLUSH, _MCW_DN); // FPCR.FZ
#elif defined(__aarch64__)
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" : : "r"(fpcr | (std::uint64_t{1} << 24))); // FZ
#elif defined(__arm__) && defined(__ARM_FP)
    std::uint32_t fpscr = 0;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr | (std::uint32_t{1} << 24))); // FZ
#endif
}

void LowerPriorityOfThisThread() noexcept
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void RaisePriorityOfThisThreadForAudio() noexcept
{
#if defined(_WIN32)
    // MMCSS "Pro Audio", the class hosts and drivers run their own audio threads in. avrt.dll
    // is loaded rather than linked so a system without it still gets the fallback.
    using SetCharacteristicsFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    static const auto setCharacteristics = []() -> SetCharacteristicsFn {
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        return avrt ? reinterpret_cast<SetCharacteristicsFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))
                    : nullptr;
    }();

    DWORD taskIndex = 0;

    if (setCharacteristics == nullptr || setCharacteristics(L"Pro Audio", &taskIndex) == nullptr)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

#elif defined(__APPLE__)
    // A time-constraint thread, as the host's own audio thread is. Without this a helper is an
    // ordinary thread: when the host is not the frontmost app (a minimised window will do it)
    // macOS lowers it, and on Apple silicon moves it to an efficiency core, while the audio
    // thread waits for it. Aperiodic, since a helper wakes when a block hands it work; the
    // numbers bound one lane of one block, well under the 50 ms the kernel accepts.
    mach_timebase_info_data_t timebase{};

    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0)
    {
        return;
    }

    const auto nsToTicks = [&timebase](std::uint64_t ns) {
        return static_cast<std::uint32_t>(ns * timebase.denom / timebase.numer);
    };

    thread_time_constraint_policy_data_t policy{};
    policy.period = 0;
    policy.computation = nsToTicks(5'000'000);
    policy.constraint = nsToTicks(10'000'000);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(__linux__) && !defined(__ANDROID__)
    // Only granted with an rtprio limit (the audio group on most distributions); otherwise the
    // call fails and the helper stays an ordinary thread, as before. SOUNDSHED_AUDIO_PRIO is the
    // priority a deployment gives its audio thread (the kiosk does): the audio thread waits on a
    // helper, so a helper below it would hold it back behind whatever sits between them.
    const char* const priority = std::getenv("SOUNDSHED_AUDIO_PRIO");
    sched_param param{};
    param.sched_priority = std::clamp(priority != nullptr ? std::atoi(priority) : 0,
                                      std::max(1, sched_get_priority_min(SCHED_FIFO)),
                                      sched_get_priority_max(SCHED_FIFO));
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

RealtimeTaskPool::Semaphore::~Semaphore()
{
    if (!mHandle)
    {
        return;
    }

#if defined(_WIN32)
    CloseHandle(static_cast<HANDLE>(mHandle));
#elif defined(__APPLE__)
    dispatch_release(static_cast<dispatch_semaphore_t>(mHandle));
#else
    auto* semaphore = static_cast<sem_t*>(mHandle);
    sem_destroy(semaphore);
    delete semaphore;
#endif
}

bool RealtimeTaskPool::Semaphore::Create()
{
    if (mHandle)
    {
        return true;
    }

#if defined(_WIN32)
    mHandle = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
#elif defined(__APPLE__)
    mHandle = dispatch_semaphore_create(0);
#else
    auto* semaphore = new sem_t;

    if (sem_init(semaphore, 0, 0) == 0)
    {
        mHandle = semaphore;
    }
    else
    {
        delete semaphore;
    }

#endif

    return mHandle != nullptr;
}

void RealtimeTaskPool::Semaphore::Post(int count) noexcept
{
    if (count <= 0)
    {
        return;
    }

#if defined(_WIN32)
    ReleaseSemaphore(static_cast<HANDLE>(mHandle), count, nullptr);
#elif defined(__APPLE__)

    for (int i = 0; i < count; ++i)
    {
        dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(mHandle));
    }

#else

    for (int i = 0; i < count; ++i)
    {
        sem_post(static_cast<sem_t*>(mHandle));
    }

#endif
}

void RealtimeTaskPool::Semaphore::Wait() noexcept
{
#if defined(_WIN32)
    WaitForSingleObject(static_cast<HANDLE>(mHandle), INFINITE);
#elif defined(__APPLE__)
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(mHandle), DISPATCH_TIME_FOREVER);
#else

    while (sem_wait(static_cast<sem_t*>(mHandle)) != 0 && errno == EINTR)
    {
    }

#endif
}

bool RealtimeTaskPool::Semaphore::TryWait() noexcept
{
#if defined(_WIN32)
    return WaitForSingleObject(static_cast<HANDLE>(mHandle), 0) == WAIT_OBJECT_0;
#elif defined(__APPLE__)
    return dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(mHandle), DISPATCH_TIME_NOW) == 0;
#else

    while (sem_trywait(static_cast<sem_t*>(mHandle)) != 0)
    {
        if (errno != EINTR)
        {
            return false;
        }
    }

    return true;
#endif
}

// ---------------------------------------------------------------------------
// RealtimeTaskPool
// ---------------------------------------------------------------------------

RealtimeTaskPool::~RealtimeTaskPool()
{
    Stop();
}

void RealtimeTaskPool::SetWorkerClaimHookForTesting(ClaimHook hook) noexcept
{
    gWorkerClaimHook.store(hook, std::memory_order_release);
}

void RealtimeTaskPool::Start(int workerCount, bool avoidDispatcherCore)
{
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    StopLocked();

    if (workerCount <= 0 || !mWake.Create())
    {
        return;
    }

    mQuit.store(false, std::memory_order_seq_cst);
    mDispatcherCpu.store(-1, std::memory_order_relaxed);
    mAvoidDispatcherCore.store(false, std::memory_order_relaxed);

#if defined(_WIN32)

    if (avoidDispatcherCore)
    {
        (void)CoreAvoidance::Instance(); // reads the topology here, on the message thread
        mAvoidDispatcherCore.store(true, std::memory_order_relaxed);
    }

#else
    (void)avoidDispatcherCore;
#endif

    try
    {
        mThreads.reserve(static_cast<std::size_t>(workerCount));

        for (int i = 0; i < workerCount; ++i)
        {
            mThreads.emplace_back([this] { WorkerLoop(); });
        }
    }
    catch (...)
    {
        // Fewer helpers rather than none: the dispatcher does whatever they cannot.
    }

    mWorkerCount.store(static_cast<int>(mThreads.size()), std::memory_order_release);
}

void RealtimeTaskPool::Stop()
{
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    StopLocked();
}

void RealtimeTaskPool::StopLocked()
{
    if (mThreads.empty())
    {
        return;
    }

    // A Run() already in flight keeps going: a worker finishes the task it has claimed
    // before it looks at mQuit, and the dispatcher takes whatever is left unclaimed.
    mWorkerCount.store(0, std::memory_order_release);
    mQuit.store(true, std::memory_order_seq_cst);

    const int count = static_cast<int>(mThreads.size());
    mPendingWakes.fetch_add(count, std::memory_order_seq_cst);
    mWake.Post(count);

    for (auto& thread : mThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    mThreads.clear();

    // Wake-ups nobody took, so the next workers do not start by waking for nothing.
    while (mWake.TryWait())
    {
        mPendingWakes.fetch_sub(1, std::memory_order_seq_cst);
    }
}

void RealtimeTaskPool::WorkerLoop()
{
    FlushDenormalsOnThisThread();
    RaisePriorityOfThisThreadForAudio();

#if defined(_WIN32)
    std::optional<DispatcherCoreAvoider> avoider;

    if (mAvoidDispatcherCore.load(std::memory_order_relaxed))
    {
        avoider.emplace();
    }

#endif

    while (true)
    {
        mWake.Wait();
        mPendingWakes.fetch_sub(1, std::memory_order_seq_cst);

        if (mQuit.load(std::memory_order_seq_cst))
        {
            return;
        }

        RunClaims(true);

#if defined(_WIN32)

        if (avoider)
        {
            avoider->Follow(mDispatcherCpu.load(std::memory_order_relaxed));
        }

#endif
    }
}

void RealtimeTaskPool::WakeWorkers(int wanted) noexcept
{
    // The claim word was published (seq_cst) before this read, and a waking worker takes
    // its wake-up off the count (seq_cst) before it reads the word. So either this read
    // sees that decrement and posts another, or the worker sees this dispatch.
    int pending = mPendingWakes.load(std::memory_order_seq_cst);

    while (pending < wanted)
    {
        if (mPendingWakes.compare_exchange_weak(pending, wanted, std::memory_order_seq_cst))
        {
            mWake.Post(wanted - pending);
            return;
        }
    }
}

void RealtimeTaskPool::RunClaims(bool onWorker) noexcept
{
    while (true)
    {
        if (onWorker)
        {
            if (const ClaimHook hook = gWorkerClaimHook.load(std::memory_order_acquire))
            {
                hook();
            }
        }

        std::uint64_t word = mClaim.load(std::memory_order_seq_cst);
        int index = -1;

        while (index < 0)
        {
            if (HeadOf(word) >= CountOf(word))
            {
                return;
            }

            // Succeeds only if the word is still the one just read, generation included, so
            // the index belongs to the dispatch it was read from. A failure reloads the word,
            // which may be a newer dispatch; claiming from that one is just as valid.
            if (mClaim.compare_exchange_weak(word, word + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                index = HeadOf(word);
            }
        }

        mTask(mContext, index);
        mDone.fetch_add(1, std::memory_order_release);
    }
}

void RealtimeTaskPool::Run(TaskFn task, void* context, int count) noexcept
{
    if (count <= 0)
    {
        return;
    }

    const int workers = WorkerCount();
    const int fanned = std::min(count, kMaxTasks);

    if (workers == 0 || fanned < 2 || detail::tSharedHelpersRefused)
    {
        for (int i = 0; i < count; ++i)
        {
            task(context, i);
        }

        return;
    }

    // Every index of the previous dispatch finished before its Run() returned, so no worker
    // is reading these, and none can count towards this dispatch until the word below
    // publishes it.
    mTask = task;
    mContext = context;
    mDone.store(0, std::memory_order_relaxed);

#if defined(_WIN32)

    if (mAvoidDispatcherCore.load(std::memory_order_relaxed))
    {
        mDispatcherCpu.store(CurrentCpu(), std::memory_order_relaxed);
    }

#endif

    ++mGeneration;
    mClaim.store((mGeneration << kGenerationShift) | (static_cast<std::uint64_t>(fanned) << kCountShift),
                 std::memory_order_seq_cst);

    WakeWorkers(std::min(fanned - 1, workers));
    RunClaims(false);

    for (int i = fanned; i < count; ++i)
    {
        task(context, i);
    }

    int spins = 0;

    while (mDone.load(std::memory_order_acquire) < fanned)
    {
        if (++spins < kSpinsBeforeYield)
        {
            CpuRelax();
        }
        else
        {
            std::this_thread::yield();
        }
    }
}

// ---------------------------------------------------------------------------
// DualLaneExecutor
// ---------------------------------------------------------------------------

DualLaneExecutor& DualLaneExecutor::Instance()
{
    static DualLaneExecutor instance;
    return instance;
}

DualLaneExecutor::Lease DualLaneExecutor::Acquire()
{
    auto& executor = Instance();
    const std::lock_guard<std::mutex> lock(executor.mLeaseMutex);

    if (executor.mLeases++ == 0 && kParallelDspSupported && std::thread::hardware_concurrency() >= 2)
    {
        executor.mPool.Start(1, true);
    }

    return Lease(true);
}

int DualLaneExecutor::LeaseCountForTesting()
{
    auto& executor = Instance();
    const std::lock_guard<std::mutex> lock(executor.mLeaseMutex);
    return executor.mLeases;
}

void DualLaneExecutor::Release()
{
    const std::lock_guard<std::mutex> lock(mLeaseMutex);

    if (mLeases > 0 && --mLeases == 0)
    {
        // Nobody can be inside Run(): every caller is an effect that holds a Lease. Were one
        // mid-dispatch anyway, Stop() lets it finish, as it does for the other pools.
        mPool.Stop();
    }
}

DualLaneExecutor::Lease::~Lease()
{
    if (mHeld)
    {
        Instance().Release();
    }
}

DualLaneExecutor::Lease& DualLaneExecutor::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other)
    {
        if (mHeld)
        {
            Instance().Release();
        }

        mHeld = std::exchange(other.mHeld, false);
    }

    return *this;
}
} // namespace rtparallel
} // namespace guitarfx
