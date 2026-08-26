// Hosted plugin state recall: how a hosted plugin's opaque state chunk survives graph
// edits, scene switches, mixer slots, preset saves, DAW project round-trips and standalone
// restarts.
//
// The real hosted plugin effect lives in the JUCE layer and needs a plugin binary, so these
// tests register a stand-in under the same effect type. It behaves like the real one where
// it matters for the controller: SetConfig/GetConfig round-trip the state key, GetConfig
// reads back live (not the value that was pushed in), and it can be told to mutate its own
// state without notifying anyone — which is exactly the failure mode that made state
// vanish on rebuild.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;

namespace
{
constexpr const char* kStateKey = "pluginStateBase64";
constexpr const char* kStateLengthKey = "pluginStateBase64Length";
constexpr const char* kStableIdKey = "pluginStableId";
constexpr const char* kPluginNodeId = "plugin-host-node";

// ── Fake hosted plugin ─────────────────────────────────────────────────────────

/**
 * Stand-in for JuceHostedPluginEffect.
 *
 * Keeps its "plugin state" in a member the test can poke directly, standing in for a real
 * plugin whose user changed something inside its own editor. Instances register themselves
 * in a global table keyed by the state they were seeded with so tests can find the live one.
 */
class FakeHostedPluginEffect final : public guitarfx::EffectProcessor
{
public:
    // Outgoing mixer instances are destroyed on the mixer's reaper thread, so this registry
    // is touched from more than one thread and the destruction of a superseded fake is not
    // ordered against the test's reads.
    static std::mutex& RegistryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::vector<FakeHostedPluginEffect*>& Instances()
    {
        static std::vector<FakeHostedPluginEffect*> instances;
        return instances;
    }

    FakeHostedPluginEffect()
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Instances().push_back(this);
    }

    ~FakeHostedPluginEffect() override
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto& all = Instances();
        all.erase(std::remove(all.begin(), all.end(), this), all.end());
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
            return;
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
    }

    void Reset() override {}

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
    }

    void SetParam(const std::string&, double) override {}
    [[nodiscard]] double GetParam(const std::string&) const override { return 0.0; }

    void SetConfig(const std::string& key, const std::string& value) override
    {
        mConfig[key] = value;
    }

    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        const auto it = mConfig.find(key);
        return it != mConfig.end() ? it->second : std::string{};
    }

    void SetRuntimeConfigChangedCallback(RuntimeConfigChangedCallback callback) override
    {
        mRuntimeConfigChanged = std::move(callback);
    }

    bool LoadResources(const std::vector<guitarfx::ResourceRef>&,
                       const std::vector<fs::path>&) override
    {
        return true;
    }

    [[nodiscard]] bool RequiresResource() const override { return true; }
    [[nodiscard]] bool HasResource() const override { return true; }
    [[nodiscard]] std::string GetType() const override { return "plugin_host"; }
    [[nodiscard]] std::string GetCategory() const override { return "utility"; }

    /// The plugin changed its own state and told nobody — a preset browser inside the
    /// plugin, a sample reload, anything that does not go through a host-visible parameter.
    void MutateStateSilently(const std::string& newState) { mConfig[kStateKey] = newState; }

    /// The plugin changed its state and did notify, i.e. auto-capture fires.
    void MutateStateAndNotify(const std::string& newState)
    {
        mConfig[kStateKey] = newState;
        if (mRuntimeConfigChanged)
            mRuntimeConfigChanged(kStateKey, newState);
    }

    [[nodiscard]] std::string LiveState() const { return GetConfig(kStateKey); }

private:
    std::map<std::string, std::string> mConfig;
    RuntimeConfigChangedCallback mRuntimeConfigChanged;
};

void RegisterFakeHostedPluginEffect()
{
    static bool registered = false;
    if (registered)
        return;

    guitarfx::EffectTypeInfo info;
    info.type = guitarfx::EffectGuids::kPluginHost;
    info.aliases = {"plugin_host"};
    info.displayName = "Plugin Host (test double)";
    info.category = "utility";
    info.requiresResource = true;
    info.resourceType = "plugin";

    guitarfx::EffectRegistry::Instance().Register(info.type, info, []() {
        return std::make_unique<FakeHostedPluginEffect>();
    });
    registered = true;
}

