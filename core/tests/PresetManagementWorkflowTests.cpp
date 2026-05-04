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
#include "dsp/simd/OptimizedNAM.h"
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

guitarfx::Preset BuildPassthroughPreset(const std::string& id, const std::string& name)
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

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, out};
    preset.graph.edges = {{"in", "out", 0, 0, 1.0}};
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

bool TestLoadPresetRehydratesScrubbedHostedPluginState()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-rehydrate";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        const fs::path presetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
        fs::create_directories(presetDir, ec);

        constexpr const char* expectedPluginState = "expected-plugin-state";

        guitarfx::Preset storedPreset;
        storedPreset.id = "user-hosted-plugin-rehydrate";
        storedPreset.name = "Hosted Plugin Rehydrate";
        storedPreset.version = 2;
        storedPreset.category = "Test";

        guitarfx::GraphNode inputNode;
        inputNode.id = "__input__";
        inputNode.type = guitarfx::kNodeTypeInput;

        guitarfx::GraphNode outputNode;
        outputNode.id = "__output__";
        outputNode.type = guitarfx::kNodeTypeOutput;

        guitarfx::GraphNode pluginNode;
        pluginNode.id = "plugin-host-node";
        pluginNode.type = guitarfx::EffectGuids::kPluginHost;
        pluginNode.category = "utility";
        pluginNode.config["pluginStateBase64"] = expectedPluginState;

        storedPreset.graph.nodes = {inputNode, pluginNode, outputNode};
        storedPreset.graph.edges = {
            {"__input__", "plugin-host-node", 0, 0, 1.0},
            {"plugin-host-node", "__output__", 0, 0, 1.0},
        };
        guitarfx::NormalizePresetScenes(storedPreset);

        const fs::path presetPath = presetDir / (storedPreset.id + ".json");
        if (!guitarfx::PresetStorage::SaveToFile(storedPreset, presetPath))
        {
            std::cerr << "Failed to write stored hosted-plugin preset fixture\n";
            return false;
        }

        guitarfx::Preset scrubbedPreset = storedPreset;
        if (auto* liveNode = scrubbedPreset.graph.FindNode(pluginNode.id))
        {
            liveNode->config["pluginStateBase64Length"] = std::to_string(liveNode->config["pluginStateBase64"].size());
            liveNode->config.erase("pluginStateBase64");
        }
        for (auto& scene : scrubbedPreset.scenes)
        {
            if (auto* sceneNode = scene.graph.FindNode(pluginNode.id))
            {
                sceneNode->config["pluginStateBase64Length"] = std::to_string(std::string(expectedPluginState).size());
                sceneNode->config.erase("pluginStateBase64");
            }
        }

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        nlohmann::json message;
        message["type"] = "loadPreset";
        message["presetId"] = storedPreset.id;
        message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(scrubbedPreset));
        controller.HandleUIMessage(message.dump());

        const auto& active = controller.GetActivePreset();
        if (!active)
        {
            std::cerr << "No active preset after scrubbed hosted-plugin load\n";
            return false;
        }

        const auto* rehydratedNode = active->graph.FindNode(pluginNode.id);
        if (!rehydratedNode)
        {
            std::cerr << "Rehydrated hosted-plugin node missing from active preset\n";
            return false;
        }

        const auto stateIt = rehydratedNode->config.find("pluginStateBase64");
        if (stateIt == rehydratedNode->config.end() || stateIt->second != expectedPluginState)
        {
            std::cerr << "Hosted-plugin state was not rehydrated from stored preset data\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestLoadPresetRehydratesScrubbedHostedPluginState: " << ex.what() << "\n";
        return false;
    }
}

