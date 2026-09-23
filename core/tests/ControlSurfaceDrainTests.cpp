/**
 * @file ControlSurfaceDrainTests.cpp
 * @brief Setlist steps, bank changes and scene switches from MIDI or DAW automation apply with
 * the editor closed.
 *
 * Automation applies under mDSPMutex (MIDI in the audio callback, a DAW parameter), and each of
 * these loads a preset or rewrites the setlist, which takes that lock. So they are parked in
 * ControlSurfaceQueue for the message thread. OnIdle drains them, but only an open editor drives
 * OnIdle. The plugin drains them off a timer of its own as well, through
 * DrainControlSurfaceRequests(). This test's thread is the message thread, and never calls
 * OnIdle until the last check, as with the editor closed.
 *  - The audio-thread MIDI pass parks a scene switch, a setlist step and a bank change rather
 *    than applying them, and nothing else the host runs applies them.
 *  - The drain applies each one.
 *  - A DAW parameter's scene switch goes the same way.
 *  - A restore the host left queued from another thread is applied before a parked switch, so
 *    the switch lands on the restored preset instead of being overwritten by it.
 *  - A node parameter and a bypass moved by MIDI reach the working copy through the drain, in the
 *    active scene's graph as well as the preset's, and so reach what the host saves.
 *  - OnIdle still drains them too.
 */

#include <future>
#include <iostream>
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
constexpr const char* kPresetA = "control-surface-drain-a"; // two scenes
constexpr const char* kPresetB = "control-surface-drain-b"; // one

// Footswitches, as CC numbers.
constexpr int kSceneOneCc = 20;
constexpr int kSceneTwoCc = 21;
constexpr int kSetlistTwoCc = 22;
constexpr int kBankUpCc = 23;
constexpr int kBankDownCc = 24;
// An expression pedal on the gain node, and a footswitch bypassing it.
constexpr int kGainCc = 25;
constexpr int kGainBypassCc = 26;
constexpr const char* kGainNodeId = "gain";

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

Preset BuildPreset(const std::string& id, int sceneCount)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;

    GraphNode gain;
    gain.id = kGainNodeId;
    gain.type = "gain";
    gain.params["gainDb"] = 0.0;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {{"in", kGainNodeId, 0, 0, 1.0}, {kGainNodeId, "out", 0, 0, 1.0}};
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

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

void MapFootswitch(PluginController& controller, const std::string& slotId, int cc)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = -1;
    map.controller = cc;
    (void)controller.GetAutomationSlots().SetDefaultSlotOverrides(slotId, std::nullopt, map, std::nullopt);
}

void MapNodeSlot(PluginController& controller, const std::string& slotId, const std::string& address, int cc)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = -1;
    map.controller = cc;
    (void)controller.GetAutomationSlots().SetCustomSlot(slotId, slotId, address, std::nullopt, map, std::nullopt);
}

/// The gain node's value and bypass in `graph`, as "gainDb/enabled", or "missing".
std::string GainNodeState(const SignalGraph& graph)
{
    const auto* node = graph.FindNode(kGainNodeId);

    if (!node)
    {
        return "missing";
    }

    const auto gainDb = node->params.find("gainDb");
    return (gainDb == node->params.end() ? std::string{"none"} : std::to_string(gainDb->second)) + "/" +
           (node->enabled ? "on" : "bypassed");
}

/// The same, from the state the host would save. A preset with scenes is saved as its scenes
/// alone, so this reads the active one's graph.
std::string SavedGainNodeState(const PluginController& controller)
{
    const auto state = nlohmann::json::parse(controller.SerializeState());
    const auto& active = *controller.GetActivePreset();
    const auto& sceneId = active.scenes[static_cast<std::size_t>(controller.GetActiveSceneIndex())].id;
    const auto preset = state.value("preset", nlohmann::json::object());

    for (const auto& scene : preset.value("scenes", nlohmann::json::array()))
    {
        if (scene.value("id", "") != sceneId)
        {
            continue;
        }

        for (const auto& node : scene.value("graph", nlohmann::json::object()).value("nodes", nlohmann::json::array()))
        {
            if (node.value("id", "") == kGainNodeId)
            {
                const auto params = node.value("params", nlohmann::json::object());
                return std::to_string(params.value("gainDb", -1.0)) + "/" +
                       (node.value("enabled", true) ? "on" : "bypassed");
            }
        }
    }

    return "missing";
}

