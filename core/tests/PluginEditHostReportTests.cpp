/**
 * @file PluginEditHostReportTests.cpp
 * @brief What the host hears of a DAW parameter the plugin itself moves.
 *
 * A host's Touch and Latch automation modes, and its record of a move, need the plugin to say
 * when a hand is on a parameter. Before this, nothing did, so in REAPER a lane on a parameter
 * mapped to a knob overwrote every move of that knob made during playback.
 *  - A knob in the UI mapped to a DAW parameter moves the parameter, and it is reported.
 *  - A value the host sent is never reported back to it.
 *  - A knob of another node, or a parameter no slot maps, reports nothing.
 *  - A bypass mapped to a DAW parameter follows the node switched in the UI.
 *  - A trigger the host fired keeps the host's value, so the same value sent again on playback
 *    start or a jump (which the plugin wrapper drops as unchanged) does not fire it again, and a
 *    real rising edge still does.
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
constexpr const char* kPresetId = "plugin-edit-host-report";
constexpr const char* kGainSlot = "custom.gain";
constexpr const char* kBypassSlot = "custom.bypass";
constexpr const char* kSceneTrigger = "default.scene2";

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

bool Near(float a, float b)
{
    return std::abs(a - b) < 1.0e-4f;
}

GraphNode GainNode(const std::string& id)
{
    GraphNode node;
    node.id = id;
    node.type = "gain";
    node.params["gainDb"] = 0.0;
    return node;
}

Preset BuildPreset()
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = kPresetId;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, GainNode("gain"), GainNode("gain2"), out};
    preset.graph.edges = {{"in", "gain", 0, 0, 1.0}, {"gain", "gain2", 0, 0, 1.0}, {"gain2", "out", 0, 0, 1.0}};
    NormalizePresetScenes(preset);

    auto scene = preset.scenes.front();
    scene.id = "scene-2";
    scene.title = "Scene 2";
    preset.scenes.push_back(std::move(scene));
    return preset;
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

bool Run(const fs::path& sandbox)
{
    bool passed = true;

    test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();

    // Mapped before the parameters are bound, as a project's custom slots are.
    Send(controller,
         {{"type", "setAutomationSlot"}, {"slotId", kGainSlot}, {"label", "Gain"}, {"address", "node.gain.gainDb"}});
    Send(controller, {{"type", "setAutomationSlot"},
                      {"slotId", kBypassSlot},
                      {"label", "Gain on"},
                      {"address", "node.gain.enabled"}});

    const auto slotIds = controller.GetAutomationSlotIds();
    const auto indexOf = [&](const std::string& slotId) {
        return static_cast<int>(std::find(slotIds.begin(), slotIds.end(), slotId) - slotIds.begin());
    };
    controller.BindDawParameters(slotIds);
    const int gainParameter = indexOf(kGainSlot);
    const int bypassParameter = indexOf(kBypassSlot);
    const int triggerParameter = indexOf(kSceneTrigger);

    // What the adapter's timer would report to the host.
    const auto reported = [&]() {
        std::vector<int> indices;
        controller.GetAutomationSlots().TakeDawParametersChangedByPlugin([&](int index) { indices.push_back(index); });
        return indices;
    };

    const auto preset = BuildPreset();
    const auto presetJson = nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
    Send(controller, {{"type", "savePreset"},
                      {"saveMode", "import"},
                      {"presetId", preset.id},
                      {"name", preset.name},
                      {"preset", presetJson}});
    Send(controller, {{"type", "loadPreset"}, {"presetId", preset.id}, {"preset", presetJson}, {"sceneId", "scene-1"}});
    host.Pump();
    controller.DrainControlSurfaceRequests();
    (void)reported(); // whatever the load moved

    passed =
        Check(gainParameter < static_cast<int>(slotIds.size()) && bypassParameter < static_cast<int>(slotIds.size()) &&
                  triggerParameter < static_cast<int>(slotIds.size()),
              "setup: the mapped slots are DAW parameters") &&
        passed;

    // A knob turned in the UI. Gain runs -24..24 dB, so +12 dB is 0.75.
    Send(controller, {{"type", "updateSignalPathNodeParam"},
                      {"presetId", kPresetId},
                      {"nodeId", "gain"},
                      {"paramKey", "gainDb"},
                      {"value", 12.0}});
    auto changed = reported();
    passed = Check(Near(controller.GetDawParameterValue(gainParameter), 0.75f),
                   "UI knob at +12 dB: the host reads 0.75 (" +
                       std::to_string(controller.GetDawParameterValue(gainParameter)) + ")") &&
             passed;
    passed = Check(changed == std::vector<int>{gainParameter}, "UI knob: only its parameter is reported") && passed;

    // The same value again is no news.
    Send(controller, {{"type", "updateSignalPathNodeParam"},
                      {"presetId", kPresetId},
                      {"nodeId", "gain"},
                      {"paramKey", "gainDb"},
                      {"value", 12.0}});
    passed = Check(reported().empty(), "UI knob moved to where it already was: nothing reported") && passed;

    // The host's lane writes it: applied, and never sent back.
    controller.ApplyAutomationFromDAW(kGainSlot, 0.25f);
    controller.DrainControlSurfaceRequests();
    passed = Check(Near(controller.GetDawParameterValue(gainParameter), 0.25f) && reported().empty(),
                   "host lane at 0.25: applied, and not reported back") &&
             passed;

    // Another node of the type: the slot goes by type and drives the first, so not this one.
    Send(controller, {{"type", "updateSignalPathNodeParam"},
                      {"presetId", kPresetId},
                      {"nodeId", "gain2"},
                      {"paramKey", "gainDb"},
                      {"value", -6.0}});
    passed = Check(Near(controller.GetDawParameterValue(gainParameter), 0.25f) && reported().empty(),
                   "a second gain node's knob: the parameter driving the first is left alone") &&
             passed;

    // Switched off in the UI: the bypass parameter ("enabled": high means on) follows. A new slot
    // holds 0 until something writes it, so the host's lane puts it at "on" first.
    controller.ApplyAutomationFromDAW(kBypassSlot, 1.0f);
    controller.DrainControlSurfaceRequests();
    (void)reported();
    Send(controller,
         {{"type", "updateSignalPathNodeBypass"}, {"presetId", kPresetId}, {"nodeId", "gain"}, {"enabled", false}});
    changed = reported();
    passed = Check(controller.GetDawParameterValue(bypassParameter) == 0.0f &&
                       std::find(changed.begin(), changed.end(), bypassParameter) != changed.end(),
                   "node switched off in the UI: its enabled parameter reads 0 and is reported") &&
             passed;
    Send(controller,
         {{"type", "updateSignalPathNodeBypass"}, {"presetId", kPresetId}, {"nodeId", "gain"}, {"enabled", true}});
    passed = Check(controller.GetDawParameterValue(bypassParameter) == 1.0f && !reported().empty(),
                   "and back on: it reads 1 and is reported") &&
             passed;

    // A trigger the host fires keeps the host's value.
    controller.ApplyAutomationFromDAW(kSceneTrigger, 1.0f);
    controller.DrainControlSurfaceRequests();
    passed = Check(controller.GetActiveSceneIndex() == 1, "host fires Scene 2: scene 2 plays") && passed;
    passed = Check(controller.GetDawParameterValue(triggerParameter) == 1.0f,
                   "the trigger's parameter still reads the host's 1, so the same value sent on playback "
                   "start is dropped as unchanged") &&
             passed;

    // A real rising edge after the user picked another scene still fires.
    Send(controller, {{"type", "selectScene"}, {"sceneId", "scene-1"}});
    controller.DrainControlSurfaceRequests();
    controller.ApplyAutomationFromDAW(kSceneTrigger, 0.0f);
    controller.ApplyAutomationFromDAW(kSceneTrigger, 1.0f);
    controller.DrainControlSurfaceRequests();
    passed =
        Check(controller.GetActiveSceneIndex() == 1, "the lane falls to 0 and rises again: scene 2 plays") && passed;

    // A trigger fired from the UI (its Test button) still falls back to 0, so it fires again.
    Send(controller, {{"type", "selectScene"}, {"sceneId", "scene-1"}});
    controller.DrainControlSurfaceRequests();
    Send(controller, {{"type", "setAutomationValue"}, {"slotId", kSceneTrigger}, {"value", 1.0}});
    controller.DrainControlSurfaceRequests();
    passed = Check(controller.GetActiveSceneIndex() == 1 && controller.GetDawParameterValue(triggerParameter) == 0.0f,
                   "Test button fires Scene 2, and its parameter falls back to 0") &&
             passed;

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-plugin-edit-host-report-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    const bool passed = Run(sandbox);
    std::cout << (passed ? "PluginEditHostReportTests PASSED\n" : "PluginEditHostReportTests FAILED\n");
    return passed ? 0 : 1;
}
