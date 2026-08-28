#include <cstdlib>
#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/NAMSampleRate.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "util/Base64.h"

namespace fs = std::filesystem;

namespace
{
/// Reads persisted state back out of a sandbox's document store. Persistence
/// moved from a JSON file tree into SQLite, so assertions that used to reopen
/// app.json or a preset file go through here instead. The controller under test
/// must be destroyed (or at least done writing) first.
class StoreReader
{
  public:
    explicit StoreReader(const fs::path& sandbox)
    {
        std::string error;
        mOpen = mStore.Open(sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db", error);
        if (!mOpen)
        {
            std::cerr << "StoreReader could not open the store: " << error << "\n";
        }
    }

    [[nodiscard]] bool Ok() const
    {
        return mOpen;
    }

    /// app.json's contents, reassembled from the one-row-per-key layout.
    [[nodiscard]] nlohmann::json AppSettings()
    {
        nlohmann::json settings = nlohmann::json::object();
        for (const auto& item : mStore.List(guitarfx::storage::ItemType::kSetting))
        {
            if (auto parsed = item.Parse())
            {
                settings[item.id] = std::move(*parsed);
            }
        }
        return settings;
    }

    [[nodiscard]] std::optional<guitarfx::Preset> Preset(const std::string& id)
    {
        return guitarfx::PresetStorage::LoadFromStore(mStore, id);
    }

    [[nodiscard]] bool HasPreset(const std::string& id)
    {
        return guitarfx::PresetStorage::ExistsInStore(mStore, id);
    }

    [[nodiscard]] nlohmann::json Document(const std::string& id)
    {
        return mStore.Get(guitarfx::storage::ItemType::kDocument, id).value_or(nlohmann::json::object());
    }

    /// Writes a preset directly, for tests that need to simulate an edit made
    /// outside the controller.
    [[nodiscard]] bool SavePreset(const guitarfx::Preset& preset)
    {
        return guitarfx::PresetStorage::SaveToStore(mStore, preset);
    }

  private:
    guitarfx::storage::JsonStore mStore;
    bool mOpen = false;
};

class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath, fs::path bundledAssetsPath = {}, bool standalone = false)
        : mUserDataPath(std::move(userDataPath)),
          mBundledAssetsPath(bundledAssetsPath.empty() ? mUserDataPath : std::move(bundledAssetsPath)),
          mStandalone(standalone)
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        sentMessages.push_back(jsonMessage);
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

    [[nodiscard]] bool IsStandalone() const override
    {
        return mStandalone;
    }

    void NotifyStateChanged() override
    {
        ++stateChangedCount;
    }

    int stateChangedCount = 0;
    std::vector<std::string> sentMessages;

  private:
    fs::path mUserDataPath;
    fs::path mBundledAssetsPath;
    bool mStandalone = false;
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

std::optional<nlohmann::json> FindLatestMessageOfType(const std::vector<std::string>& messages, const std::string& type)
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
        const fs::path sandbox =
            fs::temp_directory_path() / "guitarfx-preset-management-tests" / "riff-path-normalization";
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
            index["riffs"] = nlohmann::json::array(
                {{{"id", "riff-1"},
                  {"title", "Riff One"},
                  {"favorite", false},
                  {"used", false},
                  {"takes", nlohmann::json::array(
                                {{{"id", "take-1"}, {"filePath", takePath.string()}, {"durationSec", 1.0}}})}}});
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

        StoreReader reader(sandbox);
        if (!reader.Ok())
        {
            return false;
        }
        const nlohmann::json storedIndex = reader.Document("riff-library");

        if (!storedIndex.is_object() || storedIndex.empty())
        {
            std::cerr << "Riff index is missing or invalid after save\n";
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

bool TestPluginStateRestoresActiveScene()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "active-scene-state";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController source(host);
    source.Initialize();

    auto preset = BuildPassthroughPreset("p-scenes", "Scenes");
    guitarfx::NormalizePresetScenes(preset);
    auto secondScene = preset.scenes.front();
    secondScene.id = "scene-2";
    secondScene.title = "Scene 2";
    preset.scenes.push_back(std::move(secondScene));

    source.HandleUIMessage(nlohmann::json{
        {"type", "loadPreset"},
        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))},
        {"presetId", preset.id},
        {"sceneId", "scene-2"},
    }
                               .dump());

    const auto savedState = nlohmann::json::parse(source.SerializeState());
    if (savedState.value("activeSceneId", std::string{}) != "scene-2")
    {
        std::cerr << "Plugin state did not serialize the active scene\n";
        return false;
    }

    guitarfx::PluginController restored(host);
    restored.Initialize();
    restored.DeserializeState(savedState.dump());

    const auto restoredState = nlohmann::json::parse(restored.SerializeState());
    if (restoredState.value("activeSceneId", std::string{}) != "scene-2")
    {
        std::cerr << "Plugin state did not restore the active scene\n";
        return false;
    }

    return true;
}

bool TestPluginStateRestoresGlobalGate()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "plugin-global-gate-state";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    // Hosted plugin instance (standalone = false): global FX ride in host state.
    TestHost host(sandbox);
    guitarfx::PluginController source(host);
    source.Initialize();

    source.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.enabled"}, {"value", true}}.dump());
    source.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.threshold"}, {"value", -52.0}}.dump());
    source.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "eq.enabled"}, {"value", true}}.dump());

    const auto savedState = nlohmann::json::parse(source.SerializeState());

    guitarfx::PluginController restored(host);
    restored.Initialize();
    restored.DeserializeState(savedState.dump());

    const auto chain = restored.GetMixer().GetGlobalChainConfig();
    const auto* gate = chain.preChainGraph.FindNode("global_gate");
    if (!gate)
    {
        std::cerr << "Restored plugin state has no global gate node\n";
        return false;
    }
    if (!gate->enabled)
    {
        std::cerr << "Global gate was enabled before save but is disabled after reopening the plugin\n";
        return false;
    }
    const auto thresholdIt = gate->params.find("threshold");
    if (thresholdIt == gate->params.end() || std::abs(thresholdIt->second - (-52.0)) > 1e-9)
    {
        std::cerr << "Global gate threshold was not restored from plugin state\n";
        return false;
    }

    const auto* eq = chain.postChainGraph.FindNode("global_eq");
    if (!eq || !eq->enabled)
    {
        std::cerr << "Global EQ enable was not restored from plugin state\n";
        return false;
    }

    return true;
}

bool TestSetParameterMessageRoutesToGlobalChain()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "set-parameter-alias";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    // "setParameter" is the documented flat-name entry point. It carries a number for
    // every parameter, including the boolean and integer ones, and must land on the
    // same global chain state that setGlobalChainParam writes.
    controller.HandleUIMessage(nlohmann::json{{"type", "setParameter"}, {"name", "gateEnabled"}, {"value", 1}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setParameter"}, {"name", "gateThreshold"}, {"value", -47.5}}.dump());
    controller.HandleUIMessage(nlohmann::json{{"type", "setParameter"}, {"name", "transpose"}, {"value", -5}}.dump());
    // snake_case spelling: what the older UI sliders emitted.
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setParameter"}, {"name", "output_trim"}, {"value", -2.5}}.dump());
    // Unknown names must be ignored rather than throwing or writing anything.
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setParameter"}, {"name", "notAParameter"}, {"value", 1}}.dump());

    const auto chain = controller.GetMixer().GetGlobalChainConfig();

    const auto* gate = chain.preChainGraph.FindNode("global_gate");
    if (!gate || !gate->enabled)
    {
        std::cerr << "setParameter gateEnabled did not enable the global gate\n";
        return false;
    }

    const auto thresholdIt = gate->params.find("threshold");
    if (thresholdIt == gate->params.end() || std::abs(thresholdIt->second - (-47.5)) > 1e-9)
    {
        std::cerr << "setParameter gateThreshold did not reach the global gate\n";
        return false;
    }

    const auto* transpose = chain.preChainGraph.FindNode("global_transpose");
    const auto semitonesIt = transpose ? transpose->params.find("semitones") : decltype(transpose->params.end()){};
    if (!transpose || semitonesIt == transpose->params.end() || std::abs(semitonesIt->second - (-5.0)) > 1e-9)
    {
        std::cerr << "setParameter transpose did not reach the global transpose node\n";
        return false;
    }

    if (std::abs(chain.outputGain - (-2.5)) > 1e-9)
    {
        std::cerr << "setParameter output_trim (snake_case) did not reach the global output gain\n";
        return false;
    }

    return true;
}

bool TestPluginInstanceNamQualityNeverReachesSharedStore()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "instance-owned-nam-quality";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox); // standalone = false: NAM quality is instance-owned here.
    guitarfx::PluginController controller(host);
    controller.Initialize();

    double storedBefore = 0.0;
    {
        StoreReader reader(sandbox);
        if (!reader.Ok())
        {
            return false;
        }
        storedBefore = reader.AppSettings().value("audio.nam.oversampling", 0.0);
    }

    // Instance-owned: this instance's tier, never the machine's.
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setSetting"}, {"key", "audio.nam.oversampling"}, {"value", 3}}.dump());

    // A shared setting changed afterwards must still be persisted — and must not drag
    // the instance-owned key out to the store along with it.
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setSetting"}, {"key", "app.updateCheckEnabled"}, {"value", false}}.dump());

    StoreReader reader(sandbox);
    if (!reader.Ok())
    {
        return false;
    }
    const auto stored = reader.AppSettings();

    if (stored.value("app.updateCheckEnabled", true) != false)
    {
        std::cerr << "A shared setting changed after an instance-owned one was not persisted\n";
        return false;
    }

    if (std::abs(stored.value("audio.nam.oversampling", 0.0) - storedBefore) > 1e-9)
    {
        std::cerr << "Instance-owned NAM quality leaked into the shared store via an unrelated save\n";
        return false;
    }

    // The instance itself still runs at the tier the user picked.
    if (controller.GetAppSettings().value("audio.nam.oversampling", 0.0) != 3.0)
    {
        std::cerr << "Instance-owned NAM quality was not retained by the instance\n";
        return false;
    }

    return true;
}

