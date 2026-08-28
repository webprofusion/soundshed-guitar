/**
 * @file PracticeToolCrossfadeTests.cpp
 * @brief Focused tests for PracticeToolService's loop-wrap/seek crossfade
 * logic (ReadSourceWindow / BeginCrossfade), per the design plan's "Loop-
 * boundary handling" requirement: stretch.reset() must never be called on a
 * loop wrap or seek, so wraps are handled entirely in the source domain with
 * a short equal-power crossfade. These tests exercise that source-domain
 * logic directly and synchronously (via a friend test-access struct), with a
 * synthetic in-memory buffer — no file I/O, no background render thread
 * timing involved.
 */

#include "controller/PracticeToolService.h"
#include "IPluginHost.h"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace guitarfx
{

// Friend accessor granted by PracticeToolService — exposes just enough
// of its private surface (ReadSourceWindow + the TrackBuffer type) to drive
// the crossfade logic directly with a synthetic buffer.
struct PracticeToolServiceTestAccess
{
    static std::shared_ptr<PracticeToolService::TrackBuffer> MakeBuffer(std::vector<float> left,
                                                                        std::vector<float> right, double sampleRate)
    {
        auto buffer = std::make_shared<PracticeToolService::TrackBuffer>();
        buffer->sampleRate = sampleRate;
        buffer->channels = 2;
        buffer->totalFrames = left.size();
        buffer->channelSamples = {std::move(left), std::move(right)};
        return buffer;
    }

    static int ReadSourceWindow(PracticeToolService& svc,
                                const std::shared_ptr<PracticeToolService::TrackBuffer>& buffer, float* outL,
                                float* outR, std::size_t& cursor, int numFrames)
    {
        return svc.ReadSourceWindow(buffer, outL, outR, cursor, numFrames);
    }
};

} // namespace guitarfx

namespace
{

using guitarfx::PracticeToolService;
using guitarfx::PracticeToolServiceTestAccess;

constexpr double kSampleRate = 48000.0;

// Minimal IPluginHost stub. The crossfade logic under test never touches the
// host; PracticeToolService's constructor just requires a reference.
class NullPluginHost : public guitarfx::IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)>) override
    {
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)>) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        if (fn)
        {
            fn();
        }
    }

    [[nodiscard]] std::filesystem::path GetUserDataPath() const override
    {
        return {};
    }

    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override
    {
        return {};
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }
};

std::unique_ptr<PracticeToolService> MakeService(guitarfx::IPluginHost& host, std::mutex& mutex)
{
    auto svc = std::make_unique<PracticeToolService>(
        host, mutex, [](const std::string&, const std::string&) {}, [](const std::string&) {});
    // Starts the background render thread, but since we never LoadFile()
    // through the public API in these tests, mBuffer stays null and the
    // thread just idles — harmless. Needed to give the service a sample
    // rate (ReadSourceWindow/BeginCrossfade read it for the crossfade length).
    svc->Prepare(kSampleRate, 512);
    return svc;
}

// A loop region whose two endpoints hold clearly different values (a ramp
// from -1 at regionStart to ~+1 just before regionEnd, held flat at +1
// afterward) — representing a realistic "arbitrary" loop selection that
// doesn't happen to start/end at matching phase. A hard cut at the wrap
// would jump ~2.0 in a single sample; a proper crossfade should ramp
// smoothly over the crossfade window instead.
// Return type is deduced (rather than spelling PracticeToolService::
// TrackBuffer) because this function is not itself a friend of
// PracticeToolService — only PracticeToolServiceTestAccess is, so
// only that struct's members may name the private nested type directly.
auto MakeRampLoopBuffer(std::size_t total, std::size_t regionStart, std::size_t regionEnd)
{
    std::vector<float> left(total, -1.0f);
    const double span = static_cast<double>(regionEnd - regionStart);
    for (std::size_t i = regionStart; i < regionEnd; ++i)
    {
        const double t = static_cast<double>(i - regionStart) / span;
        left[i] = static_cast<float>(-1.0 + 2.0 * t);
    }
    for (std::size_t i = regionEnd; i < total; ++i)
    {
        left[i] = 1.0f; // continuous with the ramp's end value
    }

    return PracticeToolServiceTestAccess::MakeBuffer(left, left, kSampleRate);
}

