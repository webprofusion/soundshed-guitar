/**
 * @file PracticeToolCrossfadeTests.cpp
 * @brief Focused tests for PracticeToolService's loop-wrap crossfade logic
 * (ReadSourceWindow / BeginCrossfade), per the design plan's "Loop-boundary
 * handling" requirement: stretch.reset() must never be called on a loop wrap,
 * so wraps are handled entirely in the source domain with a short
 * equal-power crossfade. A jump (seek, loop selected) is the opposite case:
 * JumpTo() restarts the stretcher so none of the old position plays, and the
 * last test holds it to that. These tests exercise that source-domain
 * logic directly and synchronously (via a friend test-access struct), with a
 * synthetic in-memory buffer — no file I/O, no background render thread
 * timing involved.
 */

#include "controller/PracticeToolService.h"
#include "IPluginHost.h"

#include <algorithm>
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

    // The render thread configures the stretcher once it has a buffer; these
    // tests never hand it one, so it stays idle and the test thread owns it.
    static void ConfigureStretch(PracticeToolService& svc)
    {
        svc.mStretch.presetDefault(2, static_cast<float>(kTestSampleRate), false);
        svc.mStretch.reset();
        svc.mStretchConfigured = true;
    }

    static void JumpTo(PracticeToolService& svc, const std::shared_ptr<PracticeToolService::TrackBuffer>& buffer,
                       std::size_t& cursor, std::size_t target)
    {
        svc.JumpTo(buffer, cursor, target);
    }

    /// Runs the render path (source read, stretch, ring) for `frames` output
    /// frames and returns the left channel as the audio thread would pop it.
    static std::vector<float> Render(PracticeToolService& svc,
                                     const std::shared_ptr<PracticeToolService::TrackBuffer>& buffer,
                                     std::size_t& cursor, std::size_t frames)
    {
        std::vector<float> left;
        std::vector<PracticeToolService::StereoFrame> popped(1024);

        while (left.size() < frames)
        {
            svc.RenderChunk(buffer, cursor);
            const std::size_t n = svc.mOutputRing->Pop(popped.data(), popped.size());

            if (n == 0)
            {
                break;
            }

            for (std::size_t i = 0; i < n; ++i)
            {
                left.push_back(popped[i].l);
            }
        }

        left.resize(std::min(left.size(), frames));
        return left;
    }

    static constexpr double kTestSampleRate = 48000.0;
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

// 2 s of a loud 220 Hz tone, then 2 s of silence: where the audio came from is
// then plain from its level alone.
auto MakeToneThenSilenceBuffer()
{
    const auto total = static_cast<std::size_t>(kSampleRate * 4.0);
    const auto toneEnd = static_cast<std::size_t>(kSampleRate * 2.0);
    std::vector<float> left(total, 0.0f);

    for (std::size_t i = 0; i < toneEnd; ++i)
    {
        left[i] =
            0.5f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * static_cast<double>(i) / kSampleRate));
    }

    return PracticeToolServiceTestAccess::MakeBuffer(left, left, kSampleRate);
}

float PeakOf(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    float peak = 0.0f;

    for (std::size_t i = from; i < std::min(to, samples.size()); ++i)
    {
        peak = std::max(peak, std::abs(samples[i]));
    }

    return peak;
}

// Selecting a loop jumps playback. The stretcher holds ~120 ms of whatever it
// was last fed, so a jump streamed through it played that history first — the
// glitch heard at the top of a loop. JumpTo() must leave none of it, and must
// start the target immediately rather than after the stretcher's latency.
bool TestJumpLeavesNothingOfTheOldPositionBehind()
{
    std::cout << "\n--- PracticeToolService Jump Alignment Tests ---\n";

    NullPluginHost host;
    std::mutex dspMutex;
    auto svc = MakeService(host, dspMutex);
    auto buffer = MakeToneThenSilenceBuffer();
    PracticeToolServiceTestAccess::ConfigureStretch(*svc);

    const auto ms = [](double v) { return static_cast<std::size_t>(kSampleRate * v / 1000.0); };
    const auto silenceAt = static_cast<std::size_t>(kSampleRate * 3.0);

    // Control: the old way, moving the cursor and streaming on. The tone the
    // stretcher was full of must still come out, or this test proves nothing.
    std::size_t cursor = 0;
    PracticeToolServiceTestAccess::Render(*svc, buffer, cursor, ms(500));
    cursor = silenceAt;
    const auto streamed = PracticeToolServiceTestAccess::Render(*svc, buffer, cursor, ms(200));
    const float streamedLeak = PeakOf(streamed, 0, ms(200));
    const bool controlLeaks = streamedLeak > 0.1f;

    // Tone → silence: nothing of the tone after the jump.
    PracticeToolServiceTestAccess::ConfigureStretch(*svc);
    cursor = 0;
    PracticeToolServiceTestAccess::Render(*svc, buffer, cursor, ms(500));
    PracticeToolServiceTestAccess::JumpTo(*svc, buffer, cursor, silenceAt);
    const auto jumped = PracticeToolServiceTestAccess::Render(*svc, buffer, cursor, ms(200));
    const float jumpLeak = PeakOf(jumped, 0, ms(200));
    const bool noLeak = jumpLeak < 0.01f;

    // Silence → tone: the tone is there from the first frames (past the 5 ms
    // fade-in), not after the stretcher's latency.
    PracticeToolServiceTestAccess::JumpTo(*svc, buffer, cursor, static_cast<std::size_t>(kSampleRate * 1.0));
    const auto started = PracticeToolServiceTestAccess::Render(*svc, buffer, cursor, ms(50));
    const float earlyPeak = PeakOf(started, ms(6), ms(20));
    const bool startsAtOnce = earlyPeak > 0.3f;

    std::cout << "  " << std::left << std::setw(48)
              << "Control: a streamed jump plays old audio:" << (controlLeaks ? "PASS" : "FAIL")
              << " (peak=" << streamedLeak << ")\n";
    std::cout << "  " << std::left << std::setw(48)
              << "JumpTo leaves none of the old position:" << (noLeak ? "PASS" : "FAIL") << " (peak=" << jumpLeak
              << ")\n";
    std::cout << "  " << std::left << std::setw(48)
              << "JumpTo starts the target at once:" << (startsAtOnce ? "PASS" : "FAIL")
              << " (peak 6-20 ms=" << earlyPeak << ")\n";

    return controlLeaks && noLeak && startsAtOnce;
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

    if (!TestJumpLeavesNothingOfTheOldPositionBehind())
    {
        allPassed = false;
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
