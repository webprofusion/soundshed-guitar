#include "AndroidAudioThread.h"

#if defined(__ANDROID__)
    #include <android/log.h>
    #include <dlfcn.h>
    #include <sys/system_properties.h>
    #include <time.h>
    #include <unistd.h>

    #include <algorithm>
    #include <cmath>
    #include <cstdio>
    #include <cstring>
    #include <vector>
#endif

namespace soundshed::android_audio
{
#if defined(__ANDROID__)
namespace
{
constexpr const char* kLogTag = "Sepia";
constexpr const char* kPolicyProperty = "debug.soundshed.audio.cores";

// The NDK's APerformanceHint_* functions arrived in API 33 and minSdk is 29, so they are looked
// up at runtime, as Oboe's AdpfWrapper does.
using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void* manager, const std::int32_t* tids, std::size_t count, std::int64_t targetNanos);
using UpdateTargetFn = int (*)(void* session, std::int64_t targetNanos);
using ReportActualFn = int (*)(void* session, std::int64_t actualNanos);
using CloseSessionFn = void (*)(void* session);

struct HintApi
{
    GetManagerFn getManager = nullptr;
    CreateSessionFn createSession = nullptr;
    UpdateTargetFn updateTarget = nullptr;
    ReportActualFn reportActual = nullptr;
    CloseSessionFn closeSession = nullptr;

    [[nodiscard]] bool usable() const noexcept
    {
        return getManager && createSession && updateTarget && reportActual && closeSession;
    }
};

HintApi gHintApi;

void BindHintApi()
{
    static const bool bound = [] {
        // libandroid is always loaded in an app process; RTLD_NODELETE keeps the symbols valid.
        if (void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NODELETE))
        {
            gHintApi.getManager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
            gHintApi.createSession = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
            gHintApi.updateTarget =
                reinterpret_cast<UpdateTargetFn>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
            gHintApi.reportActual =
                reinterpret_cast<ReportActualFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
            gHintApi.closeSession = reinterpret_cast<CloseSessionFn>(dlsym(lib, "APerformanceHint_closeSession"));
        }

        return true;
    }();
    (void)bound;
}

std::int64_t NowNanos() noexcept
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// A cpufreq or capacity file's single number, or -1.
long ReadNumber(const char* path)
{
    long value = -1;

    if (FILE* file = std::fopen(path, "r"))
    {
        if (std::fscanf(file, "%ld", &value) != 1)
        {
            value = -1;
        }

        std::fclose(file);
    }

    return value;
}

// How fast each core is, by the kernel's own measure: cpu_capacity is what the energy-aware
// scheduler places by (1024 for the fastest class); the maximum clock stands in without it.
std::vector<long> ReadCoreCapacities()
{
    std::vector<long> capacities;
    char path[96];

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        long capacity = ReadNumber(path);

        if (capacity <= 0)
        {
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            capacity = ReadNumber(path);
        }

        if (capacity <= 0)
        {
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);

            // A core with neither file but present (offline, say) is skipped; past the last one, stop.
            if (access(path, F_OK) != 0)
            {
                break;
            }
        }

        capacities.push_back(capacity);
    }

    return capacities;
}

void FormatCores(const cpu_set_t& set, char* out, std::size_t size) noexcept
{
    std::size_t used = 0;
    out[0] = '\0';

    for (int cpu = 0; cpu < CPU_SETSIZE && used + 5 < size; ++cpu)
    {
        if (CPU_ISSET(cpu, &set))
        {
            used += static_cast<std::size_t>(std::snprintf(out + used, size - used, used ? ",%d" : "%d", cpu));
        }
    }
}
} // namespace

AudioThreadPlacement::~AudioThreadPlacement()
{
    closeSession();
}

void AudioThreadPlacement::prepare(double sampleRate)
{
    closeSession();
    mPlacedTid = 0;
    mTargetNanos = 0;
    mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

    char value[PROP_VALUE_MAX] = {};
    __system_property_get(kPolicyProperty, value);

    if (std::strcmp(value, "big") == 0)
    {
        mPolicy = Policy::big;
    }
    else if (std::strcmp(value, "prime") == 0)
    {
        mPolicy = Policy::prime;
    }
    else if (std::strcmp(value, "off") == 0)
    {
        mPolicy = Policy::off;
    }
    else
    {
        mPolicy = Policy::automatic;
    }

    // Every core, not just the ones this process may use now: Android moves the app between
    // cpusets as it comes and goes (cpu0-2 only, behind the lock screen, on a Galaxy S21), and a
    // set worked out in the background would be the little cores alone. The kernel narrows a pin
    // to whatever the cpuset allows, and checkAffinity() pins again when that changes.
    const auto capacities = ReadCoreCapacities();
    long slowest = 0;
    long fastest = 0;

    for (std::size_t cpu = 0; cpu < capacities.size(); ++cpu)
    {
        if (capacities[cpu] <= 0)
        {
            continue;
        }

        slowest = slowest == 0 ? capacities[cpu] : std::min(slowest, capacities[cpu]);
        fastest = std::max(fastest, capacities[cpu]);
    }

    CPU_ZERO(&mBigCores);
    CPU_ZERO(&mPrimeCores);

    for (std::size_t cpu = 0; cpu < capacities.size(); ++cpu)
    {
        if (capacities[cpu] <= 0)
        {
            continue;
        }

        if (capacities[cpu] > slowest)
        {
            CPU_SET(static_cast<int>(cpu), &mBigCores);
        }

        if (capacities[cpu] == fastest)
        {
            CPU_SET(static_cast<int>(cpu), &mPrimeCores);
        }
    }

    // One core class (every core alike) has nothing to choose between.
    mHaveBigCores = CPU_COUNT(&mBigCores) > 0;
    mHavePrimeCores = mHaveBigCores && CPU_COUNT(&mPrimeCores) > 0;

    BindHintApi();
}

