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

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace guitarfx
{

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
    };

    using SendMessageFn = std::function<void(const std::string&)>;

    explicit TunerService(SendMessageFn sendMessage);

    /// Audio thread: records the latest reading, replacing any not yet published.
    void PostReading(const Reading& reading);

    /// Message thread: publishes the latest reading, if there is a new one.
    void OnIdle();

    void SetActive(bool active) { mActive.store(active, std::memory_order_release); }
    [[nodiscard]] bool IsActive() const { return mActive.load(std::memory_order_acquire); }

private:
    SendMessageFn mSendMessage;

    std::atomic<bool> mActive{false};
    std::atomic<bool> mPending{false};
    Reading mReading;
    mutable std::mutex mMutex;
};

} // namespace guitarfx