/// The live fake currently holding a given state.
FakeHostedPluginEffect* FindLiveFake(const std::string& seededState)
{
    std::lock_guard<std::mutex> lock(FakeHostedPluginEffect::RegistryMutex());
    for (auto* instance : FakeHostedPluginEffect::Instances())
    {
        if (instance->LiveState() == seededState)
            return instance;
    }
    return nullptr;
}

/// The most recently constructed fake, i.e. the one the latest rebuild installed. Preset
/// swaps build the incoming instance before retiring the outgoing one, and the outgoing one
/// is freed asynchronously by the mixer's reaper, so "the newest" is the only stable way to
/// name the live processor.
FakeHostedPluginEffect* NewestLiveFake()
{
    std::lock_guard<std::mutex> lock(FakeHostedPluginEffect::RegistryMutex());
    auto& all = FakeHostedPluginEffect::Instances();
    return all.empty() ? nullptr : all.back();
}

// ── Harness ────────────────────────────────────────────────────────────────────

class TestHost final : public guitarfx::IPluginHost
{
public:
    explicit TestHost(fs::path userDataPath, bool standalone = false)
        : mUserDataPath(std::move(userDataPath))
        , mStandalone(standalone)
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

    void RunOnMainThread(std::function<void()> fn) override { fn(); }

    [[nodiscard]] fs::path GetUserDataPath() const override { return mUserDataPath; }
    [[nodiscard]] fs::path GetBundledAssetsPath() const override { return mUserDataPath; }
    [[nodiscard]] double GetSampleRate() const override { return 48000.0; }
    [[nodiscard]] int GetBlockSize() const override { return 512; }
    [[nodiscard]] bool IsStandalone() const override { return mStandalone; }

    void NotifyStateChanged() override { ++stateChangedNotifications; }

    std::vector<std::string> sentMessages;
    int stateChangedNotifications = 0;

private:
    fs::path mUserDataPath;
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

fs::path MakeSandbox(const std::string& name)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-hosted-plugin-state-tests" / name;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);
    return sandbox;
}

/// A preset with input -> plugin host -> gain -> output. The extra node gives reorder and
/// delete tests something to move that is not the plugin itself.
guitarfx::Preset BuildHostedPluginPreset(const std::string& id,
                                         const std::string& name,
                                         const std::string& initialState,
                                         const std::string& stableId = "test.plugin.one")
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

    GraphNode plugin;
    plugin.id = kPluginNodeId;
    plugin.type = EffectGuids::kPluginHost;
    plugin.category = "utility";
    if (!initialState.empty())
        plugin.config[kStateKey] = initialState;
    plugin.config[kStableIdKey] = stableId;
    plugin.resources.push_back(ResourceRef{"plugin", stableId, fs::path{}, ""});

    GraphNode tail;
    tail.id = "tail";
    tail.type = EffectGuids::kGain;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, plugin, tail, out};
    preset.graph.edges = {
        {"in", kPluginNodeId, 0, 0, 1.0},
        {kPluginNodeId, "tail", 0, 0, 1.0},
        {"tail", "out", 0, 0, 1.0},
    };

    NormalizePresetScenes(preset);
    return preset;
}

/// The UI never holds hosted plugin state — SerializePresetForUi strips every chunk and
/// leaves a length marker behind. Payloads the UI sends back therefore arrive scrubbed, and
/// the backend rehydrates them from its own working copy. Tests that stand in for the UI
/// have to send the same shape or they are exercising a path the app never takes.
guitarfx::Preset ScrubLikeUi(guitarfx::Preset preset)
{
    const auto scrubGraph = [](guitarfx::SignalGraph& graph) {
        for (auto& node : graph.nodes)
        {
            const auto stateIt = node.config.find(kStateKey);
            if (stateIt == node.config.end())
                continue;
            node.config[kStateLengthKey] = std::to_string(stateIt->second.size());
            node.config.erase(stateIt);
        }
    };

    scrubGraph(preset.graph);
    for (auto& scene : preset.scenes)
        scrubGraph(scene.graph);
    return preset;
}

