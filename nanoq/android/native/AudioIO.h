#pragma once

// The Android app's audio: Oboe (AAudio on the API levels the app supports), a low-latency
// output stream in Float stereo and an input stream at its rate, run as one full-duplex pair.
// Each data callback hands the input and output to the ClapHost on Oboe's real-time thread, in
// blocks of at most the size the plugin was activated for.
//
// The plugin's activation follows the streams: Start activates it at the output's rate, and a
// disconnected stream (a USB interface plugged in or out, headphones, another app taking the
// device) is closed by Oboe and reopened from the main thread at the next Poll. Main thread
// only, apart from the data callback.
//
// The input is captured with the Unprocessed preset: no AGC, noise suppression or band
// limiting in front of the amp, as Soundshed Guitar asks (android/oboe-overlay).
//
// See docs/plans/nano-q-android.md, "Audio".

#include "AndroidAudioThread.h"

#include <oboe/Oboe.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace nanoq::android
{
class ClapHost;

class AudioIO : public oboe::FullDuplexStream, public oboe::AudioStreamErrorCallback
{
public:
    explicit AudioIO(ClapHost& host);
    ~AudioIO() override;

    AudioIO(const AudioIO&) = delete;
    AudioIO& operator=(const AudioIO&) = delete;

    /// Opens and starts the streams; `withInput` false (no microphone permission) plays with a
    /// silent input. False if the output could not be opened.
    bool Start(bool withInput);
    void Stop();
    /// Reopens running streams as they were opened (after the MMAP rule changes).
    void Restart();
    bool Running() const
    {
        return mRunning;
    }

    /// Main thread, once a display frame: reopens disconnected streams, and logs, a few seconds
    /// after a start, whether the callbacks are coming (a stream can start without error and
    /// never call back).
    void Poll(double nowSeconds);

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override;
    oboe::DataCallbackResult onBothStreamsReady(const void* inputData, int numInputFrames, void* outputData,
                                                int numOutputFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    void CloseStreams();

    ClapHost& mHost;
    std::shared_ptr<oboe::AudioStream> mOutput;
    std::shared_ptr<oboe::AudioStream> mInput;
    bool mRunning = false;
    bool mWithInput = false;
    int32_t mInputChannels = 0;

    // The block the plugin is activated for: the output's burst, rounded up to a power of two
    // (at least 64). A callback that brings more is processed in blocks of this size.
    uint32_t mBlock = 0;
    double mSampleRate = 0;

    // Deinterleaved input and output for one block, allocated before the streams start.
    std::vector<float> mIn[2];
    std::vector<float> mOut[2];

    // Set by Oboe's error thread when a stream was disconnected; acted on in Poll.
    std::atomic<bool> mRestart{false};

    // The start-up check: callbacks and peaks since the streams last started.
    double mStartedAt = -1;
    bool mChecked = false;
    std::atomic<uint64_t> mCallbacks{0};
    std::atomic<float> mInPeak{0.0f};
    std::atomic<float> mOutPeak{0.0f};

    // Which cores the callback runs on (shared with Soundshed Guitar's Android app).
    soundshed::android_audio::AudioThreadPlacement mPlacement;

    // DSP load, reported every 10 s by Poll: time spent in callbacks against the time their
    // frames last, and the worst single callback against its own frames.
    std::atomic<uint64_t> mBusyNanos{0};
    std::atomic<uint64_t> mLoadFrames{0};
    std::atomic<float> mWorstLoad{0.0f};
    double mLoadReportAt = -1;
};
} // namespace nanoq::android
