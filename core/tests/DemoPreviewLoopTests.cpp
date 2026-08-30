/**
 * @file DemoPreviewLoopTests.cpp
 * @brief Tests for DemoPreviewService's in-engine preview loop.
 *
 * Riff previews used to "repeat" by having the UI re-request the whole clip
 * every time it ended — an engine->UI->engine round trip plus a fresh WAV
 * encode per cycle, with an audible gap at the seam. The engine now wraps the
 * clip in place with an equal-power crossfade, the same way PracticeToolService
 * wraps a loop region.
 *
 * These drive MixIntoInput() directly against a synthetic buffer (via a friend
 * test-access struct), so the wrap is exercised synchronously with no decode,
 * no audio device and no timing involved.
 */

#include "controller/DemoPreviewService.h"
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
// Friend accessor granted by DemoPreviewService — exposes just enough of its
// private surface to seat a synthetic clip and inspect the resulting playback.
struct DemoPreviewServiceTestAccess
{
    static void Seat(DemoPreviewService& svc, std::vector<float> left, std::vector<float> right, double sampleRate,
                     double startSec, double endSec, bool looping)
    {
        auto buffer = std::make_shared<DemoPreviewService::DemoAudioBuffer>();
        buffer->id = "test-clip";
        buffer->title = "Test Clip";
        buffer->sampleRate = sampleRate;
        buffer->channels = 2;
        buffer->channelSamples = {std::move(left), std::move(right)};

        auto region = DemoPreviewService::BuildRegion(buffer, startSec, endSec, looping);

        svc.mDemoAudioBuffer = buffer;
        svc.mPreviewRegion = region;
        svc.mDemoAudioCursor.store(region ? region->startFrame : 0);
        svc.mCarryPos = 0;
        svc.mDemoAudioActive.store(true);
    }

    static bool HasRegion(const DemoPreviewService& svc)
    {
        return static_cast<bool>(svc.mPreviewRegion);
    }

    static std::size_t CarryLength(const DemoPreviewService& svc)
    {
        return svc.mPreviewRegion ? svc.mPreviewRegion->carryL.size() : 0;
    }

    static bool IsActive(const DemoPreviewService& svc)
    {
        return svc.mDemoAudioActive.load();
    }
};
} // namespace guitarfx

namespace
{
using guitarfx::DemoPreviewService;
using guitarfx::DemoPreviewServiceTestAccess;

constexpr double kSampleRate = 48000.0;

/// Minimal IPluginHost stub — the mix path under test never calls back into it.
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

struct Fixture
{
    NullPluginHost host;
    guitarfx::MultiPresetMixer mixer;
    std::mutex dspMutex;
    std::atomic<bool> signalTestActive{false};
    std::unique_ptr<DemoPreviewService> svc;

    Fixture()
    {
        svc = std::make_unique<DemoPreviewService>(
            host, mixer, dspMutex, signalTestActive, [](const std::string&, const std::string&) {},
            [](const std::string&) {});
    }
};

/// A clip whose loop endpoints hold clearly different values: a ramp from -1 at
/// regionStart to +1 just before regionEnd. A hard cut at the wrap would jump
/// ~2.0 in one sample; a crossfade should traverse it gradually.
std::vector<float> MakeRamp(std::size_t total, std::size_t regionStart, std::size_t regionEnd)
{
    std::vector<float> samples(total, 1.0f);
    const auto span = static_cast<double>(regionEnd - regionStart);

    for (std::size_t i = 0; i < total; ++i)
    {
        if (i < regionStart)
        {
            samples[i] = -1.0f;
        }
        else if (i < regionEnd)
        {
            samples[i] = static_cast<float>(-1.0 + 2.0 * (static_cast<double>(i - regionStart) / span));
        }
    }

    return samples;
}

/// Runs `blocks` blocks of `blockSize` frames through the mix and returns the
/// left channel, concatenated. MixIntoInput is additive, so the scratch starts
/// zeroed each block.
std::vector<float> Render(DemoPreviewService& svc, int blockSize, int blocks)
{
    std::vector<float> out;
    std::vector<float> l(static_cast<std::size_t>(blockSize));
    std::vector<float> r(static_cast<std::size_t>(blockSize));

    for (int b = 0; b < blocks; ++b)
    {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        float* channels[2] = {l.data(), r.data()};
        svc.MixIntoInput(channels, blockSize);
        out.insert(out.end(), l.begin(), l.end());
    }

    return out;
}

double LargestJump(const std::vector<float>& samples)
{
    double worst = 0.0;

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        worst = std::max(worst, std::abs(static_cast<double>(samples[i]) - static_cast<double>(samples[i - 1])));
    }

    return worst;
}

void Report(const char* label, bool passed)
{
    std::cout << "  " << std::left << std::setw(52) << label << (passed ? "PASS" : "FAIL") << "\n";
}