void LoadPreset(guitarfx::PluginController& controller, const guitarfx::Preset& preset)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "loadPreset"},
        {"presetId", preset.id},
        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))}
    }.dump());
}

std::string NodeStateInGraph(const guitarfx::SignalGraph& graph, const std::string& nodeId)
{
    const auto* node = graph.FindNode(nodeId);
    if (!node)
        return "<no node>";
    const auto it = node->config.find(kStateKey);
    return it != node->config.end() ? it->second : std::string{"<no state>"};
}

std::string ActiveGraphState(const guitarfx::PluginController& controller)
{
    const auto& preset = controller.GetActivePreset();
    if (!preset)
        return "<no preset>";
    return NodeStateInGraph(preset->graph, kPluginNodeId);
}

std::string SceneState(const guitarfx::Preset& preset, const std::string& sceneId)
{
    const auto* scene = guitarfx::FindPresetScene(preset, sceneId);
    if (!scene)
        return "<no scene>";
    return NodeStateInGraph(scene->graph, kPluginNodeId);
}

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
        std::cerr << "  " << message << "\n";
    return condition;
}

bool ExpectEqual(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual == expected)
        return true;
    std::cerr << "  " << what << ": expected '" << expected << "', got '" << actual << "'\n";
    return false;
}

// ── Tests ──────────────────────────────────────────────────────────────────────

/// A graph edit rebuilds every processor. State the plugin changed without notifying the
/// host has to be pulled out of the live processor before the rebuild, or it is gone.
bool TestGraphEditPreservesUnnotifiedPluginState()
{
    const auto sandbox = MakeSandbox("graph-edit-preserves-state");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    const auto preset = BuildHostedPluginPreset("hp-graph-edit", "Graph Edit", "state-initial");
    LoadPreset(controller, preset);

    auto* live = NewestLiveFake();
    if (!Expect(live != nullptr, "no live hosted plugin processor after load"))
        return false;

    // The plugin's own editor changed something and never told the host about it.
    live->MutateStateSilently("state-edited-in-plugin-ui");

    // Reordering an unrelated node tears down and rebuilds the whole chain.
    controller.HandleUIMessage(nlohmann::json{
        {"type", "reorderSignalPathNode"},
        {"nodeId", "tail"},
        {"targetNodeId", kPluginNodeId}
    }.dump());

    if (!ExpectEqual(ActiveGraphState(controller), "state-edited-in-plugin-ui",
                     "working copy after reorder"))
        return false;

    auto* rebuilt = NewestLiveFake();
    if (!Expect(rebuilt != nullptr, "no live processor after reorder"))
        return false;

    return ExpectEqual(rebuilt->LiveState(), "state-edited-in-plugin-ui",
                       "rebuilt processor state after reorder");
}

/// Two scenes sharing a plugin node id must keep two distinct state chunks. Saving used to
/// stamp whichever scene was live over all of them.
bool TestSavePreservesPerScenePluginState()
{
    const auto sandbox = MakeSandbox("per-scene-state");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    auto preset = BuildHostedPluginPreset("hp-scenes", "Scenes", "state-scene-a");
    // A second scene whose plugin node carries its own, different state.
    guitarfx::PresetScene sceneB;
    sceneB.id = "scene-b";
    sceneB.title = "Scene B";
    sceneB.graph = preset.graph;
    if (auto* node = sceneB.graph.FindNode(kPluginNodeId))
        node->config[kStateKey] = "state-scene-b";
    preset.scenes.push_back(sceneB);

    const std::string sceneAId = preset.scenes.front().id;

    LoadPreset(controller, preset);

    controller.HandleUIMessage(nlohmann::json{
        {"type", "savePreset"},
        {"name", "Scenes"},
        {"presetId", preset.id},
        {"sourcePresetId", preset.id}
    }.dump());

    const auto& saved = controller.GetActivePreset();
    if (!Expect(saved.has_value(), "no active preset after save"))
        return false;

    bool ok = true;
    ok &= ExpectEqual(SceneState(*saved, sceneAId), "state-scene-a", "scene A after save");
    ok &= ExpectEqual(SceneState(*saved, "scene-b"), "state-scene-b", "scene B after save");
    return ok;
}

