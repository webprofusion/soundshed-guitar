/**
 * IRCabAutomationRealtimeTests.cpp — automating the IR cab's Normalize and Low Latency must not
 * rebuild its convolvers on the audio thread.
 *
 * MIDI and DAW automation apply on the audio thread: PluginController::ProcessQueuedMidi takes the
 * DSP lock there with a try-lock and hands each event to AutomationSlotTable::HandleMidi, which
 * reaches the node's SetParam through MultiPresetMixer::FindAutomationTarget. Any node parameter can
 * be bound to a slot. Changing either of these two used to rebuild the convolvers inside SetParam,
 * which allocates and runs the FFT setup.
 *
 * This binds both to MIDI CCs, sends the CCs from a thread standing in for the audio thread, and
 * counts what that thread allocates, against a slot on the cab's Mix sent the same way. The node.*
 * route itself allocates nothing (AutomationNodeRealtimeTests), so the effect must add nothing to it.
 */

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "helpers/AudioThreadAllocations.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace audio_thread_allocations;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr const char* kPresetId = "cabPreset";
constexpr const char* kCabNodeId = "cab";
constexpr int kMixCc = 20;
constexpr int kLowLatencyCc = 21;
constexpr int kNormalizeCc = 22;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

Preset MakeCabPreset(const std::filesystem::path& irPath)
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Cab";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode cab{kCabNodeId, "cab_ir", "cab", "IR Cabinet", true};
    // Both settings start on, so the CCs below turn them off.
    cab.params["mix"] = 1.0;
    cab.params["lowLatency"] = 1.0;
    cab.params["normalizeIR"] = 1.0;

    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = irPath;
    cab.resources.push_back(ir);

    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, cab, out};
    preset.graph.edges = {GraphEdge{"in", kCabNodeId, 0, 0, 1.0}, GraphEdge{kCabNodeId, "out", 0, 0, 1.0}};
    return preset;
}

MidiControlMap CcMap(int controller)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = 0;
    map.controller = controller;
    map.mode = MidiControlMap::Mode::Absolute;
    return map;
}

bool BindSlot(AutomationSlotTable& table, const std::string& slotId, const std::string& address, int controller)
{
    return table.SetCustomSlot(slotId, std::optional<std::string>(slotId), std::optional<std::string>(address),
                               std::nullopt, std::optional<MidiControlMap>(CcMap(controller)), std::nullopt);
}

bool ProcessBlocks(MultiPresetMixer& mixer, int blocks)
{
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    bool finite = true;

    for (int block = 0; block < blocks; ++block)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const float sample = 0.25f * std::sin(0.05f * static_cast<float>(block * kBlockSize + i));
            inL[static_cast<std::size_t>(i)] = sample;
            inR[static_cast<std::size_t>(i)] = sample;
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        mixer.Process(inputs, outputs, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i)
        {
            finite = finite && std::isfinite(outL[static_cast<std::size_t>(i)]) &&
                     std::isfinite(outR[static_cast<std::size_t>(i)]);
        }
    }

    return finite;
}

/// The message thread's part, as PluginController::ApplyDeferredNodeRebuilds does it but with no
/// DSP lock, since nothing else runs here. Returns how many graphs had a rebuild waiting.
std::size_t ApplyDeferredRebuilds(MultiPresetMixer& mixer)
{
    std::vector<MultiPresetMixer::GraphRebuildWork> rebuilds;
    mixer.TakeDeferredRebuilds(rebuilds);

    for (auto& rebuild : rebuilds)
    {
        rebuild.work->Build();
    }

    mixer.CommitDeferredRebuilds(rebuilds);
    return rebuilds.size();
}

/// A cab with these settings from the start, loaded the way a preset loads one.
std::unique_ptr<EffectProcessor> MakeLoadedCab(const std::filesystem::path& irPath, double lowLatency, double normalize)
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");
    cab->SetParam("mix", 1.0);
    cab->SetParam("lowLatency", lowLatency);
    cab->SetParam("normalizeIR", normalize);
    cab->Prepare(kSampleRate, kBlockSize);

    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = irPath;
    cab->LoadResources({ir}, {irPath});
    return cab;
}

