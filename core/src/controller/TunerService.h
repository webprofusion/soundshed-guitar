#pragma once

// TunerService — carries pitch-detection results from the DSP to the UI.
//
// The mixer's tuner callback fires on the audio thread, so it cannot format
// JSON or touch the message bridge. It writes one struct under a short mutex
// and raises a flag; OnIdle() on the message thread copies the struct out and
// publishes it. Only the latest reading survives — a tuner display has no use
// for a backlog, and dropping stale readings is what keeps the audio-thread
// side bounded.
//
// The mutex is held for a handful of assignments and never while allocating,
// so the audio thread's worst case is one uncontended lock per detection.
//
// It also answers the tuner's UI messages itself (RegisterMessageHandlers): turning
// the mixer's tuner on and off, live-vs-muted monitoring and the reference pitch,
// each set under the DSP lock and acknowledged to the UI.
//
// While the Scale Guide listens it also says what it hears (music/ListenAnalysis.h): each
// tunerUpdate carries `heard`, the key of the recent notes (or chords) and in Chords the chord
// a strum most likely is. HoldListen() keeps the guide's own playing out of it.

#include "music/ListenAnalysis.h"

#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class MessageHandlerRegistry;
class MultiPresetMixer;

class TunerService
{
  public:
    struct Reading
    {
        std::string noteName;
        int octave = 0;
        double frequency = 0.0;
        double centOffset = 0.0;
        double confidence = 0.0;
        bool detected = false;
        double levelDb = -120.0;
        /// While the Scale Guide listens for chords: energy per pitch class, C first.
        bool hasChroma = false;
        std::array<float, 12> chroma{};
    };

    using SendMessageFn = std::function<void(const std::string&)>;

    TunerService(SendMessageFn sendMessage, MultiPresetMixer& mixer, std::mutex& dspMutex);

    /// Registers "tuner" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// Audio thread: records the latest reading, replacing any not yet published.
    void PostReading(const Reading& reading);

    /// Message thread: publishes the latest reading, if there is a new one.
    void OnIdle();

    void SetActive(bool active)
    {
        mActive.store(active, std::memory_order_release);
    }

    /// The Scale Guide is sounding for this long, from now: what LISTEN hears meanwhile is not the
    /// player's. Replaces any hold before it, so a stop can shorten one.
    void HoldListen(double seconds);

    [[nodiscard]] bool IsActive() const
    {
        return mActive.load(std::memory_order_acquire);
    }

  private:
    /// "tuner": start, stop, setLiveMode, setReference, listen, or a bare {enabled}.
    void HandleTunerRequest(const nlohmann::json& payload);

    SendMessageFn mSendMessage;
    MultiPresetMixer& mMixer;
    std::mutex& mDSPMutex;

    std::atomic<bool> mActive{false};
    std::atomic<bool> mPending{false};
    Reading mReading;

    // Message thread only: LISTEN's state (the listen request and OnIdle).
    bool mListening = false;
    bool mListenChroma = false;
    music::ListenTracker mListen;
    std::chrono::steady_clock::time_point mListenHeldUntil{};
    mutable std::mutex mMutex;
};
} // namespace guitarfx