/// Switching scenes banks the outgoing scene's live state into that scene, and loads the
/// incoming scene's own state into the plugin.
bool TestSceneSwitchBanksOutgoingSceneState()
{
    const auto sandbox = MakeSandbox("scene-switch-banks-state");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    auto preset = BuildHostedPluginPreset("hp-scene-switch", "Scene Switch", "state-scene-a");
    guitarfx::PresetScene sceneB;
    sceneB.id = "scene-b";
    sceneB.title = "Scene B";
    sceneB.graph = preset.graph;
    if (auto* node = sceneB.graph.FindNode(kPluginNodeId))
        node->config[kStateKey] = "state-scene-b";
    preset.scenes.push_back(sceneB);

    const std::string sceneAId = preset.scenes.front().id;

    LoadPreset(controller, preset);

    auto* live = NewestLiveFake();
    if (!Expect(live != nullptr, "no live processor after load"))
        return false;
    live->MutateStateSilently("state-scene-a-edited");

    // A UI scene switch is a load of the same preset, scrubbed as the UI always sends it,
    // with a different sceneId.
    controller.HandleUIMessage(nlohmann::json{
        {"type", "loadPreset"},
        {"presetId", preset.id},
        {"sceneId", "scene-b"},
        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(ScrubLikeUi(preset)))}
    }.dump());

    const auto& afterSwitch = controller.GetActivePreset();
    if (!Expect(afterSwitch.has_value(), "no active preset after scene switch"))
        return false;

    bool ok = true;
    ok &= ExpectEqual(SceneState(*afterSwitch, sceneAId), "state-scene-a-edited",
                      "outgoing scene A state after switch");
    ok &= ExpectEqual(SceneState(*afterSwitch, "scene-b"), "state-scene-b",
                      "incoming scene B state after switch");
    ok &= ExpectEqual(ActiveGraphState(controller), "state-scene-b",
                      "live graph after switch");
    return ok;
}

/// "Save as" mints a new preset id while the mixer slot still answers to the old one.
/// Without a re-key, every later capture for that node is dropped on the floor.
bool TestSaveAsKeepsRuntimeCaptureWorking()
{
    const auto sandbox = MakeSandbox("save-as-rekeys-slot");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    const auto preset = BuildHostedPluginPreset("hp-save-as", "Save As Source", "state-before-save");
    LoadPreset(controller, preset);

    controller.HandleUIMessage(nlohmann::json{
        {"type", "savePreset"},
        {"name", "Save As Target"},
        {"saveMode", "save-as"},
        {"sourcePresetId", preset.id}
    }.dump());

    const auto& saved = controller.GetActivePreset();
    if (!Expect(saved.has_value(), "no active preset after save-as"))
        return false;
    if (!Expect(saved->id != preset.id, "save-as did not mint a new preset id"))
        return false;

    // The plugin is edited *after* the save-as, and does notify this time. The capture has
    // to reach the working copy under the preset's new id.
    auto* live = NewestLiveFake();
    if (!Expect(live != nullptr, "no live processor after save-as"))
        return false;
    live->MutateStateAndNotify("state-after-save-as");

    if (!ExpectEqual(ActiveGraphState(controller), "state-after-save-as",
                     "working copy after post-save-as capture"))
        return false;

    // Manual capture addresses the preset by its new id; it must find the running slot.
    host.sentMessages.clear();
    live->MutateStateSilently("state-manual-capture");
    controller.HandleUIMessage(nlohmann::json{
        {"type", "updateSignalPathNodeConfig"},
        {"presetId", saved->id},
        {"nodeId", kPluginNodeId},
        {"key", kStateKey},
        {"capture", true}
    }.dump());

    for (const auto& message : host.sentMessages)
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (!parsed.is_discarded() && parsed.value("type", "") == "error")
        {
            std::cerr << "  manual capture after save-as reported an error: " << message << "\n";
            return false;
        }
    }

    return ExpectEqual(ActiveGraphState(controller), "state-manual-capture",
                       "working copy after manual capture");
}

