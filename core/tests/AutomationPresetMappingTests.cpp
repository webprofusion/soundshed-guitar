/**
 * AutomationPresetMappingTests.cpp — Per-preset MIDI mappings.
 *
 * A per-preset slot answers MIDI only while its preset is active, and while it does it takes
 * its MIDI control over from a global mapping on the same control, so one pedal can drive a
 * different target in each preset. Per-preset slots stay out of the DAW parameter layout and
 * the custom slot budget, survive a save and load, and go when their preset's mappings are
 * removed.
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "automation/AutomationSlotTable.h"
#include "dsp/MultiPresetMixer.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr double kTolerance = 1e-4;

// global.inputTrim and global.outputTrim both span -40..+20 dB.
constexpr double kTrimMin = -40.0;
constexpr double kTrimMax = 20.0;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < kTolerance;
}

MidiControlMap Cc(int controller)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = 0;
    map.controller = controller;
    map.mode = MidiControlMap::Mode::Absolute;
    return map;
}

MidiEvent CcEvent(int controller, int value)
{
    return MidiEvent{0xB0, static_cast<uint8_t>(controller), static_cast<uint8_t>(value), 0};
}

void InitializeTable(AutomationSlotTable& table, MultiPresetMixer& mixer)
{
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
}

bool AddCustom(AutomationSlotTable& table, const std::string& slotId, const std::string& address, int controller)
{
    return table.SetCustomSlot(slotId, std::optional<std::string>(slotId), std::optional<std::string>(address),
                               std::nullopt, std::optional<MidiControlMap>(Cc(controller)), std::nullopt);
}

bool AddPresetSlot(AutomationSlotTable& table, const std::string& slotId, const std::string& presetId,
                   const std::string& address, int controller)
{
    return table.SetPresetSlot(slotId, presetId, std::optional<std::string>(slotId),
                               std::optional<std::string>(address), std::nullopt,
                               std::optional<MidiControlMap>(Cc(controller)));
}

/// Each DAW parameter reads a cell its slot mirrors its value into. Slot moves were defaulted, and
/// so deleted (std::atomic cannot move): the vector copied instead, and a copy does not take the
/// cell. A custom slot added past the vector's capacity left every other slot unjoined, and
/// removing one from the middle wrote each later slot's value into its neighbour's cell.
bool TestDawParametersFollowSlotsAsTheTableChanges(MultiPresetMixer& mixer)
{
    AutomationSlotTable table;
    InitializeTable(table, mixer);

    std::vector<std::string> layout = table.GetSlotIds();

    for (int i = 0; i < kMaxCustomSlots; ++i)
    {
        layout.push_back("custom." + std::to_string(i));
    }

    table.BindDawParameters(layout);
    const auto cell = [&](const std::string& slotId) {
        const auto it = std::find(layout.begin(), layout.end(), slotId);
        return table.GetDawParameterValue(static_cast<int>(it - layout.begin()));
    };
    const auto set = [&](const std::string& slotId, float value) {
        table.FindSlot(slotId)->SetValue(value, AutomationSource::UI);
    };
    bool passed = true;

    for (int i = 0; i < kMaxCustomSlots; ++i)
    {
        const auto id = "custom." + std::to_string(i);
        passed &= Expect(AddCustom(table, id, "global.outputTrim", 40 + i), "adding " + id);
        set(id, 0.01f * static_cast<float>(i + 1));
    }

    const auto show = [&](const std::string& slotId) { return ": " + std::to_string(cell(slotId)); };
    set("default.inputLevel", 0.75f);
    passed &= Expect(Near(cell("default.inputLevel"), 0.75),
                     "with the custom slots added, Input Level's parameter follows it" + show("default.inputLevel"));
    passed &= Expect(Near(cell("custom.0"), 0.01), "and so does the first custom slot's" + show("custom.0"));

    passed &= Expect(table.RemoveCustomSlot("custom.3"), "removing custom.3");
    passed &= Expect(cell("custom.3") == 0.0f, "a removed slot's parameter reads 0" + show("custom.3"));
    set("custom.4", 0.9f);
    passed &= Expect(Near(cell("custom.4"), 0.9) && cell("custom.3") == 0.0f,
                     "the slot after it keeps its own parameter" + show("custom.4"));
    return passed;
}
} // namespace

int main()
{
    bool allPassed = true;

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto inputTrim = [&mixer]() { return mixer.GetGlobalChainConfig().inputGain; };
    const auto outputTrim = [&mixer]() { return mixer.GetGlobalChainConfig().outputGain; };

    AutomationSlotTable table;
    InitializeTable(table, mixer);

    // CC 7 drives Input Trim and CC 8 Output Trim everywhere; preset A maps CC 7 to Output Trim.
    allPassed &= Expect(AddCustom(table, "custom.input", "global.inputTrim", 7), "creating the global CC 7 slot");
    allPassed &= Expect(AddCustom(table, "custom.output", "global.outputTrim", 8), "creating the global CC 8 slot");
    allPassed &=
        Expect(AddPresetSlot(table, "preset.1", "presetA", "global.outputTrim", 7), "creating preset A's CC 7 slot");

    // No preset active: only the global mapping answers CC 7.
    table.HandleMidi(CcEvent(7, 127));
    allPassed &= Expect(Near(inputTrim(), kTrimMax), "with no preset active, CC 7 should drive Input Trim");
    allPassed &= Expect(Near(outputTrim(), 0.0), "with no preset active, preset A's CC 7 mapping should stay silent");

    // Preset A active: its mapping takes CC 7 over, and the global one stands down.
    table.SetActivePresetId("presetA");
    table.HandleMidi(CcEvent(7, 0));
    allPassed &= Expect(Near(outputTrim(), kTrimMin), "in preset A, CC 7 should drive Output Trim");
    allPassed &= Expect(Near(inputTrim(), kTrimMax), "in preset A, the global CC 7 mapping should not also fire");

    // A control preset A does not map is not taken over.
    table.HandleMidi(CcEvent(8, 127));
    allPassed &= Expect(Near(outputTrim(), kTrimMax), "in preset A, the global CC 8 mapping should still answer");

    // Preset B active: preset A's mapping is not live, so the global CC 7 mapping answers again.
    table.SetActivePresetId("presetB");
    table.HandleMidi(CcEvent(7, 64));
    const double midTrim = kTrimMin + (64.0 / 127.0) * (kTrimMax - kTrimMin);
    allPassed &= Expect(Near(inputTrim(), midTrim), "in preset B, CC 7 should drive Input Trim again");
    allPassed &= Expect(Near(outputTrim(), kTrimMax), "in preset B, preset A's CC 7 mapping should stay silent");

    // Per-preset slots are not DAW parameters, and a slot keeps its kind.
    const auto ids = table.GetSlotIds();
    allPassed &= Expect(std::find(ids.begin(), ids.end(), "preset.1") == ids.end(),
                        "GetSlotIds must leave per-preset slots out of the DAW parameter layout");
    allPassed &= Expect(!table.SetCustomSlot("preset.1", std::optional<std::string>("x"), std::nullopt, std::nullopt,
                                             std::nullopt, std::nullopt),
                        "SetCustomSlot must not rewrite a per-preset slot");
    allPassed &= Expect(!AddPresetSlot(table, "custom.input", "presetA", "global.inputTrim", 9),
                        "SetPresetSlot must not turn a custom slot into a per-preset one");

    // A save and load keeps the per-preset slot as one, and not as a custom slot.
    const auto saved = table.SaveToJson();
    AutomationSlotTable reloaded;
    InitializeTable(reloaded, mixer);
    reloaded.LoadFromJson(saved);
    const auto* restored = reloaded.FindSlot("preset.1");
    allPassed &= Expect(restored && restored->presetId == "presetA" && restored->address == "global.outputTrim" &&
                            restored->midiMap && restored->midiMap->controller == 7,
                        "a per-preset slot should survive a save and load");
    allPassed &= Expect(saved["customSlots"].dump().find("preset.1") == std::string::npos,
                        "a per-preset slot must not be saved among the custom slots");

    // Per-preset slots do not use up custom slots: the two customs above leave the rest free.
    int customsAdded = 0;

    while (customsAdded < 64 && AddCustom(table, "custom.fill" + std::to_string(customsAdded), "global.inputTrim", 20))
    {
        ++customsAdded;
    }

    allPassed &= Expect(customsAdded == kMaxCustomSlots - 2,
                        "per-preset slots must not count as custom slots: added " + std::to_string(customsAdded));

    // Each preset has its own budget.
    int presetSlotsAdded = 0;

    while (presetSlotsAdded < 64 &&
           AddPresetSlot(table, "preset.fill" + std::to_string(presetSlotsAdded), "presetA", "global.inputTrim", 30))
    {
        ++presetSlotsAdded;
    }

    allPassed &= Expect(presetSlotsAdded == kMaxPresetSlotsPerPreset - 1,
                        "preset A should take up to its own limit: added " + std::to_string(presetSlotsAdded));
    allPassed &= Expect(AddPresetSlot(table, "preset.b", "presetB", "global.inputTrim", 7),
                        "a full preset A should not stop preset B getting a slot");

    // Removing a preset's mappings leaves other presets' alone.
    const int removed = table.RemovePresetSlots("presetA");
    allPassed &=
        Expect(removed == kMaxPresetSlotsPerPreset && !table.FindSlot("preset.1") && table.FindSlot("preset.b"),
               "removing preset A's mappings should leave preset B's: removed " + std::to_string(removed));

    allPassed &= TestDawParametersFollowSlotsAsTheTableChanges(mixer);

    if (allPassed)
    {
        std::cout << "AutomationPresetMapping tests passed" << std::endl;
        return 0;
    }

    return 1;
}
