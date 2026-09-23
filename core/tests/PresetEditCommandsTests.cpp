/**
 * PresetEditCommandsTests.cpp - The engine-owned preset edits give the result the web UI's
 * own edits gave.
 *
 * Soundshed Guitar's web UI used to edit its copy of a preset and send it back whole; both
 * UIs now ask the engine instead (controller/PluginControllerPresetEdits.cpp). These run each
 * command against a headless controller and check the preset, document or setting it leaves
 * behind against what the old path, or the web UI's rules (core/ui/ts/presetScenes.ts,
 * presets/favorites.ts), produce: docs/plans/native-ui.md, rule C3.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"

namespace fs = std::filesystem;
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

/// A fresh profile per test, so none sees another's store (and none collides with a
/// concurrent run of the same exe: the folder is keyed on when this run started).
fs::path Sandbox(const std::string& name)
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    const auto root = fs::temp_directory_path() / ("guitarfx-preset-edit-tests-" + run) / name;
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

GraphNode Node(const std::string& id, const std::string& type, bool enabled, double gain)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.enabled = enabled;
    node.params["gain"] = gain;
    return node;
}

/// Two scenes: the second bypasses the gain node and changes its level.
Preset BuildTwoScenePreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = "Two Scenes";
    preset.category = "Test";
    preset.version = 2;

    guitarfx::SignalGraph graph;
    graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput, true, 0.0),
                   Node("gain_1", guitarfx::EffectGuids::kGain, true, 3.0),
                   Node("__output__", guitarfx::kNodeTypeOutput, true, 0.0)};
    graph.edges = {GraphEdge{"__input__", "gain_1", 0, 0, 1.0}, GraphEdge{"gain_1", "__output__", 0, 0, 1.0}};

    auto second = graph;
    second.nodes[1].enabled = false;
    second.nodes[1].params["gain"] = -6.0;

    preset.graph = graph;
    preset.scenes = {{"scene-1", "Clean", graph}, {"scene-2", "Lead", second}};
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
}

void LoadBody(guitarfx::PluginController& controller, const Preset& preset, const std::string& sceneId = {})
{
    nlohmann::json payload = {{"preset", PresetJson(preset)}, {"presetId", preset.id}};

    if (!sceneId.empty())
    {
        payload["sceneId"] = sceneId;
    }

    Send(controller, "loadPreset", payload);
}

/// Saves a preset into the user store under its own id, as the web UI's save does.
void Store(guitarfx::PluginController& controller, const Preset& preset)
{
    Send(controller, "savePreset",
         {{"saveMode", "overwrite"}, {"presetId", preset.id}, {"name", preset.name}, {"category", preset.category},
          {"preset", PresetJson(preset)}});
}

nlohmann::json Scenes(const guitarfx::PluginController& controller)
{
    return PresetJson(*controller.GetActivePreset())["scenes"];
}

/// OnIdle re-checks the unsaved-changes flag at most every 0.4 s.
void SettleDirty(guitarfx::PluginController& controller)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    controller.OnIdle();
}

void TestSelectSceneMatchesLoadWithSceneId()
{
    const auto preset = BuildTwoScenePreset("p-scenes");

    TestHost webHost(Sandbox("select-web"));
    guitarfx::PluginController web(webHost);
    web.Initialize();
    LoadBody(web, preset, "scene-2");

    TestHost nanoHost(Sandbox("select-nano"));
    guitarfx::PluginController nano(nanoHost);
    nano.Initialize();
    LoadBody(nano, preset);
    Send(nano, "selectScene", {{"sceneId", "scene-2"}});

    Expect(PresetJson(*web.GetActivePreset()) == PresetJson(*nano.GetActivePreset()),
           "selectScene leaves the working copy a load with sceneId leaves");
    const auto loaded = Latest(nanoHost, "presetLoaded");
    Expect(loaded && loaded->value("sceneId", std::string{}) == "scene-2", "selectScene reports the scene it chose");
    Expect(!nano.GetActivePreset()->graph.nodes[1].enabled, "the active graph is scene 2's");
}

void TestAddRenameRemoveSceneFollowWebRules()
{
    TestHost host(Sandbox("scene-edits"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    auto preset = BuildTwoScenePreset("p-edit");
    preset.scenes[1].id = "scene-3"; // a gap: the next free id is scene-4, not scene-3
    LoadBody(controller, preset, "scene-3");

    Send(controller, "addScene");
    auto scenes = Scenes(controller);
    Expect(scenes.size() == 3, "addScene appends a scene");
    Expect(scenes[2]["id"] == "scene-4", "a new scene takes the next free scene-N id");
    Expect(scenes[2]["title"] == "Scene 3", "a new scene is titled by its position");
    Expect(scenes[2]["graph"] == scenes[1]["graph"], "a new scene copies the scene that was playing");
    Expect(Latest(host, "presetLoaded")->value("sceneId", std::string{}) == "scene-4", "the new scene is selected");

    Send(controller, "renameScene", {{"sceneId", "scene-4"}, {"title", "  Solo  "}});
    Expect(Scenes(controller)[2]["title"] == "Solo", "renameScene trims the title");
    Send(controller, "renameScene", {{"sceneId", "scene-4"}, {"title", "   "}});
    Expect(Scenes(controller)[2]["title"] == "Scene", "an empty title becomes \"Scene\"");

    Send(controller, "removeScene", {{"sceneId", "scene-4"}});
    scenes = Scenes(controller);
    Expect(scenes.size() == 2, "removeScene removes the scene");
    Expect(Latest(host, "presetLoaded")->value("sceneId", std::string{}) == "scene-3",
           "removing the playing last scene selects the new last scene");

    Send(controller, "removeScene", {{"sceneId", "scene-1"}});
    Send(controller, "removeScene", {{"sceneId", "scene-3"}});
    Expect(Scenes(controller).size() == 1, "the last scene is never removed");
    Expect(Latest(host, "error").has_value(), "removing the last scene is refused with an error");
}

void TestLoadByIdMatchesLoadWithBody()
{
    TestHost host(Sandbox("load-by-id"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto stored = BuildTwoScenePreset("p-stored");
    Store(controller, stored);
    LoadBody(controller, BuildTwoScenePreset("p-other"));

    LoadBody(controller, stored);
    const auto viaBody = PresetJson(*controller.GetActivePreset());
    LoadBody(controller, BuildTwoScenePreset("p-other"));
    Send(controller, "loadPreset", {{"presetId", "p-stored"}});
    Expect(controller.GetActivePreset()->id == "p-stored", "loadPreset {presetId} loads the stored preset");
    Expect(PresetJson(*controller.GetActivePreset()) == viaBody, "a load by id gives the working copy a load with its body gives");

    Send(controller, "loadPreset", {{"presetId", "p-missing"}});
    Expect(controller.GetActivePreset()->id == "p-stored", "an unknown id leaves the preset alone");
    Expect(Latest(host, "error").has_value(), "an unknown id is reported");
}

void TestDirtyFlagFollowsTheWorkingCopy()
{
    TestHost host(Sandbox("dirty"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    Send(controller, "uiReady");

    Store(controller, BuildTwoScenePreset("p-dirty"));
    Send(controller, "loadPreset", {{"presetId", "p-dirty"}});
    SettleDirty(controller);
    Expect(!Latest(host, "presetLoaded")->value("activePresetDirty", true), "a loaded preset opens clean");

    Send(controller, "selectScene", {{"sceneId", "scene-2"}});
    SettleDirty(controller);
    auto dirty = Latest(host, "presetDirtyChanged");
    Expect(!dirty || !dirty->value("dirty", true), "switching scenes is not an edit");

    Send(controller, "updateSignalPathNodeParam", {{"nodeId", "gain_1"}, {"paramKey", "gain"}, {"value", 1.5}});
    SettleDirty(controller);
    dirty = Latest(host, "presetDirtyChanged");
    Expect(dirty && dirty->value("dirty", false), "a parameter change makes the preset dirty");

    Send(controller, "updateSignalPathNodeParam", {{"nodeId", "gain_1"}, {"paramKey", "gain"}, {"value", -6.0}});
    SettleDirty(controller);
    Expect(!Latest(host, "presetDirtyChanged")->value("dirty", true), "putting it back makes it clean again");

    Send(controller, "updateSignalPathNodeBypass", {{"nodeId", "gain_1"}, {"bypassed", false}});
    SettleDirty(controller);
    Expect(Latest(host, "presetDirtyChanged")->value("dirty", false), "a bypass change makes it dirty");

    Send(controller, "savePreset", {{"saveMode", "overwrite"}, {"presetId", "p-dirty"}, {"name", "Two Scenes"}});
    Expect(!Latest(host, "presetDirtyChanged")->value("dirty", true), "saving makes it clean");
}

void TestFavoriteAndRatingPatches()
{
    TestHost host(Sandbox("marks"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "setPresetFavorite", {{"presetId", "p1"}, {"favorite", true}});
    Send(controller, "setPresetFavorite", {{"presetId", "p2"}, {"favorite", true}});
    Send(controller, "setPresetFavorite", {{"presetId", "p1"}, {"favorite", true}});
    Send(controller, "setPresetFavorite", {{"presetId", "p1"}, {"favorite", false}});
    Expect(Latest(host, "presetFavorites")->at("favorites") == nlohmann::json::array({"p2"}),
           "setPresetFavorite adds and removes one id, once");

    Send(controller, "getPresetFavorites");
    Expect(Latest(host, "presetFavorites")->at("favorites") == nlohmann::json::array({"p2"}),
           "the favourites document holds what the patches left");

    Send(controller, "setPresetRating", {{"presetId", "p1"}, {"rating", 4}});
    Send(controller, "setPresetRating", {{"presetId", "p2"}, {"rating", 9}});
    Send(controller, "setPresetRating", {{"presetId", "p1"}, {"rating", 0}});
    Expect(Latest(host, "presetRatings")->at("ratings") == nlohmann::json({{"p2", 5}}),
           "setPresetRating sets, clamps to 5, and clears at 0");
}

void TestRecentsKeepTheWebUiRules()
{
    TestHost host(Sandbox("recents"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    for (const auto* id : {"r1", "r2", "r3"})
    {
        Store(controller, BuildTwoScenePreset(id));
    }

    for (const auto* id : {"r1", "r2", "r1", "r3"})
    {
        Send(controller, "loadPreset", {{"presetId", id}});
    }

    Send(controller, "getPresetRecents");
    Expect(Latest(host, "presetRecents")->at("presetIds") == nlohmann::json::array({"r3", "r2", "r1"}),
           "a new preset goes first; one already listed keeps its place");

    for (const auto* id : {"r4", "r5"})
    {
        Store(controller, BuildTwoScenePreset(id));
        Send(controller, "loadPreset", {{"presetId", id}});
    }

    Send(controller, "getPresetRecents");
    Expect(Latest(host, "presetRecents")->at("presetIds") == nlohmann::json::array({"r5", "r4", "r3", "r2"}),
           "the list keeps four");
}

void TestNewPresetUsesTheDefaultTemplate()
{
    TestHost host(Sandbox("new"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "newPreset");
    const auto& preset = *controller.GetActivePreset();
    const auto loaded = Latest(host, "presetLoaded");

    Expect(loaded && loaded->value("created", false), "newPreset reports the preset as created");
    Expect(preset.id.rfind("user-", 0) == 0, "a new preset has a user- id");
    Expect(preset.name == "New Preset" && preset.category == "User", "a new preset is named as the web UI names it");

    std::vector<std::string> ids;

    for (const auto& node : preset.graph.nodes)
    {
        ids.push_back(node.id);
    }

    Expect(ids == std::vector<std::string>{"__input__", "gate_0", "fx_0", "amp_0", "cab_0", "__output__"},
           "the template's nodes, in order");
    const auto* amp = preset.graph.FindNode("amp_0");
    Expect(amp && amp->enabled && amp->type == guitarfx::EffectGuids::kAmpNamOptimized, "the amp is the one node on");
    Expect(!preset.graph.FindNode("gate_0")->enabled && !preset.graph.FindNode("cab_0")->enabled,
           "the gate and cab start bypassed");
}

void TestOutputMuteSurvivesAPresetLoad()
{
    TestHost host(Sandbox("mute"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "setOutputMuted", {{"muted", true}});
    Expect(controller.GetMixer().IsOutputMuted(), "setOutputMuted mutes the output stage");
    Expect(Latest(host, "outputMutedChanged")->value("muted", false), "and says so");

    LoadBody(controller, BuildTwoScenePreset("p-after-mute"));
    Expect(controller.GetMixer().IsOutputMuted(), "a preset load, which rebuilds the global chain, keeps the mute");

    Send(controller, "setOutputMuted", {{"muted", false}});
    Expect(!controller.GetMixer().IsOutputMuted(), "unmuting restores the output");
}

void TestSelectSetlistMovesTheCursorOnly()
{
    TestHost host(Sandbox("setlists"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const nlohmann::json setlists = nlohmann::json::array(
        {{{"id", "s1"}, {"name", "One"}, {"slots", nlohmann::json::array()}},
         {{"id", "s2"}, {"name", "Two"}, {"slots", nlohmann::json::array({{{"presetId", "x"}}})}}});
    Send(controller, "setSetlists", {{"setlists", setlists}, {"activeSetlistId", "s1"}, {"cursorIndex", 0}});
    Send(controller, "selectSetlist", {{"setlistId", "s2"}});

    const auto cursor = Latest(host, "setlistCursorChanged");
    Expect(cursor && cursor->value("activeSetlistId", std::string{}) == "s2", "selectSetlist activates the setlist");
    Send(controller, "getSetlists");
    const auto stored = Latest(host, "setlists");
    Expect(stored->value("activeSetlistId", std::string{}) == "s2", "and stores it");
    Expect(stored->at("setlists") == setlists, "without touching the setlists themselves");
}

void TestUiSettingsKeepNanosPart()
{
    TestHost host(Sandbox("ui-settings"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    Send(controller, "uiReady");

    Send(controller, "uiSettingsChanged", {{"patch", {{"native", {{"scale", 1.25}}}}}});
    Send(controller, "uiSettingsChanged", {{"settings", {{"zoom", 1.1}}}});
    controller.OnIdle();
    const auto state = Latest(host, "state");
    Expect(state && state->at("uiSettings").value("zoom", 0.0) == 1.1, "the web UI's whole-blob write lands");
    Expect(state && state->at("uiSettings")["native"].value("scale", 0.0) == 1.25,
           "and keeps Nano's part, which it never sends");
}

void TestDemoClipsPlayById()
{
    const auto root = Sandbox("demo");
    std::error_code ec;
    fs::create_directories(root / "ui" / "demo", ec);
    fs::copy_file(fs::path(GUITARFX_DEMO_AUDIO_DIR) / "guitar-riff-01.wav", root / "ui" / "demo" / "riff.wav", ec);
    std::ofstream(root / "ui" / "demo" / "clips.json")
        << R"({"clips":[{"id":"riff","title":"A Riff","file":"riff.wav"},{"id":"escape","file":"../../x.wav"}]})";

    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.OnWebContentLoaded();
    controller.OnIdle();

    const auto state = Latest(host, "state");
    Expect(state && state->at("demoClips").size() == 2 && state->at("demoClips")[0].value("title", "") == "A Riff",
           "the full state lists the demo clips");

    Send(controller, "previewDemoAudio", {{"clipId", "riff"}, {"repeat", true}});
    const auto started = Latest(host, "previewStarted");
    Expect(started && started->value("title", std::string{}) == "A Riff", "previewDemoAudio {clipId} plays the clip");

    host.sent.clear();
    Send(controller, "previewDemoAudio", {{"clipId", "escape"}});
    Expect(Latest(host, "error").has_value() && !Latest(host, "previewStarted"),
           "a clip naming a file outside the demo folder is refused");
}

void TestResourceFavoritePatch()
{
    TestHost host(Sandbox("resource-favorites"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Send(controller, "setResourceFavorite", {{"resourceId", "nam-1"}, {"favorite", true}});
    Send(controller, "setResourceFavorite", {{"resourceId", "ir-2"}, {"favorite", true}});
    Send(controller, "setResourceFavorite", {{"resourceId", "nam-1"}, {"favorite", false}});
    Expect(controller.GetAppSettings().value("resources.favorites", nlohmann::json()) == nlohmann::json::array({"ir-2"}),
           "setResourceFavorite edits the one id in resources.favorites");
    Expect(Latest(host, "appSettingChanged")->at("value") == nlohmann::json::array({"ir-2"}),
           "and tells the UI the setting's new value");
}

void TestSceneEditsKeepTheMix()
{
    TestHost host(Sandbox("scene-mix"));
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.Prepare(48000.0, 256);

    auto first = BuildTwoScenePreset("user-mix-a");
    auto second = BuildTwoScenePreset("user-mix-b");
    second.name = "Second";
    Store(controller, first);
    Store(controller, second);
    LoadBody(controller, first);
    Send(controller, "addActivePreset", {{"presetId", second.id}});

    const auto mixSize = [&controller] { return controller.GetMixer().GetActivePresetIds().size(); };
    Expect(mixSize() == 2, "the mix holds both presets before the scene edits");

    // Whichever preset is being edited, its scene switch rebuilds its slot alone.
    Send(controller, "selectScene", {{"sceneId", "scene-2"}});
    Expect(mixSize() == 2, "selectScene on a preset in a mix keeps the other preset playing");
    Expect(controller.GetActivePreset() && !controller.GetActivePreset()->graph.nodes[1].enabled,
           "and the edited preset now plays the new scene");

    Send(controller, "removeScene", {{"sceneId", "scene-2"}});
    Expect(mixSize() == 2, "removing the scene that is playing keeps the mix too");
    Expect(controller.GetActivePreset() && controller.GetActivePreset()->graph.nodes[1].enabled,
           "and the scene that takes its place is heard");
}

void TestDeleteSendsTheNewList()
{
    TestHost host(Sandbox("delete"));
    guitarfx::PluginController controller(host);
    controller.Initialize();

    Store(controller, BuildTwoScenePreset("user-delete-me"));
    host.sent.clear();
    Send(controller, "deletePreset", {{"presetId", "user-delete-me"}});

    // Shared sync only reaches other instances, so the UI that asked must get the list itself.
    const auto list = Latest(host, "presetList");
    Expect(list.has_value(), "deletePreset sends the preset list to the UI that asked");

    if (list)
    {
        bool stillListed = false;

        for (const auto& preset : list->value("presets", nlohmann::json::array()))
        {
            stillListed = stillListed || preset.value("id", std::string{}) == "user-delete-me";
        }

        Expect(!stillListed, "and the deleted preset is not in it");
    }
}
} // namespace

int main()
{
    TestSelectSceneMatchesLoadWithSceneId();
    TestAddRenameRemoveSceneFollowWebRules();
    TestLoadByIdMatchesLoadWithBody();
    TestDirtyFlagFollowsTheWorkingCopy();
    TestFavoriteAndRatingPatches();
    TestRecentsKeepTheWebUiRules();
    TestNewPresetUsesTheDefaultTemplate();
    TestOutputMuteSurvivesAPresetLoad();
    TestSelectSetlistMovesTheCursorOnly();
    TestUiSettingsKeepNanosPart();
    TestDemoClipsPlayById();
    TestResourceFavoritePatch();
    TestDeleteSendsTheNewList();
    TestSceneEditsKeepTheMix();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "PresetEditCommandsTests passed" << std::endl;
    return 0;
}
