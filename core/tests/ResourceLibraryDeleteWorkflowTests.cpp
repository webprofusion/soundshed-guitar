#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "presets/PresetTypesJson.h"

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

/// A profile of its own per run, removed when the guard goes. Declare the guard before the
/// controller so the controller has closed the store by the time the folder is deleted.
struct SandboxGuard
{
    fs::path root;

    ~SandboxGuard()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

fs::path MakeCleanupSandbox(const std::string& name)
{
    // Per run, so a concurrent run of this suite cannot delete the profile from under this one.
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-resource-cleanup-tests-" + run) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    SetSettingsEnvRoot(root);
    return root;
}

std::optional<nlohmann::json> CleanupResources(guitarfx::PluginController& controller, TestHost& host,
                                               const std::vector<SavedResourceInfo>& resources)
{
    nlohmann::json list = nlohmann::json::array();

    for (const auto& resource : resources)
    {
        list.push_back({{"type", resource.type}, {"id", resource.id}});
    }

    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "cleanupResourceLibrary"},
        {"scope", "all"},
        {"removeFiles", true},
        {"resources", list},
    }
                                   .dump());
    return FindLastMessageOfType(host.messages, "resourceCleanupResult");
}

std::optional<SavedResourceInfo> SaveStoredResource(guitarfx::PluginController& controller, TestHost& host,
                                                    const std::string& name, const std::string& data)
{
    return SaveLocalResource(controller, host,
                             nlohmann::json{
                                 {"type", "saveLocalLibraryResource"},
                                 {"resourceType", "wasm"},
                                 {"name", name},
                                 {"fileName", name + ".wasm"},
                                 {"data", data},
                             });
}

bool TestCleanupRemovalPersistsAcrossRestart()
{
    const fs::path sandbox = MakeCleanupSandbox("restart");
    const SandboxGuard guard{sandbox};
    std::error_code ec;
    fs::create_directories(sandbox / "external", ec);

    const fs::path externalFile = sandbox / "external" / "cleanup-external.wasm";
    {
        std::ofstream file(externalFile, std::ios::binary);
        file.write("\x05\x06\x07\x08", 4);
    }

    std::optional<SavedResourceInfo> stored;
    std::optional<SavedResourceInfo> external;
    std::optional<SavedResourceInfo> kept;

    {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        stored = SaveStoredResource(controller, host, "cleanup-stored", "AQID");
        // Added since the store migration, so the legacy index never heard of it.
        external = SaveLocalResource(controller, host,
                                     nlohmann::json{
                                         {"type", "saveLocalLibraryResource"},
                                         {"resourceType", "wasm"},
                                         {"name", "Cleanup External"},
                                         {"filePath", externalFile.string()},
                                     });
        kept = SaveStoredResource(controller, host, "cleanup-kept", "AQIDBA==");

        if (!stored || !external || !kept)
        {
            std::cerr << "Failed to save the cleanup restart resources\n";
            return false;
        }

        const auto result = CleanupResources(controller, host, {*stored, *external});

        if (!result || result->value("removed", 0) != 2)
        {
            std::cerr << "Cleanup should remove the stored and external entries, got "
                      << (result ? result->dump() : std::string{"no reply"}) << "\n";
            return false;
        }
    }

    if (fs::exists(stored->filePath))
    {
        std::cerr << "Cleaned-up resource's file in the content folder should be deleted\n";
        return false;
    }

    if (!fs::exists(externalFile))
    {
        std::cerr << "Cleaned-up external entry's file, outside the content folder, should remain\n";
        return false;
    }

    if (LibraryIndexContains(sandbox, stored->type, stored->id) ||
        LibraryIndexContains(sandbox, external->type, external->id))
    {
        std::cerr << "Cleaned-up resources are still rows in the document store\n";
        return false;
    }

    // The library is read from the store alone at startup.
    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto& library = controller.GetResourceLibrary();

    if (!library.HasResource(kept->type, kept->id))
    {
        std::cerr << "Resource left out of the cleanup is missing after restart, so the restart proves nothing\n";
        return false;
    }

    if (library.HasResource(stored->type, stored->id) || library.HasResource(external->type, external->id))
    {
        std::cerr << "Cleaned-up resource reappeared after restart\n";
        return false;
    }

    return true;
}

