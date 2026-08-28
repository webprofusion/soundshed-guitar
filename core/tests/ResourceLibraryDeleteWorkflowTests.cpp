#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        messages.push_back(jsonMessage);
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
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    std::vector<std::string> messages;

  private:
    fs::path mRoot;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

std::optional<nlohmann::json> FindLastMessageOfType(const std::vector<std::string>& messages, const std::string& type)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        try
        {
            const auto parsed = nlohmann::json::parse(*it);

            if (parsed.value("type", "") == type)
            {
                return parsed;
            }
        }
        catch (const std::exception&)
        {
        }
    }

    return std::nullopt;
}

/// The resource index lives in the document store now, so "is this resource
/// persisted?" is a store lookup rather than a scan of resources-index.json.
/// Opens its own connection each call — WAL allows a second reader alongside
/// the controller's handle.
bool LibraryIndexContains(const fs::path& sandbox, const std::string& resourceType, const std::string& resourceId)
{
    guitarfx::storage::JsonStore store;
    std::string error;

    if (!store.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error))
    {
        std::cerr << "Could not open the document store: " << error << "\n";
        return false;
    }

    return store.Has(guitarfx::storage::ItemType::kResource,
                     guitarfx::ResourceLibrary::MakeStoreId(resourceType, resourceId));
}

guitarfx::Preset BuildSingleNodeResourcePreset(const std::string& nodeId, const std::string& resourceType,
                                               const std::string& resourceId)
{
    using namespace guitarfx;

    Preset preset;
    preset.id = "resource-delete-test-preset";
    preset.name = "Resource Delete Test";

    GraphNode input;
    input.id = "in";
    input.type = kNodeTypeInput;

    GraphNode effect;
    effect.id = nodeId;
    effect.type = "utility";
    effect.enabled = true;
    effect.resources.resize(1);
    effect.resources[0].resourceType = resourceType;
    effect.resources[0].resourceId = resourceId;

    GraphNode output;
    output.id = "out";
    output.type = kNodeTypeOutput;

    preset.graph.nodes = {input, effect, output};
    preset.graph.edges = {
        {"in", nodeId, 0, 0, 1.0},
        {nodeId, "out", 0, 0, 1.0},
    };

    return preset;
}

bool LoadPreset(guitarfx::PluginController& controller, const guitarfx::Preset& preset)
{
    const auto presetJson = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = presetJson;
    controller.HandleUIMessage(message.dump());
    return controller.GetActivePreset().has_value();
}

struct SavedResourceInfo
{
    std::string type;
    std::string id;
    fs::path filePath;
};

std::optional<SavedResourceInfo> SaveLocalResource(guitarfx::PluginController& controller, TestHost& host,
                                                   const nlohmann::json& payload)
{
    host.messages.clear();
    controller.HandleUIMessage(payload.dump());

    const auto importedMessage = FindLastMessageOfType(host.messages, "resourceImported");

    if (!importedMessage)
    {
        return std::nullopt;
    }

    SavedResourceInfo info;
    info.type = importedMessage->value("resourceType", "");
    info.id = importedMessage->value("id", "");
    info.filePath = importedMessage->value("filePath", "");

    if (info.type.empty() || info.id.empty() || info.filePath.empty())
    {
        return std::nullopt;
    }

    return info;
}

bool DeleteResourceAndExpectRemoved(guitarfx::PluginController& controller, TestHost& host,
                                    const std::string& resourceType, const std::string& resourceId)
{
    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "deleteLibraryResource"},
        {"resourceType", resourceType},
        {"resourceId", resourceId},
    }
                                   .dump());

    const auto removedMessage = FindLastMessageOfType(host.messages, "resourceRemoved");
    return removedMessage && removedMessage->value("resourceType", "") == resourceType &&
           removedMessage->value("id", "") == resourceId;
}

