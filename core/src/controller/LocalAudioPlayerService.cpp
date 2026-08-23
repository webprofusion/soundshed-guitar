#include "controller/LocalAudioPlayerService.h"

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

namespace
{
constexpr double kMinSpeed = 0.25;
constexpr double kMaxSpeed = 2.0;
constexpr double kMinPitchSemitones = -12.0;
constexpr double kMaxPitchSemitones = 12.0;
constexpr double kTonalityLimitHz = 8000.0; // matches PitchShiftEffect's convention
constexpr double kCrossfadeSeconds = 0.008; // ~8ms, within the plan's 5-10ms guidance
constexpr std::size_t kMaxCrossfadeCapacityFrames = 16384; // safety ceiling (~340ms @ 48kHz)
constexpr int kRenderChunkOutFrames = 1024;
constexpr double kHalfPi = 1.5707963267948966;

[[nodiscard]] std::size_t SecondsToFrames(double seconds, double sampleRate)
{
    if (!(seconds > 0.0) || sampleRate <= 0.0)
        return 0;
    return static_cast<std::size_t>(std::llround(seconds * sampleRate));
}

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
        return peaks;

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
                peak = p;
        }
        peaks.push_back(static_cast<double>(std::clamp(peak, 0.0f, 1.0f)));
    }

    return peaks;
}

} // namespace

LocalAudioPlayerService::LocalAudioPlayerService(IPluginHost& host,
                                                 std::mutex& dspMutex,
                                                 std::function<void(const std::string&, const std::string&)> reportError,
                                                 std::function<void(const std::string&)> sendMessage)
    : mHost(host)
    , mDSPMutex(dspMutex)
    , mReportError(std::move(reportError))
    , mSendMessage(std::move(sendMessage))
{
}

LocalAudioPlayerService::~LocalAudioPlayerService()
{
    Shutdown();
}

void LocalAudioPlayerService::Prepare(double sampleRate, int maxBlockSize)
{
    mSampleRate.store(sampleRate, std::memory_order_release);
    mMaxBlockSize = std::max(maxBlockSize, 0);
    mPopScratch.assign(static_cast<std::size_t>(std::max(mMaxBlockSize, 1)), StereoFrame{});

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
        mOutputRing = std::make_unique<util::SpscRingBuffer<StereoFrame>>(
            std::max<std::size_t>(framesForLookahead, 4096));
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

void LocalAudioPlayerService::Shutdown()
{
    mRenderThreadQuit.store(true, std::memory_order_release);
    mRenderWake.notify_all();
    if (mRenderThread.joinable())
        mRenderThread.join();
    mRenderThreadRunning.store(false, std::memory_order_release);
}

void LocalAudioPlayerService::LoadFile(const std::string& path)
{
    const auto bytes = util::ReadFileBytes(util::PathFromUtf8(path));
    if (bytes.empty())
    {
        mReportError("Unable to load audio file", "File is empty or could not be opened: " + path);
        return;
    }

    LoadDecodedBytes(bytes, path);
}

void LocalAudioPlayerService::LoadFileFromBytes(const std::vector<std::uint8_t>& bytes,
                                                const std::string& displayName)
{
    if (bytes.empty())
    {
        mReportError("Unable to load audio file", "Dropped file was empty: " + displayName);
        return;
    }

    LoadDecodedBytes(bytes, displayName);
}

void LocalAudioPlayerService::LoadDecodedBytes(const std::vector<std::uint8_t>& bytes,
                                               const std::string& displayPath)
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
            channel.resize(minFrames);
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
    }

    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>{}, std::memory_order_release);
    mLoopingEnabled.store(false, std::memory_order_relaxed);
    mReadCursorFrames.store(0, std::memory_order_relaxed);
    mPendingSeekSeconds.store(0.0, std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release); // render thread starts this fresh file at 0
    mState.store(static_cast<int>(PlaybackState::Stopped), std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();

    const double durationSec = buffer->sampleRate > 0.0
        ? static_cast<double>(buffer->totalFrames) / buffer->sampleRate
        : 0.0;

    nlohmann::json msg;
    msg["type"] = "localAudioFileLoaded";
    msg["path"] = displayPath;
    msg["title"] = buffer->title;
    msg["durationSec"] = durationSec;
    msg["waveformPeaksL"] = peaksJsonL;
    msg["waveformPeaksR"] = peaksJsonR;
    mSendMessage(msg.dump());

    SendTransportStateToUI();
}