bool TestLoadPresetRehydratesScrubbedHostedPluginStateFromActivePreset()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-rehydrate-active";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        const fs::path presetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
        fs::create_directories(presetDir, ec);

        constexpr const char* storedPluginState = "stored-plugin-state";
        constexpr const char* activePluginState = "active-plugin-state";

        auto buildHostedPreset = [](const std::string& stateValue) {
            guitarfx::Preset preset;
            preset.id = "user-hosted-plugin-active-rehydrate";
            preset.name = "Hosted Plugin Active Rehydrate";
            preset.version = 2;
            preset.category = "Test";

            guitarfx::GraphNode inputNode;
            inputNode.id = "__input__";
            inputNode.type = guitarfx::kNodeTypeInput;

            guitarfx::GraphNode outputNode;
            outputNode.id = "__output__";
            outputNode.type = guitarfx::kNodeTypeOutput;

            guitarfx::GraphNode pluginNode;
            pluginNode.id = "plugin-host-node";
            pluginNode.type = guitarfx::EffectGuids::kPluginHost;
            pluginNode.category = "utility";
            pluginNode.config["pluginStateBase64"] = stateValue;

            preset.graph.nodes = {inputNode, pluginNode, outputNode};
            preset.graph.edges = {
                {"__input__", "plugin-host-node", 0, 0, 1.0},
                {"plugin-host-node", "__output__", 0, 0, 1.0},
            };
            guitarfx::NormalizePresetScenes(preset);
            return preset;
        };

        const auto storedPreset = buildHostedPreset(storedPluginState);
        const fs::path presetPath = presetDir / (storedPreset.id + ".json");
        if (!guitarfx::PresetStorage::SaveToFile(storedPreset, presetPath))
        {
            std::cerr << "Failed to write stored hosted-plugin preset fixture\n";
            return false;
        }

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        const auto activePreset = buildHostedPreset(activePluginState);
        nlohmann::json initialLoad;
        initialLoad["type"] = "loadPreset";
        initialLoad["presetId"] = activePreset.id;
        initialLoad["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(activePreset));
        controller.HandleUIMessage(initialLoad.dump());

        guitarfx::Preset scrubbedPreset = activePreset;
        if (auto* liveNode = scrubbedPreset.graph.FindNode("plugin-host-node"))
        {
            liveNode->config["pluginStateBase64Length"] = std::to_string(liveNode->config["pluginStateBase64"].size());
            liveNode->config.erase("pluginStateBase64");
        }
        for (auto& scene : scrubbedPreset.scenes)
        {
            if (auto* sceneNode = scene.graph.FindNode("plugin-host-node"))
            {
                sceneNode->config["pluginStateBase64Length"] = std::to_string(std::string(activePluginState).size());
                sceneNode->config.erase("pluginStateBase64");
            }
        }

        nlohmann::json reloadMessage;
        reloadMessage["type"] = "loadPreset";
        reloadMessage["presetId"] = activePreset.id;
        reloadMessage["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(scrubbedPreset));
        controller.HandleUIMessage(reloadMessage.dump());

        const auto& active = controller.GetActivePreset();
        if (!active)
        {
            std::cerr << "No active preset after scrubbed hosted-plugin reload from memory\n";
            return false;
        }

        const auto* rehydratedNode = active->graph.FindNode("plugin-host-node");
        if (!rehydratedNode)
        {
            std::cerr << "Rehydrated hosted-plugin node missing from active preset\n";
            return false;
        }

        const auto stateIt = rehydratedNode->config.find("pluginStateBase64");
        if (stateIt == rehydratedNode->config.end() || stateIt->second != activePluginState)
        {
            std::cerr << "Hosted-plugin state did not prefer the active in-memory preset over stored data\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestLoadPresetRehydratesScrubbedHostedPluginStateFromActivePreset: " << ex.what() << "\n";
        return false;
    }
}

bool TestLoadPresetRemapsHostedPluginResourceByStableId()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-id-remap";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        guitarfx::LibraryResource pluginResource;
        pluginResource.type = "plugin";
        pluginResource.id = "local:plugin:acme-ultra-chorus";
        pluginResource.name = "Ultra Chorus";
        pluginResource.category = "Local";
        pluginResource.filePath = sandbox / "plugins" / "UltraChorus.vst3";
        pluginResource.metadata["pluginStableId"] = "acme.ultra-chorus";
        pluginResource.metadata["pluginFormat"] = "vst3";
        controller.GetResourceLibrary().AddResource(pluginResource);

        guitarfx::Preset preset;
        preset.id = "user-hosted-plugin-id-remap";
        preset.name = "Hosted Plugin ID Remap";
        preset.version = 2;
        preset.category = "Test";

        guitarfx::GraphNode inputNode;
        inputNode.id = "__input__";
        inputNode.type = guitarfx::kNodeTypeInput;

        guitarfx::GraphNode outputNode;
        outputNode.id = "__output__";
        outputNode.type = guitarfx::kNodeTypeOutput;

        guitarfx::GraphNode pluginNode;
        pluginNode.id = "plugin-host-node";
        pluginNode.type = guitarfx::EffectGuids::kPluginHost;
        pluginNode.category = "utility";
        pluginNode.config["pluginStableId"] = "acme.ultra-chorus";
        pluginNode.resources.push_back(guitarfx::ResourceRef{"plugin", "foreign-plugin-id", fs::path{}, ""});

        preset.graph.nodes = {inputNode, pluginNode, outputNode};
        preset.graph.edges = {
            {"__input__", "plugin-host-node", 0, 0, 1.0},
            {"plugin-host-node", "__output__", 0, 0, 1.0},
        };
        guitarfx::NormalizePresetScenes(preset);

        nlohmann::json message;
        message["type"] = "loadPreset";
        message["presetId"] = preset.id;
        message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
        controller.HandleUIMessage(message.dump());

        const auto& active = controller.GetActivePreset();
        if (!active)
        {
            std::cerr << "No active preset after hosted-plugin stable-id remap load\n";
            return false;
        }

        const auto* remappedNode = active->graph.FindNode(pluginNode.id);
        if (!remappedNode)
        {
            std::cerr << "Remapped hosted-plugin node missing from active preset\n";
            return false;
        }

        if (remappedNode->resources.empty())
        {
            std::cerr << "Hosted-plugin node has no resources after remap load\n";
            return false;
        }

        if (remappedNode->resources.front().resourceId != pluginResource.id)
        {
            std::cerr << "Hosted-plugin resource ID was not remapped by stable ID\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestLoadPresetRemapsHostedPluginResourceByStableId: " << ex.what() << "\n";
        return false;
    }
}