/// A hosted plugin in a mixer slot that is not the editing focus still has to persist.
bool TestUnfocusedMixerSlotPersistsPluginState()
{
    const auto sandbox = MakeSandbox("unfocused-mixer-slot");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    const auto presetA = BuildHostedPluginPreset("hp-slot-a", "Slot A", "state-a");
    const auto presetB = BuildHostedPluginPreset("hp-slot-b", "Slot B", "state-b");

    LoadPreset(controller, presetA);
    if (!Expect(controller.AddActivePreset(presetB, presetB.id, presetB.name),
                "could not add second mixer slot"))
        return false;

    // Slot A holds the editing focus; slot B's plugin is edited from its own editor.
    auto* liveB = FindLiveFake("state-b");
    if (!Expect(liveB != nullptr, "no live processor for the unfocused slot"))
        return false;
    liveB->MutateStateAndNotify("state-b-edited");

    // The focused slot must not have absorbed the other slot's state.
    if (!ExpectEqual(ActiveGraphState(controller), "state-a", "focused slot state"))
        return false;

    // Round-trip through host state: the unfocused slot's edit has to come back.
    const auto serialized = controller.SerializeState();
    const auto parsed = nlohmann::json::parse(serialized);
    if (!Expect(parsed.contains("mixer") && parsed["mixer"].contains("presetData"),
                "host state carries no mixer preset data"))
        return false;

    const auto& presetData = parsed["mixer"]["presetData"];
    if (!Expect(presetData.contains(presetB.id), "host state has no data for the unfocused slot"))
        return false;

    auto restoredB = guitarfx::PresetStorage::DeserializeFromJson(presetData[presetB.id].dump());
    if (!Expect(restoredB.has_value(), "unfocused slot data did not deserialize"))
        return false;

    return ExpectEqual(NodeStateInGraph(restoredB->graph, kPluginNodeId), "state-b-edited",
                       "unfocused slot state in host state");
}

/// The DAW round-trip: state captured into a project must come back into the plugin.
bool TestHostStateRoundTripRestoresPluginState()
{
    const auto sandbox = MakeSandbox("host-state-round-trip");

    std::string serialized;
    {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(48000.0, 512);

        const auto preset = BuildHostedPluginPreset("hp-round-trip", "Round Trip", "state-initial");
        LoadPreset(controller, preset);

        auto* live = NewestLiveFake();
        if (!Expect(live != nullptr, "no live processor before serialize"))
            return false;
        live->MutateStateSilently("state-in-project");

        serialized = controller.SerializeState();
    }

    const auto parsed = nlohmann::json::parse(serialized);
    if (!Expect(parsed.contains("preset"), "serialized host state carries no preset"))
        return false;

    auto projectPreset = guitarfx::PresetStorage::DeserializeFromJson(parsed["preset"].dump());
    if (!Expect(projectPreset.has_value(), "project preset did not deserialize"))
        return false;
    if (!ExpectEqual(NodeStateInGraph(projectPreset->graph, kPluginNodeId), "state-in-project",
                     "state captured into the project"))
        return false;

    TestHost restoredHost(sandbox);
    guitarfx::PluginController restored(restoredHost);
    restored.Initialize();
    restored.Prepare(48000.0, 512);
    restored.DeserializeState(serialized);

    if (!ExpectEqual(ActiveGraphState(restored), "state-in-project", "working copy after restore"))
        return false;

    auto* live = NewestLiveFake();
    if (!Expect(live != nullptr, "no live processor after restore"))
        return false;

    return ExpectEqual(live->LiveState(), "state-in-project", "processor state after restore");
}

