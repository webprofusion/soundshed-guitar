#include "Engine.h"

#include "FileDialogs.h"
#include "Paths.h"
#include "UiBridge.h"

#include <chrono>

namespace nanoq
{
namespace
{
constexpr auto kTick = std::chrono::milliseconds(33); // 30 Hz, like the JUCE product's idle timer
constexpr size_t kMaxOutbox = 4096;
} // namespace

Engine::Engine()
{
    ApplyProfileOverride();
    mAssetRoot = guitarfx::ui::ResolveResourceRoot(ResourceCandidates());

    std::promise<void> ready;
    auto future = ready.get_future();
    mThread = std::thread([this, ready = std::move(ready)]() mutable { Run(std::move(ready)); });
    future.wait();
}

Engine::~Engine()
{
    {
        std::lock_guard<std::mutex> lock(mTaskMutex);
        mStopping = true;
    }
    mWake.notify_all();
    if (mThread.joinable())
        mThread.join();
}

void Engine::Run(std::promise<void> ready)
{
    mThreadId = std::this_thread::get_id();

    // The controller is built here so that the thread that made it is the one it calls
    // its message thread.
    mController = std::make_unique<guitarfx::PluginController>(*this);
    mController->Initialize();
    ready.set_value();

    std::unique_lock<std::mutex> lock(mTaskMutex);
    while (!mStopping)
    {
        mWake.wait_for(lock, kTick, [this] { return mStopping || !mTasks.empty(); });

        // Run everything queued, without holding the lock across a task: a task may post.
        while (!mTasks.empty())
        {
            auto task = std::move(mTasks.front());
            mTasks.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }

        if (mStopping)
            break;

        lock.unlock();
        // A footswitch mapped to a setlist preset or scene must work with no view open; the
        // rest of the idle work only runs while a view is there to show it (as in the JUCE
        // product, where a closed editor does not tick).
        mController->DrainControlSurfaceRequests();
        if (mViewAttached.load(std::memory_order_acquire))
            mController->OnIdle();
        lock.lock();
    }
    lock.unlock();

    // Tasks left behind may own state the controller touches; drop them first.
    mTasks.clear();
    mController.reset();
}

void Engine::Post(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(mTaskMutex);
        if (mStopping)
            return;
        mTasks.push_back(std::move(fn));
    }
    mWake.notify_one();
}

void Engine::Call(const std::function<void()>& fn)
{
    if (IsMessageThread())
    {
        fn();
        return;
    }

    std::promise<void> done;
    auto finished = done.get_future();
    Post([&] {
        fn();
        done.set_value();
    });
    finished.wait();
}

void Engine::SetViewAttached(bool attached)
{
    mViewAttached.store(attached, std::memory_order_release);
    if (!attached)
    {
        std::lock_guard<std::mutex> lock(mOutboxMutex);
        mOutbox.clear();
    }
}

std::vector<std::string> Engine::TakeMessagesForView()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mOutboxMutex);
    out.swap(mOutbox);
    return out;
}

void Engine::SendRequest(std::string json)
{
    Post([this, json = std::move(json)] { mController->HandleUIMessage(json); });
}

void Engine::SetStreamInfo(double sampleRate, int blockSize, int inputs, int outputs)
{
    mSampleRate.store(sampleRate);
    mBlockSize.store(blockSize);
    mInputs.store(inputs);
    mOutputs.store(outputs);
}

void Engine::SetTransport(double tempo, bool playing)
{
    mTempo.store(tempo, std::memory_order_relaxed);
    mPlaying.store(playing, std::memory_order_relaxed);
}

// ── IPluginHost ─────────────────────────────────────────────────────────────────

void Engine::SendMessageToUI(const std::string& jsonMessage)
{
    // The controller may send from any thread (the audio thread's telemetry too). With no view
    // there is nobody to read it.
    if (!mViewAttached.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(mOutboxMutex);
    if (mOutbox.size() < kMaxOutbox)
        mOutbox.push_back(jsonMessage);
}

void Engine::BrowseFileAsync(guitarfx::BrowseFileType type, const std::string& title,
                             std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
    // The callback is documented to fire on the message thread: bounce the result onto it.
    ShowOpenDialog(type, title, [this, callback = std::move(callback)](const guitarfx::BrowseFileResult& result) {
        Post([callback, result] { callback(result); });
    });
}

void Engine::SaveFileAsync(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                           std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
    ShowSaveDialog(type, title, defaultName,
                   [this, callback = std::move(callback)](const guitarfx::BrowseFileResult& result) {
                       Post([callback, result] { callback(result); });
                   });
}

void Engine::RunOnMainThread(std::function<void()> fn)
{
    Post(std::move(fn));
}

bool Engine::IsMessageThread() const
{
    return std::this_thread::get_id() == mThreadId;
}

std::filesystem::path Engine::GetUserDataPath() const
{
    return ProfileFolder();
}

std::filesystem::path Engine::GetBundledAssetsPath() const
{
    return mAssetRoot;
}

double Engine::GetSampleRate() const
{
    return mSampleRate.load();
}

int Engine::GetBlockSize() const
{
    return mBlockSize.load();
}

bool Engine::IsStandalone() const
{
    // clap-wrapper's standalone app links the plugin into its own executable, which is what tells
    // it apart from a plugin in a DAW. The controller keeps a standalone's session in the profile,
    // and a plugin's in the host project. Its audio device settings are the wrapper's own window
    // (Alt+Space on Windows).
    static const bool standalone = RunningAsStandalone();
    return standalone;
}

double Engine::GetHostTempo() const
{
    return mTempo.load(std::memory_order_relaxed);
}

bool Engine::IsHostPlaying() const
{
    return mPlaying.load(std::memory_order_relaxed);
}

guitarfx::IPluginHost::AudioChannelCounts Engine::GetAudioChannelCounts() const
{
    return {mInputs.load(), mOutputs.load()};
}
} // namespace nanoq