bool TestHostStateRestoreDoesNotRepublishProjectSettings()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "host-state-no-republish";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);

    // The user's current, machine-wide choice.
    {
        guitarfx::PluginController seed(host);
        seed.Initialize();
        seed.HandleUIMessage(
            nlohmann::json{{"type", "setSetting"}, {"key", "audio.dsp.nominalOperatingLevelDbfs"}, {"value", -24.0}}
                .dump());
    }

    // A DAW project saved back when the setting was something else.
    nlohmann::json staleProject;
    staleProject["appSettings"] = nlohmann::json{{"audio.dsp.nominalOperatingLevelDbfs", -12.0}};

    guitarfx::PluginController restored(host);
    restored.Initialize();
    restored.DeserializeState(staleProject.dump());

    // Any later save at all is enough to publish the merged snapshot, if it was left
    // looking like this instance's own edits.
    restored.HandleUIMessage(
        nlohmann::json{{"type", "setSetting"}, {"key", "app.updateCheckEnabled"}, {"value", false}}.dump());

    StoreReader reader(sandbox);
    if (!reader.Ok())
    {
        return false;
    }
    const auto stored = reader.AppSettings();

    if (std::abs(stored.value("audio.dsp.nominalOperatingLevelDbfs", 0.0) - (-24.0)) > 1e-9)
    {
        std::cerr << "Reopening a stale project republished its settings over the shared store\n";
        return false;
    }

    if (stored.value("app.updateCheckEnabled", true) != false)
    {
        std::cerr << "A genuine setting change after a host-state restore was not persisted\n";
        return false;
    }

    return true;
}

bool TestHostStateRestoreAppliesMergedSettings()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "host-state-applies-settings";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    nlohmann::json state;
    state["appSettings"] = nlohmann::json{
        {"audio.dsp.nominalOperatingLevelDbfs", -12.0},
        {"audio.userInputCalibration.activeProfileId", "profile-a"},
        {"audio.userInputCalibration.profiles",
         nlohmann::json::array({nlohmann::json{{"id", "profile-a"}, {"name", "Profile A"}, {"gainDb", 4.5}}})},
    };

    controller.DeserializeState(state.dump());

    if (std::abs(guitarfx::GetNominalOperatingLevelDbfs() - (-12.0)) > 1e-9)
    {
        std::cerr << "Restored DSP level target was merged into settings but never applied\n";
        return false;
    }

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 4.5) > 1e-9)
    {
        std::cerr << "Restored user input calibration was merged into settings but never applied\n";
        return false;
    }

    return true;
}

bool TestPluginInstanceDoesNotOwnLastPresetId()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "last-preset-ownership";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    auto appPreset = BuildPassthroughPreset("p-app", "App Preset");
    auto daw = BuildPassthroughPreset("p-daw", "DAW Preset");
    {
        StoreReader writer(sandbox);
        if (!writer.Ok() || !writer.SavePreset(appPreset) || !writer.SavePreset(daw))
        {
            return false;
        }
    }

    // Standalone owns the machine-wide "last preset".
    {
        TestHost appHost(sandbox, {}, /*standalone=*/true);
        guitarfx::PluginController app(appHost);
        app.Initialize();
        app.HandleUIMessage(nlohmann::json{
            {"type", "loadPreset"},
            {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(appPreset))},
            {"presetId",
             appPreset.id}}.dump());
    }

    {
        StoreReader reader(sandbox);
        if (reader.AppSettings().value("lastPresetId", std::string{}) != "p-app")
        {
            std::cerr << "Standalone should record the last preset it loaded\n";
            return false;
        }
    }

    // A hosted instance seeds from it...
    TestHost host(sandbox);
    guitarfx::PluginController plugin(host);
    plugin.Initialize();
    if (!plugin.GetActivePreset() || plugin.GetActivePreset()->id != "p-app")
    {
        std::cerr << "A new plugin instance should seed from the shared last preset\n";
        return false;
    }

    // ...but loading a preset inside the DAW is the project's business, not the machine's.
    plugin.HandleUIMessage(nlohmann::json{
        {"type", "loadPreset"},
        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(daw))},
        {"presetId", daw.id}}.dump());

    StoreReader reader(sandbox);
    if (reader.AppSettings().value("lastPresetId", std::string{}) != "p-app")
    {
        std::cerr << "A plugin instance overwrote the shared last preset with its own choice\n";
        return false;
    }

    // The instance itself still switched, and carries it in host state.
    const auto hostState = nlohmann::json::parse(plugin.SerializeState());
    if (hostState.value("presetId", std::string{}) != "p-daw")
    {
        std::cerr << "Plugin instance did not carry its own preset choice in host state\n";
        return false;
    }

    return true;
}

// The DAW hands a plugin instance its editor size back when the project - or just the
// editor window - is reopened, so a resize has to survive a host state round trip. The
// sizes a host reports while it tears the window down must not, or the next open comes
// back at the editor's minimum size instead of the one the user chose.
bool TestPluginStateRemembersEditorWindowSize()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "editor-window-size";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController source(host);
    source.Initialize();

    // The editor drives OnIdle, so an editor that is open and settled is two idle ticks.
    const auto settle = [&source] {
        source.OnIdle();
        source.OnIdle();
    };

    // A brand-new instance has no remembered size; the wrapper opens at its default.
    if (source.GetEditorWindowSize().IsValid())
    {
        std::cerr << "A new plugin instance should not claim a remembered editor size\n";
        return false;
    }

    const int changesBefore = host.stateChangedCount;

    source.SetEditorWindowSize(1536, 1024);

    // Not committed on the spot: the host has to be given a chance to take it back.
    if (source.GetEditorWindowSize().IsValid())
    {
        std::cerr << "An editor size was committed before it had settled\n";
        return false;
    }
    source.OnIdle();
    if (host.stateChangedCount != changesBefore)
    {
        std::cerr << "Editor resize notified the host before the drag had settled\n";
        return false;
    }

    source.OnIdle();
    if (source.GetEditorWindowSize().width != 1536 || source.GetEditorWindowSize().height != 1024)
    {
        std::cerr << "A settled editor resize was never remembered\n";
        return false;
    }
    if (host.stateChangedCount != changesBefore + 1)
    {
        std::cerr << "Editor resize never told the host its saved state was stale\n";
        return false;
    }
    settle();
    if (host.stateChangedCount != changesBefore + 1)
    {
        std::cerr << "A settled editor size kept notifying the host\n";
        return false;
    }

    // Out-of-range layout passes are dropped outright.
    source.SetEditorWindowSize(0, 0);
    source.SetEditorWindowSize(-1, 900);
    source.SetEditorWindowSize(200000, 200000);
    settle();
    if (source.GetEditorWindowSize().width != 1536 || source.GetEditorWindowSize().height != 1024)
    {
        std::cerr << "An out-of-range layout pass overwrote the remembered editor size\n";
        return false;
    }

    // The regression: closing the window makes the host resize the editor, and the
    // editor's own constrainer clamps a degenerate rect up to its minimum size, which is
    // in range and looks entirely plausible. The editor is destroyed immediately after, so
    // it never idles again - and a size that never settles must never be remembered.
    source.SetEditorWindowSize(640, 400);
    if (source.GetEditorWindowSize().width != 1536 || source.GetEditorWindowSize().height != 1024)
    {
        std::cerr << "A teardown resize replaced the size the user chose\n";
        return false;
    }

    const auto savedState = nlohmann::json::parse(source.SerializeState());
    if (savedState["editorWindow"].value("width", 0) != 1536 || savedState["editorWindow"].value("height", 0) != 1024)
    {
        std::cerr << "Host state did not carry the editor window size\n";
        return false;
    }

    guitarfx::PluginController restored(host);
    restored.Initialize();
    restored.DeserializeState(savedState.dump());

    // Readable the moment the editor is constructed, without waiting for an idle tick.
    const auto restoredSize = restored.GetEditorWindowSize();
    if (restoredSize.width != 1536 || restoredSize.height != 1024)
    {
        std::cerr << "Reopening the project did not restore the editor window size\n";
        return false;
    }

    return true;
}