void AudioThreadPlacement::release()
{
    closeSession();
    mPlacedTid = 0;
}

void AudioThreadPlacement::beginBlock() noexcept
{
    // Bionic caches the tid in the thread, so this costs no system call.
    if (gettid() != mPlacedTid)
    {
        placeCurrentThread();
    }
    else if (mPin != nullptr && --mBlocksUntilAffinityCheck <= 0)
    {
        checkAffinity();
    }

    if (mSession != nullptr)
    {
        mBlockStartNanos = NowNanos();
    }
}

void AudioThreadPlacement::endBlock(int numSamples) noexcept
{
    if (mSession == nullptr || numSamples <= 0)
    {
        return;
    }

    const auto actual = NowNanos() - mBlockStartNanos;
    const auto target = static_cast<std::int64_t>(std::llround(1.0e9 * numSamples / mSampleRate));

    if (target != mTargetNanos)
    {
        mTargetNanos = target;
        gHintApi.updateTarget(mSession, target);
    }

    gHintApi.reportActual(mSession, std::max<std::int64_t>(actual, 1));
}

void AudioThreadPlacement::placeCurrentThread() noexcept
{
    // Once per callback thread: AAudio makes a new one when a stream is reopened. The session
    // and affinity are system calls, not real-time safe, but this runs only on that first block.
    const pid_t tid = gettid();
    mPlacedTid = tid;
    mPin = nullptr;
    mPinState = PinState::unknown;
    closeSession();

    if (mPolicy == Policy::off)
    {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "Audio thread %d: left to the scheduler (cores=off)", tid);
        return;
    }

    if (gHintApi.usable())
    {
        if (void* manager = gHintApi.getManager())
        {
            const std::int32_t tids[] = {tid};
            // A guess at the first block's budget; endBlock corrects it from the real block size.
            mTargetNanos = static_cast<std::int64_t>(std::llround(1.0e9 * 96.0 / mSampleRate));
            mSession = gHintApi.createSession(manager, tids, 1, mTargetNanos);
        }
    }

    if (mPolicy == Policy::prime && mHavePrimeCores)
    {
        mPin = &mPrimeCores;
    }
    else if (mPolicy == Policy::big && mHaveBigCores)
    {
        mPin = &mBigCores;
    }
    else if (mPolicy == Policy::automatic && mSession == nullptr && mHaveBigCores)
    {
        mPin = &mBigCores;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Audio thread %d: performance hint %s%s", tid,
                        mSession != nullptr ? "on" : "unavailable",
                        mPin != nullptr ? "" : ", cores left to the scheduler");

    if (mPin != nullptr)
    {
        checkAffinity();
    }
}

void AudioThreadPlacement::checkAffinity() noexcept
{
    // A few times a second. Android moves the app between cpusets as it goes to the background
    // and back, and on older kernels (5.4 on a Galaxy S21) each move resets every thread's
    // affinity to the new cpuset, so a pin set once does not last. Reading it back is one cheap
    // system call; setting it again only happens after such a move.
    mBlocksUntilAffinityCheck = std::max(1, static_cast<int>(mSampleRate / (4.0 * 96.0)));

    cpu_set_t current;
    CPU_ZERO(&current);

    if (sched_getaffinity(0, sizeof(current), &current) != 0)
    {
        return;
    }

    if (mPinState == PinState::pinned && CPU_EQUAL(&current, &mPinnedAs))
    {
        return;
    }

    // Fails while the cpuset holds none of the wanted cores (the background one), and is tried
    // again at the next check; otherwise the kernel narrows it to the cores the cpuset allows.
    const bool pinned =
        sched_setaffinity(0, sizeof(cpu_set_t), mPin) == 0 && sched_getaffinity(0, sizeof(mPinnedAs), &mPinnedAs) == 0;
    const auto state = pinned ? PinState::pinned : PinState::refused;

    if (state != mPinState || pinned)
    {
        char cores[96] = "";
        FormatCores(pinned ? mPinnedAs : current, cores, sizeof(cores));
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            pinned ? "Audio thread %d: pinned to cores %s"
                                   : "Audio thread %d: cannot pin, cpuset allows cores %s",
                            mPlacedTid, cores);
    }

    mPinState = state;
}

void AudioThreadPlacement::closeSession() noexcept
{
    if (mSession != nullptr && gHintApi.closeSession != nullptr)
    {
        gHintApi.closeSession(mSession);
    }

    mSession = nullptr;
}

#else

AudioThreadPlacement::~AudioThreadPlacement() = default;

void AudioThreadPlacement::prepare(double)
{
}

void AudioThreadPlacement::release()
{
}

#endif
} // namespace soundshed::android_audio
