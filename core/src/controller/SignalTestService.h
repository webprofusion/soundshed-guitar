#pragma once

// SignalTestService — injects a sine tone at the head of the chain and
// measures what comes out, so a user can prove the signal path is actually
// carrying audio.
//
// The test replaces the guitar input for its duration, so it must never run
// while anything else is driving the input: DemoPreviewService checks
// IsActive() before starting, which is why the flag is exposed.
//
// Threading. Start() is called on the message thread; InjectInput() and
// CollectOutput() run on the audio thread and do no allocation, locking or
// I/O. The handoff is two atomics: the audio thread clears mActive and raises
// mResultPending when the tone has played out, and OnIdle() on the message
// thread is the only reader of the accumulated state after that point. Nothing
// touches mState concurrently, because mActive gates the audio-thread writes
// and mResultPending gates the message-thread read.

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace guitarfx
{
class SignalTestService
{
  public:
    struct Result
    {
        double sampleRate = 0.0;
        double frequencyHz = 0.0;
        double durationSeconds = 0.0;
        double elapsedSeconds = 0.0;
        double inputRMS = 0.0;
        std::array<double, 2> outputRMS{0.0, 0.0};
        bool passed = false;
    };

    using SendMessageFn = std::function<void(const std::string&)>;

    explicit SignalTestService(SendMessageFn sendMessage);

    /// Begins a test at `sampleRate`. Returns false (and starts nothing) if the
    /// host has no valid sample rate yet.
    bool Start(double frequencyHz, double durationSeconds, double sampleRate);

    /// Audio thread: overwrites `inputs` with the test tone while one is running.
    void InjectInput(float** inputs, int numSamples);

    /// Audio thread: accumulates output energy. Runs for one block past the end
    /// of the tone so the tail of the chain is measured too.
    void CollectOutput(float* const* outputs, int numSamples);

    /// Message thread: publishes the result once the tone has finished.
    void OnIdle();

    [[nodiscard]] bool IsActive() const
    {
        return mActive.load(std::memory_order_acquire);
    }

    /// DemoPreviewService refuses to start while a test is running, and needs
    /// the flag itself rather than a snapshot of it.
    [[nodiscard]] std::atomic<bool>& ActiveFlag()
    {
        return mActive;
    }

  private:
    struct RuntimeState
    {
        double frequencyHz = 0.0;
        double phase = 0.0;
        double phaseIncrement = 0.0;
        int samplesRemaining = 0;
        int totalSamples = 0;
        double sampleRate = 0.0;
        double inputSumSquares = 0.0;
        std::array<double, 2> outputSumSquares{0.0, 0.0};
        std::chrono::steady_clock::time_point startTime;
    };

    SendMessageFn mSendMessage;

    std::atomic<bool> mActive{false};
    std::atomic<bool> mResultPending{false};
    RuntimeState mState;
    Result mResult;
};
} // namespace guitarfx