bool TestUiLayoutIsNotSharedBetweenPluginAndStandalone()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "ui-layout-ownership";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    // The standalone app owns the machine-wide layout.
    {
        TestHost appHost(sandbox, {}, /*standalone=*/true);
        guitarfx::PluginController app(appHost);
        app.Initialize();
        app.HandleUIMessage(nlohmann::json{{"type", "uiSettingsChanged"},
                                           {"settings", nlohmann::json{{"zoom", 1.0}, {"signalPathHeight", 300}}}}
                                .dump());
    }

    {
        StoreReader reader(sandbox);
        const auto stored = reader.AppSettings();
        if (std::abs(stored["uiSettings"].value("zoom", 0.0) - 1.0) > 1e-9)
        {
            std::cerr << "Standalone should persist UI layout to the shared store\n";
            return false;
        }
    }

    TestHost host(sandbox);
    guitarfx::PluginController plugin(host);
    plugin.Initialize();

    // A new instance is seeded from the shared layout...
    if (std::abs(plugin.GetAppSettings()["uiSettings"].value("zoom", 0.0) - 1.0) > 1e-9)
    {
        std::cerr << "A new plugin instance should seed its layout from the shared store\n";
        return false;
    }

    // ...but resizing the editor in a DAW is the project's business.
    plugin.HandleUIMessage(nlohmann::json{{"type", "uiSettingsChanged"},
                                          {"settings", nlohmann::json{{"zoom", 1.75}, {"signalPathHeight", 640}}}}
                               .dump());

    StoreReader reader(sandbox);
    const auto stored = reader.AppSettings();
    if (std::abs(stored["uiSettings"].value("zoom", 0.0) - 1.0) > 1e-9 ||
        stored["uiSettings"].value("signalPathHeight", 0) != 300)
    {
        std::cerr << "A plugin instance's editor layout leaked into the shared store\n";
        return false;
    }

    // The instance keeps its own layout, and persists it in host state.
    const auto hostState = nlohmann::json::parse(plugin.SerializeState());
    if (std::abs(hostState["uiSettings"].value("zoom", 0.0) - 1.75) > 1e-9)
    {
        std::cerr << "Plugin instance did not carry its editor layout in host state\n";
        return false;
    }

    // A shared-settings reload from another instance must not resize this editor.
    plugin.HandleUIMessage(nlohmann::json{{"type", "getSharedSyncState"}}.dump());
    const auto afterReload = nlohmann::json::parse(plugin.SerializeState());
    if (std::abs(afterReload["uiSettings"].value("zoom", 0.0) - 1.75) > 1e-9)
    {
        std::cerr << "A shared-settings reload clobbered the plugin instance's editor layout\n";
        return false;
    }

    return true;
}

bool TestSharedSyncReloadAppliesSharedSettingsAndKeepsInstanceOwned()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "shared-reload-apply";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController plugin(host);
    plugin.Initialize();

    // This instance picks its own NAM tier. Instance-owned: nobody else may move it.
    plugin.HandleUIMessage(
        nlohmann::json{{"type", "setSetting"}, {"key", "audio.nam.oversampling"}, {"value", 3}}.dump());

    // Another instance changes a genuinely shared setting.
    {
        TestHost otherHost(sandbox, {}, /*standalone=*/true);
        guitarfx::PluginController other(otherHost);
        other.Initialize();
        other.HandleUIMessage(
            nlohmann::json{{"type", "setSetting"}, {"key", "audio.dsp.nominalOperatingLevelDbfs"}, {"value", -27.0}}
                .dump());
    }

    // The level target lives in process-global storage, which the other controller above
    // already wrote on its way past. Knock it off that value first, or this asserts what
    // that controller did rather than what the reload does.
    guitarfx::SetNominalOperatingLevelDbfs(guitarfx::kDefaultNominalOperatingLevelDbfs);

    plugin.HandleUIMessage(nlohmann::json{{"type", "getSharedSyncState"}}.dump());

    // The shared change has to reach the DSP, not just mAppSettings — a reload that only
    // merged would leave the engine on the old target while the UI showed the new one.
    if (std::abs(guitarfx::GetNominalOperatingLevelDbfs() - (-27.0)) > 1e-9)
    {
        std::cerr << "Shared-sync reload merged a shared setting but never applied it\n";
        return false;
    }

    // ...and must not drag this instance's own tier along with it.
    if (plugin.GetAppSettings().value("audio.nam.oversampling", 0.0) != 3.0)
    {
        std::cerr << "Shared-sync reload clobbered this instance's NAM tier\n";
        return false;
    }

    return true;
}

bool TestLoadPresetRehydratesScrubbedHostedPluginState()
{
    try
    {
        const fs::path sandbox =
            fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-rehydrate";
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
        const fs::path sandbox =
            fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-rehydrate-active";
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
        std::cerr << "Exception in TestLoadPresetRehydratesScrubbedHostedPluginStateFromActivePreset: " << ex.what()
                  << "\n";
        return false;
    }
}

bool TestLoadPresetRemapsHostedPluginResourceByStableId()
{
    try
    {
        const fs::path sandbox =
            fs::temp_directory_path() / "guitarfx-preset-management-tests" / "hosted-plugin-id-remap";
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

bool TestLoadPresetPreservesInstanceGlobalFxState()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "global-fx-preset-load";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", -3.25}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "output.gain"}, {"value", -1.5}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.enabled"}, {"value", true}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.threshold"}, {"value", -52.0}}.dump());

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
    if (std::abs(chain.inputGain - (-3.25)) > 1e-9 || std::abs(chain.outputGain - (-1.5)) > 1e-9)
    {
        std::cerr << "Preset load should not replace instance global input/output levels\n";
        return false;
    }

    if (chain.autoLevelInput || chain.autoLevelOutput)
    {
        std::cerr << "Legacy mixer auto-level should be retired on preset load\n";
        return false;
    }

    const auto* gate = chain.preChainGraph.FindNode("global_gate");
    if (!gate || !gate->enabled || std::abs(gate->params.at("threshold") - (-52.0)) > 1e-9)
    {
        std::cerr << "Preset load should not replace instance global gate state\n";
        return false;
    }

    // Re-read the chain rather than a shadow copy: it is the only record of these values.
    const auto rechecked = controller.GetMixer().GetGlobalChainConfig();
    if (std::abs(rechecked.inputGain - (-3.25)) > 1e-9 || std::abs(rechecked.outputGain - (-1.5)) > 1e-9)
    {
        std::cerr << "Global chain config did not retain the preserved input/output levels\n";
        return false;
    }

    if (active->global.autoLevelInput || active->global.autoLevelOutput)
    {
        std::cerr << "Active preset still carries retired mixer auto-level flags\n";
        return false;
    }

    return true;
}

