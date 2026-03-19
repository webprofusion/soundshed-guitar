#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
public:
    explicit TestHost(fs::path userDataPath, fs::path bundledAssetsPath = {})
        : mUserDataPath(std::move(userDataPath))
        , mBundledAssetsPath(bundledAssetsPath.empty() ? mUserDataPath : std::move(bundledAssetsPath))
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
        return mBundledAssetsPath;
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
    fs::path mBundledAssetsPath;
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

std::uint32_t Crc32(const std::vector<std::uint8_t>& data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t byte : data)
    {
        crc ^= static_cast<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit)
        {
            const std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void AppendLe16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void AppendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    AppendLe16(bytes, static_cast<std::uint16_t>(value & 0xFFFFu));
    AppendLe16(bytes, static_cast<std::uint16_t>((value >> 16u) & 0xFFFFu));
}

struct StoredZipEntry
{
    std::string name;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> BuildStoredZip(const std::vector<StoredZipEntry>& entries)
{
    std::vector<std::uint8_t> bytes;
    struct CentralRecord
    {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t localOffset = 0;
    };
    std::vector<CentralRecord> central;
    central.reserve(entries.size());

    for (const auto& entry : entries)
    {
        CentralRecord record;
        record.name = entry.name;
        record.crc = Crc32(entry.data);
        record.size = static_cast<std::uint32_t>(entry.data.size());
        record.localOffset = static_cast<std::uint32_t>(bytes.size());

        AppendLe32(bytes, 0x04034B50u);
        AppendLe16(bytes, 20);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe32(bytes, record.crc);
        AppendLe32(bytes, record.size);
        AppendLe32(bytes, record.size);
        AppendLe16(bytes, static_cast<std::uint16_t>(record.name.size()));
        AppendLe16(bytes, 0);
        bytes.insert(bytes.end(), record.name.begin(), record.name.end());
        bytes.insert(bytes.end(), entry.data.begin(), entry.data.end());

        central.push_back(record);
    }

    const std::uint32_t centralOffset = static_cast<std::uint32_t>(bytes.size());
    for (const auto& record : central)
    {
        AppendLe32(bytes, 0x02014B50u);
        AppendLe16(bytes, 20);
        AppendLe16(bytes, 20);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe32(bytes, record.crc);
        AppendLe32(bytes, record.size);
        AppendLe32(bytes, record.size);
        AppendLe16(bytes, static_cast<std::uint16_t>(record.name.size()));
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe16(bytes, 0);
        AppendLe32(bytes, 0);
        AppendLe32(bytes, record.localOffset);
        bytes.insert(bytes.end(), record.name.begin(), record.name.end());
    }

    const std::uint32_t centralSize = static_cast<std::uint32_t>(bytes.size()) - centralOffset;
    AppendLe32(bytes, 0x06054B50u);
    AppendLe16(bytes, 0);
    AppendLe16(bytes, 0);
    AppendLe16(bytes, static_cast<std::uint16_t>(central.size()));
    AppendLe16(bytes, static_cast<std::uint16_t>(central.size()));
    AppendLe32(bytes, centralSize);
    AppendLe32(bytes, centralOffset);
    AppendLe16(bytes, 0);

    return bytes;
}

bool TestRiffLibraryPathNormalization()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "riff-path-normalization";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        const fs::path libraryRoot = sandbox / "riff-library-custom";
        const fs::path takePath = libraryRoot / "takes" / "riff-1" / "take.wav";
        fs::create_directories(takePath.parent_path(), ec);
        {
            std::ofstream takeFile(takePath, std::ios::binary);
            takeFile << "riff";
        }

        const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
        fs::create_directories(settingsPath.parent_path(), ec);
        {
            nlohmann::json appSettings = nlohmann::json::object();
            appSettings["riffLibrary.path"] = libraryRoot.string();
            std::ofstream appSettingsFile(settingsPath);
            appSettingsFile << appSettings.dump(2);
        }

        const fs::path indexPath = libraryRoot / "riff-library-index.json";
        {
            nlohmann::json index = nlohmann::json::object();
            index["path"] = libraryRoot.string();
            index["riffs"] = nlohmann::json::array({
                {
                    {"id", "riff-1"},
                    {"title", "Riff One"},
                    {"favorite", false},
                    {"used", false},
                    {"takes", nlohmann::json::array({
                        {
                            {"id", "take-1"},
                            {"filePath", takePath.string()},
                            {"durationSec", 1.0}
                        }
                    })}
                }
            });
            std::ofstream indexFile(indexPath);
            indexFile << index.dump(2);
        }

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        nlohmann::json normalize;
        normalize["type"] = "setRiffLibraryPath";
        normalize["path"] = libraryRoot.string();
        controller.HandleUIMessage(normalize.dump());

