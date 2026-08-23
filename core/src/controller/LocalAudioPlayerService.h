#pragma once

// LocalAudioPlayerService — plays a local backing-track audio file, mixed
// into the main output bus so a guitarist can jam along with it.
//
// Mirrors DemoPreviewService's structure (full-buffer-preload playback,
// atomic shared_ptr buffer swap) but differs in two important ways:
//   1. It mixes POST-chain (after the guitar effects, like RenderMetronome),
//      never touching the guitar input — the backing track is not the
//      player's guitar signal and must never go through the amp/cab chain.
//   2. It supports independent tempo/pitch control via signalsmith-stretch,
//      which is far too expensive to run inline in the realtime audio
//      callback. That work happens on a dedicated background thread
//      (normal OS priority — never elevated, so it can never compete with
//      the audio thread) that renders ahead into a lock-free ring buffer.
//      RenderPostChain() (the audio-thread entry point) only ever pops
//      already-computed frames from that ring: cheap, bounded-time, no
//      locks, no allocation, no blocking, and — deliberately — no
//      condition-variable signalling either (see RenderPostChain). On
//      underrun it outputs silence for the backing track rather than
//      stalling — the guitar path's timing must never depend on how the
//      render thread is doing.
//
// The engine intentionally has NO concept of a "loop library": naming,
// persistence, and multiple saved loop regions are pure UI/app-settings
// bookkeeping (see the design plan). The engine only ever knows the
// currently *active* loop's sample bounds (or none = whole track) and an
// on/off flag.

#include "IPluginHost.h"
#include "util/SpscRingBuffer.h"

#include "signalsmith-stretch.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{

class LocalAudioPlayerService
{
public:
    LocalAudioPlayerService(IPluginHost& host,
                            std::mutex& dspMutex,
                            std::function<void(const std::string&, const std::string&)> reportError,
                            std::function<void(const std::string&)> sendMessage);
    ~LocalAudioPlayerService();

    LocalAudioPlayerService(const LocalAudioPlayerService&) = delete;
    LocalAudioPlayerService& operator=(const LocalAudioPlayerService&) = delete;

    /// Starts the background render thread (once) and creates the ring
    /// buffer (once, sized generously so it never needs to be resized while
    /// the render/audio threads might be touching it concurrently). Safe to
    /// call again on sample-rate changes: the render thread notices via
    /// mSampleRate and reconfigures signalsmith-stretch itself.
    void Prepare(double sampleRate, int maxBlockSize);

    /// Joins the background render thread. Called from the destructor; safe
    /// to call more than once.
    void Shutdown();

    /// Reads the file directly from a native path (NOT base64-over-JSON —
    /// backing tracks can be multi-minute, unlike the short clips
    /// DemoPreviewService/riff preview send inline). Message-thread only.
    void LoadFile(const std::string& path);

    void Play();
    void Pause();
    void Stop();
    void SeekSeconds(double seconds);
    void SetSpeed(double ratio);              // clamped to [0.25, 2.0]
    void SetPitchSemitones(double semitones); // clamped to [-12, 12]
    void SetGain(double linearGain);          // applied on the audio thread — instant, no flush needed
    void SetBalance(double balance);          // clamped to [-1 (full left), 1 (full right)]; instant, no flush needed

    // The active loop region. No bounds set = loop the whole track. These
    // only change which bounds a *future* wrap respects; they do not move
    // the play cursor (the UI issues a separate seek alongside activating a
    // loop chip, per the message contract).
    void SetLoopRegion(double startSec, double endSec);
    void ClearLoopRegion();
    void SetLoopingEnabled(bool enabled);

    /// AUDIO THREAD. Additive mix into outputs[] — never touches inputs[].
    /// Pops already-rendered frames from the ring buffer; on underrun, adds
    /// silence for the missing frames rather than blocking or computing
    /// anything inline. Allocation-free (mPopScratch is pre-sized in
    /// Prepare()) and never touches a mutex or condition_variable.
    void RenderPostChain(float** outputs, int numSamples);

    /// Message thread, called from the OnIdle() hub at a divided-down rate
    /// (~10-15 Hz). Sends localAudioTransportState / localAudioPlaybackEnded.
    void OnIdle();

    [[nodiscard]] bool IsLoaded() const;

private:
    // Grants the unit test (LocalAudioPlayerCrossfadeTests.cpp) direct access
    // to ReadSourceWindow()/BeginCrossfade() and the private TrackBuffer type
    // so the loop-wrap/seek crossfade logic can be exercised synchronously
    // with a synthetic buffer, without needing real file I/O or the
    // background render thread running.
    friend struct LocalAudioPlayerServiceTestAccess;

    struct StereoFrame
    {
        float l = 0.0f;
        float r = 0.0f;
    };

    struct TrackBuffer
    {
        std::string path;
        std::string title;
        double sampleRate = 0.0;
        int channels = 0;
        std::vector<std::vector<float>> channelSamples; // resampled to host rate
        std::size_t totalFrames = 0;
    };

    struct ActiveLoopBounds
    {
        std::size_t startFrame = 0;
        std::size_t endFrame = 0; // exclusive
    };

    enum class PlaybackState : int
    {
        Stopped = 0,
        Playing = 1,
        Paused = 2
    };

    // ── Background render thread (everything below is render-thread-only
    // unless noted) ─────────────────────────────────────────────────────
    void RenderThreadLoop();
    void RenderChunk(const std::shared_ptr<TrackBuffer>& buffer, std::size_t& cursor);

