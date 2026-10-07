/**
 * @file MidiBankSelectTests.cpp
 * @brief A MIDI controller recalls "preset N of bank B" with Bank Select (CC0 and/or CC32)
 * followed by a Program Change, and a Program Change or Note On mapped to a trigger fires it.
 *
 * On the automation table, with every setlist and scene request recorded in the order made:
 *  - A Program Change mapped in Absolute mode, the mode MIDI learn gives it, fires a setlist
 *    preset or scene whatever its number. Programs 0-63 used to do nothing. A soft Note On
 *    fires a trigger too.
 *  - A Bank Select ahead of a Program Change that fires a setlist preset selects that bank
 *    first. CC0 alone, CC32 alone, either with the other half 0, and the 14-bit pair all name
 *    the bank they should.
 *  - The bank select belongs to the next Program Change on its own channel, which takes it,
 *    whatever that program change is mapped to.
 *  - MIDI learn skips a Bank Select, so learning a footswitch that sends one catches its
 *    Program Change, except for Select Bank, which learns it.
 * Through the controller, with real setlists and the requests parked for the message thread:
 *  - Learning a footswitch that sends Bank Select + Program Change maps the program change.
 *  - Bank Select + Program Change in one audio block switches the setlist and then loads the
 *    slot from it. The queue used to load the preset first, from the old setlist.
 *  - Bank up then a preset, in one block, loads the preset from the new bank.
 *  - A bank no setlist claims leaves the active setlist to pick from.
 */

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "automation/AutomationSlotTable.h"
#include "automation/AutomationTypes.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
using Calls = std::vector<std::string>;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

std::string Describe(const Calls& calls)
{
    std::string text = "[";

    for (const auto& call : calls)
    {
        text += (text.size() > 1 ? ", " : "") + call;
    }

    return text + "]";
}

/// `calls` is what happened since the last look; it is cleared for the next.
bool CheckCalls(Calls& calls, const Calls& expected, const std::string& what)
{
    const bool passed = Check(calls == expected, what + " " + Describe(calls));
    calls.clear();
    return passed;
}

MidiEvent Cc(int channel, int controller, int value)
{
    return MidiEvent{static_cast<std::uint8_t>(0xB0 | channel), static_cast<std::uint8_t>(controller),
                     static_cast<std::uint8_t>(value), 0};
}

/// A Program Change has one data byte. What a host's buffer holds after it is not zero, so
/// the third byte here is not either.
MidiEvent Program(int channel, int program)
{
    return MidiEvent{static_cast<std::uint8_t>(0xC0 | channel), static_cast<std::uint8_t>(program), 0x5A, 0};
}

MidiEvent NoteOn(int channel, int note, int velocity)
{
    return MidiEvent{static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
                     static_cast<std::uint8_t>(velocity), 0};
}

/// A mapping as MIDI learn makes it: one channel, Absolute.
MidiControlMap Learned(MidiControlMap::EventType eventType, int controller, int channel = -1)
{
    MidiControlMap map;
    map.eventType = eventType;
    map.channel = channel;
    map.controller = controller;
    map.mode = MidiControlMap::Mode::Absolute;
    return map;
}

bool Map(AutomationSlotTable& table, const std::string& slotId, const MidiControlMap& map)
{
    return table.SetDefaultSlotOverrides(slotId, std::nullopt, map, std::nullopt);
}

void Send(AutomationSlotTable& table, std::initializer_list<MidiEvent> events)
{
    for (const auto& event : events)
    {
        table.HandleMidi(event);
    }
}

