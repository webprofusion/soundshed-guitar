/**
 * @file SceneSelectAutomationTests.cpp
 * @brief The Scene parameter: one DAW lane whose value is the scene to play.
 *
 * Scene 1-4 are triggers, which switch only on a rising edge, so a lane of them does nothing when
 * the host jumps between its points, and a low value never selects anything. The Scene parameter
 * switches whenever its value changes, and a host sends the lane's value on a jump.
 *  - It comes after every parameter that shipped before it, so no saved project rebinds.
 *  - A value selects that scene, up or down, through the control surface drain.
 *  - Its value follows a scene chosen anywhere else, so the host sees it differ from the lane and
 *    sends the lane's value again, which puts the lane's scene back.
 *  - A step with no scene in the preset does nothing, and the value is left as the host set it
 *    rather than put back for the host to send again on every block.
 *  - The range is fixed, so adding a scene to the end leaves every earlier value on its scene.
 */

#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "automation/AutomationTypes.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetId = "scene-select-automation";
constexpr const char* kSceneSlotId = "default.scene";

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

Preset BuildPreset(int sceneCount)
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = kPresetId;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;

    GraphNode gain;
    gain.id = "gain";
    gain.type = "gain";
    gain.params["gainDb"] = 0.0;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {{"in", "gain", 0, 0, 1.0}, {"gain", "out", 0, 0, 1.0}};
    NormalizePresetScenes(preset);

    for (int i = 2; i <= sceneCount; ++i)
    {
        auto scene = preset.scenes.front();
        scene.id = "scene-" + std::to_string(i);
        scene.title = "Scene " + std::to_string(i);
        preset.scenes.push_back(std::move(scene));
    }

    return preset;
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

/// The normalised value a host's lane holds for scene `scene` (1-based).
float SceneValue(int scene)
{
    return static_cast<float>(scene - 1) / static_cast<float>(kSceneSelectSteps - 1);
}

/// What the plugin's timer does with the editor closed.
void Drain(PluginController& controller)
{
    controller.DrainControlSurfaceRequests();
}

/// The host's lane reaching a point, then the plugin's next tick.
void HostSends(PluginController& controller, int scene)
{
    controller.ApplyAutomationFromDAW(kSceneSlotId, SceneValue(scene));
    Drain(controller);
}

/// 1-based, as the user counts them.
int ScenePlaying(const PluginController& controller)
{
    return controller.GetActiveSceneIndex() + 1;
}

bool Run(const fs::path& sandbox)
{
    bool passed = true;

    // The layout. The adapter puts defaults past kLayoutStableDefaultSlots after the custom
    // slots, in their order here, so the new one has to be last to move nothing.
    passed = Check(std::string(std::prev(std::end(kDefaultSlots))->slotId) == kSceneSlotId &&
                       std::string(std::prev(std::end(kDefaultSlots))->address) == kSceneSelectAddress,
                   "layout: the Scene parameter is the last default slot") &&
             passed;
    passed = Check(kLayoutStableDefaultSlots == 13 && std::size(kDefaultSlots) == 18,
                   "layout: nothing before it moved (13 shipped, four triggers, then Scene)") &&
             passed;

    test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();

    // Bound as the adapter would, so the host's view of the value can be read.
    const auto slotIds = controller.GetAutomationSlotIds();
    const auto sceneParameter =
        static_cast<int>(std::find(slotIds.begin(), slotIds.end(), kSceneSlotId) - slotIds.begin());
    controller.BindDawParameters(slotIds);
    const auto hostReads = [&]() { return controller.GetDawParameterValue(sceneParameter); };

    const auto preset = BuildPreset(3);
    const auto presetJson = nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
    Send(controller, {{"type", "savePreset"},
                      {"saveMode", "import"},
                      {"presetId", preset.id},
                      {"name", preset.name},
                      {"preset", presetJson}});
    Send(controller, {{"type", "loadPreset"}, {"presetId", preset.id}, {"preset", presetJson}, {"sceneId", "scene-1"}});
    host.Pump();
    Drain(controller);

    passed = Check(sceneParameter < static_cast<int>(slotIds.size()) && ScenePlaying(controller) == 1 &&
                       hostReads() == SceneValue(1),
                   "setup: three scenes, scene 1, and the host reads scene 1") &&
             passed;

    // Up and down. A trigger lane never selects anything going down.
    HostSends(controller, 3);
    passed = Check(ScenePlaying(controller) == 3 && hostReads() == SceneValue(3), "lane at scene 3: scene 3 plays") &&
             passed;
    HostSends(controller, 1);
    passed = Check(ScenePlaying(controller) == 1, "lane back down to scene 1: scene 1 plays") && passed;

    // Chosen in the UI against the lane, then the host jumps to a point on scene 1.
    Send(controller, {{"type", "selectScene"}, {"sceneId", "scene-2"}});
    Drain(controller);
    passed = Check(ScenePlaying(controller) == 2 && hostReads() == SceneValue(2),
                   "scene 2 chosen in the UI: the host reads scene 2 (" + std::to_string(hostReads()) + ")") &&
             passed;
    HostSends(controller, 1);
    passed = Check(ScenePlaying(controller) == 1, "host jumps to a point on scene 1: scene 1 plays again") && passed;

    // Asked for a scene the preset does not have.
    HostSends(controller, 5);
    passed = Check(ScenePlaying(controller) == 1, "lane at scene 5 of three: nothing changes") && passed;
    passed =
        Check(hostReads() == SceneValue(5), "lane at scene 5 of three: the value stays as the host set it") && passed;

    // A fourth scene, added to the end. The add switches to it.
    Send(controller, {{"type", "addScene"}});
    Drain(controller);
    passed = Check(ScenePlaying(controller) == 4 && hostReads() == SceneValue(4),
                   "scene added: it plays, and the host reads scene 4") &&
             passed;

    for (int scene = 1; scene <= 4; ++scene)
    {
        HostSends(controller, scene);
        passed = Check(ScenePlaying(controller) == scene, "after the add, the lane's value for scene " +
                                                              std::to_string(scene) + " still plays it (" +
                                                              std::to_string(ScenePlaying(controller)) + ")") &&
                 passed;
    }

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-scene-select-automation-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    const bool passed = Run(sandbox);
    std::cout << (passed ? "SceneSelectAutomationTests PASSED\n" : "SceneSelectAutomationTests FAILED\n");
    return passed ? 0 : 1;
}
