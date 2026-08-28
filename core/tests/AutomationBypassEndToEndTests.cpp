/**
 * AutomationBypassEndToEndTests.cpp — End-to-end MIDI → automation → mixer bypass path.
 *
 * Drives raw MIDI events through AutomationSlotTable wired to a real
 * MultiPresetMixer + EffectRegistry, mapping a NoteOn+Toggle slot to a
 * by-type bypass address (node.<effectType>.bypassed) and asserting the
 * matching node's enabled state flips on every key press — including
 * re-activation after it has been bypassed.
 */

#include <iostream>
#include <optional>
#include <string>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectGuids.h"
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

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

Preset MakeGainPreset()
{
    Preset preset;
    preset.id = "e2ePreset";
    preset.name = "E2EPreset";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode gain{"g1", "gain", "utility", "Gain", true};
    gain.params["gainDb"] = 0.0;
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, gain, out};
    preset.graph.edges = {
        GraphEdge{"in", "g1", 0, 0, 1.0},
        GraphEdge{"g1", "out", 0, 0, 1.0},
    };
    return preset;
}

// Returns true when at least one enabled node of the given type exists.
bool GainEnabled(const MultiPresetMixer& mixer)
{
    return mixer.FindFirstEnabledNodeOfType("gain").has_value();
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto preset = MakeGainPreset();

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add gain preset" << std::endl;
        return 1;
    }

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    // Map a custom slot to the gain node's by-type bypass with a NoteOn Toggle.
    MidiControlMap midiMap;
    midiMap.eventType = MidiControlMap::EventType::NoteOn;
    midiMap.channel = 0;
    midiMap.controller = 60;
    midiMap.mode = MidiControlMap::Mode::Toggle;

    const bool created = table.SetCustomSlot("custom.bypassGain", std::optional<std::string>("Bypass Gain"),
                                             std::optional<std::string>("node.gain.bypassed"), std::nullopt,
                                             std::optional<MidiControlMap>(midiMap), std::nullopt);
    allPassed &= Expect(created, "Failed to create custom bypass slot");

    const MidiEvent noteOn{0x90, 60, 100, 0};      // press
    const MidiEvent noteOnRelease{0x90, 60, 0, 0}; // release (NoteOn vel 0)
    const MidiEvent noteOff{0x80, 60, 0, 0};       // release (NoteOff)

    allPassed &= Expect(GainEnabled(mixer), "Gain should start enabled");

    // Press 1: bypass on (disable node).
    table.HandleMidi(noteOn);
    allPassed &= Expect(!GainEnabled(mixer), "Press 1 should bypass (disable) the gain node");

    // Release must not change state.
    table.HandleMidi(noteOnRelease);
    allPassed &= Expect(!GainEnabled(mixer), "Release (vel 0) must not re-enable the gain node");

    // Press 2: bypass off (re-enable node). This is the reported failure case.
    table.HandleMidi(noteOn);
    allPassed &= Expect(GainEnabled(mixer), "Press 2 should re-enable the gain node");

    table.HandleMidi(noteOff);
    allPassed &= Expect(GainEnabled(mixer), "NoteOff release must not disable the gain node");

    // Press 3: bypass on again.
    table.HandleMidi(noteOn);
    allPassed &= Expect(!GainEnabled(mixer), "Press 3 should bypass the gain node again");

    // Press 4: re-enable again — full cycle stability.
    table.HandleMidi(noteOn);
    allPassed &= Expect(GainEnabled(mixer), "Press 4 should re-enable the gain node again");

    if (allPassed)
    {
        std::cout << "AutomationBypassEndToEnd MIDI toggle test passed" << std::endl;
        return 0;
    }

    return 1;
}