bool TestLoopWrapStaysInBoundsAndBlends()
{
    std::cout << "\n--- PracticeToolService Loop-Wrap Crossfade Tests ---\n";

    NullPluginHost host;
    std::mutex dspMutex;
    auto svc = MakeService(host, dspMutex);

    constexpr std::size_t kTotalFrames = 20000;
    constexpr std::size_t kLoopStart = 5000;
    constexpr std::size_t kLoopEnd = 15000;
    auto buffer = MakeRampLoopBuffer(kTotalFrames, kLoopStart, kLoopEnd);

    svc->SetLoopRegion(static_cast<double>(kLoopStart) / kSampleRate, static_cast<double>(kLoopEnd) / kSampleRate);
    svc->SetLoopingEnabled(true);

    constexpr int kChunk = 777;     // deliberately not a divisor of the loop length
    constexpr int kNumChunks = 200; // several full loop traversals at this chunk size
    std::vector<float> outL(static_cast<std::size_t>(kChunk));
    std::vector<float> outR(static_cast<std::size_t>(kChunk));
    std::size_t cursor = kLoopStart;

    bool everyCallFullyFilled = true;
    bool cursorAlwaysInBounds = true;
    bool noNonFiniteSamples = true;
    float maxAbsDelta = 0.0f;
    float prevSample = -1.0f;
    int intermediateValueCount = 0;

    for (int c = 0; c < kNumChunks; ++c)
    {
        const int written =
            PracticeToolServiceTestAccess::ReadSourceWindow(*svc, buffer, outL.data(), outR.data(), cursor, kChunk);
        if (written != kChunk)
        {
            everyCallFullyFilled = false;
        }
        if (cursor >= kTotalFrames)
        {
            cursorAlwaysInBounds = false;
        }

        for (int i = 0; i < written; ++i)
        {
            const float sample = outL[static_cast<std::size_t>(i)];
            if (!std::isfinite(sample))
            {
                noNonFiniteSamples = false;
            }
            const float delta = std::fabs(sample - prevSample);
            maxAbsDelta = std::max(maxAbsDelta, delta);
            prevSample = sample;
            // "Intermediate" = clearly between the ramp's flat extremes, i.e.
            // actually mid-blend rather than sitting at -1 or +1.
            if (sample > -0.8f && sample < 0.8f)
            {
                ++intermediateValueCount;
            }
        }
    }

    // With an ~8ms crossfade at 48kHz (~384 frames) and equal-power blending,
    // the largest sample-to-sample step should be a small fraction of the
    // full ~2.0 range a hard cut would produce at the wrap.
    const bool noHardCutJump = maxAbsDelta < 0.25f;
    // A genuine blend spends many samples transitioning, not just one.
    const bool sawSustainedBlend = intermediateValueCount > 50;

    std::cout << "  " << std::left << std::setw(48)
              << "Every call fully filled while looping:" << (everyCallFullyFilled ? "PASS" : "FAIL") << "\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Cursor always stays in [0, total):" << (cursorAlwaysInBounds ? "PASS" : "FAIL") << "\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Output stays finite (no NaN/Inf):" << (noNonFiniteSamples ? "PASS" : "FAIL") << "\n";
    std::cout << "  " << std::left << std::setw(48)
              << "No hard-cut jump at wrap (blends):" << (noHardCutJump ? "PASS" : "FAIL")
              << " (maxAbsDelta=" << maxAbsDelta << ")\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Sustained blend across the wrap:" << (sawSustainedBlend ? "PASS" : "FAIL")
              << " (count=" << intermediateValueCount << ")\n";

    return everyCallFullyFilled && cursorAlwaysInBounds && noNonFiniteSamples && noHardCutJump && sawSustainedBlend;
}

