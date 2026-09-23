/**
 * AutomationNodeParamRangeTests.cpp — A node.* slot drives its parameter across the range
 * the effect declares.
 *
 * Slot values are 0..1 whether MIDI, the DAW or the keyboard wrote them, while an effect
 * takes its parameters in native units. This maps a CC to the gain effect's gainDb and
 * checks the node reads back the ends and middle of that dB range — not 0..1 dB, which is
 * what a node.* slot delivered before the value was mapped.
 *
 * A node can narrow that range with its own settings: an expression pedal on a pitch shift
 * sweeps the node's Range Min..Range Max, in whole semitones only while it snaps.
 *
 * A parameter on a log taper is swept by ratio instead: a pedal on the ring modulator's
 * 1-2000 Hz Frequency reaches 44.7 Hz, the geometric mean, at half travel, not 1000 Hz.
 */

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/ParamTaper.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr double kToleranceDb = 1e-4;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

constexpr double kPitchRangeMin = 0.0;
constexpr double kPitchRangeMax = 7.0;

Preset MakeRangePreset()
{
    Preset preset;
    preset.id = "rangePreset";
    preset.name = "RangePreset";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode gain{"g1", "gain", "utility", "Gain", true};
    gain.params["gainDb"] = 0.0;
    GraphNode pitch{"p1", "pitch_shift", "pitch", "Pitch Shift", true};
    pitch.params["semitones"] = 0.0;
    pitch.params["minSemitones"] = kPitchRangeMin;
    pitch.params["maxSemitones"] = kPitchRangeMax;
    pitch.params["stepMode"] = 1.0;
    GraphNode ring{"r1", "ring_mod", "modulation", "Ring Mod", true};
    ring.params["frequency"] = 440.0;
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, gain, pitch, ring, out};
    preset.graph.edges = {
        GraphEdge{"in", "g1", 0, 0, 1.0},
        GraphEdge{"g1", "p1", 0, 0, 1.0},
        GraphEdge{"p1", "r1", 0, 0, 1.0},
        GraphEdge{"r1", "out", 0, 0, 1.0},
    };
    return preset;
}

/// The taper arithmetic on its own: exact ends, an inverse that round-trips, and a fallback
/// to linear for a range a log taper cannot map, rather than a NaN.
bool TestTaperMaths()
{
    bool passed = true;
    const auto logTaper = ParamTaper::Log;
    const auto linearTaper = ParamTaper::Linear;

    passed &= Expect(TaperPositionToValue(logTaper, 20.0, 20000.0, 0.0) == 20.0 &&
                         TaperPositionToValue(logTaper, 20.0, 20000.0, 1.0) == 20000.0,
                     "A log taper should land exactly on both ends");
    passed &= Expect(std::abs(TaperPositionToValue(logTaper, 20.0, 20000.0, 1.0 / 3.0) - 200.0) < 1e-9,
                     "A third of the way along 20 Hz..20 kHz should be one decade up, 200 Hz");
    passed &= Expect(std::abs(TaperPositionToValue(linearTaper, 20.0, 20000.0, 0.5) - 10010.0) < 1e-9,
                     "A linear taper should be unchanged");

    for (const double position : {0.0, 0.1, 0.25, 0.5, 0.9, 1.0})
    {
        const double value = TaperPositionToValue(logTaper, 1.0, 2000.0, position);
        passed &= Expect(std::abs(TaperValueToPosition(logTaper, 1.0, 2000.0, value) - position) < 1e-12,
                         "Value to position should invert position to value at " + std::to_string(position));
    }

    passed &= Expect(TaperPositionToValue(logTaper, 0.0, 1.0, 0.5) == 0.5 &&
                         TaperPositionToValue(logTaper, -12.0, 12.0, 0.5) == 0.0,
                     "A log taper over a range that is not positive should fall back to linear");
    passed &= Expect(TaperPositionToValue(logTaper, 1.0, 2000.0, 1.5) == 2000.0 &&
                         TaperPositionToValue(logTaper, 1.0, 2000.0, -0.5) == 1.0,
                     "A position outside 0..1 should clamp to the range");
    passed &= Expect(TaperPositionToValue(logTaper, 1.0, 2000.0, std::numeric_limits<double>::quiet_NaN()) == 1.0,
                     "A non-finite position should read as the bottom of the range");
    passed &= Expect(ParseParamTaper("log") == logTaper && ParseParamTaper("linear") == linearTaper &&
                         !ParseParamTaper("exp").has_value(),
                     "Taper names should parse, and an unknown one should not");
    return passed;
}

