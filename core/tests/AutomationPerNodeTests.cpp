/**
 * AutomationPerNodeTests.cpp — MIDI on one effect when the preset has two of its type, and
 * toggles that start from the effect's real state.
 *
 * A node.* address names an effect type, so a mapping on it drives the first node of that type
 * (a parameter) or every node of it (a bypass). A per-preset mapping can name its node instead,
 * with an "id:<nodeId>" node selector, so two amps in parallel each answer to their own pedal.
 *
 * A toggle used to flip the value the mapping last set, which a preset load or a click in the
 * editor leaves behind: the first press then set the state the effect was already in. It now
 * reads the node. The UI's Test button and a Trigger key take the same path.
 */

#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
const std::string kPresetId = "twoGains";

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

/// Input → g1 → g2 → Output, two nodes of one type, with g2 saved bypassed: the preset's own
/// "off when it loads" state.
Preset MakeTwoGainPreset()
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Two Gains";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode g1{"g1", "gain", "utility", "Gain 1", true};
    g1.params["gainDb"] = 0.0;
    GraphNode g2{"g2", "gain", "utility", "Gain 2", false};
    g2.params["gainDb"] = 0.0;
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, g1, g2, out};
    preset.graph.edges = {
        GraphEdge{"in", "g1", 0, 0, 1.0},
        GraphEdge{"g1", "g2", 0, 0, 1.0},
        GraphEdge{"g2", "out", 0, 0, 1.0},
    };
    return preset;
}

MidiControlMap Cc(int controller, MidiControlMap::Mode mode)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = 0;
    map.controller = controller;
    map.mode = mode;
    return map;
}

void SendCc(AutomationSlotTable& table, int controller, int value)
{
    table.HandleMidi(MidiEvent{0xB0, static_cast<uint8_t>(controller), static_cast<uint8_t>(value), 0});
}

bool NodeEnabled(MultiPresetMixer& mixer, const std::string& nodeId)
{
    const auto* processor = mixer.GetNodeProcessor(kPresetId, nodeId);
    return processor != nullptr && processor->IsEnabled();
}

double NodeGainDb(MultiPresetMixer& mixer, const std::string& nodeId)
{
    const auto* processor = mixer.GetNodeProcessor(kPresetId, nodeId);
    return processor != nullptr ? processor->GetParam("gainDb") : std::nan("");
}

void InitializeTable(AutomationSlotTable& table, MultiPresetMixer& mixer)
{
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());
    table.SetActivePresetId(kPresetId);
}

bool AddPresetSlot(AutomationSlotTable& table, const std::string& slotId, const std::string& address,
                   const std::string& nodeSelector, const MidiControlMap& map)
{
    return table.SetPresetSlot(slotId, kPresetId, std::optional<std::string>(slotId),
                               std::optional<std::string>(address), std::optional<std::string>(nodeSelector),
                               std::optional<MidiControlMap>(map));
}

std::vector<NodeChangeQueue::Change> TakeChanges(AutomationSlotTable& table)
{
    std::vector<NodeChangeQueue::Change> changes;
    table.TakeNodeChanges([&changes](NodeChangeQueue::Change&& change) { changes.push_back(std::move(change)); });
    return changes;
}

struct Rig
{
    ResourceLibrary library;
    MultiPresetMixer mixer;
    AutomationSlotTable table;

    bool Start()
    {
        mixer.SetResourceLibrary(&library);
        mixer.Prepare(kSampleRate, kBlockSize);
        const auto preset = MakeTwoGainPreset();

        if (!mixer.AddActivePreset(preset, preset.id, preset.name))
        {
            return false;
        }

        InitializeTable(table, mixer);
        return true;
    }
};