bool TestVeryShortLoopRegionStaysInBounds()
{
    std::cout << "\n--- PracticeToolService Short-Loop Bounds Test ---\n";

    NullPluginHost host;
    std::mutex dspMutex;
    auto svc = MakeService(host, dspMutex);

    // A loop region much shorter than the nominal ~384-frame crossfade
    // window, forcing the fade length to clamp down to a fraction of the
    // (tiny) region instead of over-reading past either boundary.
    constexpr std::size_t kTotalFrames = 2000;
    constexpr std::size_t kLoopStart = 900;
    constexpr std::size_t kLoopEnd = 950; // 50-frame loop region
    auto buffer = MakeRampLoopBuffer(kTotalFrames, kLoopStart, kLoopEnd);

    svc->SetLoopRegion(static_cast<double>(kLoopStart) / kSampleRate, static_cast<double>(kLoopEnd) / kSampleRate);
    svc->SetLoopingEnabled(true);

    constexpr int kChunk = 137;
    constexpr int kNumChunks = 300; // many wraps of this very short loop
    std::vector<float> outL(static_cast<std::size_t>(kChunk));
    std::vector<float> outR(static_cast<std::size_t>(kChunk));
    std::size_t cursor = kLoopStart;

    bool everyCallFullyFilled = true;
    bool cursorAlwaysInBounds = true;
    bool noNonFiniteSamples = true;

    for (int c = 0; c < kNumChunks; ++c)
    {
        const int written =
            PracticeToolServiceTestAccess::ReadSourceWindow(*svc, buffer, outL.data(), outR.data(), cursor, kChunk);
        if (written != kChunk)
        {
            everyCallFullyFilled = false;
        }
        if (cursor >= kTotalFrames)
        {
            cursorAlwaysInBounds = false;
        }
        for (int i = 0; i < written; ++i)
        {
            if (!std::isfinite(outL[static_cast<std::size_t>(i)]))
            {
                noNonFiniteSamples = false;
            }
        }
    }

    std::cout << "  " << std::left << std::setw(48)
              << "Every call fully filled (tiny loop):" << (everyCallFullyFilled ? "PASS" : "FAIL") << "\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Cursor always stays in [0, total):" << (cursorAlwaysInBounds ? "PASS" : "FAIL") << "\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Output stays finite (no NaN/Inf):" << (noNonFiniteSamples ? "PASS" : "FAIL") << "\n";

    return everyCallFullyFilled && cursorAlwaysInBounds && noNonFiniteSamples;
}

bool TestNonLoopingExhaustionReturnsShortAtEnd()
{
    std::cout << "\n--- PracticeToolService Non-Looping Exhaustion Test ---\n";

    NullPluginHost host;
    std::mutex dspMutex;
    auto svc = MakeService(host, dspMutex);

    constexpr std::size_t kTotalFrames = 1000;
    std::vector<float> left(kTotalFrames, 0.25f);
    auto buffer = PracticeToolServiceTestAccess::MakeBuffer(left, left, kSampleRate);

    // Looping left disabled (the service's default) — no active loop region.
    constexpr int kChunk = 300;
    std::vector<float> outL(static_cast<std::size_t>(kChunk));
    std::vector<float> outR(static_cast<std::size_t>(kChunk));
    std::size_t cursor = 800; // 200 frames of real audio remain

    const int firstWritten =
        PracticeToolServiceTestAccess::ReadSourceWindow(*svc, buffer, outL.data(), outR.data(), cursor, kChunk);
    const bool firstCallShortAtEnd = firstWritten == 200 && cursor == kTotalFrames;

    const int secondWritten =
        PracticeToolServiceTestAccess::ReadSourceWindow(*svc, buffer, outL.data(), outR.data(), cursor, kChunk);
    const bool secondCallReturnsZero = secondWritten == 0;

    std::cout << "  " << std::left << std::setw(48)
              << "Non-looping read stops exactly at end:" << (firstCallShortAtEnd ? "PASS" : "FAIL")
              << " (written=" << firstWritten << ", cursor=" << cursor << ")\n";
    std::cout << "  " << std::left << std::setw(48)
              << "Further reads past end return 0:" << (secondCallReturnsZero ? "PASS" : "FAIL") << "\n";

    return firstCallShortAtEnd && secondCallReturnsZero;
}

} // namespace

int main()
{
    bool allPassed = true;

    if (!TestLoopWrapStaysInBoundsAndBlends())
    {
        allPassed = false;
    }
    if (!TestVeryShortLoopRegionStaysInBounds())
    {
        allPassed = false;
    }
    if (!TestNonLoopingExhaustionReturnsShortAtEnd())
    {
        allPassed = false;
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
