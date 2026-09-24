/**
 * PresetArchiveInstallTests.cpp - Installing tones downloaded from tone sharing, and taking
 * them out again (controller/PluginControllerArchiveInstall.cpp).
 *
 * Each test builds real zips in the web UI's archive format (preset.json or presets.json plus
 * resources/<file>), sends them to a headless controller as Soundshed Guitar Nano does, and
 * checks the library, the presets, the folders and the toneSharing.installedPacks entry the
 * web UI reads.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "util/Base64.h"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::Preset;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        sent.push_back(json);
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
        return 256;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    std::vector<std::string> sent;

  private:
    fs::path mRoot;
};

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

std::optional<nlohmann::json> Latest(const TestHost& host, const std::string& type)
{
    for (auto it = host.sent.rbegin(); it != host.sent.rend(); ++it)
    {
        auto parsed = nlohmann::json::parse(*it, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            return parsed;
        }
    }

    return std::nullopt;
}

void Send(guitarfx::PluginController& controller, const std::string& type, nlohmann::json payload = {})
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["type"] = type;
    controller.HandleUIMessage(payload.dump());
}

fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-archive-install-tests-" + run) / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

Bytes BuildZip(const std::vector<std::pair<std::string, Bytes>>& entries)
{
    mz_zip_archive archive{};

    if (!mz_zip_writer_init_heap(&archive, 0, 0))
    {
        return {};
    }

    for (const auto& [name, data] : entries)
    {
        mz_zip_writer_add_mem(&archive, name.c_str(), data.empty() ? nullptr : data.data(), data.size(),
                              MZ_DEFAULT_COMPRESSION);
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    Bytes zip;

    if (mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size))
    {
        zip.assign(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + size);
    }

    mz_zip_writer_end(&archive);
    return zip;
}

Bytes ToBytes(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

/// Input, one NAM node playing `resourceId` (with a blend id if given), output.
Preset BuildPreset(const std::string& id, const std::string& name, const std::string& resourceId,
                   const std::string& blendId = {})
{
    Preset preset;
    preset.id = id;
    preset.name = name;
    preset.category = "Shared";
    preset.version = 2;

    GraphNode input;
    input.id = "__input__";
    input.type = guitarfx::kNodeTypeInput;
    GraphNode amp;
    amp.id = "amp_1";
    amp.type = "amp_nam_optimized";
    amp.resources.push_back({"nam", resourceId});

    if (!blendId.empty())
    {
        amp.config["blendId"] = blendId;
    }

    GraphNode output;
    output.id = "__output__";
    output.type = guitarfx::kNodeTypeOutput;
    preset.graph.nodes = {input, amp, output};
    preset.graph.edges = {GraphEdge{"__input__", "amp_1", 0, 0, 1.0}, GraphEdge{"amp_1", "__output__", 0, 0, 1.0}};
    preset.globalSignalChain = guitarfx::GlobalSignalChainConfig{};
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
}

/// A one-preset archive, as the web UI shares one: the model under its hash-prefixed name.
Bytes SinglePresetArchive(const Preset& preset, const std::string& resourceId, const Bytes& model,
                          const nlohmann::json& extra = nlohmann::json::object())
{
    nlohmann::json root = {
        {"formatVersion", 1},
        {"preset", PresetJson(preset)},
        {"resources",
         {{{"id", resourceId}, {"name", "Shared Model"}, {"category", "amp"}, {"type", "nam"}, {"fileName", resourceId + "-model.nam"},
           {"hash", resourceId}}}},
        {"blends", nlohmann::json::array()},
    };

    for (const auto& [key, value] : extra.items())
    {
        root[key] = value;
    }

    return BuildZip({{"preset.json", ToBytes(root.dump())}, {"resources/" + resourceId + "-model.nam", model}});
}

nlohmann::json Archive(const Bytes& zip, const std::string& title)
{
    return {{"data", guitarfx::util::EncodeBase64(zip)}, {"title", title}};
}

std::optional<Preset> StoredPreset(TestHost& host, guitarfx::PluginController& controller, const std::string& id)
{
    const auto before = host.sent.size();
    Send(controller, "getPresetById", {{"presetId", id}});
    std::optional<nlohmann::json> reply;

    for (auto i = before; i < host.sent.size(); ++i)
    {
        auto parsed = nlohmann::json::parse(host.sent[i], nullptr, false);

        if (parsed.is_object() && parsed.value("type", "") == "presetData" && parsed.value("requestedPresetId", "") == id)
        {
            reply = parsed;
        }
    }

    if (!reply || !reply->contains("preset"))
    {
        return std::nullopt;
    }

    return guitarfx::PresetStorage::DeserializeFromJson((*reply)["preset"].dump());
}

nlohmann::json InstalledEntries(const guitarfx::PluginController& controller)
{
    return controller.GetAppSettings().value("toneSharing.installedPacks", nlohmann::json::array());
}

std::string ActivePresetId(const guitarfx::PluginController& controller)
{
    return controller.GetActivePreset() ? controller.GetActivePreset()->id : std::string{};
}

std::size_t LibraryCount(guitarfx::PluginController& controller, const std::string& type)
{
    std::size_t count = 0;

    for (const auto& resource : controller.GetResourceLibrary().GetAllResources())
    {
        count += resource.type == type ? 1 : 0;
    }

    return count;
}

const Bytes kModelA = ToBytes(R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[1.0,0.0]})");
const Bytes kModelB = ToBytes(R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[0.5,0.0]})");

void TestInstallOnePreset()
{
    TestHost host(Sandbox("one"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto activeBefore = ActivePresetId(controller);
    const auto modelsBefore = LibraryCount(controller, "nam");

    const auto zip = SinglePresetArchive(BuildPreset("shared-1", "Original Name", "sha-a"), "sha-a", kModelA);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r1"},
          {"entry", {{"id", "tone-sharing-api:item:item-1"}, {"title", "Shared Crunch"}, {"source", "toneSharingApi"}}},
          {"archives", {Archive(zip, "Shared Crunch")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    Expect(reply.has_value(), "an install is answered with presetArchivesInstalled");

    if (!reply)
    {
        return;
    }

    Expect(reply->value("requestId", "") == "r1", "the reply carries the request id");
    const auto presetIds = reply->value("presetIds", nlohmann::json::array());
    Expect(presetIds.size() == 1, "one preset installed");
    Expect(LibraryCount(controller, "nam") == modelsBefore + 1, "its model joins the library");
    Expect(ActivePresetId(controller) == activeBefore, "the running preset is left alone");
    Expect(Latest(host, "presetList").has_value(), "the UI gets the new preset list");

    const auto id = presetIds.empty() ? std::string{} : presetIds[0].get<std::string>();
    Expect(id != "shared-1" && id.rfind("user-", 0) == 0, "the preset gets a new user id");

    if (const auto stored = StoredPreset(host, controller, id))
    {
        Expect(stored->name == "Shared Crunch", "a one-preset archive takes the shared item's title");
        Expect(!stored->globalSignalChain.has_value(), "the sharer's global chain is not installed");

        const auto& amp = stored->graph.nodes[1];
        const auto resourceId = amp.resources.empty() ? std::string{} : amp.resources[0].resourceId;
        const auto resource = controller.GetResourceLibrary().LookupResource("nam", resourceId);
        Expect(resourceId != "sha-a" && resource.has_value(), "the preset plays the library's copy of the model");

        if (resource)
        {
            Expect(fs::exists(resource->filePath), "whose file was written");
        }
    }
    else
    {
        Expect(false, "the installed preset can be read back");
    }

    const auto entries = InstalledEntries(controller);
    Expect(entries.size() == 1 && entries[0].value("id", "") == "tone-sharing-api:item:item-1",
           "the install is recorded in toneSharing.installedPacks");

    if (!entries.empty())
    {
        Expect(entries[0].value("presetIds", nlohmann::json::array()) == presetIds, "with its preset ids");
        Expect(entries[0].value("resources", nlohmann::json::array()).size() == 1, "and the model it added");
        Expect(entries[0].value("source", "") == "toneSharingApi" && entries[0].contains("importedAt"),
               "in the web UI's schema");
    }

    const auto changed = Latest(host, "appSettingChanged");
    Expect(changed && changed->value("key", "") == "toneSharing.installedPacks", "and the UI is told of the new setting");
}

void TestSecondInstallReusesTheModel()
{
    TestHost host(Sandbox("reuse"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto zip = SinglePresetArchive(BuildPreset("shared-1", "A", "sha-a"), "sha-a", kModelA);
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:one"}, {"title", "One"}}}, {"archives", {Archive(zip, "One")}}});
    const auto models = LibraryCount(controller, "nam");

    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:two"}, {"title", "Two"}}}, {"archives", {Archive(zip, "Two")}}});

    Expect(LibraryCount(controller, "nam") == models, "the same model is not copied into the library twice");
    const auto entries = InstalledEntries(controller);
    Expect(entries.size() == 2 && entries[0].value("id", "") == "tone-sharing-api:item:two", "the newest install is listed first");

    if (!entries.empty())
    {
        Expect(entries[0].value("resources", nlohmann::json::array()).empty(),
               "a model the library already had is not counted as this install's");
    }
}

void TestReusesAnOlderImportOfTheSameFile()
{
    TestHost host(Sandbox("older-import"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // An earlier import of the same file, recorded with a hash of another kind (the browser's
    // SHA-256, as older web UI imports stored it).
    const auto folder = fs::path(host.GetUserDataPath()) / "older";
    fs::create_directories(folder);
    const auto path = folder / "sha-a-model.nam";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kModelA.data()), static_cast<std::streamsize>(kModelA.size()));
    }

    guitarfx::LibraryResource older;
    older.type = "nam";
    older.id = "older-model";
    older.name = "Older";
    older.filePath = path;
    older.hash = "sha-a";
    controller.GetResourceLibrary().AddResource(older);
    const auto models = LibraryCount(controller, "nam");

    const auto zip = SinglePresetArchive(BuildPreset("p", "Again", "sha-a"), "sha-a", kModelA);
    host.sent.clear();
    Send(controller, "installPresetArchives", {{"entry", {{"id", "tone-sharing-api:item:again"}}}, {"archives", {Archive(zip, "Again")}}});

    Expect(LibraryCount(controller, "nam") == models, "a file the library has under its archive name, same bytes, is reused");
    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto ids = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    const auto preset = ids.empty() ? std::nullopt : StoredPreset(host, controller, ids[0].get<std::string>());
    Expect(preset && preset->graph.nodes[1].resources[0].resourceId == "older-model", "and the preset plays it");
}

void TestPackGetsAFolder()
{
    TestHost host(Sandbox("pack"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto first = SinglePresetArchive(BuildPreset("p1", "First", "sha-a"), "sha-a", kModelA);
    const auto second = SinglePresetArchive(BuildPreset("p2", "Second", "sha-b"), "sha-b", kModelB);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:pack-9"}, {"title", "Metal Pack"}, {"packId", "pack-9"}}},
          {"folder", "Metal Pack"},
          {"archives", {Archive(first, "Chug"), Archive(second, "Lead")}}});

    const auto reply = Latest(host, "presetArchivesInstalled");
    const auto presetIds = reply ? reply->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    Expect(presetIds.size() == 2, "a pack installs every preset");

    const auto folders = Latest(host, "presetFolders");
    Expect(folders.has_value(), "the UI gets the new folders");

    bool found = false;

    for (const auto& folder : folders ? folders->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        if (folder.value("id", "") == "tone-sharing::pack-9")
        {
            found = true;
            Expect(folder.value("name", "") == "Metal Pack", "the folder is named after the pack");
            Expect(folder.value("presetIds", nlohmann::json::array()) == presetIds, "and holds its presets");
        }
    }

    Expect(found, "the pack gets a folder of its own");

    if (presetIds.size() == 2)
    {
        const auto chug = StoredPreset(host, controller, presetIds[0].get<std::string>());
        Expect(chug && chug->name == "Chug", "each preset takes its item's title");
    }

    const auto entries = InstalledEntries(controller);
    Expect(!entries.empty() && entries[0].value("packId", "") == "pack-9", "the entry records the pack id");
}

void TestMissingTone3000ModelsInstallNothing()
{
    TestHost host(Sandbox("tone3000"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto models = LibraryCount(controller, "nam");

    const nlohmann::json extra = {
        {"tone3000Resources",
         {{{"id", "t3k-1"}, {"name", "Borrowed Amp"}, {"type", "nam"}, {"toneId", "123"}, {"modelId", "456"}}}}};
    const auto zip = SinglePresetArchive(BuildPreset("p", "Needs T3K", "sha-a"), "sha-a", kModelA, extra);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"requestId", "r2"}, {"entry", {{"id", "tone-sharing-api:item:t3k"}}}, {"archives", {Archive(zip, "Needs T3K")}}});

    const auto failed = Latest(host, "presetArchivesInstallFailed");
    Expect(failed.has_value() && failed->value("requestId", "") == "r2", "the install fails, answering the request");

    if (failed)
    {
        Expect(failed->value("detail", "").find("Borrowed Amp") != std::string::npos, "naming the model it needs");
    }

    Expect(!Latest(host, "presetArchivesInstalled").has_value(), "nothing reports success");
    Expect(LibraryCount(controller, "nam") == models, "no model was written");
    Expect(InstalledEntries(controller).empty(), "and nothing is recorded");
}

void TestUnreadableDownloadIsRefused()
{
    TestHost host(Sandbox("unreadable"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:item:bad"}}}, {"archives", {Archive(ToBytes("not a zip"), "Bad")}}});

    Expect(Latest(host, "presetArchivesInstallFailed").has_value(), "a download that is not an archive is refused");
    Expect(InstalledEntries(controller).empty(), "and not recorded");
}

void TestDeleteTakesOutWhatItAdded()
{
    TestHost host(Sandbox("delete"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // The user already has model B; the pack's second preset reuses it.
    const auto own = SinglePresetArchive(BuildPreset("mine", "Mine", "sha-b"), "sha-b", kModelB);
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "zipImport:mine"}, {"title", "Mine"}}}, {"archives", {Archive(own, "Mine")}}});

    const auto first = SinglePresetArchive(BuildPreset("p1", "First", "sha-a"), "sha-a", kModelA);
    const auto second = SinglePresetArchive(BuildPreset("p2", "Second", "sha-b"), "sha-b", kModelB);
    host.sent.clear();
    Send(controller, "installPresetArchives",
         {{"entry", {{"id", "tone-sharing-api:pack-1"}, {"title", "Pack"}, {"packId", "pack-1"}}},
          {"folder", "Pack"},
          {"archives", {Archive(first, "One"), Archive(second, "Two")}}});

    const auto installed = Latest(host, "presetArchivesInstalled");
    const auto packPresetIds = installed ? installed->value("presetIds", nlohmann::json::array()) : nlohmann::json::array();
    const auto entries = InstalledEntries(controller);
    const auto addedModels = entries.empty() ? nlohmann::json::array() : entries[0].value("resources", nlohmann::json::array());
    Expect(addedModels.size() == 1, "the pack added only model A");

    const auto addedId = addedModels.empty() ? std::string{} : addedModels[0].value("id", "");
    const auto addedPath = controller.GetResourceLibrary().LookupResource("nam", addedId).value_or(guitarfx::LibraryResource{}).filePath;
    const auto modelsBefore = LibraryCount(controller, "nam");

    Send(controller, "setPresetFavorite", {{"presetId", packPresetIds.empty() ? "" : packPresetIds[0].get<std::string>()}, {"favorite", true}});

    host.sent.clear();
    Send(controller, "deleteInstalledPresetArchive", {{"id", "tone-sharing-api:pack-1"}});

    const auto deleted = Latest(host, "installedPresetArchiveDeleted");
    Expect(deleted.has_value() && deleted->value("presetIds", nlohmann::json::array()).size() == 2,
           "removing the pack deletes both its presets");

    for (const auto& id : packPresetIds)
    {
        Expect(!StoredPreset(host, controller, id.get<std::string>()).has_value(), "a removed preset is gone from the store");
    }

    Expect(LibraryCount(controller, "nam") == modelsBefore - 1, "the model only the pack used is removed");
    Expect(addedPath.empty() || !fs::exists(addedPath), "with its file");

    const auto remaining = InstalledEntries(controller);
    Expect(remaining.size() == 1 && remaining[0].value("id", "") == "zipImport:mine", "the entry is gone; the other stays");

    const auto folders = Latest(host, "presetFolders");
    bool folderLeft = false;

    for (const auto& folder : folders ? folders->value("folders", nlohmann::json::array()) : nlohmann::json::array())
    {
        folderLeft = folderLeft || folder.value("id", "") == "tone-sharing::pack-1";
    }

    Expect(folders.has_value() && !folderLeft, "and so is the pack's folder");

    const auto favorites = Latest(host, "presetFavorites");
    Expect(favorites && favorites->value("favorites", nlohmann::json::array()).empty(), "and its favourite");
}
} // namespace

int main()
{
    TestInstallOnePreset();
    TestSecondInstallReusesTheModel();
    TestReusesAnOlderImportOfTheSameFile();
    TestPackGetsAFolder();
    TestMissingTone3000ModelsInstallNothing();
    TestUnreadableDownloadIsRefused();
    TestDeleteTakesOutWhatItAdded();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "PresetArchiveInstallTests passed" << std::endl;
    return 0;
}