bool TestStandalonePersistsGlobalFxSettingsBetweenLaunches()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "standalone-global-fx";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    {
        TestHost host(sandbox, {}, true);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", -6.0}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "output.gain"}, {"value", -2.0}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.enabled"}, {"value", true}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "gate.threshold"}, {"value", -48.0}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "transpose.enabled"}, {"value", true}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "transpose.semitones"}, {"value", 5}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "eq.enabled"}, {"value", true}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "eq.lowGain"}, {"value", 1.75}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "eq.highMidQ"}, {"value", 1.4}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "doubler.enabled"}, {"value", true}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "doubler.delay"}, {"value", 31.0}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "doubler.mix"}, {"value", 0.35}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "doubler.detune"}, {"value", 7.5}}.dump());
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "limiter.enabled"}, {"value", true}}.dump());
    }

    {
        StoreReader reader(sandbox);
        if (!reader.Ok())
        {
            return false;
        }
        if (!reader.AppSettings().contains("globalFx.settings"))
        {
            std::cerr << "Standalone global FX settings were not written to app settings\n";
            return false;
        }
    }

    TestHost reloadedHost(sandbox, {}, true);
    guitarfx::PluginController reloaded(reloadedHost);
    reloaded.Initialize();

    const auto chain = reloaded.GetMixer().GetGlobalChainConfig();
    if (std::abs(chain.inputGain - (-6.0)) > 1e-9 || std::abs(chain.outputGain - (-2.0)) > 1e-9)
    {
        std::cerr << "Standalone global input/output levels were not restored\n";
        return false;
    }

    const auto* gate = chain.preChainGraph.FindNode("global_gate");
    if (!gate || !gate->enabled || std::abs(gate->params.at("threshold") - (-48.0)) > 1e-9)
    {
        std::cerr << "Standalone global gate settings were not restored\n";
        return false;
    }

    const auto* transpose = chain.preChainGraph.FindNode("global_transpose");
    if (!transpose || !transpose->enabled || std::abs(transpose->params.at("semitones") - 5.0) > 1e-9)
    {
        std::cerr << "Standalone global transpose settings were not restored\n";
        return false;
    }

    const auto* eq = chain.postChainGraph.FindNode("global_eq");
    if (!eq || !eq->enabled || std::abs(eq->params.at("lowGain") - 1.75) > 1e-9 ||
        std::abs(eq->params.at("highMidQ") - 1.4) > 1e-9)
    {
        std::cerr << "Standalone global EQ settings were not restored\n";
        return false;
    }

    const auto* doubler = chain.postChainGraph.FindNode("global_doubler");
    if (!doubler || !doubler->enabled || std::abs(doubler->params.at("time") - 31.0) > 1e-9 ||
        std::abs(doubler->params.at("mix") - 0.35) > 1e-9 || std::abs(doubler->params.at("detune") - 7.5) > 1e-9)
    {
        std::cerr << "Standalone global doubler settings were not restored\n";
        return false;
    }

    if (chain.limiterEnabled)
    {
        std::cerr
            << "Limiter should not be part of standalone global FX persistence; it is mixer state, not pre/post FX\n";
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
    auto* ampNode = preset.graph.FindNode("amp");
    if (!ampNode)
    {
        std::cerr << "Failed to build NAM test node\n";
        return false;
    }

    // Set legacy params that should be normalized away
    ampNode->params["autoLevelInput"] = 1.0;
    ampNode->params["autoLevelOutput"] = 1.0;
    ampNode->params["calibrationInputLevel"] = -12.0;
    ampNode->params["calibrationOutputLevel"] = -6.0;

    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    message["presetId"] = preset.id;
    controller.HandleUIMessage(message.dump());

    const auto& active = controller.GetActivePreset();
    if (!active)
    {
        std::cerr << "No active preset after NAM migration load\n";
        return false;
    }

    const auto* loadedAmp = active->graph.FindNode("amp");
    if (!loadedAmp)
    {
        std::cerr << "Loaded preset missing NAM node\n";
        return false;
    }

    // autoLevelInput should have been replaced by useCalibration
    if (loadedAmp->params.count("autoLevelInput"))
    {
        std::cerr << "autoLevelInput should be removed on preset load\n";
        return false;
    }
    if (loadedAmp->params.count("autoLevelOutput"))
    {
        std::cerr << "autoLevelOutput should be removed on preset load\n";
        return false;
    }

    const auto calIt = loadedAmp->params.find("useCalibration");
    if (calIt == loadedAmp->params.end() || std::abs(calIt->second - 1.0) > 1e-9)
    {
        std::cerr << "NAM useCalibration should default to enabled on preset load\n";
        return false;
    }

    if (loadedAmp->params.count("calibrationInputLevel") || loadedAmp->params.count("calibrationOutputLevel"))
    {
        std::cerr << "Legacy NAM calibration params should be removed on preset load\n";
        return false;
    }

    return true;
}

bool TestLoadPresetPreservesDisabledNamCalibrationToggle()
{
    const auto tempRoot = fs::temp_directory_path() / "soundshed_guitar_test_nam_use_calibration";
    std::error_code ec;
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot);
    SetSettingsEnvRoot(tempRoot);

    TestHost host(tempRoot);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildPreset("p-nam-use-calibration-disabled", "NAM Use Calibration Disabled");
    auto* ampNode = preset.graph.FindNode("amp");
    if (!ampNode)
    {
        std::cerr << "Failed to build NAM test node\n";
        return false;
    }

    ampNode->params["useCalibration"] = 0.0;
    ampNode->params["calibrationInputLevel"] = -12.0;

    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    message["presetId"] = preset.id;
    controller.HandleUIMessage(message.dump());

    const auto& active = controller.GetActivePreset();
    if (!active)
    {
        std::cerr << "No active preset after NAM toggle-preservation load\n";
        return false;
    }

    const auto* loadedAmp = active->graph.FindNode("amp");
    if (!loadedAmp)
    {
        std::cerr << "Loaded preset missing NAM node\n";
        return false;
    }

    const auto calIt = loadedAmp->params.find("useCalibration");
    if (calIt == loadedAmp->params.end() || std::abs(calIt->second - 0.0) > 1e-9)
    {
        std::cerr << "NAM useCalibration should preserve an explicit disabled state on preset load\n";
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
        settings["audio.userInputCalibration.profiles"] =
            nlohmann::json::array({{{"id", "guitar-x"},
                                    {"name", "Guitar X, Interface Gain at 0"},
                                    {"description", "Bridge humbucker"},
                                    {"capturedPeakDbfs", -18.0},
                                    {"targetPeakDbfs", -12.0},
                                    {"gainDb", 6.0}}});
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
    if (appSettings.contains("audio.interfaceCalibration.enabled") ||
        appSettings.contains("audio.interfaceCalibration.referenceDbu"))
    {
        std::cerr << "Legacy interface calibration settings should be removed during app settings migration\n";
        return false;
    }

    StoreReader migratedReader(sandbox);
    if (!migratedReader.Ok())
    {
        return false;
    }
    const auto persisted = migratedReader.AppSettings();

    if (persisted.contains("audio.interfaceCalibration.enabled") ||
        persisted.contains("audio.interfaceCalibration.referenceDbu"))
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

bool TestLoadAppSettingsPrunesUnusedLegacyKeys()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "app-settings-cleanup";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["appSettings"] = nlohmann::json::object({{"legacy", true}});
        settings["audioSettings"] = nlohmann::json::object({{"autoLevelInput", true}});
        settings["lastPresetJson"] = "{}";
        settings["parameters"] = nlohmann::json::array({0, 1, 2});
        settings["metronomeEnabled"] = false;
        settings["performancePads.open"] = false;
        settings["toneSharing.apiBase"] = "https://api-guitar.soundshed.com/v1";
        settings["ui.experimentalFeaturesEnabled"] = true;
        settings["audio.processing.namMonoOnly"] = false;
        settings["app.lastUpdateCheck"] = 1234;
        settings["globalFx.settings"] = guitarfx::GlobalSignalChainConfig{};
        settings["globalSignalChain"] = guitarfx::GlobalSignalChainConfig{};
        settings["metronome.bpm"] = 120.0;
        settings["metronomeBpm"] = 140.0;
        settings["theme"] = "dark";

        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    TestHost host(sandbox, {}, true);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto& appSettings = controller.GetAppSettings();
    const std::array<const char*, 12> removedKeys = {"appSettings",
                                                     "audioSettings",
                                                     "lastPresetJson",
                                                     "parameters",
                                                     "metronomeEnabled",
                                                     "performancePads.open",
                                                     "toneSharing.apiBase",
                                                     "ui.experimentalFeaturesEnabled",
                                                     "audio.processing.namMonoOnly",
                                                     "app.lastUpdateCheck",
                                                     "globalSignalChain",
                                                     "metronomeBpm"};
    for (const auto* key : removedKeys)
    {
        if (appSettings.contains(key))
        {
            std::cerr << "Legacy key should be removed from in-memory app settings: " << key << "\n";
            return false;
        }
    }

    if (!appSettings.contains("globalFx.settings") || !appSettings.contains("theme"))
    {
        std::cerr << "Cleanup removed active app settings unexpectedly\n";
        return false;
    }

    StoreReader cleanedReader(sandbox);
    if (!cleanedReader.Ok())
    {
        return false;
    }
    const auto persisted = cleanedReader.AppSettings();

    for (const auto* key : removedKeys)
    {
        if (persisted.contains(key))
        {
            std::cerr << "Legacy key should be removed from persisted app settings: " << key << "\n";
            return false;
        }
    }

    return true;
}

bool TestUserInputCalibrationTrainingBypassesActiveProfileWithoutPersistingSelection()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "user-input-calibration-training";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["audio.userInputCalibration.profiles"] =
            nlohmann::json::array({{{"id", "guitar-x"},
                                    {"name", "Guitar X, Interface Gain at 0"},
                                    {"description", "Bridge humbucker"},
                                    {"capturedPeakDbfs", -18.0},
                                    {"targetPeakDbfs", -12.0},
                                    {"gainDb", 6.0}}});
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

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setUserInputCalibrationTrainingActive"}, {"active", true}}.dump());

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

    StoreReader trainingReader(sandbox);
    if (!trainingReader.Ok())
    {
        return false;
    }
    const auto duringTrainingPersisted = trainingReader.AppSettings();

    if (duringTrainingPersisted.value("audio.userInputCalibration.activeProfileId", std::string{}) != "guitar-x")
    {
        std::cerr << "Training mode should not rewrite the saved active calibration id\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setUserInputCalibrationTrainingActive"}, {"active", false}}.dump());

    if (std::abs(controller.GetMixer().GetUserInputCalibrationGainDb() - 6.0) > 1e-9)
    {
        std::cerr << "Active user input calibration gain was not restored after training mode\n";
        return false;
    }

    return true;
}