bool TestLoadPresetRestoresUnifiedLevelState()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "level-load";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildPassthroughPreset("p-level-load", "Level Load");
    preset.global.inputTrim = -9.5;
    preset.global.outputTrim = -4.0;
    preset.global.autoLevelInput = true;
    preset.global.autoLevelOutput = true;

    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    message["presetId"] = preset.id;
    controller.HandleUIMessage(message.dump());

    const auto& active = controller.GetActivePreset();
    if (!active)
    {
        std::cerr << "No active preset after level-state load\n";
        return false;
    }

    const auto chain = controller.GetMixer().GetGlobalChainConfig();
    if (std::abs(chain.inputGain - preset.global.inputTrim) > 1e-9
        || std::abs(chain.outputGain - preset.global.outputTrim) > 1e-9)
    {
        std::cerr << "Unified level state did not migrate preset trims into global chain\n";
        return false;
    }

    if (chain.autoLevelInput || chain.autoLevelOutput)
    {
        std::cerr << "Legacy mixer auto-level should be retired on preset load\n";
        return false;
    }

    if (std::abs(controller.GetParamValue(guitarfx::PluginController::kParamInputTrim) - preset.global.inputTrim) > 1e-9
        || std::abs(controller.GetParamValue(guitarfx::PluginController::kParamOutputTrim) - preset.global.outputTrim) > 1e-9)
    {
        std::cerr << "Controller parameter values were not synced to unified level state\n";
        return false;
    }

    if (active->global.autoLevelInput || active->global.autoLevelOutput)
    {
        std::cerr << "Active preset still carries retired mixer auto-level flags\n";
        return false;
    }

    return true;
}

