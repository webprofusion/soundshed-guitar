/**
 * @file SepiaUiReplay.cpp
 * @brief Answers a list of UI messages with a real engine, for looking at the web UI in a browser.
 *
 * The web UI is plain HTML and TypeScript, and opens in any Chromium. On its own it is empty:
 * it has no effect catalog, no presets, no state. This runs the messages a UI would send (a JSON
 * array of message objects) through a headless controller that has the factory pack bundled, and
 * writes every message the engine sends back, in order, as a JSON array of strings. Replaying
 * those through window.IPlugReceiveData shows the UI as the app would, which is how a change of
 * theme or layout is checked without building the JUCE app (see tools/sepia-ui/shot.js).
 *
 *   SepiaUiReplay <requests.json> <responses.json> [presetId]
 *   SepiaUiReplay --serve [presetId]
 *
 * --serve keeps the engine up and talks over the standard streams, so the page is live and can be
 * clicked through: each line read is one UI message, and each message the engine sends back is
 * written as one line. A line "." is written when a request has been fully answered.
 *
 * With no arguments it only checks that a controller starts and answers, which is what ctest runs.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AudioSnapshotSupport.h"
#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/effects/BuiltinEffects.h"

namespace
{
namespace fs = std::filesystem;
using nlohmann::json;

class RecordingHost final : public guitarfx::IPluginHost
{
  public:
    explicit RecordingHost(fs::path profile) : mProfile(std::move(profile))
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        sent.push_back(message);
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mProfile;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mProfile / "bundled";
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 256;
    }

    [[nodiscard]] double GetHostTempo() const override
    {
        return 120.0;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    std::vector<std::string> sent;

  private:
    fs::path mProfile;
};
} // namespace

int main(int argc, char** argv)
{
    guitarfx::RegisterAllEffects();

    const fs::path profile = fs::temp_directory_path() / "sepia-ui-replay";
    std::error_code ec;
    fs::remove_all(profile, ec);
    const fs::path factory = profile / "bundled" / "ui" / "presets" / "factory";
    fs::create_directories(factory, ec);
    const fs::path uiRoot = fs::path(GUITARFX_DEMO_AUDIO_DIR) / "..";

    for (const auto& entry : fs::directory_iterator(uiRoot / "presets" / "factory", ec))
    {
        fs::copy_file(entry.path(), factory / entry.path().filename(), fs::copy_options::overwrite_existing, ec);
    }

    audio_snapshot::SetSettingsEnvRoot(profile);
    json requests = json::array();

    if (argc > 1 && std::string(argv[1]) != "--serve")
    {
        std::ifstream in(argv[1], std::ios::binary);
        requests = json::parse(in, nullptr, false);
    }

    int failures = 0;
    const bool serve = argc > 1 && std::string(argv[1]) == "--serve";

    if (serve)
    {
        RecordingHost host(profile);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(48000.0, 256);
        controller.OnIdle();
        controller.HandleUIMessage(
            json{{"type", "loadPreset"}, {"presetId", argc > 2 ? argv[2] : "sepia-warm-clean-room"}}.dump());
        controller.OnIdle();

        const auto flush = [&host] {
            for (const auto& message : host.sent)
            {
                std::cout << message << "\n";
            }

            host.sent.clear();
            std::cout << "." << std::endl;
        };

        flush();
        std::string line;

        while (std::getline(std::cin, line))
        {
            if (!line.empty())
            {
                controller.HandleUIMessage(line);
                controller.OnIdle();
            }

            flush();
        }

        fs::remove_all(profile, ec);
        return 0;
    }

    {
        RecordingHost host(profile);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(48000.0, 256);
        controller.OnIdle();

        if (requests.is_array())
        {
            for (const auto& request : requests)
            {
                controller.HandleUIMessage(request.dump());
                controller.OnIdle();
            }
        }

        controller.HandleUIMessage(
            json{{"type", "loadPreset"}, {"presetId", argc > 3 ? argv[3] : "sepia-warm-clean-room"}}.dump());
        controller.OnIdle();
        controller.HandleUIMessage(json{{"type", "getEffectCatalog"}}.dump());
        controller.OnIdle();

        if (host.sent.empty())
        {
            std::cout << "  [FAIL] the controller sent the UI nothing" << std::endl;
            ++failures;
        }

        if (argc > 2)
        {
            std::ofstream out(argv[2], std::ios::binary);
            out << json(host.sent).dump() << std::endl;
        }

        std::cout << host.sent.size() << " messages for the UI" << std::endl;
    }

    fs::remove_all(profile, ec);
    return failures == 0 ? 0 : 1;
}