        nlohmann::json storedIndex;
        {
            std::ifstream input(indexPath);
            if (!input)
            {
                std::cerr << "Failed to open riff index after save\n";
                return false;
            }
            storedIndex = nlohmann::json::parse(input, nullptr, false);
        }

        if (storedIndex.is_discarded() || !storedIndex.is_object())
        {
            std::cerr << "Riff index is invalid after save\n";
            return false;
        }

        const auto riffs = storedIndex.value("riffs", nlohmann::json::array());
        if (!riffs.is_array() || riffs.empty())
        {
            std::cerr << "Riff index has no riffs after save\n";
            return false;
        }
        const auto takes = riffs[0].value("takes", nlohmann::json::array());
        if (!takes.is_array() || takes.empty())
        {
            std::cerr << "Riff index has no takes after save\n";
            return false;
        }

        const auto storedPath = fs::path(takes[0].value("filePath", ""));
        if (storedPath.empty() || storedPath.is_absolute())
        {
            std::cerr << "Riff take path was not normalized to relative in index\n";
            return false;
        }

        TestHost hostReload(sandbox);
        guitarfx::PluginController controllerReload(hostReload);
        controllerReload.Initialize();

        nlohmann::json getLibrary;
        getLibrary["type"] = "getRiffLibrary";
        controllerReload.HandleUIMessage(getLibrary.dump());

        const auto libraryMsg = FindLatestMessageOfType(hostReload.sentMessages, "riffLibraryState");
        if (!libraryMsg || !libraryMsg->contains("library"))
        {
            std::cerr << "riffLibraryState not emitted\n";
            return false;
        }

        const auto library = (*libraryMsg).value("library", nlohmann::json::object());
        const auto runtimeRiffs = library.value("riffs", nlohmann::json::array());
        if (!runtimeRiffs.is_array() || runtimeRiffs.empty())
        {
            std::cerr << "riffLibraryState has no riffs\n";
            return false;
        }
        const auto runtimeTakes = runtimeRiffs[0].value("takes", nlohmann::json::array());
        if (!runtimeTakes.is_array() || runtimeTakes.empty())
        {
            std::cerr << "riffLibraryState has no takes\n";
            return false;
        }

        const auto runtimePath = fs::path(runtimeTakes[0].value("filePath", ""));
        if (runtimePath.empty() || !runtimePath.is_absolute())
        {
            std::cerr << "Loaded riff take path is not absolute runtime path\n";
            return false;
        }

        if (runtimePath.lexically_normal() != takePath.lexically_normal())
        {
            std::cerr << "Loaded riff take path does not resolve to expected file\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestRiffLibraryPathNormalization: " << ex.what() << "\n";
        return false;
    }
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

    const fs::path savedPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user" / (saveId + ".json");
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

bool TestFactoryPresetArchiveStartupImport()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "factory-archive";
    const fs::path bundledAssets = sandbox / "bundled-assets";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(bundledAssets / "ui" / "presets" / "factory", ec);
    SetSettingsEnvRoot(sandbox);

    guitarfx::Preset preset = BuildPreset("factory-archive-preset", "Archive Factory Preset");
    preset.category = "Factory Archive";
    if (preset.graph.nodes.size() > 1)
    {
        preset.graph.nodes[1].resources.clear();
        preset.graph.nodes[1].type = guitarfx::EffectGuids::kAmpNamBlend;
        preset.graph.nodes[1].config["blendId"] = "archive-blend";
    }

