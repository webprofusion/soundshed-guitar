/**
 * UiBackgroundTests.cpp - The player's own background picture for the play page.
 *
 * browseUiBackgroundImage copies the picked image into the profile and answers with the URL the
 * UI draws it from; picking another replaces it, clearUiBackgroundImage removes it, and the
 * editor's `ui-backgrounds/` route serves only that folder's pictures.
 */

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/UiBackgroundService.h"

namespace fs = std::filesystem;

namespace
{
using Service = guitarfx::UiBackgroundService;

/// Answers every file dialog with `picked`, and records what the controller sends the UI.
class PickingHost final : public guitarfx::test::PumpedTestHost
{
  public:
    explicit PickingHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), 48000.0, 512)
    {
    }

    fs::path picked;

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        guitarfx::BrowseFileResult result;
        result.success = !picked.empty();
        result.path = picked;
        callback(result);
    }

    void SendMessageToUI(const std::string& message) override
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] std::optional<nlohmann::json> TakeLastOfType(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);

        for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
        {
            const auto parsed = nlohmann::json::parse(*it, nullptr, false);

            if (parsed.is_object() && parsed.value("type", "") == type)
            {
                mMessages.clear();
                return parsed;
            }
        }

        return std::nullopt;
    }

  private:
    std::mutex mMutex;
    std::vector<std::string> mMessages;
};

fs::path FreshSandbox(const std::string& name)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-ui-background-tests" / name;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    guitarfx::test::SetSettingsEnvRoot(sandbox);
    return sandbox;
}

fs::path WriteFile(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<unsigned char> Jpeg(unsigned char tag)
{
    return {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 2, tag};
}

std::size_t FilesIn(const fs::path& dir)
{
    std::error_code ec;
    std::size_t count = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        count += entry.is_regular_file() ? 1 : 0;
    }

    return count;
}

bool Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "  " << what << "\n";
    }

    return condition;
}

bool TestPickReplaceAndClear()
{
    const auto sandbox = FreshSandbox("pick");
    PickingHost host(sandbox);
    guitarfx::PluginController controller(host);

    host.picked = WriteFile(sandbox / "My Photo.JPEG", Jpeg(1));
    controller.HandleUIMessage(R"({"type":"browseUiBackgroundImage"})");
    const auto first = host.TakeLastOfType("uiBackgroundImageSelected");

    bool ok = Check(first.has_value(), "picking an image did not answer uiBackgroundImageSelected");
    const auto firstUrl = first ? first->value("url", "") : std::string();
    ok &= Check(firstUrl.rfind("ui-backgrounds/", 0) == 0 && firstUrl.size() == 15 + 16 + 4 &&
                    firstUrl.substr(firstUrl.size() - 4) == ".jpg",
                "the URL is not ui-backgrounds/<16 hex>.jpg: " + firstUrl);

    const auto served = Service::ResolveUiResource(firstUrl);
    ok &= Check(served && served->second == "image/jpeg", "the editor's route does not serve the picture");
    ok &= Check(FilesIn(Service::Directory()) == 1, "the folder does not hold exactly the one picture");

    host.picked = WriteFile(sandbox / "other.png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 9});
    controller.HandleUIMessage(R"({"type":"browseUiBackgroundImage"})");
    const auto second = host.TakeLastOfType("uiBackgroundImageSelected");
    const auto secondUrl = second ? second->value("url", "") : std::string();

    ok &= Check(secondUrl.size() > 4 && secondUrl.substr(secondUrl.size() - 4) == ".png", "the second pick's URL");
    ok &= Check(!Service::ResolveUiResource(firstUrl), "the replaced picture is still served");
    ok &= Check(Service::ResolveUiResource(secondUrl).has_value(), "the new picture is not served");
    ok &= Check(FilesIn(Service::Directory()) == 1, "picking another left the old one behind");

    controller.HandleUIMessage(R"({"type":"clearUiBackgroundImage"})");
    ok &= Check(FilesIn(Service::Directory()) == 0, "clearing left a picture behind");
    ok &= Check(!Service::ResolveUiResource(secondUrl), "a cleared picture is still served");
    return ok;
}

bool TestRefusals()
{
    const auto sandbox = FreshSandbox("refusals");
    PickingHost host(sandbox);
    guitarfx::PluginController controller(host);

    host.picked = WriteFile(sandbox / "drawing.svg", {'<', 's', 'v', 'g', '/', '>'});
    controller.HandleUIMessage(R"({"type":"browseUiBackgroundImage"})");
    bool ok = Check(!host.TakeLastOfType("uiBackgroundImageSelected"), "an SVG was taken");

    host.picked.clear();
    controller.HandleUIMessage(R"({"type":"browseUiBackgroundImage"})");
    ok &= Check(!host.TakeLastOfType("uiBackgroundImageSelected"), "a cancelled dialog answered");

    // A file in the folder under a name Import would not give it is never served.
    std::error_code ec;
    fs::create_directories(Service::Directory(), ec);
    WriteFile(Service::Directory() / "0123456789abcdef.JPG", Jpeg(2));
    WriteFile(Service::Directory() / "notes.txt", {'x'});
    ok &= Check(!Service::ResolveUiResource("ui-backgrounds/0123456789abcdef.JPG"), "an upper-case name was served");
    ok &= Check(!Service::ResolveUiResource("ui-backgrounds/notes.txt"), "a non-picture was served");
    ok &= Check(!Service::ResolveUiResource("ui-backgrounds/../settings/app.json"), "a path outside was served");
    ok &= Check(!Service::ResolveUiResource("ui-backgrounds/0123456789abcdef.svg"), "an SVG name was served");
    ok &= Check(!Service::ResolveUiResource("layout-images/0123456789abcdef.jpg"), "another prefix was served");
    return ok;
}
} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        (ok ? passed : failed)++;
    };

    run("A picked picture is stored, served, replaced and cleared", TestPickReplaceAndClear());
    run("Only PNG, JPEG, GIF and WebP pictures are taken or served", TestRefusals());

    std::cout << "\nUI background tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