bool TestLoadPresetRetiresNamInputAutoLeveling()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "nam-level-migration";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildPreset("p-nam-level-migration", "NAM Level Migration");
    auto *ampNode = preset.graph.FindNode("amp");
    if (!ampNode)
    {
        std::cerr << "Failed to build NAM test node\n";
        return false;
    }

    ampNode->params["autoLevelInput"] = 1.0;
    ampNode->params["calibrationInputLevel"] = -12.0;
    ampNode->params["calibrationOutputLevel"] = -6.0;

    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    message["presetId"] = preset.id;
    controller.HandleUIMessage(message.dump());

    const auto &active = controller.GetActivePreset();
    if (!active)
    {
        std::cerr << "No active preset after NAM migration load\n";
        return false;
    }

    const auto *loadedAmp = active->graph.FindNode("amp");
    if (!loadedAmp)
    {
        std::cerr << "Loaded preset missing NAM node\n";
        return false;
    }

    const auto autoInputIt = loadedAmp->params.find("autoLevelInput");
    if (autoInputIt == loadedAmp->params.end() || std::abs(autoInputIt->second) > 1e-9)
    {
        std::cerr << "NAM autoLevelInput should be retired to false on preset load\n";
        return false;
    }

    const auto autoOutputIt = loadedAmp->params.find("autoLevelOutput");
    if (autoOutputIt == loadedAmp->params.end() || std::abs(autoOutputIt->second - 1.0) > 1e-9)
    {
        std::cerr << "NAM autoLevelOutput should default to enabled on preset load\n";
        return false;
    }

    if (loadedAmp->params.count("calibrationInputLevel") || loadedAmp->params.count("calibrationOutputLevel"))
    {
        std::cerr << "Legacy NAM calibration params should be removed on preset load\n";
        return false;
    }

    return true;
}

bool TestLoadAppSettingsAppliesUserInputCalibrationProfile()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "user-input-calibration";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["audio.interfaceCalibration.enabled"] = true;
        settings["audio.interfaceCalibration.referenceDbu"] = 12.0;
        settings["audio.userInputCalibration.profiles"] = nlohmann::json::array({
            {
                {"id", "guitar-x"},
                {"name", "Guitar X, Interface Gain at 0"},
                {"description", "Bridge humbucker"},
                {"capturedPeakDbfs", -18.0},
                {"targetPeakDbfs", -12.0},
                {"gainDb", 6.0}
            }
        });
        settings["audio.userInputCalibration.activeProfileId"] = "guitar-x";

        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) > 1e-9)
    {
        std::cerr << "Active user input calibration gain was not applied from app settings\n";
        return false;
    }

    const auto& appSettings = controller.GetAppSettings();
    if (appSettings.contains("audio.interfaceCalibration.enabled")
        || appSettings.contains("audio.interfaceCalibration.referenceDbu"))
    {
        std::cerr << "Legacy interface calibration settings should be removed during app settings migration\n";
        return false;
    }

    std::ifstream input(settingsPath);
    const auto persisted = nlohmann::json::parse(input, nullptr, false);
    if (persisted.is_discarded())
    {
        std::cerr << "Failed to reload migrated app settings JSON\n";
        return false;
    }

    if (persisted.contains("audio.interfaceCalibration.enabled")
        || persisted.contains("audio.interfaceCalibration.referenceDbu"))
    {
        std::cerr << "Persisted app settings still contain legacy interface calibration keys\n";
        return false;
    }

    if (persisted.value("audio.userInputCalibration.activeProfileId", std::string{}) != "guitar-x")
    {
        std::cerr << "Persisted active user input calibration profile id mismatch\n";
        return false;
    }

    return true;
}

bool TestUserInputCalibrationTrainingBypassesActiveProfileWithoutPersistingSelection()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "user-input-calibration-training";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["audio.userInputCalibration.profiles"] = nlohmann::json::array({
            {
                {"id", "guitar-x"},
                {"name", "Guitar X, Interface Gain at 0"},
                {"description", "Bridge humbucker"},
                {"capturedPeakDbfs", -18.0},
                {"targetPeakDbfs", -12.0},
                {"gainDb", 6.0}
            }
        });
        settings["audio.userInputCalibration.activeProfileId"] = "guitar-x";

        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) > 1e-9)
    {
        std::cerr << "Initial user input calibration gain was not applied\n";
        return false;
    }

    controller.HandleUIMessage(nlohmann::json{{"type", "setUserInputCalibrationTrainingActive"}, {"active", true}}.dump());

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb()) > 1e-9)
    {
        std::cerr << "Training mode should bypass the active user input calibration gain\n";
        return false;
    }

    const auto& appSettings = controller.GetAppSettings();
    if (appSettings.value("audio.userInputCalibration.activeProfileId", std::string{}) != "guitar-x")
    {
        std::cerr << "Training mode should not clear the persisted active calibration id\n";
        return false;
    }

    std::ifstream duringTrainingInput(settingsPath);
    const auto duringTrainingPersisted = nlohmann::json::parse(duringTrainingInput, nullptr, false);
    if (duringTrainingPersisted.is_discarded())
    {
        std::cerr << "Failed to reload app settings during training bypass test\n";
        return false;
    }

    if (duringTrainingPersisted.value("audio.userInputCalibration.activeProfileId", std::string{}) != "guitar-x")
    {
        std::cerr << "Training mode should not rewrite the saved active calibration id\n";
        return false;
    }

    controller.HandleUIMessage(nlohmann::json{{"type", "setUserInputCalibrationTrainingActive"}, {"active", false}}.dump());

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) > 1e-9)
    {
        std::cerr << "Active user input calibration gain was not restored after training mode\n";
        return false;
    }

    return true;
}