/// A press and release, handed over in one audio block the way the plugin's processBlock does.
void PressFootswitch(PluginController& controller, int cc)
{
    const auto number = static_cast<std::uint8_t>(cc);
    controller.EnqueueMidi(MidiEvent{0xB0, number, 127, 0});
    controller.EnqueueMidi(MidiEvent{0xB0, number, 0, 0});
    controller.ProcessQueuedMidi();
}

std::string ActivePresetId(const PluginController& controller)
{
    const auto& active = controller.GetActivePreset();
    return active ? active->id : std::string{"<none>"};
}

/// What the plugin's timer does with the editor closed.
void Drain(PluginController& controller)
{
    controller.DrainControlSurfaceRequests();
}

bool Run(const fs::path& sandbox)
{
    test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();

    for (const auto& preset : {BuildPreset(kPresetA, 2), BuildPreset(kPresetB, 1)})
    {
        Send(controller,
             {{"type", "savePreset"}, {"presetId", preset.id}, {"name", preset.name}, {"preset", PresetJson(preset)}});
    }

    const auto slots = nlohmann::json::array({{{"presetId", kPresetA}}, {{"presetId", kPresetB}}});
    Send(controller,
         {{"type", "setSetlists"},
          {"setlists", nlohmann::json::array({{{"id", "set-1"}, {"name", "Set 1"}, {"bank", 1}, {"slots", slots}},
                                              {{"id", "set-2"}, {"name", "Set 2"}, {"bank", 2}, {"slots", slots}}})},
          {"activeSetlistId", "set-1"},
          {"cursorIndex", 0}});
    Send(controller, {{"type", "loadPreset"},
                      {"presetId", kPresetA},
                      {"preset", PresetJson(BuildPreset(kPresetA, 2))},
                      {"sceneId", "scene-1"}});
    host.Pump();

    MapFootswitch(controller, "default.scene1", kSceneOneCc);
    MapFootswitch(controller, "default.scene2", kSceneTwoCc);
    MapFootswitch(controller, "default.setlistPreset2", kSetlistTwoCc);
    MapFootswitch(controller, "default.bankUp", kBankUpCc);
    MapFootswitch(controller, "default.bankDown", kBankDownCc);

    bool passed = Check(ActivePresetId(controller) == kPresetA && controller.GetActiveSceneIndex() == 0 &&
                            controller.GetSetlistBankNumber() == 1,
                        "setup: preset A, scene 1, bank 1");

    // MIDI scene switch.
    PressFootswitch(controller, kSceneTwoCc);
    host.Pump();
    passed = Check(controller.GetActiveSceneIndex() == 0,
                   "MIDI scene switch: parked by the audio-thread pass, and nothing the host runs applies it") &&
             passed;
    Drain(controller);
    passed = Check(controller.GetActiveSceneIndex() == 1, "MIDI scene switch: the drain applies it") && passed;

    // DAW parameter scene switch. The adapter applies it under the DSP lock, as automation does.
    controller.ApplyAutomationFromDAW("default.scene1", 1.0f);
    passed = Check(controller.GetActiveSceneIndex() == 1, "DAW scene switch: parked") && passed;
    Drain(controller);
    passed = Check(controller.GetActiveSceneIndex() == 0, "DAW scene switch: the drain applies it") && passed;

    const auto savedOnSceneOne = controller.SerializeState();

    // MIDI setlist step.
    PressFootswitch(controller, kSetlistTwoCc);
    host.Pump();
    passed = Check(ActivePresetId(controller) == kPresetA && controller.GetSetlistCursorIndex() == 0,
                   "MIDI setlist step: parked") &&
             passed;
    Drain(controller);
    passed = Check(ActivePresetId(controller) == kPresetB && controller.GetSetlistCursorIndex() == 1,
                   "MIDI setlist step: the drain loads the slot's preset (" + ActivePresetId(controller) + ")") &&
             passed;

    // A restore from a host thread, left queued because the message thread is busy (it waits on
    // the call here), then a scene switch parked after it. The restore puts preset A back on
    // scene 1; the switch has to land on top of it, not be overwritten when the restore runs.
    auto restore = std::async(std::launch::async, [&] { return controller.DeserializeState(savedOnSceneOne); });
    passed = Check(!restore.get(), "host-thread restore: left queued while the message thread was busy") && passed;
    PressFootswitch(controller, kSceneTwoCc);
    Drain(controller);
    passed = Check(ActivePresetId(controller) == kPresetA && controller.GetActiveSceneIndex() == 1,
                   "host-thread restore: the drain applies it first, then the switch parked after it (" +
                       ActivePresetId(controller) + ", scene index " +
                       std::to_string(controller.GetActiveSceneIndex()) + ")") &&
             passed;
    host.Pump();
    passed = Check(ActivePresetId(controller) == kPresetA && controller.GetActiveSceneIndex() == 1,
                   "host-thread restore: its own task later finds nothing left to do") &&
             passed;

    // MIDI bank change.
    PressFootswitch(controller, kBankUpCc);
    host.Pump();
    passed = Check(controller.GetSetlistBankNumber() == 1, "MIDI bank up: parked") && passed;
    Drain(controller);
    passed = Check(controller.GetSetlistBankNumber() == 2, "MIDI bank up: the drain applies it") && passed;

    // A pedal on the gain node and a footswitch bypassing it, with the editor closed. The audio
    // pass applies both to the engine; the drain folds them into the working copy.
    MapNodeSlot(controller, "custom.gain", "node.gain.gainDb", kGainCc);
    MapNodeSlot(controller, "custom.gainBypass", "node.gain.bypassed", kGainBypassCc);
    const auto sceneGraph = [&controller]() -> const SignalGraph& {
        return controller.GetActivePreset()->scenes[static_cast<std::size_t>(controller.GetActiveSceneIndex())].graph;
    };
    const std::string before = "0.000000/on";
    const std::string after = "24.000000/bypassed";

    const auto pedal = static_cast<std::uint8_t>(kGainCc);
    const auto bypass = static_cast<std::uint8_t>(kGainBypassCc);
    controller.EnqueueMidi(MidiEvent{0xB0, pedal, 127, 0});
    controller.EnqueueMidi(MidiEvent{0xB0, bypass, 127, 0});
    controller.ProcessQueuedMidi();
    host.Pump();
    passed = Check(controller.GetAutomationSlots().HasNodeChanges() &&
                       GainNodeState(controller.GetActivePreset()->graph) == before,
                   "MIDI node changes: waiting for the message thread, the working copy as it was") &&
             passed;
    Drain(controller);
    passed = Check(GainNodeState(controller.GetActivePreset()->graph) == after,
                   "MIDI node changes: the drain folds them into the preset's graph (" +
                       GainNodeState(controller.GetActivePreset()->graph) + ")") &&
             passed;
    passed = Check(GainNodeState(sceneGraph()) == after,
                   "MIDI node changes: and into the active scene's, which a broadcast copies over it (" +
                       GainNodeState(sceneGraph()) + ")") &&
             passed;
    passed = Check(SavedGainNodeState(controller) == after,
                   "MIDI node changes: the host's save carries them (" + SavedGainNodeState(controller) + ")") &&
             passed;
    passed =
        Check(!controller.GetAutomationSlots().HasNodeChanges(), "MIDI node changes: nothing left waiting") && passed;

    // With the editor open, OnIdle drains them as before.
    PressFootswitch(controller, kBankDownCc);
    controller.OnIdle();
    passed = Check(controller.GetSetlistBankNumber() == 1, "OnIdle still drains them") && passed;

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-control-surface-drain-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    const bool passed = Run(sandbox);
    std::cout << (passed ? "ControlSurfaceDrainTests PASSED\n" : "ControlSurfaceDrainTests FAILED\n");
    return passed ? 0 : 1;
}
