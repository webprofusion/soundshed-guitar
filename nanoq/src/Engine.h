#pragma once

// Engine - Soundshed's engine (guitarfx::PluginController) hosted without a framework.
//
// It implements IPluginHost for the controller and owns the thread the controller treats as its
// message thread. QPlug has no handle on the host's main thread that a controller can post to,
// so the engine runs its own: every controller call that is not on the audio thread is made
// there - UI requests, idle work, file dialog results - and everything the controller sends
// to "the UI" is queued for whichever view is open to collect. With no view open the messages
// are dropped, as they are when the JUCE editor is closed; a view that opens asks for the
// full state again (UiClient::Start).

#include "IPluginHost.h"
#include "PluginController.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nanoq
{
class Engine final : public guitarfx::IPluginHost
{
public:
    Engine();
    ~Engine() override;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Valid from construction until destruction. Calls other than ProcessAudio, Prepare and
    /// EnqueueMidi belong on the engine thread: use Post().
    [[nodiscard]] guitarfx::PluginController& Controller()
    {
        return *mController;
    }

    /// Runs `fn` on the engine thread, in order. Any thread.
    void Post(std::function<void()> fn);

    /// Runs `fn` on the engine thread and waits for it. Never call it from the engine thread.
    void Call(const std::function<void()>& fn);

    // ── The view's end ──────────────────────────────────────────────────────────
    /// A view is open (or not). Only while one is does the engine collect messages for it and
    /// run the controller's idle work.
    void SetViewAttached(bool attached);

    /// Takes what the controller has sent since the last call. Any thread.
    [[nodiscard]] std::vector<std::string> TakeMessagesForView();

    /// Passes one JSON request to the controller, on the engine thread.
    void SendRequest(std::string json);

    // ── What the processor reports ──────────────────────────────────────────────
    void SetStreamInfo(double sampleRate, int blockSize, int inputs, int outputs);
    void SetTransport(double tempo, bool playing);

    // ── IPluginHost ─────────────────────────────────────────────────────────────
    void SendMessageToUI(const std::string& jsonMessage) override;
    void BrowseFileAsync(guitarfx::BrowseFileType type, const std::string& title,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override;
    void SaveFileAsync(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override;
    void RunOnMainThread(std::function<void()> fn) override;
    [[nodiscard]] bool IsMessageThread() const override;
    [[nodiscard]] std::filesystem::path GetUserDataPath() const override;
    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override;
    [[nodiscard]] double GetSampleRate() const override;
    [[nodiscard]] int GetBlockSize() const override;
    [[nodiscard]] bool IsStandalone() const override;
    [[nodiscard]] double GetHostTempo() const override;
    [[nodiscard]] bool IsHostPlaying() const override;
    [[nodiscard]] AudioChannelCounts GetAudioChannelCounts() const override;

private:
    void Run(std::promise<void> ready);

    std::unique_ptr<guitarfx::PluginController> mController;
    std::filesystem::path mAssetRoot;

    std::thread mThread;
    std::thread::id mThreadId;
    std::mutex mTaskMutex;
    std::condition_variable mWake;
    std::deque<std::function<void()>> mTasks;
    bool mStopping = false;

    std::mutex mOutboxMutex;
    std::vector<std::string> mOutbox;
    std::atomic<bool> mViewAttached{false};

    std::atomic<double> mSampleRate{48000.0};
    std::atomic<int> mBlockSize{256};
    std::atomic<int> mInputs{2};
    std::atomic<int> mOutputs{2};
    std::atomic<double> mTempo{120.0};
    std::atomic<bool> mPlaying{false};
};
} // namespace nanoq
