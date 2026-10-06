// PracticeToolServiceRender.cpp — the background render thread half of
// PracticeToolService.
//
// Everything here runs on that thread (never the audio thread, never the
// message thread): reading the source window, the loop-wrap/seek crossfade,
// driving signalsmith-stretch, and filling the SPSC ring the audio thread
// pops from. The other half of the class — lifecycle, file loading,
// transport, and the audio-thread mix — lives in PracticeToolService.cpp.
//
// Split purely by thread ownership, which is also what makes each file
// reviewable: the realtime-safety rules that apply differ completely between
// the two, and having them interleaved was what made the whole class hard to
// audit.

#include "controller/PracticeToolService.h"

#include "controller/internal/PracticeToolSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace guitarfx
{
using namespace controller_detail;

namespace
{
constexpr double kTonalityLimitHz = 8000.0;                // matches PitchShiftEffect's convention
constexpr double kCrossfadeSeconds = 0.008;                // ~8ms, within the plan's 5-10ms guidance
constexpr std::size_t kMaxCrossfadeCapacityFrames = 16384; // safety ceiling (~340ms @ 48kHz)
constexpr double kJumpFadeInSeconds = 0.005;               // a jump is a cut; ramp the new audio in
constexpr int kRenderChunkOutFrames = 1024;
constexpr double kHalfPi = 1.5707963267948966;
} // namespace

// ════════════════════════════════════════════════════════════════════
// Background render thread
// ════════════════════════════════════════════════════════════════════

void PracticeToolService::ApplyPitchToStretch(double semitones)
{
    const double sr = mSampleRate.load(std::memory_order_relaxed);

    if (sr <= 0.0)
    {
        return;
    }

    const float tonalityLimit = static_cast<float>(kTonalityLimitHz / sr);
    mStretch.setTransposeSemitones(static_cast<float>(semitones), tonalityLimit);
}

void PracticeToolService::EnsureRenderScratchCapacity(int numInFrames, int numOutFrames)
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

void PracticeToolService::EnsureFadeCarryCapacity(std::size_t frames)
{
    if (mFadeCarryL.size() < frames)
    {
        mFadeCarryL.resize(frames);
        mFadeCarryR.resize(frames);
    }
}

void PracticeToolService::BeginCrossfade(const std::shared_ptr<TrackBuffer>& buffer, std::size_t fromFrame,
                                         std::size_t toFrame)
{
    mFadeCarryCount = 0;
    mFadeCarryPos = 0;

    if (!buffer || buffer->totalFrames == 0)
    {
        return;
    }

    const std::size_t total = buffer->totalFrames;

    if (fromFrame >= total || toFrame >= total)
    {
        return; // nothing to blend at/past end-of-file; caller hard-cuts
    }

    const double sr = mSampleRate.load(std::memory_order_relaxed);
    const std::size_t nominal = static_cast<std::size_t>(std::max(1.0, sr * kCrossfadeSeconds));
    const std::size_t maxByFrom = total - fromFrame;
    const std::size_t maxByTo = total - toFrame;
    const std::size_t fadeLen = std::min({nominal, maxByFrom, maxByTo, kMaxCrossfadeCapacityFrames});

    if (fadeLen == 0)
    {
        return;
    }

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

void PracticeToolService::JumpTo(const std::shared_ptr<TrackBuffer>& buffer, std::size_t& cursor, std::size_t target)
{
    // Nothing of the old position may reach the stretcher: no loop-wrap blend
    // in flight, and the read starts cleanly at the target.
    mFadeCarryCount = 0;
    mFadeCarryPos = 0;
    cursor = target;

    if (!mStretchConfigured)
    {
        return;
    }

    // outputSeek() resets the engine, then uses the input past its own input
    // latency to pre-roll the output, so the first frame process() returns is
    // `target`. It infers the playback rate from how much input it is handed,
    // hence outputSeekLength(speed). Read through ReadSourceWindow so a loop
    // shorter than the pre-roll wraps exactly as playback will.
    const double speed =
        std::clamp(mSpeed.load(std::memory_order_relaxed), kPracticeToolMinSpeed, kPracticeToolMaxSpeed);
    const int seekLength = std::max(1, mStretch.outputSeekLength(static_cast<float>(speed)));

    EnsureRenderScratchCapacity(seekLength, 0);

    const int read = ReadSourceWindow(buffer, mSourceScratchL.data(), mSourceScratchR.data(), cursor, seekLength);

    if (read < seekLength) // the end of a track that is not looping
    {
        std::fill(mSourceScratchL.begin() + read, mSourceScratchL.begin() + seekLength, 0.0f);
        std::fill(mSourceScratchR.begin() + read, mSourceScratchR.begin() + seekLength, 0.0f);
    }

    float* inPtrs[2] = {mSourceScratchL.data(), mSourceScratchR.data()};
    mStretch.outputSeek(inPtrs, seekLength);

    const double sr = mSampleRate.load(std::memory_order_relaxed);
    mJumpFadeLen = static_cast<std::size_t>(std::max(1.0, sr * kJumpFadeInSeconds));
    mJumpFadePos = 0;
}

std::size_t PracticeToolService::DrainFadeCarry(float* outL, float* outR, std::size_t maxCount)
{
    if (mFadeCarryPos >= mFadeCarryCount)
    {
        return 0;
    }

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

int PracticeToolService::ReadSourceWindow(const std::shared_ptr<TrackBuffer>& buffer, float* outL, float* outR,
                                          std::size_t& cursor, int numFrames)
{
    if (!buffer || buffer->totalFrames == 0 || numFrames <= 0)
    {
        return 0;
    }

    int written = static_cast<int>(DrainFadeCarry(outL, outR, static_cast<std::size_t>(numFrames)));

    if (written >= numFrames)
    {
        return written;
    }

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
        {
            break; // absolute end of file — caller pads silence / handles non-looping exhaustion
        }

        if (loopEnabled && cursor >= regionEnd)
        {
            BeginCrossfade(buffer, regionEnd, regionStart);
            cursor = regionStart + mFadeCarryCount; // mFadeCarryCount==0 => hard-cut fallback to regionStart
            const std::size_t n =
                DrainFadeCarry(outL + written, outR + written, static_cast<std::size_t>(numFrames - written));
            written += static_cast<int>(n);
            continue;
        }

        if (!loopEnabled && cursor >= regionEnd) // regionEnd == total when not looping
        {
            break;
        }

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

void PracticeToolService::RenderChunk(const std::shared_ptr<TrackBuffer>& buffer, std::size_t& cursor)
{
    const double speed =
        std::clamp(mSpeed.load(std::memory_order_relaxed), kPracticeToolMinSpeed, kPracticeToolMaxSpeed);
    const int roomFrames = static_cast<int>(
        std::min<std::size_t>(mOutputRing->AvailableToWrite(), static_cast<std::size_t>(kRenderChunkOutFrames)));

    if (roomFrames <= 0)
    {
        return;
    }

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

    for (int i = 0; i < numOutFrames && mJumpFadePos < mJumpFadeLen; ++i, ++mJumpFadePos)
    {
        const float ramp = static_cast<float>(
            std::sin((static_cast<double>(mJumpFadePos) + 0.5) / static_cast<double>(mJumpFadeLen) * kHalfPi));
        mStretchOutL[static_cast<std::size_t>(i)] *= ramp;
        mStretchOutR[static_cast<std::size_t>(i)] *= ramp;
    }

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

void PracticeToolService::RenderThreadLoop()
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
                return mRenderThreadQuit.load(std::memory_order_acquire) ||
                       std::atomic_load_explicit(&mBuffer, std::memory_order_acquire) != nullptr;
            });
            continue;
        }

        const double sr = mSampleRate.load(std::memory_order_acquire);

        if (sr > 0.0 && sr != configuredSampleRate)
        {
            // Engine (re)initialization on startup / sample-rate change —
            // NOT a loop-wrap or seek, so calling reset() here is fine (and
            // necessary: configuring requires it, same as
            // PitchShiftEffect::Prepare()).
            //
            // presetDefault, not the low-latency config the live effects use:
            // this thread renders ~1.5s ahead into a ring, so Stretch's latency
            // costs nothing but time-to-first-audio, and the wider window is
            // measurably cleaner (1.9 vs 3.0 cents mean pitch error, -0.3 vs
            // -1.3 dB harmonic purity across the guitar range).
            mStretch.presetDefault(2, static_cast<float>(sr), false);
            mStretch.reset();
            configuredSampleRate = sr;
            mStretchConfigured = true;
            mStretchInputLatency.store(mStretch.inputLatency(), std::memory_order_relaxed);
            mStretchOutputLatency.store(mStretch.outputLatency(), std::memory_order_relaxed);
            ApplyPitchToStretch(mPitchSemitones.load(std::memory_order_relaxed));
        }

        const bool isNewBuffer = (buffer.get() != lastSeenBuffer.get());
        const std::uint64_t gen = mParamGeneration.load(std::memory_order_acquire);

        if (isNewBuffer || gen != localGeneration)
        {
            localGeneration = gen;
            const bool seekReq = mSeekPending.exchange(false, std::memory_order_acq_rel);
            const double seekSecs = mPendingSeekSeconds.load(std::memory_order_relaxed);

            if (isNewBuffer || seekReq)
            {
                lastSeenBuffer = buffer;
                const std::size_t target =
                    seekReq && buffer->totalFrames > 0
                        ? std::min(PracticeToolSecondsToFrames(seekSecs, buffer->sampleRate), buffer->totalFrames - 1)
                        : std::size_t{0};

                // A new file, a seek and a loop being selected are all jumps,
                // and none of them may carry the stretcher's history across:
                // it is the audio from before the jump, and streaming the
                // jump through let ~120 ms of it play before the target did.
                // The ring is flushed below, so what was audible is cut there
                // regardless; JumpTo() fades the new audio in over that cut.
                // Loop *wraps* never come through here — they stay a
                // crossfade in ReadSourceWindow, inside one continuous stream.
                JumpTo(buffer, localCursor, target);
            }

            // else: generation bumped for a non-seek reason (a loop handle
            // dragged, looping toggled, speed, pitch) — leave localCursor
            // untouched; only the stale ring lookahead needs discarding below.
            // Selecting a loop asks for its seek in the same change (see
            // SetLoopRegion's `restart`), so it always arrives as a seek.

            if (buffer->totalFrames > 0)
            {
                localCursor = std::min(localCursor, buffer->totalFrames - 1);
            }
            else
            {
                localCursor = 0;
            }

            ApplyPitchToStretch(mPitchSemitones.load(std::memory_order_relaxed));
            mOutputRing->RequestFlush();
            // Publish the jump now rather than after the next chunk: while
            // paused there is no next chunk, and the position readout would
            // keep reporting where playback used to be.
            mReadCursorFrames.store(localCursor, std::memory_order_relaxed);
        }

        // Only actually produce audio while playing. Rendering ahead while
        // Stopped/Paused would let a track/loop-region shorter than the
        // ring's ~1.5s lookahead hit "exhausted" before the user ever
        // pressed Play — firing a spurious practiceToolPlaybackEnded and then
        // wrapping the cursor back to 0 to keep filling, which would make a
        // short clip audibly repeat itself the first time it's actually
        // played. Resync work above (seek/loop/pitch/speed) still applies
        // immediately regardless of transport state, so everything is ready
        // the instant playback starts.
        if (mState.load(std::memory_order_acquire) != static_cast<int>(PlaybackState::Playing))
        {
            std::unique_lock<std::mutex> lock(mRenderWakeMutex);
            mRenderWake.wait_for(lock, std::chrono::milliseconds(50), [this, gen]() {
                return mRenderThreadQuit.load(std::memory_order_acquire) ||
                       mState.load(std::memory_order_acquire) == static_cast<int>(PlaybackState::Playing) ||
                       mParamGeneration.load(std::memory_order_acquire) != gen;
            });
            continue;
        }

        if (mOutputRing->AvailableToWrite() == 0)
        {
            std::unique_lock<std::mutex> lock(mRenderWakeMutex);
            mRenderWake.wait_for(lock, std::chrono::milliseconds(20), [this, gen]() {
                return mRenderThreadQuit.load(std::memory_order_acquire) ||
                       mParamGeneration.load(std::memory_order_acquire) != gen ||
                       (mOutputRing && mOutputRing->AvailableToWrite() > 0);
            });
            continue;
        }

        RenderChunk(buffer, localCursor);
        mReadCursorFrames.store(localCursor, std::memory_order_relaxed);
    }
}
} // namespace guitarfx
