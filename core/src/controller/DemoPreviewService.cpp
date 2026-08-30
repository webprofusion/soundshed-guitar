#include "controller/DemoPreviewService.h"

#include "util/AudioDecoder.h"
#include "util/Base64.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

namespace guitarfx
{
namespace
{
// Matches PracticeToolService's loop wrap (kCrossfadeSeconds there) — the two
// features loop the same way and should sound the same doing it.
constexpr double kPreviewCrossfadeSeconds = 0.008;
constexpr std::size_t kMaxPreviewCrossfadeFrames = 16384; // ~340ms @ 48kHz; safety ceiling
constexpr double kHalfPi = 1.5707963267948966;
} // namespace

std::shared_ptr<const DemoPreviewService::PreviewRegion>
DemoPreviewService::BuildRegion(const std::shared_ptr<DemoAudioBuffer>& buffer, double startSec, double endSec,
                               bool looping)
{
    if (!buffer || buffer->channelSamples.empty() || buffer->sampleRate <= 0.0)
    {
        return nullptr;
    }

    const std::size_t total = buffer->channelSamples[0].size();

    if (total == 0)
    {
        return nullptr;
    }

    // endSec <= 0 means "to the end of the clip", which is how a caller asks to
    // loop the whole thing without having to know its length.
    const double clipSec = static_cast<double>(total) / buffer->sampleRate;
    const double resolvedEndSec = endSec > 0.0 ? endSec : clipSec;

    if (!(resolvedEndSec > startSec))
    {
        return nullptr;
    }

    const auto toFrame = [&](double seconds) -> std::size_t {
        const double frame = std::max(0.0, seconds) * buffer->sampleRate;
        return static_cast<std::size_t>(std::min(frame, static_cast<double>(total)));
    };

    auto region = std::make_shared<PreviewRegion>();
    region->startFrame = std::min(toFrame(startSec), total - 1);
    region->endFrame = std::min(toFrame(resolvedEndSec), total);
    region->looping = looping;

    if (region->endFrame <= region->startFrame)
    {
        return nullptr;
    }

    if (!looping)
    {
        return region; // played once — no seam, so no crossfade to build
    }

    // The blend reads forward from both the seam and the loop start, so it can
    // only be as long as the shortest of: the nominal fade, what is left after
    // each of those two points, and the loop itself (a very short loop must not
    // crossfade over more than it plays).
    const std::size_t regionLength = region->endFrame - region->startFrame;
    const std::size_t nominal = static_cast<std::size_t>(std::max(1.0, buffer->sampleRate * kPreviewCrossfadeSeconds));
    const std::size_t fadeLength = std::min({nominal, total - region->endFrame, total - region->startFrame,
                                             regionLength, kMaxPreviewCrossfadeFrames});

    if (fadeLength > 0)
    {
        const auto& ch0 = buffer->channelSamples[0];
        const auto& ch1 = buffer->channelSamples.size() > 1 ? buffer->channelSamples[1] : buffer->channelSamples[0];

        region->carryL.resize(fadeLength);
        region->carryR.resize(fadeLength);

        for (std::size_t i = 0; i < fadeLength; ++i)
        {
            const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(fadeLength);
            const float fadeOut = static_cast<float>(std::cos(t * kHalfPi));
            const float fadeIn = static_cast<float>(std::sin(t * kHalfPi));
            region->carryL[i] = ch0[region->endFrame + i] * fadeOut + ch0[region->startFrame + i] * fadeIn;
            region->carryR[i] = ch1[region->endFrame + i] * fadeOut + ch1[region->startFrame + i] * fadeIn;
        }
    }

    return region;
}

DemoPreviewService::DemoPreviewService(IPluginHost& host, MultiPresetMixer& mixer, std::mutex& dspMutex,
                                       std::atomic<bool>& signalTestActive,
                                       std::function<void(const std::string&, const std::string&)> reportError,
                                       std::function<void(const std::string&)> sendMessage)
    : mHost(host), mPresetMixer(mixer), mDSPMutex(dspMutex), mSignalTestActive(signalTestActive),
      mReportError(std::move(reportError)), mSendMessage(std::move(sendMessage))
{
}

void DemoPreviewService::MixIntoInput(float** inputs, int numSamples)
{
    if (!mDemoAudioActive.load(std::memory_order_acquire))
    {
        return;
    }

    auto buf = std::atomic_load_explicit(&mDemoAudioBuffer, std::memory_order_acquire);

    if (!buf || buf->channels < 1)
    {
        return;
    }

    size_t cursor = mDemoAudioCursor.load(std::memory_order_relaxed);
    const size_t totalSamples = buf->channelSamples[0].size();
    const auto& ch0 = buf->channelSamples[0];
    const auto& ch1 = (buf->channels > 1) ? buf->channelSamples[1] : buf->channelSamples[0];

    auto region = std::atomic_load_explicit(&mPreviewRegion, std::memory_order_acquire);
    const bool looping = region && region->looping;
    const size_t regionEnd = region ? std::min(region->endFrame, totalSamples) : totalSamples;

    int i = 0;

    while (i < numSamples)
    {
        // Play out the wrap crossfade first when one is in flight. It was
        // computed on the message thread; here it is only a copy.
        if (looping && mCarryPos < region->carryL.size())
        {
            const size_t take = std::min(region->carryL.size() - mCarryPos, static_cast<size_t>(numSamples - i));

            for (size_t n = 0; n < take; ++n)
            {
                inputs[0][i + static_cast<int>(n)] += region->carryL[mCarryPos + n];
                inputs[1][i + static_cast<int>(n)] += region->carryR[mCarryPos + n];
            }

            mCarryPos += take;
            i += static_cast<int>(take);
            continue;
        }

        if (cursor >= regionEnd || cursor >= totalSamples)
        {
            if (!looping)
            {
                break;
            }

            // Wrap: emit the crossfade, then resume just past the frames it
            // already covered so the seam is not played twice.
            mCarryPos = 0;
            cursor = region->carryL.empty()
                         ? region->startFrame // no room for a fade — hard cut
                         : region->startFrame + region->carryL.size();

            continue;
        }

        const size_t run = std::min(regionEnd - cursor, static_cast<size_t>(numSamples - i));

        for (size_t n = 0; n < run; ++n)
        {
            inputs[0][i + static_cast<int>(n)] += ch0[cursor + n];
            inputs[1][i + static_cast<int>(n)] += ch1[cursor + n];
        }

        cursor += run;
        i += static_cast<int>(run);
    }

    mDemoAudioCursor.store(cursor, std::memory_order_relaxed);

    // A looping preview never completes on its own — it runs until the UI stops
    // it, which is the whole point of looping in the engine. A one-shot finishes
    // at the end of its *region*, which is the end of the clip only when no
    // region is set: a trimmed preview that waited for the clip end would go
    // silent and then never report completion.
    if (!looping && cursor >= regionEnd)
    {
        mDemoAudioActive.store(false, std::memory_order_release);
    }
}

void DemoPreviewService::SetPreviewRegion(double startSec, double endSec, bool looping)
{
    auto buffer = std::atomic_load_explicit(&mDemoAudioBuffer, std::memory_order_acquire);

    if (!buffer)
    {
        return;
    }

    auto region = BuildRegion(buffer, startSec, endSec, looping);

    // Taking the DSP lock keeps the swap off the audio thread's toes for the
    // same reason StartPreview does; the audio thread itself still only does an
    // atomic load and never blocks.
    std::lock_guard<std::mutex> lock(mDSPMutex);
    std::atomic_store_explicit(&mPreviewRegion, region, std::memory_order_release);
}

void DemoPreviewService::StartPreview(const nlohmann::json& payload)
{
    if (mSignalTestActive.load(std::memory_order_acquire))
    {
        mReportError("Demo preview unavailable", "Signal path test is currently running");
        return;
    }

    const auto audioIter = payload.find("audio");

    if (audioIter == payload.end() || !audioIter->is_object())
    {
        mReportError("Demo preview unavailable", "Audio payload is missing");
        return;
    }

    const std::string dataEncoded = audioIter->value("data", "");

    if (dataEncoded.empty())
    {
        mReportError("Demo preview unavailable", "Audio payload did not include data");
        return;
    }

    const auto decodedBytes = util::DecodeBase64(dataEncoded);

    if (decodedBytes.empty())
    {
        mReportError("Demo preview unavailable", "Unable to decode audio data");
        return;
    }

    const auto wavData = util::DecodeAudioBytes(decodedBytes);

    if (!wavData)
    {
        mReportError("Demo preview unavailable", "Unsupported audio format (expected WAV, AIFF, or MP3)");
        return;
    }

    const double hostSampleRate = mHost.GetSampleRate();
    const double targetSampleRate = hostSampleRate > 0.0 ? hostSampleRate : wavData->sampleRate;

    if (targetSampleRate <= 0.0)
    {
        mReportError("Demo preview unavailable", "Target sample rate is invalid");
        return;
    }

    auto resampled = util::ConvertToSampleRate(*wavData, targetSampleRate);

    if (resampled.empty() || resampled.front().empty())
    {
        mReportError("Demo preview unavailable", "Audio buffer is empty");
        return;
    }

    std::size_t minFrames = resampled.front().size();

    for (const auto& channel : resampled)
    {
        if (channel.empty())
        {
            mReportError("Demo preview unavailable", "Audio buffer is empty");
            return;
        }

        minFrames = std::min(minFrames, channel.size());
    }

    if (minFrames == 0)
    {
        mReportError("Demo preview unavailable", "Audio buffer is empty");
        return;
    }

    for (auto& channel : resampled)
    {
        if (channel.size() > minFrames)
        {
            channel.resize(minFrames);
        }
    }

    auto buffer = std::make_shared<DemoAudioBuffer>();
    buffer->id = audioIter->value("id", "");
    buffer->title = audioIter->value("title", buffer->id);
    buffer->sampleRate = targetSampleRate;
    buffer->channels = static_cast<int>(resampled.size());
    buffer->channelSamples = std::move(resampled);

    // An optional sub-range. Absent (the demo/preset previews) means the whole
    // clip plays once and completes, exactly as before.
    std::shared_ptr<const PreviewRegion> region;
    const auto regionIter = payload.find("region");

    if (regionIter != payload.end() && regionIter->is_object())
    {
        region = BuildRegion(buffer, regionIter->value("startSec", 0.0), regionIter->value("endSec", 0.0),
                             regionIter->value("looping", false));
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mDemoAudioCursor.store(region ? region->startFrame : 0, std::memory_order_release);
        mCarryPos = 0;
        std::atomic_store_explicit(&mPreviewRegion, region, std::memory_order_release);
        std::atomic_store_explicit(&mDemoAudioBuffer, buffer, std::memory_order_release);
        mDemoAudioActive.store(true, std::memory_order_release);
    }

    nlohmann::json startMsg;
    startMsg["type"] = "previewStarted";
    startMsg["id"] = buffer->id;
    startMsg["title"] = buffer->title;
    mSendMessage(startMsg.dump());
}

void DemoPreviewService::StopPreview()
{
    mDemoAudioActive.store(false, std::memory_order_release);
    std::atomic_store_explicit(&mPreviewRegion, std::shared_ptr<const PreviewRegion>{}, std::memory_order_release);
    auto stopped =
        std::atomic_exchange_explicit(&mDemoAudioBuffer, std::shared_ptr<DemoAudioBuffer>{}, std::memory_order_acq_rel);
    nlohmann::json msg;
    msg["type"] = "previewStopped";

    if (stopped)
    {
        msg["id"] = stopped->id;
        msg["title"] = stopped->title;
    }

    mSendMessage(msg.dump());
}

void DemoPreviewService::OnIdle()
{
    auto demoBuffer = std::atomic_load_explicit(&mDemoAudioBuffer, std::memory_order_acquire);

    if (!demoBuffer || mDemoAudioActive.load(std::memory_order_acquire))
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "previewComplete";
    msg["id"] = demoBuffer->id;
    msg["title"] = demoBuffer->title;
    mSendMessage(msg.dump());
    std::atomic_store_explicit(&mDemoAudioBuffer, std::shared_ptr<DemoAudioBuffer>{}, std::memory_order_release);
}

bool DemoPreviewService::IsPreviewActive() const
{
    return mDemoAudioActive.load(std::memory_order_acquire);
}
} // namespace guitarfx