bool TestLoopWrapIsCrossfadedNotCut()
{
    std::cout << "\nLoop wrap blends the seam instead of cutting it\n";
    Fixture f;

    const std::size_t total = 24000; // 0.5s
    const std::size_t start = 4800;  // 0.1s
    const std::size_t end = 19200;   // 0.4s
    DemoPreviewServiceTestAccess::Seat(*f.svc, MakeRamp(total, start, end), MakeRamp(total, start, end), kSampleRate,
                                       static_cast<double>(start) / kSampleRate,
                                       static_cast<double>(end) / kSampleRate, true);

    const bool built = DemoPreviewServiceTestAccess::HasRegion(*f.svc);
    const bool hasFade = DemoPreviewServiceTestAccess::CarryLength(*f.svc) > 0;

    // Long enough to cross the seam several times.
    const auto rendered = Render(*f.svc, 512, 100);
    const double jump = LargestJump(rendered);

    // A hard cut at these endpoints steps ~2.0 in one sample. The ramp itself
    // climbs 2.0 over 14400 frames, so anything smooth stays far below this.
    const bool blended = jump < 0.5;
    const bool stillPlaying = DemoPreviewServiceTestAccess::IsActive(*f.svc);

    Report("Region built:", built);
    Report("Wrap crossfade present:", hasFade);
    Report("No hard discontinuity at the seam:", blended);
    Report("Looping preview never self-completes:", stillPlaying);
    std::cout << "    largest sample-to-sample jump: " << jump << "\n";

    return built && hasFade && blended && stillPlaying;
}

bool TestNonLoopingRegionPlaysOnceAndStops()
{
    std::cout << "\nA region without repeat plays once and completes\n";
    Fixture f;

    const std::size_t total = 24000;
    const std::size_t start = 4800;
    const std::size_t end = 9600;
    DemoPreviewServiceTestAccess::Seat(*f.svc, MakeRamp(total, start, end), MakeRamp(total, start, end), kSampleRate,
                                       static_cast<double>(start) / kSampleRate,
                                       static_cast<double>(end) / kSampleRate, false);

    const bool noFade = DemoPreviewServiceTestAccess::CarryLength(*f.svc) == 0;
    const auto rendered = Render(*f.svc, 512, 40); // 20480 frames, well past the region
    const bool stopped = !DemoPreviewServiceTestAccess::IsActive(*f.svc);

    // Everything past the region's length must be silence, not the rest of the
    // clip — trimming has to actually trim even when not repeating.
    bool silentAfterRegion = true;

    for (std::size_t i = end - start + 8; i < rendered.size(); ++i)
    {
        if (std::abs(rendered[i]) > 1e-6f)
        {
            silentAfterRegion = false;
            break;
        }
    }

    Report("No crossfade built for a one-shot:", noFade);
    Report("Playback completes:", stopped);
    Report("Silent past the region end:", silentAfterRegion);

    return noFade && stopped && silentAfterRegion;
}

bool TestDegenerateRegionIsIgnored()
{
    std::cout << "\nAn inverted or empty region falls back to the whole clip\n";
    Fixture f;

    const std::size_t total = 12000;
    // endSec before startSec — reachable by dragging a marker past its partner.
    DemoPreviewServiceTestAccess::Seat(*f.svc, std::vector<float>(total, 0.5f), std::vector<float>(total, 0.5f),
                                       kSampleRate, 0.2, 0.1, true);

    const bool noRegion = !DemoPreviewServiceTestAccess::HasRegion(*f.svc);
    const auto rendered = Render(*f.svc, 512, 40);
    const bool stopped = !DemoPreviewServiceTestAccess::IsActive(*f.svc);
    const bool playedSomething = std::abs(rendered.front() - 0.5f) < 1e-6f;

    Report("Degenerate range builds no region:", noRegion);
    Report("Falls back to playing the clip:", playedSomething);
    Report("And still completes:", stopped);

    return noRegion && stopped && playedSomething;
}

bool TestVeryShortLoopStaysInBounds()
{
    std::cout << "\nA loop shorter than the crossfade stays in bounds\n";
    Fixture f;

    const std::size_t total = 24000;
    const std::size_t start = 1000;
    const std::size_t end = 1050; // 50 frames, ~1ms — far shorter than the 8ms fade
    DemoPreviewServiceTestAccess::Seat(*f.svc, MakeRamp(total, start, end), MakeRamp(total, start, end), kSampleRate,
                                       static_cast<double>(start) / kSampleRate,
                                       static_cast<double>(end) / kSampleRate, true);

    const std::size_t fade = DemoPreviewServiceTestAccess::CarryLength(*f.svc);
    const bool fadeClamped = fade <= (end - start);

    // The real assertion is that this returns at all: a wrap that failed to
    // make progress would spin forever inside MixIntoInput.
    const auto rendered = Render(*f.svc, 512, 20);
    const bool produced = rendered.size() == 512 * 20;

    Report("Crossfade clamped to the loop length:", fadeClamped);
    Report("Mix makes progress (no wrap spin):", produced);
    std::cout << "    fade frames: " << fade << " (loop is " << (end - start) << ")\n";

    return fadeClamped && produced;
}
} // namespace

int main()
{
    bool allPassed = true;

    if (!TestLoopWrapIsCrossfadedNotCut())
    {
        allPassed = false;
    }

    if (!TestNonLoopingRegionPlaysOnceAndStops())
    {
        allPassed = false;
    }

    if (!TestDegenerateRegionIsIgnored())
    {
        allPassed = false;
    }

    if (!TestVeryShortLoopStaysInBounds())
    {
        allPassed = false;
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