    nlohmann::json archive;
    archive["formatVersion"] = 1;
    archive["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    archive["resources"] = nlohmann::json::array({
        {
            {"id", "archive-model"},
            {"name", "Archive Model"},
            {"category", "Archive"},
            {"type", "nam"},
            {"fileName", "archive-model.nam"},
            {"hash", "test-hash"}
        }
    });
    archive["blends"] = nlohmann::json::array({
        {
            {"id", "archive-blend"},
            {"name", "Archive Blend"},
            {"models", nlohmann::json::array({"archive-model"})}
        }
    });
    archive["presetFolders"] = nlohmann::json::array({
        {
            {"name", "High Gain"},
            {"presetIds", nlohmann::json::array({preset.id})},
            {"children", nlohmann::json::array()}
        }
    });

    const auto archiveText = archive.dump(2);
    const auto archiveBytes = BuildStoredZip({
        {"preset.json", std::vector<std::uint8_t>(archiveText.begin(), archiveText.end())},
        {"resources/archive-model.nam", std::vector<std::uint8_t>{'n', 'a', 'm'}}
    });
    const fs::path archivePath = bundledAssets / "ui" / "presets" / "factory" / "bundle.soundshed.preset";
    {
        std::ofstream output(archivePath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(archiveBytes.data()), static_cast<std::streamsize>(archiveBytes.size()));
    }

    nlohmann::json settings = nlohmann::json::object();
    settings["lastPresetId"] = "bundle__factory-archive-preset";
    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    TestHost host(sandbox, bundledAssets);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto& active = controller.GetActivePreset();
    if (!active || active->name != "Archive Factory Preset")
    {
        std::cerr << "Archive-backed factory preset was not restored at startup\n";
        return false;
    }

    nlohmann::json getList;
    getList["type"] = "getPresetList";
    controller.HandleUIMessage(getList.dump());
    const auto presetListMsg = FindLatestMessageOfType(host.sentMessages, "presetList");
    if (!presetListMsg)
    {
        std::cerr << "presetList not emitted\n";
        return false;
    }

    const auto presets = presetListMsg->value("presets", nlohmann::json::array());
    bool foundArchivePreset = false;
    for (const auto& item : presets)
    {
        if (item.value("name", "") == "Archive Factory Preset" && item.value("source", "") == "factory")
        {
            foundArchivePreset = true;
            break;
        }
    }
    if (!foundArchivePreset)
    {
        std::cerr << "Archive-backed factory preset missing from presetList\n";
        return false;
    }

    const auto resources = controller.GetResourceLibrary().GetAllResources();
    const auto resourceIt = std::find_if(resources.begin(), resources.end(), [](const guitarfx::LibraryResource& resource)
    {
        return resource.name == "Archive Model";
    });
    if (resourceIt == resources.end() || !fs::exists(resourceIt->filePath))
    {
        std::cerr << "Archive-backed factory resource was not imported into the runtime library\n";
        return false;
    }

    const fs::path persistedPresetPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user" / "bundle__factory-archive-preset.json";
    const auto persistedPreset = guitarfx::PresetStorage::LoadFromFile(persistedPresetPath);
    if (!persistedPreset || persistedPreset->category != "Factory")
    {
        std::cerr << "Archive-backed factory preset was not persisted as a Factory preset\n";
        return false;
    }

    const fs::path presetFoldersPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "preset-folders.json";
    if (!fs::exists(presetFoldersPath))
    {
        std::cerr << "Factory preset folders file was not created\n";
        return false;
    }

    nlohmann::json presetFoldersJson;
    {
        std::ifstream input(presetFoldersPath);
        input >> presetFoldersJson;
    }

    bool foundArchiveFolder = false;
    bool foundPresetInFolder = false;
    for (const auto& folder : presetFoldersJson.value("folders", nlohmann::json::array()))
    {
        if (!folder.is_object() || folder.value("name", "") != "High Gain")
            continue;

        foundArchiveFolder = true;
        for (const auto& presetIdValue : folder.value("presetIds", nlohmann::json::array()))
        {
            if (presetIdValue.is_string() && presetIdValue.get<std::string>() == "bundle__factory-archive-preset")
            {
                foundPresetInFolder = true;
                break;
            }
        }
    }

    if (!foundArchiveFolder || !foundPresetInFolder)
    {
        std::cerr << "Factory preset archive folder was not persisted at the top level\n";
        return false;
    }

    auto locallyModifiedPreset = *persistedPreset;
    locallyModifiedPreset.name = "Local Factory Override";
    if (!guitarfx::PresetStorage::SaveToFile(locallyModifiedPreset, persistedPresetPath))
    {
        std::cerr << "Unable to modify persisted factory preset for hash check\n";
        return false;
    }

    guitarfx::PluginController unchangedController(host);
    unchangedController.Initialize();
    const auto unchangedPreset = guitarfx::PresetStorage::LoadFromFile(persistedPresetPath);
    if (!unchangedPreset || unchangedPreset->name != "Local Factory Override")
    {
        std::cerr << "Unchanged factory archive should not have been re-imported\n";
        return false;
    }

    auto updatedPreset = preset;
    updatedPreset.name = "Archive Factory Preset Updated";
    nlohmann::json updatedArchive = archive;
    updatedArchive["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(updatedPreset));
    const auto updatedArchiveText = updatedArchive.dump(2);
    const auto updatedArchiveBytes = BuildStoredZip({
        {"preset.json", std::vector<std::uint8_t>(updatedArchiveText.begin(), updatedArchiveText.end())},
        {"resources/archive-model.nam", std::vector<std::uint8_t>{'n', 'a', 'm', '2'}}
    });
    {
        std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(updatedArchiveBytes.data()), static_cast<std::streamsize>(updatedArchiveBytes.size()));
    }

    guitarfx::PluginController updatedController(host);
    updatedController.Initialize();
    const auto reimportedPreset = guitarfx::PresetStorage::LoadFromFile(persistedPresetPath);
    if (!reimportedPreset || reimportedPreset->name != "Archive Factory Preset Updated")
    {
        std::cerr << "Changed factory archive was not re-imported\n";
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
    run("Factory preset archive startup import", TestFactoryPresetArchiveStartupImport());
    run("Riff library path normalization", TestRiffLibraryPathNormalization());

    std::cout << "\nPreset management workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