bool TestSavePresetUsesGlobalChainLevels()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "level-save";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildPassthroughPreset("p-level-save", "Level Save");
    nlohmann::json loadMessage;
    loadMessage["type"] = "loadPreset";
    loadMessage["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    loadMessage["presetId"] = preset.id;
    controller.HandleUIMessage(loadMessage.dump());

    controller.HandleUIMessage(nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", -7.0}}.dump());
    controller.HandleUIMessage(nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "output.gain"}, {"value", -2.5}}.dump());
    controller.HandleUIMessage(nlohmann::json{{"type", "setAutoLevel"}, {"autoInput", true}, {"autoOutput", true}}.dump());

    const std::string saveId = "unit-test-level-save";
    controller.HandleUIMessage(nlohmann::json{
        {"type", "savePreset"},
        {"name", "Level Save"},
        {"category", "Unit"},
        {"description", "Unified level save test"},
        {"includeGlobalSignalChain", true},
        {"presetId", saveId}
    }.dump());

    const fs::path savedPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user" / (saveId + ".json");
    const auto fromFile = guitarfx::PresetStorage::LoadFromFile(savedPath);
    if (!fromFile)
    {
        std::cerr << "Failed to load saved preset for unified level test\n";
        return false;
    }

    if (std::abs(fromFile->global.inputTrim - 7.0) < 1e-9)
    {
        std::cerr << "Unexpected sign inversion while saving input trim\n";
        return false;
    }

    if (std::abs(fromFile->global.inputTrim - (-7.0)) > 1e-9
        || std::abs(fromFile->global.outputTrim - (-2.5)) > 1e-9)
    {
        std::cerr << "Saved preset did not persist current global chain gain values\n";
        return false;
    }

    if (fromFile->global.autoLevelInput || fromFile->global.autoLevelOutput)
    {
        std::cerr << "Saved preset should not persist retired mixer auto-level flags\n";
        return false;
    }

    if (!fromFile->globalSignalChain.has_value())
    {
        std::cerr << "Saved preset missing global signal chain after unified level save\n";
        return false;
    }

    if (std::abs(fromFile->globalSignalChain->inputGain - (-7.0)) > 1e-9
        || std::abs(fromFile->globalSignalChain->outputGain - (-2.5)) > 1e-9
        || fromFile->globalSignalChain->autoLevelInput
        || fromFile->globalSignalChain->autoLevelOutput)
    {
        std::cerr << "Saved global signal chain level state mismatch\n";
        return false;
    }

    return true;
}

