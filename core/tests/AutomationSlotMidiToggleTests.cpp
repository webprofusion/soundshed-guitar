#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "automation/AutomationSlotTable.h"
#include "dsp/MultiPresetMixer.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
bool Expect(const bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}
} // namespace

int main()
{
    bool allPassed = true;

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(48000.0, 64);

    AutomationSlotTable table;
    int bankUpCalls = 0;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [&](int step) { bankUpCalls += step; }, [](int) {}, []() { return 0; },
        []() { return 0; }, [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });

    MidiControlMap midiMap;
    midiMap.eventType = MidiControlMap::EventType::NoteOn;
    midiMap.channel = 0;
    midiMap.controller = 60;
    midiMap.mode = MidiControlMap::Mode::Toggle;

    allPassed &= Expect(table.SetDefaultSlotOverrides("default.bankUp", std::nullopt, midiMap, std::nullopt),
                        "Failed to set MIDI map on default.bankUp");

    const MidiEvent noteOn{0x90, 60, 100, 0};
    const MidiEvent noteOnVelocityZero{0x90, 60, 0, 0}; // NoteOff equivalent
    const MidiEvent noteOff{0x80, 60, 0, 0};

    table.HandleMidi(noteOn);
    allPassed &= Expect(bankUpCalls == 1, "First NoteOn should trigger exactly once");

    table.HandleMidi(noteOn);
    allPassed &=
        Expect(bankUpCalls == 2, "Repeated NoteOn presses should retrigger toggle even without explicit release");

    table.HandleMidi(noteOnVelocityZero);
    allPassed &= Expect(bankUpCalls == 2, "NoteOn velocity 0 release must not retrigger toggle");

    table.HandleMidi(noteOn);
    allPassed &= Expect(bankUpCalls == 3, "NoteOn should retrigger after release reset");

    table.HandleMidi(noteOff);
    allPassed &= Expect(bankUpCalls == 3, "Explicit NoteOff must not retrigger toggle");

    table.HandleMidi(noteOn);
    allPassed &= Expect(bankUpCalls == 4, "NoteOn should retrigger after explicit NoteOff reset");

    if (allPassed)
    {
        std::cout << "AutomationSlotTable NoteOn toggle release behavior test passed" << std::endl;
        return 0;
    }

    return 1;
}