bool TestCleanupKeepsResourceUsedOnlyInLaterScene()
{
    using namespace guitarfx;

    const fs::path sandbox = MakeCleanupSandbox("later-scene");
    const SandboxGuard guard{sandbox};

    TestHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();

    const auto sceneOnly = SaveStoredResource(controller, host, "cleanup-scene-two", "AQID");
    const auto unused = SaveStoredResource(controller, host, "cleanup-unused", "AQIDBA==");

    if (!sceneOnly || !unused)
    {
        std::cerr << "Failed to save the later-scene cleanup resources\n";
        return false;
    }

    // Scene 1, which is also the working graph, has no resource; scene 2 plays sceneOnly.
    Preset preset = BuildSingleNodeResourcePreset("scene-node", sceneOnly->type, sceneOnly->id);
    preset.id = "cleanup-later-scene-preset";
    preset.name = "Cleanup Later Scene";

    PresetScene sceneOne{"scene-1", "Scene 1", preset.graph};
    sceneOne.graph.nodes[1].resources.clear();
    const PresetScene sceneTwo{"scene-2", "Scene 2", preset.graph};
    preset.graph = sceneOne.graph;
    preset.scenes = {sceneOne, sceneTwo};

    // Written through a connection of its own, so the preset is only in storage: never loaded,
    // never in a mixer slot, and found only by reading the stored presets' scenes.
    {
        storage::JsonStore store;
        std::string error;

        if (!store.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error) ||
            !PresetStorage::SaveToStore(store, preset))
        {
            std::cerr << "Could not store the later-scene preset: " << error << "\n";
            return false;
        }
    }

    const auto result = CleanupResources(controller, host, {*sceneOnly, *unused});

    if (!result || result->value("removed", 0) != 1 || result->value("skippedUsed", 0) != 1)
    {
        std::cerr << "Cleanup should remove the unused resource and skip the scene-2 one as in use, got "
                  << (result ? result->dump() : std::string{"no reply"}) << "\n";
        return false;
    }

    if (!fs::exists(sceneOnly->filePath) || !LibraryIndexContains(sandbox, sceneOnly->type, sceneOnly->id) ||
        !controller.GetResourceLibrary().HasResource(sceneOnly->type, sceneOnly->id))
    {
        std::cerr << "Resource used only in scene 2 lost its file or entry\n";
        return false;
    }

    if (fs::exists(unused->filePath) || LibraryIndexContains(sandbox, unused->type, unused->id))
    {
        std::cerr << "Unused resource's file or store row survived the cleanup\n";
        return false;
    }

    return true;
}

std::optional<nlohmann::json> QueryResourceUsage(guitarfx::PluginController& controller, TestHost& host,
                                                 const SavedResourceInfo& resource)
{
    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "queryResourceUsage"},
        {"resourceType", resource.type},
        {"resourceId", resource.id},
    }
                                   .dump());
    return FindLastMessageOfType(host.messages, "resourceUsageInfo");
}

/// The usage query names the same user a refused delete would.
bool ExpectUsageReported(guitarfx::PluginController& controller, TestHost& host, const SavedResourceInfo& resource,
                         const std::string& kind, const std::string& detail)
{
    const auto usage = QueryResourceUsage(controller, host, resource);

    if (!usage || !usage->value("inUse", false) || usage->value("usageKind", "") != kind ||
        usage->value("detail", "") != detail)
    {
        std::cerr << "Usage query should report \"" << detail << "\", got "
                  << (usage ? usage->dump() : std::string{"no reply"}) << "\n";
        return false;
    }

    return true;
}

/// Refused as in use, naming `detail`, with the file and the store row left alone.
bool ExpectDeleteRefused(guitarfx::PluginController& controller, TestHost& host, const fs::path& sandbox,
                         const SavedResourceInfo& resource, const std::string& detail)
{
    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "deleteLibraryResource"},
        {"resourceType", resource.type},
        {"resourceId", resource.id},
    }
                                   .dump());

    const auto failed = FindLastMessageOfType(host.messages, "resourceDeleteFailed");

    if (!failed || failed->value("message", "") != "Resource is in use" || failed->value("detail", "") != detail)
    {
        std::cerr << "Delete should be refused with \"" << detail << "\", got "
                  << (failed ? failed->dump() : std::string{"no refusal"}) << "\n";
        return false;
    }

    if (FindLastMessageOfType(host.messages, "resourceRemoved"))
    {
        std::cerr << "Refused delete still announced resourceRemoved\n";
        return false;
    }

    if (!fs::exists(resource.filePath) || !LibraryIndexContains(sandbox, resource.type, resource.id) ||
        !controller.GetResourceLibrary().HasResource(resource.type, resource.id))
    {
        std::cerr << "Refused delete lost the resource's file or entry\n";
        return false;
    }

    return true;
}