bool TestOptimizedNamMetadataAliasParsing()
{
    nlohmann::json metadata = {
        {"input_level_dbu", 22.903},
        {"output_level_dbu", 13.303},
        {"loudness", -17.2881},
        {"modeled_by", "unit-test-author"}
    };

    const auto inputLevel = guitarfx::nam::ReadMetadataDouble(metadata, "input_level_dbu", "input_level");
    const auto outputLevel = guitarfx::nam::ReadMetadataDouble(metadata, "output_level_dbu", "output_level");
    const auto loudness = guitarfx::nam::ReadMetadataDouble(metadata, "loudness");
    const auto author = guitarfx::nam::ReadMetadataString(metadata, "author", "modeled_by");

    if (!inputLevel || !outputLevel || !loudness || !author)
    {
        std::cerr << "Optimized NAM metadata alias parsing returned empty values\n";
        return false;
    }

    if (std::abs(*inputLevel - 22.903) > 1e-9
        || std::abs(*outputLevel - 13.303) > 1e-9
        || std::abs(*loudness - (-17.2881)) > 1e-9
        || *author != "unit-test-author")
    {
        std::cerr << "Optimized NAM metadata alias parsing returned wrong values\n";
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

bool TestSaveAsCreatesNewPresetId()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "save-as";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const std::string sourceId = "unit-test-source-preset";
    const auto sourcePreset = BuildPreset(sourceId, "Source Preset");

    nlohmann::json saveSource;
    saveSource["type"] = "savePreset";
    saveSource["name"] = "Source Preset";
    saveSource["category"] = "Unit";
    saveSource["description"] = "Original preset";
    saveSource["presetId"] = sourceId;
    saveSource["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(sourcePreset));
    controller.HandleUIMessage(saveSource.dump());

    const fs::path presetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
    const fs::path sourcePath = presetDir / (sourceId + ".json");
    if (!fs::exists(sourcePath))
    {
        std::cerr << "Source preset file missing before save as: " << sourcePath.string() << "\n";
        return false;
    }

    auto saveAsPreset = sourcePreset;
    saveAsPreset.name = "Copied Preset";

    nlohmann::json saveAs;
    saveAs["type"] = "savePreset";
    saveAs["name"] = "Copied Preset";
    saveAs["category"] = "Unit";
    saveAs["description"] = "Save as copy";
    saveAs["presetId"] = sourceId;
    saveAs["saveMode"] = "save-as";
    saveAs["sourcePresetId"] = sourceId;
    saveAs["requireNewPresetId"] = true;
    saveAs["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(saveAsPreset));
    controller.HandleUIMessage(saveAs.dump());

    const auto savedMsg = FindLatestMessageOfType(host.sentMessages, "presetSaved");
    if (!savedMsg || !savedMsg->contains("preset"))
    {
        std::cerr << "presetSaved response missing after save as\n";
        return false;
    }

    const auto savedPresetJson = (*savedMsg)["preset"];
    const std::string savedId = savedPresetJson.value("id", "");
    if (savedId.empty() || savedId == sourceId)
    {
        std::cerr << "Save As reused the source preset id\n";
        return false;
    }

    const fs::path savedPath = presetDir / (savedId + ".json");
    if (!fs::exists(savedPath))
    {
        std::cerr << "Save As preset file missing: " << savedPath.string() << "\n";
        return false;
    }

    const auto reloadedSource = guitarfx::PresetStorage::LoadFromFile(sourcePath);
    if (!reloadedSource || reloadedSource->id != sourceId || reloadedSource->name != "Source Preset")
    {
        std::cerr << "Source preset was modified by save as\n";
        return false;
    }

    const auto reloadedCopy = guitarfx::PresetStorage::LoadFromFile(savedPath);
    if (!reloadedCopy || reloadedCopy->id != savedId || reloadedCopy->name != "Copied Preset")
    {
        std::cerr << "Saved copy contents mismatch after save as\n";
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
    run("Load preset rehydrates scrubbed hosted plugin state", TestLoadPresetRehydratesScrubbedHostedPluginState());
    run("Load preset rehydrates scrubbed hosted plugin state from active preset", TestLoadPresetRehydratesScrubbedHostedPluginStateFromActivePreset());
    run("Load preset remaps hosted plugin resource by stable id", TestLoadPresetRemapsHostedPluginResourceByStableId());
    run("Load preset restores unified level state", TestLoadPresetRestoresUnifiedLevelState());
    run("Load preset retires NAM input auto-leveling", TestLoadPresetRetiresNamInputAutoLeveling());
    run("Load app settings applies user input calibration", TestLoadAppSettingsAppliesUserInputCalibrationProfile());
    run("User input calibration training bypasses active profile", TestUserInputCalibrationTrainingBypassesActiveProfileWithoutPersistingSelection());
    run("Save preset uses global chain levels", TestSavePresetUsesGlobalChainLevels());
    run("Save/Get/Delete preset workflow", TestSaveGetDeletePresetWorkflow());
    run("Save As creates new preset id", TestSaveAsCreatesNewPresetId());
    run("Factory preset archive startup import", TestFactoryPresetArchiveStartupImport());
    run("Riff library path normalization", TestRiffLibraryPathNormalization());
    run("Optimized NAM metadata alias parsing", TestOptimizedNamMetadataAliasParsing());

    std::cout << "\nPreset management workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