/// Every log taper the registry declares must have a range it can map, and the wide frequency
/// controls the taper was made for must be on it.
bool TestRegisteredTapers()
{
    bool passed = true;
    const auto& registry = EffectRegistry::Instance();

    for (const auto& info : registry.GetAllTypes())
    {
        for (const auto& param : info.parameters)
        {
            passed &= Expect(IsTaperRangeValid(param.taper, param.minValue, param.maxValue),
                             info.type + "." + param.id + " declares a log taper over a range it cannot map");
        }
    }

    const std::pair<const char*, const char*> expectedLog[] = {
        {EffectGuids::kRingMod, "frequency"},     {EffectGuids::kDelayDigital, "highCut"},
        {EffectGuids::kDelayDigital, "lowCut"},   {EffectGuids::kCabIr, "lowCutHz"},
        {EffectGuids::kCabIr, "highCutHz"},       {EffectGuids::kReverbAdvanced, "lowCut"},
        {EffectGuids::kReverbAdvanced, "highCut"}};

    for (const auto& [type, paramId] : expectedLog)
    {
        const auto* def = registry.FindParameter(type, paramId);
        passed &= Expect(def && def->taper == ParamTaper::Log,
                         std::string(type) + "." + paramId + " should be on a log taper");
    }

    const auto* mix = registry.FindParameter(EffectGuids::kRingMod, "mix");
    passed &= Expect(mix && mix->taper == ParamTaper::Linear, "An undeclared taper should be linear");
    return passed;
}

bool ExpectParam(const MultiPresetMixer& mixer, const std::string& type, const std::string& paramId, double expected,
                 const std::string& message)
{
    const auto readouts = mixer.ReadNodeParamsForType(type, {paramId});

    if (readouts.empty() || readouts.front().values.empty())
    {
        return Expect(false, message + " (no " + type + " node to read)");
    }

    const double actual = readouts.front().values.front();
    return Expect(std::abs(actual - expected) < kToleranceDb,
                  message + ": expected " + std::to_string(expected) + ", read " + std::to_string(actual));
}

bool ExpectGainDb(const MultiPresetMixer& mixer, double expected, const std::string& message)
{
    return ExpectParam(mixer, "gain", "gainDb", expected, message);
}