/// Latch (Absolute) on one node's "enabled": 127 turns that node on, 0 off, and the other node of
/// the same type is left alone. The issue's "Amp 1 on CC 21, Amp 2 with no control".
bool TestLatchDrivesOneNode()
{
    Rig rig;

    if (!Expect(rig.Start(), "Latch: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(AddPresetSlot(rig.table, "preset.1", "node.gain.enabled", "id:g2",
                                   Cc(21, MidiControlMap::Mode::Absolute)),
                     "Latch: could not add the per-preset slot");
    passed &= Expect(NodeEnabled(rig.mixer, "g1") && !NodeEnabled(rig.mixer, "g2"),
                     "Latch: g1 should load on and g2 off, as the preset saved them");

    SendCc(rig.table, 21, 127);
    passed &= Expect(NodeEnabled(rig.mixer, "g2"), "Latch: CC 127 should turn g2 on");
    passed &= Expect(NodeEnabled(rig.mixer, "g1"), "Latch: CC 127 on g2's mapping must leave g1 on");

    const auto changes = TakeChanges(rig.table);
    passed &= Expect(changes.size() == 1 && changes[0].nodeId && *changes[0].nodeId == "g2" &&
                         changes[0].binding->isBypass && changes[0].value == 1.0,
                     "Latch: the change for the working copy should name g2 and say enabled");

    SendCc(rig.table, 21, 0);
    passed &= Expect(!NodeEnabled(rig.mixer, "g2"), "Latch: CC 0 should turn g2 off");
    passed &= Expect(NodeEnabled(rig.mixer, "g1"), "Latch: CC 0 on g2's mapping must leave g1 on");

    // The inverted sense: "bypassed" on g1, so 127 turns it off.
    passed &= Expect(AddPresetSlot(rig.table, "preset.2", "node.gain.bypassed", "id:g1",
                                   Cc(22, MidiControlMap::Mode::Absolute)),
                     "Latch: could not add the inverted slot");
    SendCc(rig.table, 22, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g1"), "Latch: CC 127 on a bypass mapping should turn g1 off");
    passed &= Expect(!NodeEnabled(rig.mixer, "g2"), "Latch: g1's mapping must leave g2 as it was");

    // One CC on both mappings on purpose: both answer.
    passed &= Expect(AddPresetSlot(rig.table, "preset.3", "node.gain.enabled", "id:g2",
                                   Cc(22, MidiControlMap::Mode::Absolute)),
                     "Latch: could not add the shared-CC slot");
    SendCc(rig.table, 22, 0);
    passed &= Expect(NodeEnabled(rig.mixer, "g1") && !NodeEnabled(rig.mixer, "g2"),
                     "Latch: one CC mapped twice should switch g1 on (bypass 0) and leave g2 off (enabled 0)");
    SendCc(rig.table, 22, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g1") && NodeEnabled(rig.mixer, "g2"),
                     "Latch: one CC mapped twice should switch g1 off and g2 on");
    return passed;
}

/// A parameter mapping that names its node drives that node, even though another node of the
/// type comes first, and even while the node is bypassed.
bool TestParameterDrivesOneNode()
{
    Rig rig;

    if (!Expect(rig.Start(), "Parameter: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(AddPresetSlot(rig.table, "preset.1", "node.gain.gainDb", "id:g2",
                                   Cc(7, MidiControlMap::Mode::Absolute)),
                     "Parameter: could not add the per-preset slot");

    SendCc(rig.table, 7, 127);
    passed &= Expect(NodeGainDb(rig.mixer, "g1") == 0.0, "Parameter: g1's gain must not move");
    passed &= Expect(NodeGainDb(rig.mixer, "g2") > 0.0, "Parameter: g2's gain should go to the top of its range");

    const auto changes = TakeChanges(rig.table);
    passed &= Expect(changes.size() == 1 && changes[0].nodeId && *changes[0].nodeId == "g2" &&
                         !changes[0].binding->isBypass,
                     "Parameter: the change for the working copy should name g2's parameter");

    // A node id another type holds is not this mapping's node.
    passed &= Expect(AddPresetSlot(rig.table, "preset.2", "node.gain.gainDb", "id:out",
                                   Cc(8, MidiControlMap::Mode::Absolute)),
                     "Parameter: could not add the mismatched slot");
    SendCc(rig.table, 8, 127);
    passed &= Expect(TakeChanges(rig.table).empty(), "Parameter: a node of another type must not be driven");

    // Without a selector the address still means the first enabled node of the type.
    passed &= Expect(AddPresetSlot(rig.table, "preset.3", "node.gain.gainDb", "", Cc(9, MidiControlMap::Mode::Absolute)),
                     "Parameter: could not add the by-type slot");
    SendCc(rig.table, 9, 0);
    passed &= Expect(NodeGainDb(rig.mixer, "g1") < 0.0, "Parameter: a by-type mapping should still drive g1");
    return passed;
}

/// Toggle flips the node from the state it is in: the first press after a load turns on a node
/// the preset saved off, and a click in the editor between presses is not undone by the next.
bool TestToggleStartsFromTheNode()
{
    Rig rig;

    if (!Expect(rig.Start(), "Toggle: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(AddPresetSlot(rig.table, "preset.1", "node.gain.enabled", "id:g2",
                                   Cc(20, MidiControlMap::Mode::Toggle)),
                     "Toggle: could not add the per-preset slot");

    SendCc(rig.table, 20, 127);
    passed &= Expect(NodeEnabled(rig.mixer, "g2"), "Toggle: the first press should turn on g2, saved off");
    SendCc(rig.table, 20, 0);
    passed &= Expect(NodeEnabled(rig.mixer, "g2"), "Toggle: a release must change nothing");
    SendCc(rig.table, 20, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g2"), "Toggle: the second press should turn g2 off");
    SendCc(rig.table, 20, 0);

    // The editor turns it back on behind the mapping's back.
    rig.mixer.SetNodeEnabled(kPresetId, "g2", true);
    SendCc(rig.table, 20, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g2"), "Toggle: after the editor turned g2 on, a press should turn it off");
    SendCc(rig.table, 20, 0);

    // By type, with the other sense: "bypassed" reads the first gain node.
    passed &= Expect(AddPresetSlot(rig.table, "preset.2", "node.gain.bypassed", "", Cc(30, MidiControlMap::Mode::Toggle)),
                     "Toggle: could not add the by-type slot");
    rig.mixer.SetNodeEnabled(kPresetId, "g1", false);
    SendCc(rig.table, 30, 127);
    passed &= Expect(NodeEnabled(rig.mixer, "g1"), "Toggle: a by-type press should turn g1, found off, on");
    return passed;
}

/// "MIDI Learn toggle for this preset..." arms the learn in Toggle mode: the captured mapping
/// toggles that one node on each press, and the press that was learned changes nothing.
bool TestToggleLearn()
{
    Rig rig;

    if (!Expect(rig.Start(), "Toggle learn: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(rig.table.SetPresetSlot("preset.1", kPresetId, std::optional<std::string>("Gain 2: On/Off"),
                                             std::optional<std::string>("node.gain.enabled"),
                                             std::optional<std::string>("id:g2"), std::nullopt),
                     "Toggle learn: could not add the per-preset slot");

    rig.table.ArmMidiLearn("preset.1", MidiControlMap::Mode::Toggle);
    SendCc(rig.table, 25, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g2"), "Toggle learn: the learned press must not switch g2");
    const auto captured = rig.table.PollMidiLearnCapture();
    passed &= Expect(captured && captured->controller == 25 && captured->mode == MidiControlMap::Mode::Toggle,
                     "Toggle learn: the capture should be CC 25 in Toggle mode");

    if (!captured)
    {
        return false;
    }

    // As PluginController commits a capture.
    const auto* slot = rig.table.FindSlot("preset.1");
    passed &= Expect(rig.table.SetPresetSlot("preset.1", kPresetId, std::optional<std::string>(slot->label),
                                             std::optional<std::string>(slot->address),
                                             std::optional<std::string>(slot->nodeSelector), *captured),
                     "Toggle learn: could not commit the capture");
    SendCc(rig.table, 25, 0);
    SendCc(rig.table, 25, 127);
    passed &= Expect(NodeEnabled(rig.mixer, "g2") && NodeEnabled(rig.mixer, "g1"),
                     "Toggle learn: a press should turn g2 on and leave g1");
    SendCc(rig.table, 25, 0);
    SendCc(rig.table, 25, 127);
    passed &= Expect(!NodeEnabled(rig.mixer, "g2") && NodeEnabled(rig.mixer, "g1"),
                     "Toggle learn: the next press should turn g2 off and leave g1");

    // A plain learn still captures Absolute.
    rig.table.ArmMidiLearn("preset.1");
    SendCc(rig.table, 26, 127);
    const auto plain = rig.table.PollMidiLearnCapture();
    passed &= Expect(plain && plain->mode == MidiControlMap::Mode::Absolute,
                     "Toggle learn: a learn armed without a mode should capture Absolute");
    return passed;
}

/// The panel's Test button and a Trigger key toggle through ToggleBypassLocked, which reads the
/// node too, and a key learned on a bypass becomes a Trigger.
bool TestButtonAndKeyToggle()
{
    Rig rig;

    if (!Expect(rig.Start(), "Button: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(rig.table.SetCustomSlot("custom.1", std::optional<std::string>("Gain"),
                                             std::optional<std::string>("node.gain.bypassed"), std::nullopt,
                                             std::nullopt, std::nullopt),
                     "Button: could not add the custom slot");

    // The slot's value says 0 (not bypassed); g1 is on, so the first toggle bypasses it.
    passed &= Expect(rig.table.ToggleBypassLocked("custom.1", AutomationSource::UI), "Button: toggle should apply");
    passed &= Expect(!NodeEnabled(rig.mixer, "g1"), "Button: the first toggle should bypass g1");

    rig.mixer.SetNodeEnabled(kPresetId, "g1", true);
    passed &= Expect(rig.table.ToggleBypassLocked("custom.1", AutomationSource::UI), "Button: toggle should apply again");
    passed &= Expect(!NodeEnabled(rig.mixer, "g1"), "Button: after the editor turned g1 on, toggle should bypass it");

    passed &= Expect(!rig.table.ToggleBypassLocked("default.inputLevel", AutomationSource::UI),
                     "Button: a slot that is not a bypass is not toggled");

    const auto* slot = rig.table.FindSlot("custom.1");
    const auto maps = slot ? rig.table.LearnedKeyMaps(*slot, "b") : std::vector<KeyboardMap>{};
    passed &= Expect(maps.size() == 1 && maps[0].mode == KeyboardMap::Mode::Trigger,
                     "Key: a key learned on a bypass should be a Trigger");
    return passed;
}

/// The node selector of a per-preset mapping survives a save and load, so the mapping still
/// drives its own node.
bool TestSelectorRoundTrip()
{
    Rig rig;

    if (!Expect(rig.Start(), "Round trip: could not add the preset"))
    {
        return false;
    }

    bool passed = true;
    passed &= Expect(AddPresetSlot(rig.table, "preset.1", "node.gain.enabled", "id:g2",
                                   Cc(21, MidiControlMap::Mode::Absolute)),
                     "Round trip: could not add the per-preset slot");

    AutomationSlotTable reloaded;
    InitializeTable(reloaded, rig.mixer);
    reloaded.LoadFromJson(rig.table.SaveToJson());

    const auto* slot = reloaded.FindSlot("preset.1");
    passed &= Expect(slot && slot->nodeSelector == "id:g2", "Round trip: the node selector should be kept");
    passed &= Expect(slot && slot->nodeBinding && slot->nodeBinding->nodeId == "g2",
                     "Round trip: the reloaded slot should be bound to g2");

    SendCc(reloaded, 21, 127);
    passed &= Expect(NodeEnabled(rig.mixer, "g2"), "Round trip: the reloaded mapping should turn g2 on");
    return passed;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;
    allPassed &= TestLatchDrivesOneNode();
    allPassed &= TestParameterDrivesOneNode();
    allPassed &= TestToggleStartsFromTheNode();
    allPassed &= TestButtonAndKeyToggle();
    allPassed &= TestToggleLearn();
    allPassed &= TestSelectorRoundTrip();

    if (allPassed)
    {
        std::cout << "AutomationPerNodeTests passed" << std::endl;
        return 0;
    }

    return 1;
}