    /// Reads up to numFrames of source audio into outL/outR starting at
    /// *cursor*, advancing it, honoring the active loop region when looping
    /// is enabled. Returns the number of frames actually written; fewer
    /// than requested means the end of a non-looping track/region was
    /// reached (the caller pads the remainder with silence). When looping,
    /// a wrap never returns short — it transparently crossfades via
    /// BeginCrossfade()/DrainFadeCarry() so the stretch engine downstream
    /// always sees a continuous stream and is never reset.
    int ReadSourceWindow(const std::shared_ptr<TrackBuffer>& buffer,
                        float* outL, float* outR,
                        std::size_t& cursor, int numFrames);

    /// Prepares an equal-power crossfade between the track audio at
    /// [fromFrame, fromFrame+len) (fading out) and [toFrame, toFrame+len)
    /// (fading in) into the fade-carry buffer, where len is chosen
    /// automatically (~8ms, clamped to available source samples on both
    /// sides and to half the active region length). Used for BOTH loop
    /// wraps (from = old loop end, to = loop start) and manual seeks/loop
    /// jumps (from = position before the jump, to = the new target) so
    /// neither causes a hard click. If no room exists to blend (e.g. right
    /// at end-of-file) it leaves the carry empty — callers then fall back
    /// to a hard cut, which is the best available option in that corner
    /// case.
    void BeginCrossfade(const std::shared_ptr<TrackBuffer>& buffer, std::size_t fromFrame, std::size_t toFrame);
    /// Drains up to maxCount frames from the fade-carry buffer into
    /// outL/outR (written at index 0). Returns the count drained.
    std::size_t DrainFadeCarry(float* outL, float* outR, std::size_t maxCount);
    void EnsureFadeCarryCapacity(std::size_t frames);
    void EnsureRenderScratchCapacity(int numInFrames, int numOutFrames);
    /// Applies the current pitch to the stretch engine (setTransposeSemitones
    /// only — never reset()).
    void ApplyPitchToStretch(double semitones);

    void SendTransportStateToUI(); // message thread

    IPluginHost& mHost;
    std::mutex& mDSPMutex;
    std::function<void(const std::string&, const std::string&)> mReportError;
    std::function<void(const std::string&)> mSendMessage;

    std::atomic<double> mSampleRate{0.0}; // written by Prepare() (message thread), read by the render thread
    int mMaxBlockSize = 0;                // only touched under mDSPMutex (Prepare / RenderPostChain via ProcessAudioLocked)

    std::shared_ptr<TrackBuffer> mBuffer; // atomic load/store (message thread writes, render thread reads)

    std::atomic<std::size_t> mReadCursorFrames{0}; // owned by the render thread; read by OnIdle for position reporting
    std::atomic<int> mState{static_cast<int>(PlaybackState::Stopped)};
    std::atomic<double> mSpeed{1.0};
    std::atomic<double> mPitchSemitones{0.0};
    std::atomic<double> mGain{1.0};
    std::atomic<double> mBalance{0.0}; // -1 = full left, 0 = center, +1 = full right
    std::atomic<bool> mLoopingEnabled{false};
    std::shared_ptr<ActiveLoopBounds> mActiveLoop; // null = whole track; atomic load/store

    std::atomic<bool> mSeekPending{false};
    std::atomic<double> mPendingSeekSeconds{0.0};
    std::atomic<bool> mPlaybackEndedPending{false};

    // Bumped whenever any state that can invalidate in-flight rendered audio
    // changes (seek, loop region/enable, speed, pitch, new file, stop). The
    // render thread compares this to its own local snapshot each chunk; on
    // mismatch it flushes the ring and re-syncs to the new position/settings
    // before producing further audio. Gain deliberately does NOT bump this —
    // it's applied at mix time on the audio thread instead (see
    // RenderPostChain), so a volume change is instant rather than lagging by
    // a whole ring-buffer's worth of lookahead.
    std::atomic<std::uint64_t> mParamGeneration{0};

    signalsmith::stretch::SignalsmithStretch<float> mStretch; // background-thread only
    bool mStretchConfigured = false;

    std::vector<float> mSourceScratchL, mSourceScratchR; // background-thread only
    std::vector<float> mStretchOutL, mStretchOutR;        // background-thread only
    std::vector<StereoFrame> mPushScratch;                // background-thread only

    // Crossfade "carry" buffer: BeginCrossfade() can compute more faded
    // frames than fit in the caller's current request; the remainder is
    // stashed here and drained first on the next ReadSourceWindow() call.
    // Background-thread only.
    std::vector<float> mFadeCarryL, mFadeCarryR;
    std::size_t mFadeCarryCount = 0;
    std::size_t mFadeCarryPos = 0;

    // SPSC ring: background thread (producer) writes stretched stereo
    // frames (NOT gain-scaled — gain is applied on the audio thread); the
    // audio thread (consumer) reads them in RenderPostChain(). Sized for
    // a generous ~1.5s of lookahead so transient render-thread slowness
    // (system load, first fill after loading a large file) never becomes
    // an audible gap. Created once in Prepare() and never resized, so the
    // two threads never race a buffer swap.
    std::unique_ptr<util::SpscRingBuffer<StereoFrame>> mOutputRing;
    std::vector<StereoFrame> mPopScratch; // audio-thread-only; sized in Prepare()

    std::thread mRenderThread;
    std::atomic<bool> mRenderThreadRunning{false};
    std::atomic<bool> mRenderThreadQuit{false};
    std::condition_variable mRenderWake;
    std::mutex mRenderWakeMutex;
};

} // namespace guitarfx