/// Standalone saves its state on exit and hands it back on the next launch. It must not
/// resurrect unsaved graph edits, but it must recover the plugin chunk, which has no other
/// home once the app closes.
bool TestStandaloneRestoresPluginStateButNotGraphEdits()
{
    const auto sandbox = MakeSandbox("standalone-session-restore");

    auto storedPreset = BuildHostedPluginPreset("hp-standalone", "Standalone", "state-saved-to-disk");
    if (auto* tail = storedPreset.graph.FindNode("tail"))
        tail->params["gain"] = 1.0;
    for (auto& scene : storedPreset.scenes)
    {
        if (auto* tail = scene.graph.FindNode("tail"))
            tail->params["gain"] = 1.0;
    }

    // Seed the store the way a previous run would have: preset saved, lastPresetId set.
    std::string sessionState;
    {
        TestHost host(sandbox, /*standalone*/ true);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(48000.0, 512);

        LoadPreset(controller, storedPreset);
        controller.HandleUIMessage(nlohmann::json{
            {"type", "savePreset"},
            {"name", storedPreset.name},
            {"presetId", storedPreset.id},
            {"sourcePresetId", storedPreset.id}
        }.dump());

        // After the save the user keeps working: the plugin's state changes, and an
        // unrelated graph parameter changes. Neither is saved to the preset again.
        auto* live = NewestLiveFake();
        if (!Expect(live != nullptr, "no live processor in the first session"))
            return false;
        live->MutateStateSilently("state-unsaved-session-edit");

        controller.HandleUIMessage(nlohmann::json{
            {"type", "updateSignalPathNodeParam"},
            {"nodeId", "tail"},
            {"paramKey", "gain"},
            {"value", 9.0}
        }.dump());

        sessionState = controller.SerializeState();
    }

    // Second launch: startup loads the preset from the store, then the standalone holder
    // replays the previous session's state.
    TestHost host(sandbox, /*standalone*/ true);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    if (!Expect(controller.GetActivePreset().has_value(), "standalone startup restored no preset"))
        return false;

    controller.DeserializeState(sessionState);

    const auto& active = controller.GetActivePreset();
    if (!Expect(active.has_value(), "active preset cleared by standalone session restore"))
        return false;

    bool ok = true;
    ok &= ExpectEqual(ActiveGraphState(controller), "state-unsaved-session-edit",
                      "plugin state after standalone restart");

    // The unsaved graph edit must NOT come back: the store stays authoritative for the graph.
    const auto* tail = active->graph.FindNode("tail");
    if (!Expect(tail != nullptr, "restored preset has no tail node"))
        return false;
    const auto gainIt = tail->params.find("gain");
    const double gain = gainIt != tail->params.end() ? gainIt->second : 0.0;
    ok &= Expect(std::abs(gain - 9.0) > 1e-9,
                 "standalone restore resurrected an unsaved graph edit (gain came back as 9)");

    auto* live = NewestLiveFake();
    ok &= Expect(live != nullptr, "no live processor after standalone restore");
    if (live)
        ok &= ExpectEqual(live->LiveState(), "state-unsaved-session-edit",
                          "processor state after standalone restore");
    return ok;
}

/// A session snapshot from a different preset must be ignored rather than grafted on.
bool TestStandaloneIgnoresSessionStateForDifferentPreset()
{
    const auto sandbox = MakeSandbox("standalone-mismatched-session");

    const auto storedPreset = BuildHostedPluginPreset("hp-standalone-keep", "Keep", "state-on-disk");
    {
        TestHost host(sandbox, /*standalone*/ true);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(48000.0, 512);
        LoadPreset(controller, storedPreset);
        controller.HandleUIMessage(nlohmann::json{
            {"type", "savePreset"},
            {"name", storedPreset.name},
            {"presetId", storedPreset.id},
            {"sourcePresetId", storedPreset.id}
        }.dump());
    }

    TestHost host(sandbox, /*standalone*/ true);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    // A snapshot that claims to be a different preset entirely.
    auto foreign = BuildHostedPluginPreset("hp-some-other-preset", "Other", "state-from-elsewhere");
    nlohmann::json sessionState = nlohmann::json::object();
    sessionState["presetId"] = foreign.id;
    sessionState["preset"] = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(foreign));

    controller.DeserializeState(sessionState.dump());

    return ExpectEqual(ActiveGraphState(controller), "state-on-disk",
                       "plugin state after a mismatched session snapshot");
}

