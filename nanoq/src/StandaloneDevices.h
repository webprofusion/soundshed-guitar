#pragma once

// StandaloneDevices - the standalone app's audio and MIDI device controls, answered for the
// engine (the `audioDevice` message, docs/user-interface.md "Audio Device").
//
// The devices belong to clap-wrapper's standalone host (RtAudio and RtMidi), which only exists in
// the standalone executable. So the work is split in two:
//
// - DeviceBackend: a thin, JSON-free view of that host. Its one implementation,
//   src/standalone/ClapWrapperDevices.cpp, is compiled into the standalone target alone and
//   registers itself at static initialisation. A CLAP, VST3 or AU build links no backend, so
//   SupportsAudioDeviceSettings() stays false there and nothing standalone-only is referenced.
// - StandaloneDevices (here, in every build): the protocol. It reads requests, drives the backend,
//   and answers with the same `audioDeviceState` snapshot the JUCE standalone sends
//   (juce/source/StandaloneAudioSettings.cpp), so uiclient and any UI written against it work
//   unchanged. It also owns what the audio path needs for the controls clap-wrapper has no
//   equivalent of: the input mute, the input meter and the test tone.
//
// Threads: the engine receives a request on its own thread, but clap-wrapper's host is driven
// from the app's main thread (its own settings window, its restart and shutdown handling all run
// there, and ASIO drivers expect it). So requests are queued and run by Pump(), which the editor's
// UI tick calls on that thread. ProcessInput and ProcessOutput are the audio thread's.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nanoq
{
/// What the device host has and runs at, in one read.
struct DeviceSnapshot
{
    std::vector<std::string> apis; // the drivers built in, by display name
    std::string api;
    bool separateIO = true; // false where input and output must be one device (ASIO)
    std::vector<std::string> inputs, outputs;
    std::string input, output;                 // "" when that side is not used
    int inputChannels = 0, outputChannels = 0; // what the chosen devices have
    bool open = false;                         // a stream is running
    std::vector<int> sampleRates;
    std::vector<int> bufferSizes;
    int sampleRate = 0, bufferSize = 0; // what the stream runs at
    int latency = 0;                    // the stream's, in samples, both directions together
    std::vector<std::string> midiInputs;
    std::vector<bool> midiEnabled; // parallel to midiInputs
};

/// clap-wrapper's standalone host, as the device controls need it. Main thread only. Each setter
/// returns "" or why it failed, and saves the setup when it succeeded.
class DeviceBackend
{
  public:
    virtual ~DeviceBackend() = default;

    [[nodiscard]] virtual DeviceSnapshot Snapshot(bool rescan) = 0;
    virtual std::string SetApi(const std::string& displayName) = 0;
    /// `input` false for the output side; `name` "" for no device.
    virtual std::string SetDevice(bool input, const std::string& name) = 0;
    /// Both sides at once, for a driver that opens them together.
    virtual std::string SetLinkedDevice(const std::string& name) = 0;
    virtual std::string SetSampleRate(int rate) = 0;
    virtual std::string SetBufferSize(int frames) = 0;
    /// Closes the stream and opens it again with the same setup.
    virtual std::string Restart() = 0;
    virtual std::string SetMidiInput(const std::string& name, bool enabled) = 0;

    /// A value kept in the host's settings file beside its own (the input mute), "" if none.
    [[nodiscard]] virtual std::string ReadSetting(const std::string& key) = 0;
    virtual void WriteSetting(const std::string& key, const std::string& value) = 0;
};

/// Called once, at static initialisation, by the standalone executable's backend.
void RegisterDeviceBackend(DeviceBackend* backend);
/// The registered backend, or null in a plugin build.
[[nodiscard]] DeviceBackend* RegisteredDeviceBackend();

class StandaloneDevices
{
  public:
    using Send = std::function<void(const std::string& json)>;

    explicit StandaloneDevices(Send send);

    /// True in the standalone app, where a backend is linked in.
    [[nodiscard]] bool Supported() const;

    /// One `audioDevice` request's JSON. Any thread; it runs at the next Pump.
    void Queue(const std::string& requestJson);

    /// Runs queued requests and sends the level feed while it is leased. Main (UI) thread.
    void Pump();

    // ── The audio thread ────────────────────────────────────────────────────────
    /// Meters the device input, then silences it if the input is muted.
    void ProcessInput(float* const* channels, int numChannels, int frames);
    /// Adds the test tone, while one is playing.
    void ProcessOutput(float* const* channels, int numChannels, int frames, double sampleRate);
    /// A block given up because the DSP lock was held.
    void NoteBlockGivenUp();

  private:
    void Handle(const std::string& requestJson);
    void SendState(const std::string& error, bool rescan);
    void SendLevels();
    void WatchLevels(bool enabled);
    void WriteBlockStats();

    Send mSend;
    DeviceBackend* mBackend;

    std::mutex mQueueMutex;
    std::vector<std::string> mQueue;

    bool mStarted = false;    // the persisted mute has been read (main thread)
    double mLeaseUntil = 0.0; // the level feed's lease, in Now() seconds
    double mNextLevels = 0.0;

    std::atomic<bool> mInputMuted{false};
    std::atomic<bool> mMeterInput{false};
    std::atomic<float> mInputPeak{0.0f};
    std::atomic<std::uint32_t> mToneRequests{0};

    // The test tone: audio thread only.
    std::uint32_t mToneSeen = 0;
    std::int64_t mToneRemaining = 0;
    std::int64_t mToneLength = 0;
    double mTonePhase = 0.0;

    // Block timing, for SOUNDSHED_NANOQ_AUDIOSTATS: a file that gets one line a second telling
    // why audio broke up. A block that runs over its time is DSP load; one that starts late had
    // its thread held off (a priority or a system problem); one given up lost the DSP lock.
    std::string mBlockStatsPath;
    double mNextBlockStats = 0.0;
    std::chrono::steady_clock::time_point mBlockStart{}; // audio thread only
    std::atomic<std::uint32_t> mBlocks{0}, mBlocksOver{0}, mBlocksLate{0}, mBlocksGivenUp{0};
    std::atomic<std::uint32_t> mBlockBudgetUs{0}, mBlockMaxUs{0}, mBlockGapMaxUs{0};
    std::atomic<std::uint64_t> mBlockBusyUs{0}, mBlockTimeUs{0};
};
} // namespace nanoq