/// Both effects, reset, give bit-identical output for the same input, and it is not silence.
bool SameOutput(EffectProcessor& a, EffectProcessor& b)
{
    a.Reset();
    b.Reset();
    std::vector<float> in(kBlockSize), aL(kBlockSize), aR(kBlockSize), bL(kBlockSize), bR(kBlockSize);
    bool same = true;
    bool heard = false;

    // Long enough to reach the tail partitions of either convolution layout.
    for (int block = 0; block < 256; ++block)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const int n = block * kBlockSize + i;
            in[static_cast<std::size_t>(i)] =
                0.3f * std::sin(0.013f * static_cast<float>(n)) + (n % 997 == 0 ? 0.5f : 0.0f);
        }

        float* inputs[2] = {in.data(), in.data()};
        float* outputsA[2] = {aL.data(), aR.data()};
        float* outputsB[2] = {bL.data(), bR.data()};
        a.Process(inputs, outputsA, kBlockSize);
        b.Process(inputs, outputsB, kBlockSize);
        same = same && aL == bL && aR == bR;
        heard = heard || std::any_of(aL.begin(), aL.end(), [](float s) { return std::abs(s) > 1e-6f; });
    }

    return same && heard;
}
} // namespace

int main()
{
    RegisterAllEffects();

    const std::filesystem::path irPath =
        std::filesystem::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir" / "421 1960.wav";

    if (!std::filesystem::exists(irPath))
    {
        std::cerr << "Missing test IR: " << irPath << std::endl;
        return 1;
    }

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto preset = MakeCabPreset(irPath);

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add the cab preset" << std::endl;
        return 1;
    }

    EffectProcessor* cab = mixer.GetNodeProcessor(kPresetId, kCabNodeId);

    if (!cab || !cab->HasResource())
    {
        std::cerr << "The cab node did not load its IR" << std::endl;
        return 1;
    }

    bool passed = true;
    passed &= Expect(ProcessBlocks(mixer, 32), "The cab should render finite audio before automation");

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    passed &= Expect(BindSlot(table, "custom.cabMix", "node.cab_ir.mix", kMixCc), "Failed to bind Mix");
    passed &= Expect(BindSlot(table, "custom.cabLowLatency", "node.cab_ir.lowLatency", kLowLatencyCc),
                     "Failed to bind Low Latency");
    passed &= Expect(BindSlot(table, "custom.cabNormalize", "node.cab_ir.normalizeIR", kNormalizeCc),
                     "Failed to bind Normalize");

    // Warm the route once per slot at the value each parameter already has, so a first-use cost
    // anywhere on it cannot be charged to one of the measured events.
    CountOnAudioThread([&] {
        table.HandleMidi(MidiEvent{0xB0, kMixCc, 127, 0});
        table.HandleMidi(MidiEvent{0xB0, kLowLatencyCc, 127, 0});
        table.HandleMidi(MidiEvent{0xB0, kNormalizeCc, 127, 0});
    });

    const int latencyBefore = cab->GetLatencySamples();

    const auto mix = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kMixCc, 64, 0}); });
    const auto lowLatency = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kLowLatencyCc, 0, 0}); });
    const auto normalize = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kNormalizeCc, 0, 0}); });

    std::cout << "Audio thread, one CC each: Mix " << Describe(mix) << "; Low Latency " << Describe(lowLatency)
              << "; Normalize " << Describe(normalize) << std::endl;

    passed &= Expect(cab->GetParam("lowLatency") == 0.0 && cab->GetParam("normalizeIR") == 0.0,
                     "Both CCs should have reached the cab's SetParam");
    passed &= Expect(lowLatency.count == mix.count,
                     "Low Latency from MIDI allocated on the audio thread beyond the node.* route's own (" +
                         Describe(lowLatency) + " vs Mix " + Describe(mix) + ")");
    // Normalize is not a declared parameter, so the route skips the range lookup and allocates less.
    passed &= Expect(normalize.count <= mix.count,
                     "Normalize from MIDI allocated on the audio thread beyond the node.* route's own (" +
                         Describe(normalize) + " vs Mix " + Describe(mix) + ")");

    // The effect on its own, with nothing from the route: no allocation at all.
    const std::string lowLatencyKey = "lowLatency";
    const std::string normalizeKey = "normalizeIR";
    const auto direct = CountOnAudioThread([&] {
        cab->SetParam(lowLatencyKey, 1.0);
        cab->SetParam(normalizeKey, 1.0);
        cab->SetParam(lowLatencyKey, 0.0);
    });
    passed &= Expect(direct.count == 0, "SetParam on the cab allocated on the audio thread (" + Describe(direct) + ")");

    // Asked for Low Latency off, and Normalize off and back on again: only the first is waiting,
    // and nothing is rebuilt until the message thread builds it.
    passed &= Expect(cab->GetParam("lowLatency") == 0.0 && cab->GetParam("normalizeIR") == 1.0,
                     "GetParam should report what was last asked for");
    passed &= Expect(cab->GetLatencySamples() == latencyBefore, "Nothing should be rebuilt before the message thread");
    passed &= Expect(ProcessBlocks(mixer, 32), "The cab should render finite audio after automation");

    passed &= Expect(ApplyDeferredRebuilds(mixer) == 1, "The cab's rebuild should be waiting for the message thread");
    const int latencyAfter = cab->GetLatencySamples();
    std::cout << "Cab latency with Low Latency on: " << latencyBefore << ", off: " << latencyAfter << std::endl;
    passed &= Expect(latencyAfter > latencyBefore, "The message thread's rebuild should turn Low Latency off");
    passed &= Expect(cab->GetParam("lowLatency") == 0.0 && cab->GetParam("normalizeIR") == 1.0,
                     "The settings should read back as built");
    passed &= Expect(ApplyDeferredRebuilds(mixer) == 0, "Nothing should be left waiting once it is built");
    passed &= Expect(ProcessBlocks(mixer, 32), "The cab should render finite audio across the crossfade");

    // The rebuild is the build a cab set up that way from the start would do. Mix back to where
    // the comparison cabs have it, from the CC above.
    cab->SetParam("mix", 1.0);
    passed &= Expect(SameOutput(*cab, *MakeLoadedCab(irPath, 0.0, 1.0)),
                     "A rebuilt cab should sound exactly like one built with its settings");

    // Asking again for what is built, while the rebuild for the change is being built, drops it.
    cab->SetParam(normalizeKey, 0.0);
    auto superseded = cab->TakeDeferredRebuild();
    cab->SetParam(normalizeKey, 1.0);
    passed &= Expect(superseded != nullptr, "Normalize off should wait for the message thread");

    if (superseded)
    {
        superseded->Build();
        cab->CommitDeferredRebuild(*superseded);
    }

    passed &= Expect(cab->GetParam("normalizeIR") == 1.0 && !cab->TakeDeferredRebuild(),
                     "A rebuild superseded while it was built should not be installed");
    passed &= Expect(SameOutput(*cab, *MakeLoadedCab(irPath, 0.0, 1.0)),
                     "A superseded rebuild should leave the cab as it was");

    // A Prepare in between rebuilds with the waiting setting itself, and the late work is dropped.
    cab->SetParam(normalizeKey, 0.0);
    auto preempted = cab->TakeDeferredRebuild();
    cab->Prepare(kSampleRate, kBlockSize);

    if (preempted)
    {
        preempted->Build();
        cab->CommitDeferredRebuild(*preempted);
    }

    passed &= Expect(preempted != nullptr && cab->GetParam("normalizeIR") == 0.0 && !cab->TakeDeferredRebuild(),
                     "A Prepare should build a waiting setting in");
    passed &= Expect(SameOutput(*cab, *MakeLoadedCab(irPath, 0.0, 0.0)),
                     "A cab prepared with a setting waiting should sound like one built with it");

    std::cout << (passed ? "IRCabAutomationRealtimeTests passed" : "IRCabAutomationRealtimeTests FAILED") << std::endl;
    return passed ? 0 : 1;
}