bool RunTableTests()
{
    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(48000.0, 64);

    Calls calls;
    const auto record = [&calls](const char* what) {
        return [&calls, what](int value) { calls.push_back(std::string(what) + " " + std::to_string(value)); };
    };

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, record("preset"), record("bankUp"), record("bankDown"), []() { return 8; },
        []() { return 0; }, record("bank"), []() { return 0; }, record("scene"), []() { return -1; });

    using Type = MidiControlMap::EventType;
    bool passed = Map(table, "default.setlistPreset1", Learned(Type::ProgramChange, 0)) &&
                  Map(table, "default.setlistPreset2", Learned(Type::ProgramChange, 5)) &&
                  Map(table, "default.scene1", Learned(Type::ProgramChange, 7)) &&
                  Map(table, "default.bankUp", Learned(Type::NoteOn, 60));
    passed = Check(passed, "table: mappings set") && passed;

    // Absolute Program Change on a trigger.
    Send(table, {Program(0, 0)});
    passed = CheckCalls(calls, {"preset 0"}, "program 0 fires Setlist Preset 1") && passed;
    Send(table, {Program(0, 0)});
    passed = CheckCalls(calls, {"preset 0"}, "and fires it again") && passed;
    Send(table, {Program(0, 5)});
    passed = CheckCalls(calls, {"preset 1"}, "program 5 fires Setlist Preset 2") && passed;
    Send(table, {Program(0, 7)});
    passed = CheckCalls(calls, {"scene 0"}, "program 7 fires Scene 1") && passed;
    Send(table, {Program(0, 9)});
    passed = CheckCalls(calls, {}, "an unmapped program fires nothing") && passed;
    Send(table, {NoteOn(0, 60, 10)});
    passed = CheckCalls(calls, {"bankUp 1"}, "a soft Note On fires Bank Up") && passed;

    // Bank Select, then the Program Change it belongs to.
    Send(table, {Cc(0, 0, 3), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 3", "preset 0"}, "CC0 3, program 0: bank 3, then its first preset") && passed;
    Send(table, {Program(0, 5)});
    passed = CheckCalls(calls, {"preset 1"}, "a program change on its own selects no bank") && passed;
    Send(table, {Cc(0, 32, 4), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 4", "preset 0"}, "CC32 alone names the bank") && passed;
    Send(table, {Cc(0, 0, 0), Cc(0, 32, 5), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 5", "preset 0"}, "CC0 0 + CC32 5: bank 5") && passed;
    Send(table, {Cc(0, 0, 6), Cc(0, 32, 0), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 6", "preset 0"}, "CC0 6 + CC32 0: bank 6") && passed;
    Send(table, {Cc(0, 0, 1), Cc(0, 32, 2), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 130", "preset 0"}, "CC0 1 + CC32 2: the 14-bit bank 130") && passed;
    Send(table, {Cc(0, 0, 0), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 0", "preset 0"}, "CC0 0 names bank 0") && passed;

    // Channels keep their own.
    Send(table, {Cc(1, 0, 4), Program(0, 0)});
    passed = CheckCalls(calls, {"preset 0"}, "a bank select on channel 2 leaves channel 1's program change") && passed;
    Send(table, {Program(1, 0)});
    passed = CheckCalls(calls, {"bank 4", "preset 0"}, "and is still there for channel 2's") && passed;

    // The next Program Change takes it, whatever it is mapped to.
    Send(table, {Cc(0, 0, 8), Program(0, 7), Program(0, 0)});
    passed = CheckCalls(calls, {"scene 0", "preset 0"},
                        "a program change mapped to a scene takes the bank select, selecting no bank") &&
             passed;
    Send(table, {Cc(0, 0, 8), Program(0, 9), Program(0, 0)});
    passed = CheckCalls(calls, {"preset 0"}, "so does an unmapped one") && passed;

    // MIDI learn.
    table.ArmMidiLearn("default.setlistPreset3");
    Send(table, {Cc(2, 0, 2), Cc(2, 32, 0), Program(2, 3)});
    auto capture = table.PollMidiLearnCapture();
    passed = Check(capture && capture->eventType == Type::ProgramChange && capture->controller == 3 &&
                       capture->channel == 2 && capture->mode == MidiControlMap::Mode::Absolute,
                   "learning a preset skips the bank select and catches program 3 on channel 3") &&
             passed;
    Send(table, {Program(2, 0)});
    passed = CheckCalls(calls, {"preset 0"}, "the learned program change took its bank select") && passed;

    table.ArmMidiLearn("default.bankSelect");
    Send(table, {Cc(0, 0, 2)});
    capture = table.PollMidiLearnCapture();
    passed = Check(capture && capture->eventType == Type::CC && capture->controller == 0,
                   "learning Select Bank catches CC0") &&
             passed;
    passed = CheckCalls(calls, {}, "and applies nothing while learning") && passed;
    Send(table, {Program(0, 0)});
    passed = CheckCalls(calls, {"bank 2", "preset 0"},
                        "a bank select sent while learning still belongs to the "
                        "next program change") &&
             passed;

    // Select Bank mapped to CC0 selects at once, and again with the program change.
    passed = Check(Map(table, "default.bankSelect", Learned(Type::CC, 0)), "Select Bank mapped to CC0") && passed;
    Send(table, {Cc(0, 0, 6), Program(0, 0)});
    passed = CheckCalls(calls, {"bank 6", "bank 6", "preset 0"},
                        "with Select Bank on CC0, the bank is selected on CC0 and before the preset") &&
             passed;

    return passed;
}

// ── Through the controller ──────────────────────────────────────────────

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetA = "midi-bank-select-a";
constexpr const char* kPresetB = "midi-bank-select-b";
constexpr int kBankUpCc = 23;

Preset BuildPreset(const std::string& id)
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
    gain.id = "gain";
    gain.type = "gain";
    gain.params["gainDb"] = 0.0;

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {{"in", "gain", 0, 0, 1.0}, {"gain", "out", 0, 0, 1.0}};
    NormalizePresetScenes(preset);
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

/// One audio block's MIDI, handed over the way the plugin's processBlock does.
void Play(PluginController& controller, std::initializer_list<MidiEvent> events)
{
    for (const auto& event : events)
    {
        controller.EnqueueMidi(event);
    }

    controller.ProcessQueuedMidi();
}

std::string ActivePresetId(const PluginController& controller)
{
    const auto& active = controller.GetActivePreset();
    return active ? active->id : std::string{"<none>"};
}

std::string Where(const PluginController& controller)
{
    return "(bank " + std::to_string(controller.GetSetlistBankNumber()) + ", slot " +
           std::to_string(controller.GetSetlistCursorIndex()) + ", " + ActivePresetId(controller) + ")";
}

bool IsAt(const PluginController& controller, int bank, int slot, const std::string& presetId)
{
    return controller.GetSetlistBankNumber() == bank && controller.GetSetlistCursorIndex() == slot &&
           ActivePresetId(controller) == presetId;
}

bool RunControllerTests(const fs::path& sandbox)
{
    test::PumpedTestHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();

    for (const auto& preset : {BuildPreset(kPresetA), BuildPreset(kPresetB)})
    {
        Send(controller,
             {{"type", "savePreset"}, {"presetId", preset.id}, {"name", preset.name}, {"preset", PresetJson(preset)}});
    }

    // The two setlists hold the presets in opposite orders, so which one a slot was loaded from
    // shows in the preset playing.
    const auto slots = [](const char* first, const char* second) {
        return nlohmann::json::array({{{"presetId", first}}, {{"presetId", second}}});
    };
    Send(controller,
         {{"type", "setSetlists"},
          {"setlists", nlohmann::json::array(
                           {{{"id", "set-1"}, {"name", "Set 1"}, {"bank", 1}, {"slots", slots(kPresetA, kPresetB)}},
                            {{"id", "set-2"}, {"name", "Set 2"}, {"bank", 2}, {"slots", slots(kPresetB, kPresetA)}}})},
          {"activeSetlistId", "set-1"},
          {"cursorIndex", 0}});
    Send(controller, {{"type", "loadPreset"}, {"presetId", kPresetA}, {"preset", PresetJson(BuildPreset(kPresetA))}});
    host.Pump();

    bool passed =
        Check(IsAt(controller, 1, 0, kPresetA), "controller setup: bank 1, slot 1, preset A " + Where(controller));

    // Learn Setlist Preset 1 from a footswitch that sends Bank Select + Program Change.
    Send(controller, {{"type", "armMidiLearn"}, {"slotId", "default.setlistPreset1"}});
    Play(controller, {Cc(0, 0, 2), Cc(0, 32, 0), Program(0, 0)});
    controller.OnIdle();
    const auto* learned = controller.GetAutomationSlots().FindSlot("default.setlistPreset1");
    passed =
        Check(learned && learned->midiMap && learned->midiMap->eventType == MidiControlMap::EventType::ProgramChange &&
                  learned->midiMap->controller == 0,
              "MIDI learn maps the footswitch's program change, not its bank select") &&
        passed;
    passed = Check(IsAt(controller, 1, 0, kPresetA), "and learning applies nothing " + Where(controller)) && passed;

    MidiControlMap second;
    second.eventType = MidiControlMap::EventType::ProgramChange;
    second.channel = 0;
    second.controller = 1;
    MidiControlMap bankUp;
    bankUp.channel = -1;
    bankUp.controller = kBankUpCc;
    passed = Check(controller.GetAutomationSlots().SetDefaultSlotOverrides("default.setlistPreset2", std::nullopt,
                                                                           second, std::nullopt) &&
                       controller.GetAutomationSlots().SetDefaultSlotOverrides("default.bankUp", std::nullopt, bankUp,
                                                                               std::nullopt),
                   "Setlist Preset 2 mapped to program 1, Bank Up to a footswitch") &&
             passed;

    // Bank 2, preset 1, in one block.
    Play(controller, {Cc(0, 0, 2), Cc(0, 32, 0), Program(0, 0)});
    host.Pump();
    passed =
        Check(IsAt(controller, 1, 0, kPresetA), "bank select + program change: parked " + Where(controller)) && passed;
    controller.DrainControlSurfaceRequests();
    passed = Check(IsAt(controller, 2, 0, kPresetB),
                   "bank select + program change: bank 2, then its slot 1, preset B " + Where(controller)) &&
             passed;

    // The UI moves to bank 1; a program change on its own picks from there.
    Send(controller, {{"type", "selectSetlist"}, {"setlistId", "set-1"}});
    Play(controller, {Program(0, 0)});
    controller.DrainControlSurfaceRequests();
    passed = Check(IsAt(controller, 1, 0, kPresetA),
                   "a program change on its own picks from the active bank: slot 1 of bank 1, preset A " +
                       Where(controller)) &&
             passed;

    // Bank up, then the second preset, before the message thread gets to either.
    Play(controller, {Cc(0, kBankUpCc, 127), Cc(0, kBankUpCc, 0), Program(0, 1)});
    controller.DrainControlSurfaceRequests();
    passed = Check(IsAt(controller, 2, 1, kPresetA),
                   "bank up then program 1: slot 2 of the new bank, preset A " + Where(controller)) &&
             passed;

    // A bank no setlist claims.
    Play(controller, {Cc(0, 0, 9), Program(0, 0)});
    controller.DrainControlSurfaceRequests();
    passed = Check(IsAt(controller, 2, 0, kPresetB),
                   "an unclaimed bank leaves the active one to pick from: slot 1 of bank 2, preset B " +
                       Where(controller)) &&
             passed;

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-midi-bank-select-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    bool passed = RunTableTests();
    passed = RunControllerTests(sandbox) && passed;
    std::cout << (passed ? "MidiBankSelectTests PASSED\n" : "MidiBankSelectTests FAILED\n");
    return passed ? 0 : 1;
}