bool TestSavePresetDoesNotPersistGlobalFxSettings()
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

    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "input.gain"}, {"value", -7.0}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setGlobalChainParam"}, {"path", "output.gain"}, {"value", -2.5}}.dump());
    controller.HandleUIMessage(
        nlohmann::json{{"type", "setAutoLevel"}, {"autoInput", true}, {"autoOutput", true}}.dump());

    const std::string saveId = "unit-test-level-save";
    controller.HandleUIMessage(nlohmann::json{
        {"type", "savePreset"},
        {"name", "Level Save"},
        {"category", "Unit"},
        {"description", "Unified level save test"},
        {"includeGlobalSignalChain", true},
        {"presetId", saveId}}.dump());

    StoreReader levelReader(sandbox);
    const auto fromFile = levelReader.Ok() ? levelReader.Preset(saveId) : std::nullopt;
    if (!fromFile)
    {
        std::cerr << "Failed to load saved preset for unified level test\n";
        return false;
    }

    if (std::abs(fromFile->global.inputTrim) > 1e-9 || std::abs(fromFile->global.outputTrim) > 1e-9 ||
        fromFile->global.autoLevelInput || fromFile->global.autoLevelOutput || fromFile->globalSignalChain.has_value())
    {
        std::cerr << "Saved preset should not persist global FX state\n";
        return false;
    }

    return true;
}

