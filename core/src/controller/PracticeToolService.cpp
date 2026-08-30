// PracticeToolService.cpp — lifecycle, file loading, transport, and the
// audio-thread mix. The background render thread's half of the class lives in
// PracticeToolServiceRender.cpp; the two share only what
// internal/PracticeToolSupport.h holds.

#include "controller/PracticeToolService.h"

#include "controller/internal/PracticeToolSupport.h"
#include "util/AudioDecoder.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

namespace guitarfx
{
using namespace controller_detail;

namespace
{
constexpr double kMinPitchSemitones = -12.0;
constexpr double kMaxPitchSemitones = 12.0;

// Duplicated from PluginController.cpp's anonymous-namespace helper of the
// same name (used there for riff-capture/demo-preview waveform previews).
// Kept as a small local copy rather than a shared header so this file has
// no dependency on PluginController's internals — the two are trivial pure
// functions that are easy to keep in sync if the peak algorithm ever needs
// to change.
// Per-channel peaks (not combined) so the UI can draw a real stereo (two-lane
// L/R) waveform instead of a single collapsed trace.
[[nodiscard]] nlohmann::json BuildChannelPeaks(const std::vector<float>& channel, std::size_t bins)
{
    nlohmann::json peaks = nlohmann::json::array();

    if (channel.empty() || bins == 0)
    {
        return peaks;
    }

    const std::size_t totalSamples = channel.size();
    const std::size_t binCount = std::min<std::size_t>(bins, totalSamples);

    for (std::size_t b = 0; b < binCount; ++b)
    {
        const std::size_t start = (b * totalSamples) / binCount;
        const std::size_t end = std::max(start + 1, ((b + 1) * totalSamples) / binCount);
        float peak = 0.0f;

        for (std::size_t i = start; i < end && i < totalSamples; ++i)
        {
            const float p = std::fabs(channel[i]);

            if (p > peak)
            {
                peak = p;
            }
        }

        peaks.push_back(static_cast<double>(std::clamp(peak, 0.0f, 1.0f)));
    }

    return peaks;
}
} // namespace

PracticeToolService::PracticeToolService(IPluginHost& host, std::mutex& dspMutex,
                                         std::function<void(const std::string&, const std::string&)> reportError,
                                         std::function<void(const std::string&)> sendMessage)
    : mHost(host), mDSPMutex(dspMutex), mReportError(std::move(reportError)), mSendMessage(std::move(sendMessage))
{
}

PracticeToolService::~PracticeToolService()
{
    Shutdown();
}

void PracticeToolService::Prepare(double sampleRate, int maxBlockSize)
{
    mSampleRate.store(sampleRate, std::memory_order_release);
    mMaxBlockSize = std::max(maxBlockSize, 0);
    mPopScratch.assign(static_cast<std::size_t>(std::max(mMaxBlockSize, 1)), StereoFrame{});
    mEqScratchL.assign(mPopScratch.size(), 0.0f);
    mEqScratchR.assign(mPopScratch.size(), 0.0f);
    mEq.Prepare(sampleRate, std::max(mMaxBlockSize, 1));

    if (!mOutputRing)
    {
        // Sized once, generously, and never resized afterward: the render
        // thread (producer) and audio thread (consumer) both hold a raw
        // pointer/reference into this ring for the service's whole lifetime,
        // so recreating it while either thread might be mid-access would be
        // unsafe. ~1.5s at the prepared rate comfortably covers the plan's
        // 500ms-1s lookahead target even after rounding up to a power of two;
        // a later sample-rate change is absorbed by the render thread
        // reconfiguring signalsmith-stretch in place, not by resizing this.
        const double sizingRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const std::size_t framesForLookahead = static_cast<std::size_t>(sizingRate * 1.5);
        mOutputRing =
            std::make_unique<util::SpscRingBuffer<StereoFrame>>(std::max<std::size_t>(framesForLookahead, 4096));
    }

    if (!mRenderThreadRunning.exchange(true, std::memory_order_acq_rel))
    {
        mRenderThreadQuit.store(false, std::memory_order_release);
        mRenderThread = std::thread([this]() { RenderThreadLoop(); });
    }

    // Force the render thread to re-check the sample rate / reconfigure
    // signalsmith-stretch even if nothing else changed.
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::Shutdown()
{
    mRenderThreadQuit.store(true, std::memory_order_release);
    mRenderWake.notify_all();

    if (mRenderThread.joinable())
    {
        mRenderThread.join();
    }

    mRenderThreadRunning.store(false, std::memory_order_release);
}

void PracticeToolService::LoadFile(const std::string& path)
{
    const auto bytes = util::ReadFileBytes(util::PathFromUtf8(path));

    if (bytes.empty())
    {
        mReportError("Unable to load audio file", "File is empty or could not be opened: " + path);
        return;
    }

    LoadDecodedBytes(bytes, path);
}

void PracticeToolService::LoadFileFromBytes(const std::vector<std::uint8_t>& bytes, const std::string& displayName)
{
    if (bytes.empty())
    {
        mReportError("Unable to load audio file", "Dropped file was empty: " + displayName);
        return;
    }

    LoadDecodedBytes(bytes, displayName);
}

void PracticeToolService::LoadDecodedBytes(const std::vector<std::uint8_t>& bytes, const std::string& displayPath)
{
    const auto decoded = util::DecodeAudioBytes(bytes);

    if (!decoded)
    {
        mReportError("Unable to load audio file",
                     "Unsupported audio format (expected WAV, AIFF, or MP3): " + displayPath);
        return;
    }

    const double hostSampleRate = mSampleRate.load(std::memory_order_acquire);
    const double targetSampleRate = hostSampleRate > 0.0 ? hostSampleRate : decoded->sampleRate;

    if (targetSampleRate <= 0.0)
    {
        mReportError("Unable to load audio file", "Target sample rate is invalid");
        return;
    }

    auto resampled = util::ConvertToSampleRate(*decoded, targetSampleRate);

    if (resampled.empty() || resampled.front().empty())
    {
        mReportError("Unable to load audio file", "Decoded audio buffer is empty: " + displayPath);
        return;
    }

    std::size_t minFrames = resampled.front().size();

    for (const auto& channel : resampled)
    {
        if (channel.empty())
        {
            mReportError("Unable to load audio file", "Decoded audio buffer is empty: " + displayPath);
            return;
        }

        minFrames = std::min(minFrames, channel.size());
    }

    if (minFrames == 0)
    {
        mReportError("Unable to load audio file", "Decoded audio buffer is empty: " + displayPath);
        return;
    }

    for (auto& channel : resampled)
    {
        if (channel.size() > minFrames)
        {
            channel.resize(minFrames);
        }
    }

    // Peaks are built from the pre-move samples (BuildChannelPeaks only
    // reads, so this is just a plain pass over the vectors we're about to
    // move into the TrackBuffer). Mono files get identical L/R peaks so the
    // UI's two-lane renderer works unconditionally.
    const auto peaksJsonL = BuildChannelPeaks(resampled[0], 256);
    const auto peaksJsonR = BuildChannelPeaks(resampled.size() > 1 ? resampled[1] : resampled[0], 256);

    auto buffer = std::make_shared<TrackBuffer>();
    buffer->path = displayPath;
    buffer->title = util::PathFromUtf8(displayPath).filename().string();
    buffer->sampleRate = targetSampleRate;
    buffer->channels = static_cast<int>(resampled.size());
    buffer->totalFrames = minFrames;
    buffer->channelSamples = std::move(resampled);

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        std::atomic_store_explicit(&mBuffer, buffer, std::memory_order_release);
        mEq.Reset(); // a whole new track is the discontinuity the delay lines must not carry across
    }

    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>{}, std::memory_order_release);
    mLoopingEnabled.store(false, std::memory_order_relaxed);
    mReadCursorFrames.store(0, std::memory_order_relaxed);
    mPendingSeekSeconds.store(0.0, std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release); // render thread starts this fresh file at 0
    mState.store(static_cast<int>(PlaybackState::Stopped), std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();

    const double durationSec =
        buffer->sampleRate > 0.0 ? static_cast<double>(buffer->totalFrames) / buffer->sampleRate : 0.0;

    nlohmann::json msg;
    msg["type"] = "practiceToolFileLoaded";
    msg["path"] = displayPath;
    msg["title"] = buffer->title;
    msg["durationSec"] = durationSec;
    msg["waveformPeaksL"] = peaksJsonL;
    msg["waveformPeaksR"] = peaksJsonR;
    mSendMessage(msg.dump());

    SendTransportStateToUI();
}

void PracticeToolService::Play()
{
    if (!std::atomic_load_explicit(&mBuffer, std::memory_order_acquire))
    {
        return;
    }

    mState.store(static_cast<int>(PlaybackState::Playing), std::memory_order_release);
    mRenderWake.notify_all(); // wake the render thread immediately rather than waiting out its poll interval
    SendTransportStateToUI();
}

void PracticeToolService::Pause()
{
    mState.store(static_cast<int>(PlaybackState::Paused), std::memory_order_release);
    mRenderWake.notify_all();
    SendTransportStateToUI();
}

void PracticeToolService::Stop()
{
    mState.store(static_cast<int>(PlaybackState::Stopped), std::memory_order_release);
    mPendingSeekSeconds.store(0.0, std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
    SendTransportStateToUI();
}

void PracticeToolService::SeekSeconds(double seconds)
{
    mPendingSeekSeconds.store(std::max(0.0, seconds), std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::SetSpeed(double ratio)
{
    mSpeed.store(std::clamp(ratio, kPracticeToolMinSpeed, kPracticeToolMaxSpeed), std::memory_order_relaxed);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::SetPitchSemitones(double semitones)
{
    mPitchSemitones.store(std::clamp(semitones, kMinPitchSemitones, kMaxPitchSemitones), std::memory_order_relaxed);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::SetGain(double linearGain)
{
    // Deliberately does NOT bump mParamGeneration / flush the ring: gain is
    // applied at mix time on the audio thread (RenderPostChain), so a
    // volume-slider drag is instant rather than lagging by a whole ring
    // buffer's worth of already-rendered lookahead.
    mGain.store(std::max(0.0, linearGain), std::memory_order_relaxed);
}

void PracticeToolService::SetBalance(double balance)
{
    // Same rationale as SetGain: applied directly at mix time, no flush.
    mBalance.store(std::clamp(balance, -1.0, 1.0), std::memory_order_relaxed);
}

void PracticeToolService::SetLoopRegion(double startSec, double endSec)
{
    auto buffer = std::atomic_load_explicit(&mBuffer, std::memory_order_acquire);
    const double sr = mSampleRate.load(std::memory_order_relaxed);

    auto bounds = std::make_shared<ActiveLoopBounds>();
    bounds->startFrame = PracticeToolSecondsToFrames(startSec, sr);
    bounds->endFrame = PracticeToolSecondsToFrames(endSec, sr);

    if (buffer && buffer->totalFrames > 0)
    {
        bounds->startFrame = std::min(bounds->startFrame, buffer->totalFrames - 1);
        bounds->endFrame = std::min(bounds->endFrame, buffer->totalFrames);
    }

    if (bounds->endFrame <= bounds->startFrame)
    {
        return; // ignore degenerate/invalid region rather than looping nothing
    }

    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>(bounds), std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::ClearLoopRegion()
{
    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>{}, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::SetLoopingEnabled(bool enabled)
{
    mLoopingEnabled.store(enabled, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void PracticeToolService::SetEqEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);

    if (mEqEnabled == enabled)
    {
        return;
    }

    mEqEnabled = enabled;

    // Switching back on must not replay the delay lines from whenever it was
    // last switched off — that stale state is a click at best.
    if (enabled)
    {
        mEq.Reset();
    }
}

void PracticeToolService::SetEqParam(const std::string& key, double value)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mEq.SetParam(key, value);
}

bool PracticeToolService::IsLoaded() const
{
    return std::atomic_load_explicit(&mBuffer, std::memory_order_acquire) != nullptr;
}

// ════════════════════════════════════════════════════════════════════
// Audio thread
// ════════════════════════════════════════════════════════════════════

void PracticeToolService::RenderPostChain(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1] || numSamples <= 0)
    {
        return;
    }

    if (mPopScratch.empty()) // Prepare() not called yet
    {
        return;
    }

    if (mState.load(std::memory_order_acquire) != static_cast<int>(PlaybackState::Playing))
    {
        return;
    }

    if (!mOutputRing)
    {
        return;
    }

    numSamples = std::min(numSamples, static_cast<int>(mPopScratch.size()));

    const std::size_t popped = mOutputRing->Pop(mPopScratch.data(), static_cast<std::size_t>(numSamples));
    const float gain = static_cast<float>(mGain.load(std::memory_order_relaxed));
    const double balance = mBalance.load(std::memory_order_relaxed);
    // A "balance" control, not a full pan law: the favored channel always
    // stays at unity gain and only the other channel is attenuated, matching
    // a standard stereo-mixer balance knob rather than mono-source panning.
    const float gainL = gain * static_cast<float>(balance <= 0.0 ? 1.0 : 1.0 - balance);
    const float gainR = gain * static_cast<float>(balance >= 0.0 ? 1.0 : 1.0 + balance);

    // EQ runs on the backing track alone, before gain/balance: outputs[]
    // already holds the guitar signal, so the popped frames are filtered in
    // their own scratch buffers and written back before the one mix loop
    // below. ParametricEQEffect works on planar channels, hence the split.
    // Both the scratch buffers and the filter state are audio-thread-only,
    // and this whole function runs under mDSPMutex (see the header), so the
    // setters can never mutate coefficients mid-block.
    if (mEqEnabled && popped > 0 && mEqScratchL.size() >= popped)
    {
        for (std::size_t i = 0; i < popped; ++i)
        {
            mEqScratchL[i] = mPopScratch[i].l;
            mEqScratchR[i] = mPopScratch[i].r;
        }

        // In place: Process() reads both samples of a frame before writing them.
        float* channels[2] = {mEqScratchL.data(), mEqScratchR.data()};
        mEq.Process(channels, channels, static_cast<int>(popped));

        for (std::size_t i = 0; i < popped; ++i)
        {
            mPopScratch[i].l = mEqScratchL[i];
            mPopScratch[i].r = mEqScratchR[i];
        }
    }

    for (std::size_t i = 0; i < popped; ++i)
    {
        outputs[0][i] += mPopScratch[i].l * gainL;
        outputs[1][i] += mPopScratch[i].r * gainR;
    }

    // Underrun (popped < numSamples): per the priority principle, the
    // shortfall is left as silence rather than blocking or computing
    // anything inline here — outputs[] already holds the guitar signal for
    // those sample indices, so simply adding nothing further is correct.
    // No lock, no allocation, and (deliberately) no condition_variable
    // notify from this thread — the render thread re-checks for free ring
    // space on its own short poll interval instead, so the audio thread
    // never has to touch anything that could involve a kernel wait/wake.
}

// ════════════════════════════════════════════════════════════════════
// Message thread — idle polling
// ════════════════════════════════════════════════════════════════════

void PracticeToolService::OnIdle()
{
    const bool endedPending = mPlaybackEndedPending.exchange(false, std::memory_order_acq_rel);

    if (endedPending)
    {
        nlohmann::json msg;
        msg["type"] = "practiceToolPlaybackEnded";
        mSendMessage(msg.dump());
    }

    // Stay silent unless something is actually moving. The only thing that
    // changes on its own is the position readout, and only while playing —
    // every transport *transition* already pushes its own state message from
    // Play()/Pause()/Stop()/LoadDecodedBytes(). The one transition that does
    // not is end-of-track, which the render thread signals via
    // mPlaybackEndedPending, so that case falls through below.
    //
    // Without this guard a user who never touches the Practice Tool still
    // pays for it continuously: this ran unconditionally at kPracticeToolRateHz,
    // measured at ~12 messages/sec — 32% of all idle UI traffic — each one a
    // JSON build + dump + WebView IPC hop, and on the UI side a full
    // renderTransportControls()/renderWaveform() pass.
    if (!endedPending && mState.load(std::memory_order_acquire) != static_cast<int>(PlaybackState::Playing))
    {
        return;
    }

    SendTransportStateToUI();
}

void PracticeToolService::SendTransportStateToUI()
{
    auto buffer = std::atomic_load_explicit(&mBuffer, std::memory_order_acquire);
    const int state = mState.load(std::memory_order_acquire);

    double positionSec = 0.0;

    if (buffer && buffer->sampleRate > 0.0)
    {
        const double speed = mSpeed.load(std::memory_order_relaxed);
        const double cursorFrames = static_cast<double>(mReadCursorFrames.load(std::memory_order_relaxed));
        const double aheadOutFrames = mOutputRing ? static_cast<double>(mOutputRing->AvailableToRead()) : 0.0;
        const double aheadInFrames = aheadOutFrames * speed;
        // The render-thread cursor is ahead of what's actually audible by
        // however much lookahead currently sits in the ring; back that out
        // (converted from output-domain frames to source-domain via the
        // current speed) for a reasonable position estimate. This is an
        // approximation — exact positioning under variable time-stretch
        // would require per-frame bookkeeping the UI doesn't need for a
        // ~10-15Hz progress readout.
        //
        // When looping, `cursorFrames` wraps back to the loop start well
        // before it reaches the lookahead amount buffered in the ring, so a
        // plain subtraction can underflow to BEFORE the loop's start (or
        // even to 0) every time the render cursor is near the top of a
        // loop cycle — visually reading as the playhead sitting well to the
        // left of the loop region. Do the subtraction modulo the loop's own
        // length instead, so it wraps within the loop the same way the
        // cursor itself does.
        double approxFrames;
        const bool loopingEnabled = mLoopingEnabled.load(std::memory_order_relaxed);
        auto loop = std::atomic_load_explicit(&mActiveLoop, std::memory_order_acquire);

        if (loopingEnabled && loop && loop->endFrame > loop->startFrame)
        {
            const double loopStart = static_cast<double>(loop->startFrame);
            const double loopLen = static_cast<double>(loop->endFrame - loop->startFrame);
            double offsetIntoLoop = std::fmod(cursorFrames - loopStart, loopLen);

            if (offsetIntoLoop < 0.0)
            {
                offsetIntoLoop += loopLen;
            }

            double aheadInLoop = std::fmod(aheadInFrames, loopLen);
            double adjusted = offsetIntoLoop - aheadInLoop;

            if (adjusted < 0.0)
            {
                adjusted += loopLen;
            }

            approxFrames = loopStart + adjusted;
        }
        else
        {
            approxFrames = cursorFrames - aheadInFrames;

            if (approxFrames < 0.0)
            {
                approxFrames = 0.0;
            }
        }

        const double durationSec = static_cast<double>(buffer->totalFrames) / buffer->sampleRate;
        positionSec = std::min(approxFrames / buffer->sampleRate, durationSec);
    }

    static const char* kStateNames[] = {"stopped", "playing", "paused"};
    const int clampedState = std::clamp(state, 0, 2);

    nlohmann::json msg;
    msg["type"] = "practiceToolTransportState";
    msg["state"] = kStateNames[clampedState];
    msg["positionSec"] = positionSec;
    mSendMessage(msg.dump());
}
} // namespace guitarfx