bool TestDeleteStoredResourceRemovesFileAndIndex()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-resource-delete-tests" / "stored-resource";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    const auto saved = SaveLocalResource(controller, host,
                                         nlohmann::json{
                                             {"type", "saveLocalLibraryResource"},
                                             {"resourceType", "wasm"},
                                             {"name", "Stored Resource"},
                                             {"fileName", "stored-resource.wasm"},
                                             {"data", "AQID"},
                                         });

    if (!saved)
    {
        std::cerr << "Failed to save stored local resource\n";
        return false;
    }

    if (!fs::exists(saved->filePath) || !LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "Stored resource was not persisted before delete\n";
        return false;
    }

    if (!DeleteResourceAndExpectRemoved(controller, host, saved->type, saved->id))
    {
        std::cerr << "Stored resource delete did not emit resourceRemoved\n";
        return false;
    }

    if (fs::exists(saved->filePath))
    {
        std::cerr << "Stored app-data resource file still exists after delete\n";
        return false;
    }

    if (LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "Stored resource still present in library index after delete\n";
        return false;
    }

    return true;
}

bool TestDeleteExternalResourceKeepsFileButRemovesIndex()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-resource-delete-tests" / "external-resource";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "external", ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path externalFile = sandbox / "external" / "external-resource.wasm";
    {
        std::ofstream file(externalFile, std::ios::binary);
        file.write("\x01\x02\x03\x04", 4);
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    const auto saved = SaveLocalResource(controller, host,
                                         nlohmann::json{
                                             {"type", "saveLocalLibraryResource"},
                                             {"resourceType", "wasm"},
                                             {"name", "External Resource"},
                                             {"filePath", externalFile.string()},
                                         });

    if (!saved)
    {
        std::cerr << "Failed to index external resource\n";
        return false;
    }

    if (!LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "External resource was not added to library index before delete\n";
        return false;
    }

    if (!DeleteResourceAndExpectRemoved(controller, host, saved->type, saved->id))
    {
        std::cerr << "External resource delete did not emit resourceRemoved\n";
        return false;
    }

    if (!fs::exists(externalFile))
    {
        std::cerr << "External file should remain on disk after library delete\n";
        return false;
    }

    if (LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "External resource still present in library index after delete\n";
        return false;
    }

    return true;
}

bool TestDeleteInUseResourceIsRefused()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-resource-delete-tests" / "in-use-resource";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    const auto saved = SaveLocalResource(controller, host,
                                         nlohmann::json{
                                             {"type", "saveLocalLibraryResource"},
                                             {"resourceType", "wasm"},
                                             {"name", "In Use Resource"},
                                             {"fileName", "in-use-resource.wasm"},
                                             {"data", "AQIDBA=="},
                                         });

    if (!saved)
    {
        std::cerr << "Failed to save in-use resource\n";
        return false;
    }

    if (!LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "In-use resource was not added to library index before delete\n";
        return false;
    }

    if (!LoadPreset(controller, BuildSingleNodeResourcePreset("resource-node", saved->type, saved->id)))
    {
        std::cerr << "Failed to load preset for in-use delete test\n";
        return false;
    }

    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "deleteLibraryResource"},
        {"resourceType", saved->type},
        {"resourceId", saved->id},
    }
                                   .dump());

    const auto failedMessage = FindLastMessageOfType(host.messages, "resourceDeleteFailed");

    if (!failedMessage)
    {
        std::cerr << "Delete in-use resource did not emit resourceDeleteFailed\n";
        return false;
    }

    if (failedMessage->value("message", "") != "Resource is in use")
    {
        std::cerr << "Unexpected delete failure message for in-use resource\n";
        return false;
    }

    if (!LibraryIndexContains(sandbox, saved->type, saved->id))
    {
        std::cerr << "In-use resource should remain in library index after refused delete\n";
        return false;
    }

    if (!fs::exists(saved->filePath))
    {
        std::cerr << "In-use resource file should remain after refused delete\n";
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

        if (ok)
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    run("Delete stored resource removes file and index", TestDeleteStoredResourceRemovesFileAndIndex());
    run("Delete external resource keeps file and removes index", TestDeleteExternalResourceKeepsFileButRemovesIndex());
    run("Delete in-use resource is refused", TestDeleteInUseResourceIsRefused());

    std::cout << "\nResource library delete workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