/// Pointing a node at a different plugin must drop the previous plugin's state chunk rather
/// than handing it to the new plugin.
bool TestPluginSwapDropsStaleState()
{
    const auto sandbox = MakeSandbox("plugin-swap-drops-state");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    const auto preset = BuildHostedPluginPreset("hp-swap", "Swap", "state-plugin-one", "test.plugin.one");
    LoadPreset(controller, preset);

    if (!ExpectEqual(ActiveGraphState(controller), "state-plugin-one", "state before swap"))
        return false;

    // The user picks a different plugin for the same node.
    controller.HandleUIMessage(nlohmann::json{
        {"type", "updateNodeResource"},
        {"nodeId", kPluginNodeId},
        {"resourceIndex", 0},
        {"resourceType", "plugin"},
        {"resourceId", "test.plugin.two"}
    }.dump());

    const auto& active = controller.GetActivePreset();
    if (!Expect(active.has_value(), "no active preset after plugin swap"))
        return false;

    const auto* node = active->graph.FindNode(kPluginNodeId);
    if (!Expect(node != nullptr, "plugin node missing after swap"))
        return false;

    bool ok = true;
    ok &= Expect(node->config.count(kStateKey) == 0,
                 "the previous plugin's state chunk survived the swap");
    ok &= Expect(node->config.count(kStateLengthKey) == 0,
                 "a stale scrub marker survived the swap");
    ok &= Expect(node->config.count(kStableIdKey) == 0,
                 "the previous plugin's stable id survived the swap");
    return ok;
}

/// The runtime must never be able to erase a stored chunk by reporting an empty capture.
bool TestEmptyRuntimeCaptureDoesNotEraseStoredState()
{
    const auto sandbox = MakeSandbox("empty-capture-guard");

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 512);

    const auto preset = BuildHostedPluginPreset("hp-empty-capture", "Empty Capture", "state-worth-keeping");
    LoadPreset(controller, preset);

    auto* live = NewestLiveFake();
    if (!Expect(live != nullptr, "no live processor after load"))
        return false;

    // The processor forgets its state (a failed capture, a plugin with no automatable
    // parameters and an empty chunk) and a rebuild follows.
    live->SetConfig(kStateKey, "");

    controller.HandleUIMessage(nlohmann::json{
        {"type", "reorderSignalPathNode"},
        {"nodeId", "tail"},
        {"targetNodeId", kPluginNodeId}
    }.dump());

    return ExpectEqual(ActiveGraphState(controller), "state-worth-keeping",
                       "stored state after an empty runtime capture");
}

} // namespace

int main()
{
    RegisterFakeHostedPluginEffect();

    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        if (ok) ++passed; else ++failed;
    };

    run("Graph edit preserves un-notified plugin state", TestGraphEditPreservesUnnotifiedPluginState());
    run("Save preserves per-scene plugin state", TestSavePreservesPerScenePluginState());
    run("Scene switch banks outgoing scene state", TestSceneSwitchBanksOutgoingSceneState());
    run("Save As keeps runtime capture working", TestSaveAsKeepsRuntimeCaptureWorking());
    run("Unfocused mixer slot persists plugin state", TestUnfocusedMixerSlotPersistsPluginState());
    run("Host state round-trip restores plugin state", TestHostStateRoundTripRestoresPluginState());
    run("Standalone restores plugin state but not graph edits", TestStandaloneRestoresPluginStateButNotGraphEdits());
    run("Standalone ignores session state for a different preset", TestStandaloneIgnoresSessionStateForDifferentPreset());
    run("Plugin swap drops stale state", TestPluginSwapDropsStaleState());
    run("Empty runtime capture does not erase stored state", TestEmptyRuntimeCaptureDoesNotEraseStoredState());

    std::cout << "\nHosted plugin state workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
