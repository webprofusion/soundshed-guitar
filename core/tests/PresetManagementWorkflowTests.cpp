#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
public:
    explicit TestHost(fs::path userDataPath)
        : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        sentMessages.push_back(jsonMessage);
    }

    void BrowseFileAsync(guitarfx::BrowseFileType,
                         const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType,
                       const std::string&,
                       const std::string&,
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
        return mUserDataPath;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    std::vector<std::string> sentMessages;

private:
    fs::path mUserDataPath;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

guitarfx::Preset BuildPreset(const std::string& id, const std::string& name)
{
    using namespace guitarfx;

    Preset preset;
    preset.id = id;
    preset.name = name;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;

    GraphNode amp;
    amp.id = "amp";
    amp.type = "amp_nam";
    amp.enabled = true;
    amp.params["drive"] = 0.42;
    amp.resources.push_back(ResourceRef{"nam", "test-nam", fs::path{}, ""});

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, amp, out};
    preset.graph.edges = {
        {"in", "amp", 0, 0, 1.0},
        {"amp", "out", 0, 0, 1.0},
    };

    return preset;
}

std::optional<nlohmann::json> FindLatestMessageOfType(const std::vector<std::string>& messages,
                                                       const std::string& type)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        try
        {
            const auto payload = nlohmann::json::parse(*it);
            if (payload.value("type", "") == type)
            {
                return payload;
            }
        }
        catch (...)
        {
        }
    }

    return std::nullopt;
}

bool TestLoadPresetViaMessage()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "load";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto preset = BuildPreset("p-load", "Load Me");

    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    message["presetId"] = preset.id;
    controller.HandleUIMessage(message.dump());

    const auto& active = controller.GetActivePreset();
    if (!active)
    {
        std::cerr << "No active preset after loadPreset\n";
        return false;
    }

    if (active->id != preset.id || active->name != preset.name)
    {
        std::cerr << "Loaded preset metadata mismatch\n";
        return false;
    }

    const auto loadedMsg = FindLatestMessageOfType(host.sentMessages, "presetLoaded");
    if (!loadedMsg)
    {
        std::cerr << "presetLoaded message not emitted\n";
        return false;
    }

    return true;
}

bool TestSaveGetDeletePresetWorkflow()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "save-delete";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto preset = BuildPreset("p-save", "Saved Preset");

    const std::string saveId = "unit-test-preset";
    nlohmann::json save;
    save["type"] = "savePreset";
    save["name"] = "Saved Preset";
    save["category"] = "Unit";
    save["description"] = "Preset management workflow test";
    save["presetId"] = saveId;
    save["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    controller.HandleUIMessage(save.dump());

    const fs::path savedPath = sandbox / "presets" / "user" / (saveId + ".json");
    if (!fs::exists(savedPath))
    {
        std::cerr << "Saved preset file missing: " << savedPath.string() << "\n";
        return false;
    }

    const auto fromFile = guitarfx::PresetStorage::LoadFromFile(savedPath);
    if (!fromFile || fromFile->id != saveId || fromFile->name != "Saved Preset")
    {
        std::cerr << "Saved preset file contents mismatch\n";
        return false;
    }

    nlohmann::json get;
    get["type"] = "getPresetById";
    get["presetId"] = saveId;
    controller.HandleUIMessage(get.dump());

    const auto presetDataMsg = FindLatestMessageOfType(host.sentMessages, "presetData");
    if (!presetDataMsg || !presetDataMsg->contains("preset"))
    {
        std::cerr << "presetData response missing after getPresetById\n";
        return false;
    }

    const auto returnedPreset = (*presetDataMsg)["preset"];
    if (returnedPreset.value("id", "") != saveId)
    {
        std::cerr << "getPresetById returned wrong preset id\n";
        return false;
    }

    nlohmann::json remove;
    remove["type"] = "deletePreset";
    remove["presetId"] = saveId;
    controller.HandleUIMessage(remove.dump());

    if (fs::exists(savedPath))
    {
        std::cerr << "Preset file still exists after deletePreset\n";
        return false;
    }

    return true;
}

} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (ok) ++passed; else ++failed;
    };

    run("Load preset via message", TestLoadPresetViaMessage());
    run("Save/Get/Delete preset workflow", TestSaveGetDeletePresetWorkflow());

    std::cout << "\nPreset management workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