void LocalAudioPlayerService::Play()
{
    if (!std::atomic_load_explicit(&mBuffer, std::memory_order_acquire))
        return;
    mState.store(static_cast<int>(PlaybackState::Playing), std::memory_order_release);
    mRenderWake.notify_all(); // wake the render thread immediately rather than waiting out its poll interval
    SendTransportStateToUI();
}

void LocalAudioPlayerService::Pause()
{
    mState.store(static_cast<int>(PlaybackState::Paused), std::memory_order_release);
    mRenderWake.notify_all();
    SendTransportStateToUI();
}

void LocalAudioPlayerService::Stop()
{
    mState.store(static_cast<int>(PlaybackState::Stopped), std::memory_order_release);
    mPendingSeekSeconds.store(0.0, std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
    SendTransportStateToUI();
}

void LocalAudioPlayerService::SeekSeconds(double seconds)
{
    mPendingSeekSeconds.store(std::max(0.0, seconds), std::memory_order_relaxed);
    mSeekPending.store(true, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void LocalAudioPlayerService::SetSpeed(double ratio)
{
    mSpeed.store(std::clamp(ratio, kMinSpeed, kMaxSpeed), std::memory_order_relaxed);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void LocalAudioPlayerService::SetPitchSemitones(double semitones)
{
    mPitchSemitones.store(std::clamp(semitones, kMinPitchSemitones, kMaxPitchSemitones), std::memory_order_relaxed);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void LocalAudioPlayerService::SetGain(double linearGain)
{
    // Deliberately does NOT bump mParamGeneration / flush the ring: gain is
    // applied at mix time on the audio thread (RenderPostChain), so a
    // volume-slider drag is instant rather than lagging by a whole ring
    // buffer's worth of already-rendered lookahead.
    mGain.store(std::max(0.0, linearGain), std::memory_order_relaxed);
}

void LocalAudioPlayerService::SetBalance(double balance)
{
    // Same rationale as SetGain: applied directly at mix time, no flush.
    mBalance.store(std::clamp(balance, -1.0, 1.0), std::memory_order_relaxed);
}

void LocalAudioPlayerService::SetLoopRegion(double startSec, double endSec)
{
    auto buffer = std::atomic_load_explicit(&mBuffer, std::memory_order_acquire);
    const double sr = mSampleRate.load(std::memory_order_relaxed);

    auto bounds = std::make_shared<ActiveLoopBounds>();
    bounds->startFrame = SecondsToFrames(startSec, sr);
    bounds->endFrame = SecondsToFrames(endSec, sr);
    if (buffer && buffer->totalFrames > 0)
    {
        bounds->startFrame = std::min(bounds->startFrame, buffer->totalFrames - 1);
        bounds->endFrame = std::min(bounds->endFrame, buffer->totalFrames);
    }
    if (bounds->endFrame <= bounds->startFrame)
        return; // ignore degenerate/invalid region rather than looping nothing

    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>(bounds), std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void LocalAudioPlayerService::ClearLoopRegion()
{
    std::atomic_store_explicit(&mActiveLoop, std::shared_ptr<ActiveLoopBounds>{}, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

void LocalAudioPlayerService::SetLoopingEnabled(bool enabled)
{
    mLoopingEnabled.store(enabled, std::memory_order_release);
    mParamGeneration.fetch_add(1, std::memory_order_release);
    mRenderWake.notify_all();
}

bool LocalAudioPlayerService::IsLoaded() const
{
    return std::atomic_load_explicit(&mBuffer, std::memory_order_acquire) != nullptr;
}

// ════════════════════════════════════════════════════════════════════
// Audio thread
// ════════════════════════════════════════════════════════════════════

void LocalAudioPlayerService::RenderPostChain(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1] || numSamples <= 0)
        return;
    if (mPopScratch.empty()) // Prepare() not called yet
        return;
    if (mState.load(std::memory_order_acquire) != static_cast<int>(PlaybackState::Playing))
        return;
    if (!mOutputRing)
        return;

    numSamples = std::min(numSamples, static_cast<int>(mPopScratch.size()));

    const std::size_t popped = mOutputRing->Pop(mPopScratch.data(), static_cast<std::size_t>(numSamples));
    const float gain = static_cast<float>(mGain.load(std::memory_order_relaxed));
    const double balance = mBalance.load(std::memory_order_relaxed);
    // A "balance" control, not a full pan law: the favored channel always
    // stays at unity gain and only the other channel is attenuated, matching
    // a standard stereo-mixer balance knob rather than mono-source panning.
    const float gainL = gain * static_cast<float>(balance <= 0.0 ? 1.0 : 1.0 - balance);
    const float gainR = gain * static_cast<float>(balance >= 0.0 ? 1.0 : 1.0 + balance);
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

void LocalAudioPlayerService::OnIdle()
{
    if (mPlaybackEndedPending.exchange(false, std::memory_order_acq_rel))
    {
        nlohmann::json msg;
        msg["type"] = "localAudioPlaybackEnded";
        mSendMessage(msg.dump());
    }
    SendTransportStateToUI();
}

void LocalAudioPlayerService::SendTransportStateToUI()
{
    auto buffer = std::atomic_load_explicit(&mBuffer, std::memory_order_acquire);
    const int state = mState.load(std::memory_order_acquire);

    double positionSec = 0.0;
    if (buffer && buffer->sampleRate > 0.0)
    {
        const double speed = mSpeed.load(std::memory_order_relaxed);
        const double cursorFrames = static_cast<double>(mReadCursorFrames.load(std::memory_order_relaxed));
        const double aheadOutFrames = mOutputRing
            ? static_cast<double>(mOutputRing->AvailableToRead())
            : 0.0;
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
                offsetIntoLoop += loopLen;
            double aheadInLoop = std::fmod(aheadInFrames, loopLen);
            double adjusted = offsetIntoLoop - aheadInLoop;
            if (adjusted < 0.0)
                adjusted += loopLen;
            approxFrames = loopStart + adjusted;
        }
        else
        {
            approxFrames = cursorFrames - aheadInFrames;
            if (approxFrames < 0.0)
                approxFrames = 0.0;
        }
        const double durationSec = static_cast<double>(buffer->totalFrames) / buffer->sampleRate;
        positionSec = std::min(approxFrames / buffer->sampleRate, durationSec);
    }

    static const char* kStateNames[] = {"stopped", "playing", "paused"};
    const int clampedState = std::clamp(state, 0, 2);

    nlohmann::json msg;
    msg["type"] = "localAudioTransportState";
    msg["state"] = kStateNames[clampedState];
    msg["positionSec"] = positionSec;
    mSendMessage(msg.dump());
}

// ════════════════════════════════════════════════════════════════════
// Background render thread
// ════════════════════════════════════════════════════════════════════

void LocalAudioPlayerService::ApplyPitchToStretch(double semitones)
{
    const double sr = mSampleRate.load(std::memory_order_relaxed);
    if (sr <= 0.0)
        return;
    const float tonalityLimit = static_cast<float>(kTonalityLimitHz / sr);
    mStretch.setTransposeSemitones(static_cast<float>(semitones), tonalityLimit);
}

void LocalAudioPlayerService::EnsureRenderScratchCapacity(int numInFrames, int numOutFrames)
{
    if (static_cast<int>(mSourceScratchL.size()) < numInFrames)
    {
        mSourceScratchL.resize(static_cast<std::size_t>(numInFrames));
        mSourceScratchR.resize(static_cast<std::size_t>(numInFrames));
    }
    if (static_cast<int>(mStretchOutL.size()) < numOutFrames)
    {
        mStretchOutL.resize(static_cast<std::size_t>(numOutFrames));
        mStretchOutR.resize(static_cast<std::size_t>(numOutFrames));
    }
}

void LocalAudioPlayerService::EnsureFadeCarryCapacity(std::size_t frames)
{
    if (mFadeCarryL.size() < frames)
    {
        mFadeCarryL.resize(frames);
        mFadeCarryR.resize(frames);
    }
}

void LocalAudioPlayerService::BeginCrossfade(const std::shared_ptr<TrackBuffer>& buffer,
                                             std::size_t fromFrame, std::size_t toFrame)
{
    mFadeCarryCount = 0;
    mFadeCarryPos = 0;
    if (!buffer || buffer->totalFrames == 0)
        return;

    const std::size_t total = buffer->totalFrames;
    if (fromFrame >= total || toFrame >= total)
        return; // nothing to blend at/past end-of-file; caller hard-cuts

    const double sr = mSampleRate.load(std::memory_order_relaxed);
    const std::size_t nominal = static_cast<std::size_t>(std::max(1.0, sr * kCrossfadeSeconds));
    const std::size_t maxByFrom = total - fromFrame;
    const std::size_t maxByTo = total - toFrame;
    const std::size_t fadeLen = std::min({nominal, maxByFrom, maxByTo, kMaxCrossfadeCapacityFrames});
    if (fadeLen == 0)
        return;

    EnsureFadeCarryCapacity(fadeLen);

    const auto& ch0 = buffer->channelSamples[0];
    const auto& ch1 = buffer->channelSamples.size() > 1 ? buffer->channelSamples[1] : buffer->channelSamples[0];

    for (std::size_t i = 0; i < fadeLen; ++i)
    {
        const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(fadeLen);
        const float fadeOut = static_cast<float>(std::cos(t * kHalfPi));
        const float fadeIn = static_cast<float>(std::sin(t * kHalfPi));
        mFadeCarryL[i] = ch0[fromFrame + i] * fadeOut + ch0[toFrame + i] * fadeIn;
        mFadeCarryR[i] = ch1[fromFrame + i] * fadeOut + ch1[toFrame + i] * fadeIn;
    }

    mFadeCarryCount = fadeLen;
    mFadeCarryPos = 0;
}

std::size_t LocalAudioPlayerService::DrainFadeCarry(float* outL, float* outR, std::size_t maxCount)
{
    if (mFadeCarryPos >= mFadeCarryCount)
        return 0;
    const std::size_t remaining = mFadeCarryCount - mFadeCarryPos;
    const std::size_t n = std::min(remaining, maxCount);
    for (std::size_t i = 0; i < n; ++i)
    {
        outL[i] = mFadeCarryL[mFadeCarryPos + i];
        outR[i] = mFadeCarryR[mFadeCarryPos + i];
    }
    mFadeCarryPos += n;
    if (mFadeCarryPos >= mFadeCarryCount)
    {
        mFadeCarryCount = 0;
        mFadeCarryPos = 0;
    }
    return n;
}

int LocalAudioPlayerService::ReadSourceWindow(const std::shared_ptr<TrackBuffer>& buffer,
                                              float* outL, float* outR,
                                              std::size_t& cursor, int numFrames)
{
    if (!buffer || buffer->totalFrames == 0 || numFrames <= 0)
        return 0;

    int written = static_cast<int>(DrainFadeCarry(outL, outR, static_cast<std::size_t>(numFrames)));
    if (written >= numFrames)
        return written;

    const auto& ch0 = buffer->channelSamples[0];
    const auto& ch1 = buffer->channelSamples.size() > 1 ? buffer->channelSamples[1] : buffer->channelSamples[0];
    const std::size_t total = buffer->totalFrames;

    const bool loopEnabled = mLoopingEnabled.load(std::memory_order_relaxed);
    auto loop = std::atomic_load_explicit(&mActiveLoop, std::memory_order_acquire);

    std::size_t regionStart = 0;
    std::size_t regionEnd = total;
    if (loopEnabled && loop && loop->endFrame > loop->startFrame)
    {
        regionStart = std::min(loop->startFrame, total);
        regionEnd = std::min(loop->endFrame, total);
        if (regionEnd <= regionStart)
        {
            regionStart = 0;
            regionEnd = total;
        }
    }

    while (written < numFrames)
    {
        if (cursor >= total)
            break; // absolute end of file — caller pads silence / handles non-looping exhaustion

        if (loopEnabled && cursor >= regionEnd)
        {
            BeginCrossfade(buffer, regionEnd, regionStart);
            cursor = regionStart + mFadeCarryCount; // mFadeCarryCount==0 => hard-cut fallback to regionStart
            const std::size_t n = DrainFadeCarry(outL + written, outR + written,
                                                 static_cast<std::size_t>(numFrames - written));
            written += static_cast<int>(n);
            continue;
        }
        if (!loopEnabled && cursor >= regionEnd) // regionEnd == total when not looping
            break;

        const std::size_t boundary = loopEnabled ? regionEnd : total;
        const std::size_t n = std::min(boundary - cursor, static_cast<std::size_t>(numFrames - written));
        for (std::size_t i = 0; i < n; ++i)
        {
            outL[written + static_cast<int>(i)] = ch0[cursor + i];
            outR[written + static_cast<int>(i)] = ch1[cursor + i];
        }
        cursor += n;
        written += static_cast<int>(n);
    }

    return written;
}

void LocalAudioPlayerService::RenderChunk(const std::shared_ptr<TrackBuffer>& buffer, std::size_t& cursor)
{
    const double speed = std::clamp(mSpeed.load(std::memory_order_relaxed), kMinSpeed, kMaxSpeed);
    const int roomFrames = static_cast<int>(std::min<std::size_t>(mOutputRing->AvailableToWrite(),
                                                                  static_cast<std::size_t>(kRenderChunkOutFrames)));
    if (roomFrames <= 0)
        return;

    const int numOutFrames = roomFrames;
    const int numInFrames = std::max(1, static_cast<int>(std::lround(numOutFrames * speed)));

    EnsureRenderScratchCapacity(numInFrames, numOutFrames);

    const int written = ReadSourceWindow(buffer, mSourceScratchL.data(), mSourceScratchR.data(), cursor, numInFrames);
    const bool exhausted = written < numInFrames;
    if (exhausted)
    {
        std::fill(mSourceScratchL.begin() + written, mSourceScratchL.begin() + numInFrames, 0.0f);
        std::fill(mSourceScratchR.begin() + written, mSourceScratchR.begin() + numInFrames, 0.0f);
    }

    float* inPtrs[2] = {mSourceScratchL.data(), mSourceScratchR.data()};
    float* outPtrs[2] = {mStretchOutL.data(), mStretchOutR.data()};
    // Different in/out counts here is exactly how signalsmith-stretch
    // performs time-stretching (confirmed by reading process()'s
    // implementation directly rather than assuming); equal counts would be
    // pitch-only, as used by PitchShiftEffect/TransposeEffect.
    mStretch.process(inPtrs, numInFrames, outPtrs, numOutFrames);

    mPushScratch.resize(static_cast<std::size_t>(numOutFrames));
    for (int i = 0; i < numOutFrames; ++i)
    {
        mPushScratch[static_cast<std::size_t>(i)].l = mStretchOutL[static_cast<std::size_t>(i)];
        mPushScratch[static_cast<std::size_t>(i)].r = mStretchOutR[static_cast<std::size_t>(i)];
    }
    mOutputRing->Push(mPushScratch.data(), static_cast<std::size_t>(numOutFrames));

    if (exhausted && !mLoopingEnabled.load(std::memory_order_relaxed))
    {
        mPlaybackEndedPending.store(true, std::memory_order_release);
        mState.store(static_cast<int>(PlaybackState::Stopped), std::memory_order_release);
        cursor = 0;
    }
}

void LocalAudioPlayerService::RenderThreadLoop()
{
    std::shared_ptr<TrackBuffer> lastSeenBuffer;
    std::size_t localCursor = 0;
    std::uint64_t localGeneration = (std::numeric_limits<std::uint64_t>::max)(); // force resync on first pass
    double configuredSampleRate = -1.0;

    while (!mRenderThreadQuit.load(std::memory_order_acquire))
    {
        auto buffer = std::atomic_load_explicit(&mBuffer, std::memory_order_acquire);
        if (!buffer || buffer->totalFrames == 0)
        {
            std::unique_lock<std::mutex> lock(mRenderWakeMutex);
            mRenderWake.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                return mRenderThreadQuit.load(std::memory_order_acquire)
                    || std::atomic_load_explicit(&mBuffer, std::memory_order_acquire) != nullptr;
            });
            continue;
        }

        const double sr = mSampleRate.load(std::memory_order_acquire);
        if (sr > 0.0 && sr != configuredSampleRate)
        {
            // Engine (re)initialization on startup / sample-rate change —
            // NOT a loop-wrap or seek, so calling reset() here is fine (and
            // necessary: presetCheaper() requires it, same as
            // PitchShiftEffect::Prepare()).
            mStretch.presetCheaper(2, static_cast<float>(sr), false);
            mStretch.reset();
            configuredSampleRate = sr;
            mStretchConfigured = true;
            ApplyPitchToStretch(mPitchSemitones.load(std::memory_order_relaxed));
        }

        const bool isNewBuffer = (buffer.get() != lastSeenBuffer.get());
        const std::uint64_t gen = mParamGeneration.load(std::memory_order_acquire);
        if (isNewBuffer || gen != localGeneration)
        {
            localGeneration = gen;
            const bool seekReq = mSeekPending.exchange(false, std::memory_order_acq_rel);
            const double seekSecs = mPendingSeekSeconds.load(std::memory_order_relaxed);

            if (isNewBuffer)
            {
                lastSeenBuffer = buffer;
                mFadeCarryCount = 0;
                mFadeCarryPos = 0;
                localCursor = seekReq ? SecondsToFrames(seekSecs, buffer->sampleRate) : 0;
                // A new file is a deliberate, hard discontinuity — unlike a
                // loop wrap or seek within the same track (which must stay
                // click-free via the crossfade), there is no musical reason
                // to blend the outgoing track's spectral history into the
                // new one. Reset so the previous track's overlap-add state
                // can't bleed into the first block of the new one.
                if (mStretchConfigured)
                    mStretch.reset();
            }
            else if (seekReq)
            {
                const std::size_t target = buffer->totalFrames > 0
                    ? std::min(SecondsToFrames(seekSecs, buffer->sampleRate), buffer->totalFrames - 1)
                    : std::size_t{0};
                // Crossfade from wherever we were about to play next into the
                // new target — the same click-free transition a loop wrap
                // gets, per the plan ("The same crossfade-on-read approach
                // handles manual seeks and loop-selection jumps").
                BeginCrossfade(buffer, localCursor, target);
                localCursor = target + mFadeCarryCount;
            }
            // else: generation bumped for a non-seek reason (loop
            // region/enable toggle, speed, pitch) — the UI issues a separate
            // seek when it wants the cursor to actually move (e.g. jumping to
            // a newly-activated loop chip), so leave localCursor untouched;
            // only the stale ring lookahead needs discarding below.

            if (buffer->totalFrames > 0)
                localCursor = std::min(localCursor, buffer->totalFrames - 1);
            else
                localCursor = 0;

            ApplyPitchToStretch(mPitchSemitones.load(std::memory_order_relaxed));
            mOutputRing->RequestFlush();
        }

        // Only actually produce audio while playing. Rendering ahead while
        // Stopped/Paused would let a track/loop-region shorter than the
        // ring's ~1.5s lookahead hit "exhausted" before the user ever
        // pressed Play — firing a spurious localAudioPlaybackEnded and then
        // wrapping the cursor back to 0 to keep filling, which would make a
        // short clip audibly repeat itself the first time it's actually
        // played. Resync work above (seek/loop/pitch/speed) still applies
        // immediately regardless of transport state, so everything is ready
        // the instant playback starts.
        if (mState.load(std::memory_order_acquire) != static_cast<int>(PlaybackState::Playing))
        {
            std::unique_lock<std::mutex> lock(mRenderWakeMutex);
            mRenderWake.wait_for(lock, std::chrono::milliseconds(50), [this, gen]() {
                return mRenderThreadQuit.load(std::memory_order_acquire)
                    || mState.load(std::memory_order_acquire) == static_cast<int>(PlaybackState::Playing)
                    || mParamGeneration.load(std::memory_order_acquire) != gen;
            });
            continue;
        }

        if (mOutputRing->AvailableToWrite() == 0)
        {
            std::unique_lock<std::mutex> lock(mRenderWakeMutex);
            mRenderWake.wait_for(lock, std::chrono::milliseconds(20), [this, gen]() {
                return mRenderThreadQuit.load(std::memory_order_acquire)
                    || mParamGeneration.load(std::memory_order_acquire) != gen
                    || (mOutputRing && mOutputRing->AvailableToWrite() > 0);
            });
            continue;
        }

        RenderChunk(buffer, localCursor);
        mReadCursorFrames.store(localCursor, std::memory_order_relaxed);
    }
}

} // namespace guitarfx
