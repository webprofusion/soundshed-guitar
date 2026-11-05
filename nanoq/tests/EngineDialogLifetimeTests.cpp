// A file dialog that outlives the engine (Engine.cpp, FileDialogs.cpp).
//
// A native dialog runs on a thread of its own, which the engine cannot wait for: it is still up
// when the host removes the plugin, or closes the project, with the dialog open. The dialog's
// result then reaches an engine that has gone. It goes through a handle the engine empties as it
// goes, so the result is dropped and the controller's callback never runs.

#include "Engine.h"
#include "FileDialogs.h"

#include "util/FileSystem.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace
{
int gFailures = 0;

void Fail(const std::string& message)
{
    std::cerr << "[EngineDialogLifetimeTests] " << message << std::endl;
    ++gFailures;
}

std::filesystem::path ScratchRoot()
{
    std::random_device device;
    return std::filesystem::temp_directory_path() / ("nanoq-engine-dialog-tests-" + std::to_string(device()));
}
} // namespace

#if !defined(_WIN32) && !defined(__APPLE__)
// With no native dialog the view draws the picker: the engine leaves it the request, and a dialog
// with no view to draw it, or whose view goes, is a cancel, so what asked is never left waiting.
void TestDialogDrawnByTheView()
{
    using guitarfx::BrowseFileResult;
    using guitarfx::BrowseFileType;

    struct Outcome
    {
        std::atomic<int> calls{0};
        std::atomic<bool> success{false};
    };

    const auto ask = [](nanoq::Engine& engine, Outcome& outcome) {
        engine.BrowseFileAsync(BrowseFileType::IRFile, "Choose an IR", [&outcome](const BrowseFileResult& result) {
            outcome.success = result.success;
            ++outcome.calls;
        });
    };
    const auto settled = [](const Outcome& outcome) {
        for (int i = 0; i < 500 && outcome.calls == 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return outcome.calls == 1;
    };

    nanoq::Engine engine;

    Outcome noView;
    ask(engine, noView);

    if (!settled(noView) || noView.success)
    {
        Fail("a dialog with no view open should be a cancel");
    }

    engine.SetViewAttached(true);
    Outcome answered;
    ask(engine, answered);
    auto request = engine.TakeFileDialogRequest();

    if (!request || request->type != BrowseFileType::IRFile || request->title != "Choose an IR" || request->save)
    {
        Fail("the view should be left the request");
        return;
    }

    BrowseFileResult result;
    result.success = true;
    request->done(result);

    if (!settled(answered) || !answered.success)
    {
        Fail("the view's answer should reach the caller");
    }

    Outcome saving;
    engine.SaveFileAsync(BrowseFileType::AudioFile, "Export", "take.wav", [&saving](const BrowseFileResult& r) {
        saving.success = r.success;
        ++saving.calls;
    });
    request = engine.TakeFileDialogRequest();

    if (!request || !request->save || request->defaultName != "take.wav")
    {
        Fail("a save should reach the view as a save");
        return;
    }

    request->done({});

    if (!settled(saving) || saving.success)
    {
        Fail("a cancelled save should say so");
    }

    Outcome orphaned;
    ask(engine, orphaned);
    engine.SetViewAttached(false);

    if (!settled(orphaned) || orphaned.success)
    {
        Fail("a request the view never took should be cancelled when the view goes");
    }

    if (engine.TakeFileDialogRequest())
    {
        Fail("the cancelled request should not still be waiting");
    }
}
#endif

int main()
{
    // A data root of its own: a test never touches the real profile.
    const auto root = ScratchRoot();
    std::filesystem::create_directories(root);
    guitarfx::FileSystem::SetPlatformRootOverride(root);

    // The stand-in dialog keeps `done` for later, as a dialog left open does.
    std::mutex mutex;
    std::condition_variable shown;
    std::function<void(const guitarfx::BrowseFileResult&)> dialogDone;
    nanoq::SetFileDialogHookForTesting([&](bool, guitarfx::BrowseFileType, const std::string&, const std::string&,
                                           std::function<void(const guitarfx::BrowseFileResult&)> done) {
        std::lock_guard<std::mutex> lock(mutex);
        dialogDone = std::move(done);
        shown.notify_all();
    });

    std::atomic<bool> callbackRan{false};
    {
        auto engine = std::make_unique<nanoq::Engine>();
        engine->BrowseFileAsync(guitarfx::BrowseFileType::NAMModel, "Choose a model",
                                [&callbackRan](const guitarfx::BrowseFileResult&) { callbackRan = true; });

        std::unique_lock<std::mutex> lock(mutex);

        if (!shown.wait_for(lock, std::chrono::seconds(5), [&] { return dialogDone != nullptr; }))
        {
            Fail("the dialog was never shown");
            return 1;
        }

        // The host removes the plugin with the dialog still up.
    }

    // The player then picks a file: the dialog's thread reports it to an engine that has gone.
    std::thread dialogThread([&] {
        guitarfx::BrowseFileResult result;
        result.success = true;
        result.path = root / "model.nam";
        dialogDone(result);
    });
    dialogThread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (callbackRan)
    {
        Fail("the controller's callback ran after the engine was destroyed");
    }

    nanoq::SetFileDialogHookForTesting(nullptr);
#if !defined(_WIN32) && !defined(__APPLE__)
    TestDialogDrawnByTheView();
#endif
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    if (gFailures == 0)
    {
        std::cout << "[EngineDialogLifetimeTests] passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