bool ExpectSemitones(const MultiPresetMixer& mixer, double expected, const std::string& message)
{
    return ExpectParam(mixer, "pitch_shift", "semitones", expected, message);
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

    const auto preset = MakeRangePreset();

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add range preset" << std::endl;
        return 1;
    }

    const auto* gainDb = EffectRegistry::Instance().FindParameter("gain", "gainDb");

    if (!gainDb || gainDb->maxValue - gainDb->minValue <= 1.0)
    {
        std::cerr << "The gain effect must declare gainDb with a range wider than 0..1" << std::endl;
        return 1;
    }

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    // The value handed on to the UI (signalPathNodeParamUpdated) must be native too: the latest
    // change to `paramId` the message thread would take.
    const auto takeNotified = [&table](const std::string& paramId) {
        std::optional<double> latest;
        table.TakeNodeChanges([&](NodeChangeQueue::Change&& change) {
            if (change.binding->paramId == paramId)
            {
                latest = change.value;
            }
        });
        return latest;
    };

    MidiControlMap midiMap;
    midiMap.eventType = MidiControlMap::EventType::CC;
    midiMap.channel = 0;
    midiMap.controller = 7;
    midiMap.mode = MidiControlMap::Mode::Absolute;

    const bool created = table.SetCustomSlot("custom.gain", std::optional<std::string>("Gain"),
                                             std::optional<std::string>("node.gain.gainDb"), std::nullopt,
                                             std::optional<MidiControlMap>(midiMap), std::nullopt);
    allPassed &= Expect(created, "Failed to create the gain slot");

    const auto nativeAt = [gainDb](double normalized) {
        return gainDb->minValue + normalized * (gainDb->maxValue - gainDb->minValue);
    };

    table.HandleMidi(MidiEvent{0xB0, 7, 0, 0});
    allPassed &= ExpectGainDb(mixer, gainDb->minValue, "CC 0 should reach the bottom of the range");

    table.HandleMidi(MidiEvent{0xB0, 7, 127, 0});
    allPassed &= ExpectGainDb(mixer, gainDb->maxValue, "CC 127 should reach the top of the range");
    const auto gainNotified = takeNotified("gainDb");
    allPassed &= Expect(gainNotified.has_value() && std::abs(*gainNotified - gainDb->maxValue) < kToleranceDb,
                        "The UI notification should carry the native value");

    table.HandleMidi(MidiEvent{0xB0, 7, 64, 0});
    allPassed &= ExpectGainDb(mixer, nativeAt(64.0 / 127.0), "CC 64 should land mid-range");

    // A DAW write takes the same path.
    table.ApplyAutomationLocked("custom.gain", 0.75f, AutomationSource::DAW);
    allPassed &= ExpectGainDb(mixer, nativeAt(0.75), "A DAW value of 0.75 should land three quarters up the range");

    // An expression pedal on the pitch shift sweeps the node's own range, not the -12..12 st
    // the effect type declares.
    MidiControlMap pedalMap = midiMap;
    pedalMap.controller = 11;

    const bool pedalCreated = table.SetCustomSlot("custom.pitch", std::optional<std::string>("Whammy"),
                                                  std::optional<std::string>("node.pitch_shift.semitones"),
                                                  std::nullopt, std::optional<MidiControlMap>(pedalMap), std::nullopt);
    allPassed &= Expect(pedalCreated, "Failed to create the pitch slot");

    const double pedalMid = kPitchRangeMin + (64.0 / 127.0) * (kPitchRangeMax - kPitchRangeMin);

    table.HandleMidi(MidiEvent{0xB0, 11, 0, 0});
    allPassed &= ExpectSemitones(mixer, kPitchRangeMin, "Heel down should reach Range Min");

    table.HandleMidi(MidiEvent{0xB0, 11, 127, 0});
    allPassed &= ExpectSemitones(mixer, kPitchRangeMax, "Toe down should reach Range Max");
    const auto pitchNotified = takeNotified("semitones");
    allPassed &= Expect(pitchNotified.has_value() && std::abs(*pitchNotified - kPitchRangeMax) < kToleranceDb,
                        "The pitch notification should carry semitones");

    table.HandleMidi(MidiEvent{0xB0, 11, 64, 0});
    allPassed &= ExpectSemitones(mixer, std::round(pedalMid), "Snapped, mid-travel should land on a whole semitone");

    mixer.SetNodeParam(preset.id, "p1", "stepMode", 0.0);
    table.HandleMidi(MidiEvent{0xB0, 11, 64, 0});
    allPassed &= ExpectSemitones(mixer, pedalMid, "Free, mid-travel should glide between semitones");

    // Widening the range on the node widens the sweep with no change to the mapping.
    mixer.SetNodeParam(preset.id, "p1", "minSemitones", -12.0);
    mixer.SetNodeParam(preset.id, "p1", "maxSemitones", 12.0);
    table.HandleMidi(MidiEvent{0xB0, 11, 0, 0});
    allPassed &= ExpectSemitones(mixer, -12.0, "A widened range should reach its new min");

    // A pedal on a log-taper parameter sweeps it by ratio: every stretch of travel covers the
    // same number of octaves, so the 1-100 Hz growl region gets 60% of it, not 5%.
    const auto* frequency = EffectRegistry::Instance().FindParameter("ring_mod", "frequency");

    if (!frequency || frequency->taper != ParamTaper::Log)
    {
        std::cerr << "The ring modulator must declare frequency on a log taper" << std::endl;
        return 1;
    }

    MidiControlMap ringMap = midiMap;
    ringMap.controller = 12;

    const bool ringCreated = table.SetCustomSlot("custom.ring", std::optional<std::string>("Carrier"),
                                                 std::optional<std::string>("node.ring_mod.frequency"), std::nullopt,
                                                 std::optional<MidiControlMap>(ringMap), std::nullopt);
    allPassed &= Expect(ringCreated, "Failed to create the ring modulator slot");

    const auto hzAt = [frequency](double normalized) {
        return frequency->minValue * std::pow(frequency->maxValue / frequency->minValue, normalized);
    };

    table.HandleMidi(MidiEvent{0xB0, 12, 0, 0});
    allPassed &= ExpectParam(mixer, "ring_mod", "frequency", frequency->minValue, "CC 0 should reach 1 Hz");

    table.HandleMidi(MidiEvent{0xB0, 12, 127, 0});
    allPassed &= ExpectParam(mixer, "ring_mod", "frequency", frequency->maxValue, "CC 127 should reach 2 kHz");

    table.HandleMidi(MidiEvent{0xB0, 12, 64, 0});
    allPassed &= ExpectParam(mixer, "ring_mod", "frequency", hzAt(64.0 / 127.0), "CC 64 should land mid-way by ratio");
    const auto ringNotified = takeNotified("frequency");
    allPassed &= Expect(ringNotified.has_value() && std::abs(*ringNotified - hzAt(64.0 / 127.0)) < kToleranceDb,
                        "The frequency notification should carry Hz");

    table.ApplyAutomationLocked("custom.ring", 0.5f, AutomationSource::DAW);
    allPassed &= ExpectParam(mixer, "ring_mod", "frequency", std::sqrt(frequency->minValue * frequency->maxValue),
                             "A DAW value of 0.5 should land on the geometric mean, 44.7 Hz");

    allPassed &= TestTaperMaths();
    allPassed &= TestRegisteredTapers();

    if (allPassed)
    {
        std::cout << "AutomationNodeParamRange tests passed" << std::endl;
        return 0;
    }

    return 1;
}