bool TestDeleteResourceUsedOnlyByEffectPresetIsRefused()
{
    using namespace guitarfx;

    const fs::path sandbox = MakeCleanupSandbox("effect-preset-only");
    const SandboxGuard guard{sandbox};

    TestHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();

    const auto saved = SaveStoredResource(controller, host, "effect-preset-only", "AQID");

    if (!saved)
    {
        std::cerr << "Failed to save the effect-preset-only resource\n";
        return false;
    }

    // The effect preset is saved from a node playing the resource, then that preset is replaced by
    // one without it, so nothing but the effect preset refers to the resource.
    if (!LoadPreset(controller, BuildSingleNodeResourcePreset("resource-node", saved->type, saved->id)))
    {
        std::cerr << "Failed to load the preset the effect preset is saved from\n";
        return false;
    }

    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveEffectPreset"},
        {"effectType", "utility"},
        {"name", "Only Here"},
        {"nodeId", "resource-node"},
    }
                                   .dump());

    const auto effectPresets = FindLastMessageOfType(host.messages, "effectPresets");
    const auto saves =
        effectPresets
            ? effectPresets->value("byEffectType", nlohmann::json::object()).value("utility", nlohmann::json::array())
            : nlohmann::json::array();

    if (saves.size() != 1 || saves[0].value("resources", nlohmann::json::array()).empty())
    {
        std::cerr << "Effect preset was not saved with the node's resource\n";
        return false;
    }

    Preset other = BuildSingleNodeResourcePreset("resource-node", saved->type, saved->id);
    other.id = "resource-delete-test-other";
    other.name = "Without The Resource";
    other.graph.nodes[1].resources.clear();

    if (!LoadPreset(controller, other))
    {
        std::cerr << "Failed to load the preset without the resource\n";
        return false;
    }

    const std::string detail = "Used by effect preset: Only Here";

    if (!ExpectUsageReported(controller, host, *saved, "effectPreset", detail) ||
        !ExpectDeleteRefused(controller, host, sandbox, *saved, detail))
    {
        return false;
    }

    // With the effect preset gone the resource is free: the query's cached index was invalidated,
    // and the delete goes ahead.
    controller.HandleUIMessage(nlohmann::json{
        {"type", "deleteEffectPreset"},
        {"effectType", "utility"},
        {"presetId", saves[0].value("id", "")},
    }
                                   .dump());

    const auto usage = QueryResourceUsage(controller, host, *saved);

    if (!usage || usage->value("inUse", true))
    {
        std::cerr << "Resource should be free once its effect preset is deleted, got "
                  << (usage ? usage->dump() : std::string{"no reply"}) << "\n";
        return false;
    }

    if (!DeleteResourceAndExpectRemoved(controller, host, saved->type, saved->id) || fs::exists(saved->filePath))
    {
        std::cerr << "Resource no longer used should delete, file and all\n";
        return false;
    }

    return true;
}

bool TestDeleteResourceUsedOnlyByCompositeIsRefused()
{
    using namespace guitarfx;

    const fs::path sandbox = MakeCleanupSandbox("composite-only");
    const SandboxGuard guard{sandbox};

    TestHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();

    const auto saved = SaveStoredResource(controller, host, "composite-only", "AQID");

    if (!saved)
    {
        std::cerr << "Failed to save the composite-only resource\n";
        return false;
    }

    CompositeEffectDefinition definition;
    definition.id = "resource-delete-test-composite";
    definition.name = "Delete Test Composite";
    definition.innerGraph = BuildSingleNodeResourcePreset("inner-node", saved->type, saved->id).graph;

    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveCompositeDefinition"},
        {"definition", SerializeCompositeEffectDefinition(definition)},
    }
                                   .dump());

    if (!FindLastMessageOfType(host.messages, "compositeDefinitionAdded"))
    {
        std::cerr << "Composite definition was not saved\n";
        return false;
    }

    const std::string detail = "Used by composite: Delete Test Composite";
    return ExpectUsageReported(controller, host, *saved, "composite", detail) &&
           ExpectDeleteRefused(controller, host, sandbox, *saved, detail);
}

bool TestDeleteReadsStoredPresetsFresh()
{
    using namespace guitarfx;

    const fs::path sandbox = MakeCleanupSandbox("stale-usage-index");
    const SandboxGuard guard{sandbox};

    TestHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();

    const auto saved = SaveStoredResource(controller, host, "stale-index", "AQID");

    if (!saved)
    {
        std::cerr << "Failed to save the stale-index resource\n";
        return false;
    }

    // Builds the usage index while nothing uses the resource.
    const auto before = QueryResourceUsage(controller, host, *saved);

    if (!before || before->value("inUse", true))
    {
        std::cerr << "Resource should start unused, got " << (before ? before->dump() : std::string{"no reply"})
                  << "\n";
        return false;
    }

    // Another instance saves a preset that uses it: straight into the store, with nothing to
    // invalidate this controller's index.
    Preset preset = BuildSingleNodeResourcePreset("resource-node", saved->type, saved->id);
    preset.id = "resource-delete-test-elsewhere";
    preset.name = "Saved Elsewhere";

    {
        storage::JsonStore store;
        std::string error;

        if (!store.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error) ||
            !PresetStorage::SaveToStore(store, preset))
        {
            std::cerr << "Could not store the other instance's preset: " << error << "\n";
            return false;
        }
    }

    return ExpectDeleteRefused(controller, host, sandbox, *saved, "Used by preset: Saved Elsewhere");
}