bool TestOptimizedNamMetadataAliasParsing()
{
    nlohmann::json metadata = {{"input_level_dbu", 22.903},
                               {"output_level_dbu", 13.303},
                               {"loudness", -17.2881},
                               {"modeled_by", "unit-test-author"}};

    const auto inputLevel = guitarfx::nam::ReadMetadataDouble(metadata, "input_level_dbu", "input_level");
    const auto outputLevel = guitarfx::nam::ReadMetadataDouble(metadata, "output_level_dbu", "output_level");
    const auto loudness = guitarfx::nam::ReadMetadataDouble(metadata, "loudness");
    const auto author = guitarfx::nam::ReadMetadataString(metadata, "author", "modeled_by");

    if (!inputLevel || !outputLevel || !loudness || !author)
    {
        std::cerr << "Optimized NAM metadata alias parsing returned empty values\n";
        return false;
    }

    if (std::abs(*inputLevel - 22.903) > 1e-9 || std::abs(*outputLevel - 13.303) > 1e-9 ||
        std::abs(*loudness - (-17.2881)) > 1e-9 || *author != "unit-test-author")
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

    {
        StoreReader reader(sandbox);
        if (!reader.Ok())
        {
            return false;
        }
        if (!reader.HasPreset(saveId))
        {
            std::cerr << "Saved preset missing from the store: " << saveId << "\n";
            return false;
        }
    }

    StoreReader savedReader(sandbox);
    const auto fromStore = savedReader.Ok() ? savedReader.Preset(saveId) : std::nullopt;
    if (!fromStore || fromStore->id != saveId || fromStore->name != "Saved Preset")
    {
        std::cerr << "Saved preset contents mismatch\n";
        return false;
    }

    nlohmann::json get;
    get["type"] = "getPresetById";
    get["presetId"] = saveId;
    get["requestId"] = "preset-request-1";
    controller.HandleUIMessage(get.dump());

    const auto presetDataMsg = FindLatestMessageOfType(host.sentMessages, "presetData");
    if (!presetDataMsg || !presetDataMsg->contains("preset"))
    {
        std::cerr << "presetData response missing after getPresetById\n";
        return false;
    }

    const auto returnedPreset = (*presetDataMsg)["preset"];
    if (presetDataMsg->value("requestId", "") != "preset-request-1")
    {
        std::cerr << "getPresetById did not echo requestId\n";
        return false;
    }
    if (returnedPreset.value("id", "") != saveId)
    {
        std::cerr << "getPresetById returned wrong preset id\n";
        return false;
    }

    nlohmann::json remove;
    remove["type"] = "deletePreset";
    remove["presetId"] = saveId;
    controller.HandleUIMessage(remove.dump());

    {
        StoreReader reader(sandbox);
        if (!reader.Ok())
        {
            return false;
        }
        if (reader.HasPreset(saveId))
        {
            std::cerr << "Preset still in the store after deletePreset\n";
            return false;
        }
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

    {
        StoreReader reader(sandbox);
        if (!reader.Ok() || !reader.HasPreset(sourceId))
        {
            std::cerr << "Source preset missing from the store before save as: " << sourceId << "\n";
            return false;
        }
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

    StoreReader afterSaveAs(sandbox);
    if (!afterSaveAs.Ok())
    {
        return false;
    }

    if (!afterSaveAs.HasPreset(savedId))
    {
        std::cerr << "Save As preset missing from the store: " << savedId << "\n";
        return false;
    }

    const auto reloadedSource = afterSaveAs.Preset(sourceId);
    if (!reloadedSource || reloadedSource->id != sourceId || reloadedSource->name != "Source Preset")
    {
        std::cerr << "Source preset was modified by save as\n";
        return false;
    }

    const auto reloadedCopy = afterSaveAs.Preset(savedId);
    if (!reloadedCopy || reloadedCopy->id != savedId || reloadedCopy->name != "Copied Preset")
    {
        std::cerr << "Saved copy contents mismatch after save as\n";
        return false;
    }

    return true;
}

bool TestPresetArchiveSessionMode()
{
    try
    {
        const fs::path sandbox =
            fs::temp_directory_path() / "guitarfx-preset-management-tests" / "preset-archive-session";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        const fs::path userPresetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
        fs::create_directories(userPresetDir, ec);

        const auto normalPreset = BuildPreset("saved-preset", "Saved Preset");
        if (!guitarfx::PresetStorage::SaveToFile(normalPreset, userPresetDir / "saved-preset.json"))
        {
            std::cerr << "Failed to write normal preset fixture\n";
            return false;
        }

        nlohmann::json settings = nlohmann::json::object();
        settings["lastPresetId"] = "saved-preset";
        const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
        fs::create_directories(settingsPath.parent_path(), ec);
        {
            std::ofstream output(settingsPath);
            output << settings.dump(2);
        }

        auto archivePreset = BuildPreset("archive-preset", "Archive Preset");
        archivePreset.category = "Archive";
        nlohmann::json archive = nlohmann::json::object();
        archive["formatVersion"] = 1;
        archive["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(archivePreset));
        archive["resources"] = nlohmann::json::array();

        const auto archiveText = archive.dump(2);
        const auto archiveBytes =
            BuildStoredZip({{"preset.json", std::vector<std::uint8_t>(archiveText.begin(), archiveText.end())}});

        TestHost host(sandbox, {}, true);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        if (!controller.GetActivePreset() || controller.GetActivePreset()->id != "saved-preset")
        {
            std::cerr << "Normal preset was not restored before archive session start\n";
            return false;
        }

        nlohmann::json startMessage = nlohmann::json::object();
        startMessage["type"] = "startPresetArchiveSession";
        startMessage["fileName"] = "session-pack.soundshed.presets";
        startMessage["data"] = guitarfx::util::EncodeBase64(archiveBytes);
        controller.HandleUIMessage(startMessage.dump());

        const auto& sessionPreset = controller.GetActivePreset();
        if (!sessionPreset || sessionPreset->name != "Archive Preset")
        {
            std::cerr << "Archive session did not activate the archive preset\n";
            return false;
        }
        if (sessionPreset->id == "archive-preset")
        {
            std::cerr << "Archive session preset id was not session-scoped\n";
            return false;
        }

        nlohmann::json getList = nlohmann::json::object();
        getList["type"] = "getPresetList";
        controller.HandleUIMessage(getList.dump());
        const auto sessionPresetListMsg = FindLatestMessageOfType(host.sentMessages, "presetList");
        if (!sessionPresetListMsg)
        {
            std::cerr << "presetList missing during archive session\n";
            return false;
        }
        const auto sessionPresets = sessionPresetListMsg->value("presets", nlohmann::json::array());
        if (!sessionPresets.is_array() || sessionPresets.size() != 1 ||
            sessionPresets[0].value("source", "") != "session" ||
            sessionPresets[0].value("name", "") != "Archive Preset")
        {
            std::cerr << "Archive session preset list was not isolated to the archive presets\n";
            return false;
        }

        nlohmann::json saveMessage = nlohmann::json::object();
        saveMessage["type"] = "savePreset";
        saveMessage["presetId"] = sessionPreset->id;
        saveMessage["name"] = "Archive Preset Edited";
        saveMessage["category"] = sessionPreset->category;
        saveMessage["description"] = sessionPreset->description;
        saveMessage["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(*sessionPreset));
        controller.HandleUIMessage(saveMessage.dump());

        const fs::path normalPresetPath = userPresetDir / "saved-preset.json";
        const auto normalPresetAfterSave = guitarfx::PresetStorage::LoadFromFile(normalPresetPath);
        if (!normalPresetAfterSave || normalPresetAfterSave->name != "Saved Preset")
        {
            std::cerr << "Archive session save mutated the normal preset library\n";
            return false;
        }

        const auto savedSessionPreset = controller.GetActivePreset();
        if (!savedSessionPreset || savedSessionPreset->name != "Archive Preset Edited")
        {
            std::cerr << "Archive session save did not update the session preset\n";
            return false;
        }

        nlohmann::json endMessage = nlohmann::json::object();
        endMessage["type"] = "endPresetArchiveSession";
        controller.HandleUIMessage(endMessage.dump());

        const auto& restoredPreset = controller.GetActivePreset();
        if (!restoredPreset || restoredPreset->id != "saved-preset")
        {
            std::cerr << "Ending the archive session did not restore the normal preset\n";
            return false;
        }

        controller.HandleUIMessage(getList.dump());
        const auto restoredPresetListMsg = FindLatestMessageOfType(host.sentMessages, "presetList");
        if (!restoredPresetListMsg)
        {
            std::cerr << "presetList missing after archive session end\n";
            return false;
        }
        const auto restoredPresets = restoredPresetListMsg->value("presets", nlohmann::json::array());
        bool restoredNormalPreset = false;
        for (const auto& item : restoredPresets)
        {
            if (item.is_object() && item.value("id", "") == "saved-preset")
            {
                restoredNormalPreset = true;
                break;
            }
        }
        if (!restoredNormalPreset)
        {
            std::cerr << "Normal preset library did not return after archive session end\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestPresetArchiveSessionMode: " << ex.what() << "\n";
        return false;
    }
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
    archive["resources"] = nlohmann::json::array({{{"id", "archive-model"},
                                                   {"name", "Archive Model"},
                                                   {"category", "Archive"},
                                                   {"type", "nam"},
                                                   {"fileName", "archive-model.nam"},
                                                   {"hash", "test-hash"}}});
    archive["blends"] = nlohmann::json::array(
        {{{"id", "archive-blend"}, {"name", "Archive Blend"}, {"models", nlohmann::json::array({"archive-model"})}}});
    archive["presetFolders"] = nlohmann::json::array({{{"name", "High Gain"},
                                                       {"presetIds", nlohmann::json::array({preset.id})},
                                                       {"children", nlohmann::json::array()}}});

    const auto archiveText = archive.dump(2);
    const auto archiveBytes =
        BuildStoredZip({{"preset.json", std::vector<std::uint8_t>(archiveText.begin(), archiveText.end())},
                        {"resources/archive-model.nam", std::vector<std::uint8_t>{'n', 'a', 'm'}}});
    const fs::path archivePath = bundledAssets / "ui" / "presets" / "factory" / "bundle.soundshed.preset";
    {
        std::ofstream output(archivePath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(archiveBytes.data()),
                     static_cast<std::streamsize>(archiveBytes.size()));
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
    const auto resourceIt =
        std::find_if(resources.begin(), resources.end(),
                     [](const guitarfx::LibraryResource& resource) { return resource.name == "Archive Model"; });
    if (resourceIt == resources.end() || !fs::exists(resourceIt->filePath))
    {
        std::cerr << "Archive-backed factory resource was not imported into the runtime library\n";
        return false;
    }

    StoreReader archiveReader(sandbox);
    if (!archiveReader.Ok())
    {
        return false;
    }

    const auto persistedPreset = archiveReader.Preset("bundle__factory-archive-preset");
    if (!persistedPreset || persistedPreset->category != "Factory")
    {
        std::cerr << "Archive-backed factory preset was not persisted as a Factory preset\n";
        return false;
    }

    const nlohmann::json presetFoldersJson = archiveReader.Document("preset-folders");
    if (presetFoldersJson.empty())
    {
        std::cerr << "Factory preset folders document was not created\n";
        return false;
    }

    bool foundArchiveFolder = false;
    bool foundPresetInFolder = false;
    for (const auto& folder : presetFoldersJson.value("folders", nlohmann::json::array()))
    {
        if (!folder.is_object() || folder.value("name", "") != "High Gain")
        {
            continue;
        }

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
    if (!archiveReader.SavePreset(locallyModifiedPreset))
    {
        std::cerr << "Unable to modify persisted factory preset for hash check\n";
        return false;
    }

    {
        guitarfx::PluginController unchangedController(host);
        unchangedController.Initialize();
    }
    const auto unchangedPreset = archiveReader.Preset("bundle__factory-archive-preset");
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
    const auto updatedArchiveBytes = BuildStoredZip(
        {{"preset.json", std::vector<std::uint8_t>(updatedArchiveText.begin(), updatedArchiveText.end())},
         {"resources/archive-model.nam", std::vector<std::uint8_t>{'n', 'a', 'm', '2'}}});
    {
        std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(updatedArchiveBytes.data()),
                     static_cast<std::streamsize>(updatedArchiveBytes.size()));
    }

    {
        guitarfx::PluginController updatedController(host);
        updatedController.Initialize();
    }
    const auto reimportedPreset = archiveReader.Preset("bundle__factory-archive-preset");
    if (!reimportedPreset || reimportedPreset->name != "Archive Factory Preset Updated")
    {
        std::cerr << "Changed factory archive was not re-imported\n";
        return false;
    }

    return true;
}

bool TestStandaloneDeserializeStateIgnoresEmbeddedPresetSnapshot()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "standalone-deserialize-ignore";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["lastPresetId"] = "saved-preset";
        settings["audio.nam.interfaceCalibrationLevelDbu"] = 12.0;
        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    const fs::path userPresetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
    fs::create_directories(userPresetDir, ec);

    auto savedPreset = BuildPreset("saved-preset", "Saved Preset");
    auto* savedAmp = savedPreset.graph.FindNode("amp");
    if (!savedAmp)
    {
        std::cerr << "Saved preset is missing NAM node\n";
        return false;
    }
    savedAmp->params["drive"] = 0.25;
    if (!guitarfx::PresetStorage::SaveToFile(savedPreset, userPresetDir / "saved-preset.json"))
    {
        std::cerr << "Failed to write saved preset file\n";
        return false;
    }

    TestHost host(sandbox, {}, true);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto& restored = controller.GetActivePreset();
    if (!restored || restored->id != "saved-preset")
    {
        std::cerr << "Standalone startup did not restore the saved preset\n";
        return false;
    }

    const auto* restoredAmp = restored->graph.FindNode("amp");
    if (!restoredAmp)
    {
        std::cerr << "Standalone startup restored preset without NAM node\n";
        return false;
    }
    const auto restoredDriveIt = restoredAmp->params.find("drive");
    if (restoredDriveIt == restoredAmp->params.end() || std::abs(restoredDriveIt->second - 0.25) > 1e-9)
    {
        std::cerr << "Standalone startup restored unexpected drive value\n";
        return false;
    }

    auto unsavedSnapshot = savedPreset;
    unsavedSnapshot.name = "Unsaved Snapshot";
    auto* unsavedAmp = unsavedSnapshot.graph.FindNode("amp");
    if (!unsavedAmp)
    {
        std::cerr << "Unsaved snapshot is missing NAM node\n";
        return false;
    }
    unsavedAmp->params["drive"] = 0.75;

    nlohmann::json incomingState = nlohmann::json::object();
    incomingState["presetId"] = "saved-preset";
    incomingState["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(unsavedSnapshot));
    incomingState["appSettings"] = nlohmann::json::object({{"audio.nam.interfaceCalibrationLevelDbu", 6.0}});

    controller.DeserializeState(incomingState.dump());

    const auto& afterDeserialize = controller.GetActivePreset();
    if (!afterDeserialize)
    {
        std::cerr << "Active preset unexpectedly cleared after standalone deserialize\n";
        return false;
    }

    const auto* afterAmp = afterDeserialize->graph.FindNode("amp");
    if (!afterAmp)
    {
        std::cerr << "Active preset missing NAM node after standalone deserialize\n";
        return false;
    }

    const auto afterDriveIt = afterAmp->params.find("drive");
    if (afterDeserialize->name != "Saved Preset" || afterDriveIt == afterAmp->params.end() ||
        std::abs(afterDriveIt->second - 0.25) > 1e-9)
    {
        std::cerr << "Standalone deserialize should not apply embedded preset snapshot\n";
        return false;
    }

    const auto& appSettings = controller.GetAppSettings();
    if (std::abs(appSettings.value("audio.nam.interfaceCalibrationLevelDbu", 0.0) - 12.0) > 1e-9)
    {
        std::cerr << "Standalone deserialize should not overwrite app settings from host state\n";
        return false;
    }

    return true;
}

bool TestStandaloneStartupInputModeOverridesRestoredPreset()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-management-tests" / "standalone-input-mode-restore";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path settingsPath = sandbox / "Soundshed Guitar" / "data" / "v1" / "settings" / "app.json";
    fs::create_directories(settingsPath.parent_path(), ec);
    {
        nlohmann::json settings = nlohmann::json::object();
        settings["lastPresetId"] = "saved-preset";
        settings["inputChannel.monoMode"] = true;
        settings["inputChannel.mono"] = 1;
        std::ofstream output(settingsPath);
        output << settings.dump(2);
    }

    const fs::path userPresetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
    fs::create_directories(userPresetDir, ec);

    auto savedPreset = BuildPassthroughPreset("saved-preset", "Saved Preset");
    auto presetChain = guitarfx::GlobalSignalChainConfig::CreateDefault();
    presetChain.monoMode = false;
    presetChain.inputChannel = 0;
    savedPreset.globalSignalChain = presetChain;
    if (!guitarfx::PresetStorage::SaveToFile(savedPreset, userPresetDir / "saved-preset.json"))
    {
        std::cerr << "Failed to write saved preset file\n";
        return false;
    }

    TestHost host(sandbox, {}, true);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    if (!controller.GetMixer().IsMonoMode() || controller.GetMixer().GetInputChannel() != 1)
    {
        std::cerr << "Standalone startup restored preset input mode instead of selected app input channel\n";
        return false;
    }

    return true;
}

guitarfx::Preset BuildChainPreset(const std::string& id)
{
    using namespace guitarfx;

    Preset preset;
    preset.id = id;
    preset.name = "Chain";
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, out};
    for (const char* fxId : {"fx1", "fx2", "fx3"})
    {
        GraphNode fx;
        fx.id = fxId;
        fx.type = "gain";
        fx.category = "utility";
        fx.enabled = true;
        fx.params["gainDb"] = 0.0;
        preset.graph.nodes.push_back(fx);
    }

    preset.graph.edges = {
        {"__input__", "fx1", 0, 0, 1.0},
        {"fx1", "fx2", 0, 0, 1.0},
        {"fx2", "fx3", 0, 0, 1.0},
        {"fx3", "__output__", 0, 0, 1.0},
    };

    return preset;
}

std::vector<std::string> ChainOrder(const guitarfx::SignalGraph& graph)
{
    std::vector<std::string> order;
    std::string current = "__input__";
    for (std::size_t guard = 0; guard <= graph.nodes.size(); ++guard)
    {
        order.push_back(current);
        if (current == "__output__")
        {
            break;
        }

        std::string next;
        for (const auto& edge : graph.edges)
        {
            if (edge.from == current)
            {
                next = edge.to;
                break;
            }
        }
        if (next.empty())
        {
            break;
        }
        current = next;
    }
    return order;
}

const guitarfx::SignalGraph& ActiveEditGraph(const guitarfx::Preset& preset)
{
    if (!preset.scenes.empty())
    {
        return preset.scenes.front().graph;
    }
    return preset.graph;
}

bool ExpectChainOrder(const guitarfx::SignalGraph& graph, const std::vector<std::string>& expected,
                      const std::string& context)
{
    const auto actual = ChainOrder(graph);
    if (actual != expected)
    {
        std::cerr << context << ": unexpected chain order:";
        for (const auto& id : actual)
        {
            std::cerr << " " << id;
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool TestReorderSignalPathNodeFailuresDoNotCorruptGraph()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "reorder-node";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto preset = BuildChainPreset("p-reorder");
    nlohmann::json loadMessage;
    loadMessage["type"] = "loadPreset";
    loadMessage["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    loadMessage["presetId"] = preset.id;
    controller.HandleUIMessage(loadMessage.dump());

    if (!controller.GetActivePreset())
    {
        std::cerr << "No active preset after loadPreset\n";
        return false;
    }

    const std::vector<std::string> initialOrder = {"__input__", "fx1", "fx2", "fx3", "__output__"};
    if (!ExpectChainOrder(ActiveEditGraph(*controller.GetActivePreset()), initialOrder, "initial"))
    {
        return false;
    }

    // A drop onto an edge that no longer exists must be rejected without mutating the graph.
    nlohmann::json staleEdgeMove;
    staleEdgeMove["type"] = "reorderSignalPathNode";
    staleEdgeMove["nodeId"] = "fx2";
    staleEdgeMove["edge"] = {{"from", "ghost-a"}, {"to", "ghost-b"}, {"fromPort", 0}, {"toPort", 0}};
    controller.HandleUIMessage(staleEdgeMove.dump());
    if (!ExpectChainOrder(ActiveEditGraph(*controller.GetActivePreset()), initialOrder, "after stale edge move"))
    {
        return false;
    }

    // Dropping a node back onto its own outgoing connection is a no-op.
    nlohmann::json selfMove;
    selfMove["type"] = "reorderSignalPathNode";
    selfMove["nodeId"] = "fx2";
    selfMove["edge"] = {{"from", "fx2"}, {"to", "fx3"}, {"fromPort", 0}, {"toPort", 0}};
    controller.HandleUIMessage(selfMove.dump());
    if (!ExpectChainOrder(ActiveEditGraph(*controller.GetActivePreset()), initialOrder, "after self move"))
    {
        return false;
    }

    // A valid reorder must still work after the rejected attempts.
    nlohmann::json validMove;
    validMove["type"] = "reorderSignalPathNode";
    validMove["nodeId"] = "fx1";
    validMove["targetNodeId"] = "fx3";
    controller.HandleUIMessage(validMove.dump());
    if (!ExpectChainOrder(ActiveEditGraph(*controller.GetActivePreset()),
                          {"__input__", "fx2", "fx3", "fx1", "__output__"}, "after valid reorder"))
    {
        return false;
    }

    // Moving onto an explicit edge reference must splice the node into that edge.
    nlohmann::json edgeMove;
    edgeMove["type"] = "reorderSignalPathNode";
    edgeMove["nodeId"] = "fx1";
    edgeMove["edge"] = {{"from", "__input__"}, {"to", "fx2"}, {"fromPort", 0}, {"toPort", 0}};
    controller.HandleUIMessage(edgeMove.dump());
    if (!ExpectChainOrder(ActiveEditGraph(*controller.GetActivePreset()), initialOrder, "after edge move"))
    {
        return false;
    }

    return true;
}

// A setlist step is a preset *switch*, not a Multi-Rig add. Stepping the cursor must leave
// exactly one preset active in the mixer, no matter how many slots have been visited.
bool TestSetlistCursorSwitchesPresetWithoutStackingMixer()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "setlist-switch";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        const fs::path presetDir = sandbox / "Soundshed Guitar" / "data" / "v1" / "presets" / "user";
        fs::create_directories(presetDir, ec);

        for (const auto& [id, name] :
             std::vector<std::pair<std::string, std::string>>{{"setlist-a", "Setlist A"}, {"setlist-b", "Setlist B"}})
        {
            auto preset = BuildPassthroughPreset(id, name);
            guitarfx::NormalizePresetScenes(preset);
            if (!guitarfx::PresetStorage::SaveToFile(preset, presetDir / (id + ".json")))
            {
                std::cerr << "Failed to write setlist preset fixture " << id << "\n";
                return false;
            }
        }

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        nlohmann::json slots = nlohmann::json::array();
        slots.push_back(nlohmann::json{{"presetId", "setlist-a"}});
        slots.push_back(nlohmann::json{{"presetId", "setlist-b"}});

        nlohmann::json setlist;
        setlist["id"] = "setlist-1";
        setlist["name"] = "Set 1";
        setlist["bank"] = 1;
        setlist["slots"] = slots;

        nlohmann::json setSetlists;
        setSetlists["type"] = "setSetlists";
        setSetlists["activeSetlistId"] = "setlist-1";
        setSetlists["setlists"] = nlohmann::json::array({setlist});
        controller.HandleUIMessage(setSetlists.dump());

        const auto stepToSlot = [&](int index, const std::string& expectedPresetId) {
            nlohmann::json cursor;
            cursor["type"] = "setSetlistCursor";
            cursor["cursorIndex"] = index;
            controller.HandleUIMessage(cursor.dump());

            const auto& active = controller.GetActivePreset();
            if (!active || active->id != expectedPresetId)
            {
                std::cerr << "Setlist slot " << index << " did not become the active preset\n";
                return false;
            }

            const auto activeIds = controller.GetMixer().GetActivePresetIds();
            if (activeIds.size() != 1 || activeIds[0] != expectedPresetId)
            {
                std::cerr << "Setlist slot " << index << " left " << activeIds.size()
                          << " presets in the mixer instead of switching to one\n";
                return false;
            }

            const auto loadedMsg = FindLatestMessageOfType(host.sentMessages, "presetLoaded");
            if (!loadedMsg || loadedMsg->value("preset", nlohmann::json::object()).value("id", "") != expectedPresetId)
            {
                std::cerr << "Setlist slot " << index << " did not report presetLoaded to the UI\n";
                return false;
            }

            const auto cursorMsg = FindLatestMessageOfType(host.sentMessages, "setlistCursorChanged");
            if (!cursorMsg || cursorMsg->value("cursorIndex", -1) != index)
            {
                std::cerr << "Setlist slot " << index << " did not report setlistCursorChanged\n";
                return false;
            }

            return true;
        };

        if (!stepToSlot(0, "setlist-a"))
        {
            return false;
        }
        if (!stepToSlot(1, "setlist-b"))
        {
            return false;
        }
        if (!stepToSlot(0, "setlist-a"))
        {
            return false;
        }

        if (controller.GetSetlistCursorIndex() != 0)
        {
            std::cerr << "Setlist cursor index was not tracked\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestSetlistCursorSwitchesPresetWithoutStackingMixer: " << ex.what() << "\n";
        return false;
    }
}

// A preset switch cannot change the resource library, riff library, app settings or effect
// catalog, so its state broadcast must not carry them — they are ~99% of the full payload.
bool TestPresetSwitchBroadcastsLightState()
{
    try
    {
        const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preset-management-tests" / "light-state";
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);
        SetSettingsEnvRoot(sandbox);

        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        // "uiReady" marks the UI live and requests the initial full broadcast.
        controller.HandleUIMessage(nlohmann::json{{"type", "uiReady"}}.dump());
        controller.OnIdle();

        const auto fullState = FindLatestMessageOfType(host.sentMessages, "state");
        if (!fullState || !fullState->contains("resourceLibrary") || !fullState->contains("appSettings") ||
            !fullState->contains("riffLibrary"))
        {
            std::cerr << "Startup broadcast was not a full state payload\n";
            return false;
        }

        host.sentMessages.clear();

        const auto preset = BuildPassthroughPreset("light-state-preset", "Light State");
        nlohmann::json load;
        load["type"] = "loadPreset";
        load["presetId"] = preset.id;
        load["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
        controller.HandleUIMessage(load.dump());
        controller.OnIdle();

        const auto lightState = FindLatestMessageOfType(host.sentMessages, "state");
        if (!lightState)
        {
            std::cerr << "Preset load emitted no state broadcast\n";
            return false;
        }

        for (const char* heavyKey :
             {"resourceLibrary", "appSettings", "riffLibrary", "blendLibrary", "customEffectLibrary", "uiSettings",
              "uiViewState", "metronome", "environment", "automation"})
        {
            if (lightState->contains(heavyKey))
            {
                std::cerr << "Preset-switch state broadcast still carries '" << heavyKey << "'\n";
                return false;
            }
        }

        // The UI needs these on every switch: it reads activePresetId unconditionally,
        // requests the global chain with an extra round trip when it is missing, and
        // clears its archive-session state when that key is absent.
        for (const char* requiredKey : {"preset", "activePresetId", "activeSceneId", "mixer", "activePresetIds",
                                        "globalSignalChain", "presetArchiveSession"})
        {
            if (!lightState->contains(requiredKey))
            {
                std::cerr << "Preset-switch state broadcast is missing '" << requiredKey << "'\n";
                return false;
            }
        }

        if (FindLatestMessageOfType(host.sentMessages, "effectCatalog") ||
            FindLatestMessageOfType(host.sentMessages, "compositeLibrary"))
        {
            std::cerr << "Preset switch resent a static library the UI already has\n";
            return false;
        }

        // An explicit state request must still produce the full payload.
        host.sentMessages.clear();
        controller.HandleUIMessage(nlohmann::json{{"type", "requestState"}}.dump());
        controller.OnIdle();

        const auto refreshed = FindLatestMessageOfType(host.sentMessages, "state");
        if (!refreshed || !refreshed->contains("resourceLibrary") || !refreshed->contains("appSettings"))
        {
            std::cerr << "Explicit state request did not return the full payload\n";
            return false;
        }

        // A full request queued alongside a preset switch must win.
        host.sentMessages.clear();
        controller.HandleUIMessage(load.dump());
        controller.HandleUIMessage(nlohmann::json{{"type", "requestState"}}.dump());
        controller.OnIdle();

        const auto merged = FindLatestMessageOfType(host.sentMessages, "state");
        if (!merged || !merged->contains("resourceLibrary"))
        {
            std::cerr << "Full state request was downgraded by a concurrent preset switch\n";
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Exception in TestPresetSwitchBroadcastsLightState: " << ex.what() << "\n";
        return false;
    }
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

    run("Load preset via message", TestLoadPresetViaMessage());
    run("Plugin state restores active scene", TestPluginStateRestoresActiveScene());
    run("Plugin state restores global gate", TestPluginStateRestoresGlobalGate());
    run("setParameter routes to global chain", TestSetParameterMessageRoutesToGlobalChain());
    run("Instance-owned NAM quality never reaches shared store", TestPluginInstanceNamQualityNeverReachesSharedStore());
    run("Host state restore does not republish project settings",
        TestHostStateRestoreDoesNotRepublishProjectSettings());
    run("Host state restore applies merged settings", TestHostStateRestoreAppliesMergedSettings());
    run("Plugin instance does not own lastPresetId", TestPluginInstanceDoesNotOwnLastPresetId());
    run("UI layout is not shared between plugin and standalone", TestUiLayoutIsNotSharedBetweenPluginAndStandalone());
    run("Plugin state remembers editor window size", TestPluginStateRemembersEditorWindowSize());
    run("Shared-sync reload applies shared settings and keeps instance-owned",
        TestSharedSyncReloadAppliesSharedSettingsAndKeepsInstanceOwned());
    run("Load preset rehydrates scrubbed hosted plugin state", TestLoadPresetRehydratesScrubbedHostedPluginState());
    run("Load preset rehydrates scrubbed hosted plugin state from active preset",
        TestLoadPresetRehydratesScrubbedHostedPluginStateFromActivePreset());
    run("Load preset remaps hosted plugin resource by stable id", TestLoadPresetRemapsHostedPluginResourceByStableId());
    run("Load preset preserves instance global FX state", TestLoadPresetPreservesInstanceGlobalFxState());
    run("Standalone persists global FX settings between launches",
        TestStandalonePersistsGlobalFxSettingsBetweenLaunches());
    run("Load preset retires NAM input auto-leveling", TestLoadPresetRetiresNamInputAutoLeveling());
    run("Load preset preserves disabled NAM calibration toggle", TestLoadPresetPreservesDisabledNamCalibrationToggle());
    run("Load app settings applies user input calibration", TestLoadAppSettingsAppliesUserInputCalibrationProfile());
    run("Load app settings prunes unused legacy keys", TestLoadAppSettingsPrunesUnusedLegacyKeys());
    run("User input calibration training bypasses active profile",
        TestUserInputCalibrationTrainingBypassesActiveProfileWithoutPersistingSelection());
    run("Save preset does not persist global FX settings", TestSavePresetDoesNotPersistGlobalFxSettings());
    run("Save/Get/Delete preset workflow", TestSaveGetDeletePresetWorkflow());
    run("Save As creates new preset id", TestSaveAsCreatesNewPresetId());
    run("Factory preset archive startup import", TestFactoryPresetArchiveStartupImport());
    run("Preset archive session mode", TestPresetArchiveSessionMode());
    run("Standalone ignores embedded host snapshot", TestStandaloneDeserializeStateIgnoresEmbeddedPresetSnapshot());
    run("Standalone startup keeps selected input channel", TestStandaloneStartupInputModeOverridesRestoredPreset());
    run("Riff library path normalization", TestRiffLibraryPathNormalization());
    run("Optimized NAM metadata alias parsing", TestOptimizedNamMetadataAliasParsing());
    run("Reorder signal path node failures do not corrupt graph", TestReorderSignalPathNodeFailuresDoNotCorruptGraph());
    run("Setlist cursor switches preset without stacking mixer", TestSetlistCursorSwitchesPresetWithoutStackingMixer());
    run("Preset switch broadcasts light state", TestPresetSwitchBroadcastsLightState());

    std::cout << "\nPreset management workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