void WriteNamFile(const fs::path& path, const std::string& name, const std::string& gearType)
{
    std::ofstream file(path, std::ios::binary);
    file << R"({"version": "0.5.4", "architecture": "WaveNet", "metadata": {"name": ")" << name
         << R"(", "gear_type": ")" << gearType << R"(", "modeled_by": "Tester"}, "config": {}, "weights": []})";
}

std::optional<nlohmann::json> StoredResourceRow(const fs::path& sandbox, const std::string& type, const std::string& id)
{
    guitarfx::storage::JsonStore store;
    std::string error;

    if (!store.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error))
    {
        std::cerr << "Could not open the document store: " << error << "\n";
        return std::nullopt;
    }

    return store.Get(guitarfx::storage::ItemType::kResource, guitarfx::ResourceLibrary::MakeStoreId(type, id));
}

/// A NAM row from before imports read the file's metadata gets it at the next start, saved, and
/// the file is not read again after that. Startup used to read every model's file on every launch
/// (28 MB on a 440-model library) and keep what it found only when a category changed.
bool TestStartupBackfillsNamMetadataOnce()
{
    const fs::path sandbox = MakeCleanupSandbox("nam-backfill");
    const SandboxGuard guard{sandbox};
    std::error_code ec;
    fs::create_directories(sandbox / "external", ec);
    const fs::path namFile = sandbox / "external" / "legacy-rig.nam";
    WriteNamFile(namFile, "Legacy Rig", "amp_cab");

    {
        // First start creates the store; then a row as an older build wrote it, with no metadata.
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();
    }

    {
        guitarfx::storage::JsonStore store;
        std::string error;

        if (!store.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error))
        {
            std::cerr << "Could not open the document store: " << error << "\n";
            return false;
        }

        guitarfx::LibraryResource legacy;
        legacy.type = "nam";
        legacy.id = "legacy-rig";
        legacy.name = "Legacy Rig";
        legacy.filePath = namFile;

        if (!guitarfx::ResourceLibrary::PutInStore(store, legacy, sandbox / "resources"))
        {
            std::cerr << "Could not write the legacy row\n";
            return false;
        }
    }

    const auto metadataAfterStart = [&]() -> std::map<std::string, std::string> {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        const auto resource = controller.GetResourceLibrary().LookupResource("nam", "legacy-rig");
        return resource ? resource->metadata : std::map<std::string, std::string>{};
    };

    const auto valueOf = [](const std::map<std::string, std::string>& metadata, const std::string& key) {
        const auto it = metadata.find(key);
        return it == metadata.end() ? std::string{} : it->second;
    };

    const auto backfilled = metadataAfterStart();
    bool ok = true;

    if (valueOf(backfilled, "namFileVersion") != "0.5.4" || valueOf(backfilled, "gear_type") != "amp_cab" ||
        valueOf(backfilled, "namName") != "Legacy Rig")
    {
        std::cerr << "Startup did not backfill the legacy row's metadata from its file\n";
        ok = false;
    }

    const auto row = StoredResourceRow(sandbox, "nam", "legacy-rig");

    if (!row || row->value("metadata", nlohmann::json::object()).value("gear_type", "") != "amp_cab")
    {
        std::cerr << "Backfilled metadata was not saved: " << (row ? row->dump() : std::string{"no row"}) << "\n";
        ok = false;
    }

    // Read once: a later start keeps what was saved rather than reading the file again.
    WriteNamFile(namFile, "Changed Since", "pedal");
    const auto later = metadataAfterStart();

    if (valueOf(later, "gear_type") != "amp_cab" || valueOf(later, "namName") != "Legacy Rig")
    {
        std::cerr << "A later start read the model's file again\n";
        ok = false;
    }

    return ok;
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
    run("Cleanup removal persists across a restart", TestCleanupRemovalPersistsAcrossRestart());
    run("Cleanup keeps a resource used only in a later scene", TestCleanupKeepsResourceUsedOnlyInLaterScene());
    run("Delete refuses a resource used only by an effect preset", TestDeleteResourceUsedOnlyByEffectPresetIsRefused());
    run("Delete refuses a resource used only by a composite", TestDeleteResourceUsedOnlyByCompositeIsRefused());
    run("Delete reads stored presets fresh, not the usage index", TestDeleteReadsStoredPresetsFresh());
    run("Startup backfills NAM metadata once and saves it", TestStartupBackfillsNamMetadataOnce());

    std::cout << "\nResource library delete workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
