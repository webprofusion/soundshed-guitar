/**
 * IRReverbAutomationRealtimeTests.cpp — automating the convolution reverb's Quality and Low Latency
 * must not rebuild its convolvers on the audio thread.
 *
 * MIDI and DAW automation apply on the audio thread: PluginController::ProcessQueuedMidi takes the
 * DSP lock there with a try-lock and hands each event to AutomationSlotTable::HandleMidi, which
 * reaches the node's SetParam through MultiPresetMixer::SetNodeParamByType. Changing either of these
 * two used to rebuild the convolvers inside SetParam, which allocates, re-truncates a reverb IR that
 * runs to seconds, and runs the FFT setup.
 *
 * This binds both to MIDI CCs, sends the CCs from a thread standing in for the audio thread, and
 * counts what that thread allocates, against a slot on the reverb's Mix sent the same way. The
 * node.* route allocates a little of its own for any parameter, so the effect must add nothing to
 * it. The message thread builds the change instead (DeferredRebuild): what it holds the DSP lock for
 * must not grow with the IR, and what it builds must be exactly what loading the IR with those
 * settings builds.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "automation/AutomationSlotTable.h"
#include "dsp/EffectGuids.h"
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
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr const char* kPresetId = "reverbPreset";
constexpr const char* kReverbNodeId = "verb";
constexpr int kMixCc = 20;
constexpr int kQualityCc = 21;
constexpr int kLowLatencyCc = 22;
// Taking the work and committing it hold the DSP lock. A copy of one of these IRs would be
// megabytes. Not zero: MSVC's Debug containers allocate a small proxy for each one moved.
constexpr std::size_t kLockedBytesBudget = 4096;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

/// Exponentially decaying noise, one channel per entry, written as a 32-bit float WAV: the shape
/// of a real hall IR. Past 1.5 s, Economy cuts it.
fs::path WriteReverbIR(const std::string& name, double seconds, double sampleRate, int channels)
{
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> interleaved(frames * static_cast<std::size_t>(channels));
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    for (std::size_t i = 0; i < frames; ++i)
    {
        const double envelope = std::exp(-static_cast<double>(i) / sampleRate / (seconds / 4.0));

        for (int c = 0; c < channels; ++c)
        {
            interleaved[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
                i == 0 ? 1.0f : static_cast<float>(noise(rng) * envelope);
        }
    }

    const auto dir = fs::temp_directory_path() / "guitarfx_ir_reverb_automation_tests";
    fs::create_directories(dir);
    const auto path = dir / name;

    const auto dataSize = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const auto numChannels = static_cast<std::uint16_t>(channels);
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    const std::uint32_t byteRate = rate * numChannels * sizeof(float);
    const auto blockAlign = static_cast<std::uint16_t>(numChannels * sizeof(float));
    const std::uint16_t bitsPerSample = 32;

    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);
    return path;
}

ResourceRef IRRef(const fs::path& path)
{
    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = path;
    return ir;
}

Preset MakeReverbPreset(const fs::path& irPath)
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Reverb";

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode verb{kReverbNodeId, "reverb_ir", "reverb", "Convolution Reverb", true};
    // Declared up front so the graph's own copy of each value already has a slot: the executor
    // records every SetNodeParam there, and a new key would allocate for any effect.
    verb.params["mix"] = 1.0;
    verb.params["quality"] = 3.0;
    verb.params["lowLatency"] = 1.0;
    verb.resources.push_back(IRRef(irPath));

    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, verb, out};
    preset.graph.edges = {GraphEdge{"in", kReverbNodeId, 0, 0, 1.0}, GraphEdge{kReverbNodeId, "out", 0, 0, 1.0}};
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

/// The message thread's part for one effect, as PluginController::ApplyDeferredNodeRebuilds does
/// it but with no DSP lock, since nothing else runs here. Returns whether a rebuild was waiting.
bool ApplyDeferredRebuild(EffectProcessor& effect)
{
    auto work = effect.TakeDeferredRebuild();

    if (!work)
    {
        return false;
    }

    work->Build();
    effect.CommitDeferredRebuild(*work);
    return true;
}

/// A reverb with these settings from the start, set up the way a preset's node is: params first,
/// then the IR, then Prepare.
std::unique_ptr<EffectProcessor> MakeLoadedReverb(const fs::path& irPath, double quality, double lowLatency)
{
    auto verb = EffectRegistry::Instance().Create(EffectGuids::kReverbIr);
    verb->SetParam("mix", 1.0);
    verb->SetParam("quality", quality);
    verb->SetParam("lowLatency", lowLatency);
    verb->LoadResources({IRRef(irPath)}, {irPath});
    verb->Prepare(kSampleRate, kBlockSize);
    return verb;
}

/// Renders both effects, reset, from the same input. Whether the output was bit-identical, and not
/// silence.
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

    // 44.1 kHz against a 48 kHz host takes the resampling path, as a downloaded reverb IR usually
    // does; the true-stereo one runs all four convolvers. Only Economy cuts IRs this long, which is
    // all the checks need, and Debug resamples each comparison reverb's IR again.
    const fs::path stereoIR = WriteReverbIR("reverb_2s_44k_stereo.wav", 2.0, 44100.0, 2);
    const fs::path trueStereoIR = WriteReverbIR("reverb_2s_48k_quad.wav", 2.0, 48000.0, 4);

    ResourceLibrary library;
    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);

    const auto preset = MakeReverbPreset(stereoIR);

    if (!mixer.AddActivePreset(preset, preset.id, preset.name))
    {
        std::cerr << "Failed to add the reverb preset" << std::endl;
        return 1;
    }

    EffectProcessor* verb = mixer.GetNodeProcessor(kPresetId, kReverbNodeId);

    if (!verb || !verb->HasResource())
    {
        std::cerr << "The reverb node did not load its IR" << std::endl;
        return 1;
    }

    bool passed = true;
    passed &= Expect(!verb->TakeDeferredRebuild(), "Loading a preset should leave nothing for the message thread");
    passed &= Expect(ProcessBlocks(mixer, 32), "The reverb should render finite audio before automation");

    AutomationSlotTable table;
    table.InitializeRegistry(
        mixer, []() { return 0.0; }, [](int) {}, [](int) {}, [](int) {}, []() { return 0; }, []() { return 0; },
        [](int) {}, []() { return 0; }, [](int) {}, []() { return -1; });
    table.SetMixer(&mixer);
    table.SetEffectRegistry(&EffectRegistry::Instance());

    passed &= Expect(BindSlot(table, "custom.verbMix", "node.reverb_ir.mix", kMixCc), "Failed to bind Mix");
    passed &=
        Expect(BindSlot(table, "custom.verbQuality", "node.reverb_ir.quality", kQualityCc), "Failed to bind Quality");
    passed &= Expect(BindSlot(table, "custom.verbLowLatency", "node.reverb_ir.lowLatency", kLowLatencyCc),
                     "Failed to bind Low Latency");

    // Warm the route once per slot at the value each parameter already has, so a first-use cost
    // anywhere on it cannot be charged to one of the measured events.
    CountOnAudioThread([&] {
        table.HandleMidi(MidiEvent{0xB0, kMixCc, 127, 0});
        table.HandleMidi(MidiEvent{0xB0, kQualityCc, 127, 0});
        table.HandleMidi(MidiEvent{0xB0, kLowLatencyCc, 127, 0});
    });

    const int latencyBefore = verb->GetLatencySamples();

    const auto mix = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kMixCc, 64, 0}); });
    const auto quality = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kQualityCc, 0, 0}); });
    const auto lowLatency = CountOnAudioThread([&] { table.HandleMidi(MidiEvent{0xB0, kLowLatencyCc, 0, 0}); });

    std::cout << "Audio thread, one CC each: Mix " << Describe(mix) << "; Quality " << Describe(quality)
              << "; Low Latency " << Describe(lowLatency) << std::endl;

    passed &= Expect(verb->GetParam("quality") == 0.0 && verb->GetParam("lowLatency") == 0.0,
                     "Both CCs should have reached the reverb's SetParam");
    passed &= Expect(quality.count == mix.count, "Quality from MIDI allocated on the audio thread beyond the node.* "
                                                 "route's own (" +
                                                     Describe(quality) + " vs Mix " + Describe(mix) + ")");
    passed &= Expect(lowLatency.count == mix.count,
                     "Low Latency from MIDI allocated on the audio thread beyond the node.* route's own (" +
                         Describe(lowLatency) + " vs Mix " + Describe(mix) + ")");

    // The effect on its own, with nothing from the route: no allocation at all.
    const std::string qualityKey = "quality";
    const std::string lowLatencyKey = "lowLatency";
    const auto direct = CountOnAudioThread([&] {
        verb->SetParam(qualityKey, 2.0);
        verb->SetParam(lowLatencyKey, 1.0);
        verb->SetParam(qualityKey, 0.0);
        verb->SetParam(lowLatencyKey, 0.0);
    });
    passed &=
        Expect(direct.count == 0, "SetParam on the reverb allocated on the audio thread (" + Describe(direct) + ")");

    // Asked for Economy with Low Latency off: nothing is rebuilt until the message thread builds it.
    passed &= Expect(verb->GetParam("quality") == 0.0 && verb->GetParam("lowLatency") == 0.0,
                     "GetParam should report what was last asked for");
    passed &= Expect(verb->GetLatencySamples() == latencyBefore, "Nothing should be rebuilt before the message thread");
    passed &= Expect(ProcessBlocks(mixer, 32), "The reverb should render finite audio after automation");

    // The message thread's part, measured: what it does under the DSP lock must not grow with the
    // IR, since the audio thread outputs silence for as long as the lock is held.
    std::vector<MultiPresetMixer::GraphRebuildWork> rebuilds;
    const auto take = CountOnAudioThread([&] { mixer.TakeDeferredRebuilds(rebuilds); });
    passed &= Expect(rebuilds.size() == 1, "The reverb's rebuild should be waiting for the message thread");

    for (auto& rebuild : rebuilds)
    {
        rebuild.work->Build();
    }

    const auto commit = CountOnAudioThread([&] { mixer.CommitDeferredRebuilds(rebuilds); });
    rebuilds.clear();
    std::cout << "Message thread under the lock: take " << Describe(take) << "; commit " << Describe(commit)
              << std::endl;
    passed &= Expect(take.bytes <= kLockedBytesBudget,
                     "Taking the rebuild should not copy the impulses under the DSP lock (" + Describe(take) + ")");
    passed &= Expect(commit.bytes <= kLockedBytesBudget,
                     "Committing the rebuild should only swap under the DSP lock (" + Describe(commit) + ")");

    const int latencyAfter = verb->GetLatencySamples();
    std::cout << "Reverb latency with Low Latency on: " << latencyBefore << ", off: " << latencyAfter << std::endl;
    passed &= Expect(latencyAfter > latencyBefore, "The message thread's rebuild should turn Low Latency off");
    passed &= Expect(verb->GetParam("quality") == 0.0 && verb->GetParam("lowLatency") == 0.0,
                     "The settings should read back as built");
    passed &= Expect(!verb->TakeDeferredRebuild(), "Nothing should be left waiting once it is built");
    passed &= Expect(ProcessBlocks(mixer, 32), "The reverb should render finite audio after the swap");

    // The rebuild is the build a reverb set up that way from the start would do. Mix back to where
    // the comparison reverbs have it, from the CC above.
    verb->SetParam("mix", 1.0);
    passed &= Expect(SameOutput(*verb, *MakeLoadedReverb(stereoIR, 0.0, 0.0)),
                     "A rebuilt reverb should sound exactly like one built with its settings");
    passed &= Expect(!SameOutput(*verb, *MakeLoadedReverb(stereoIR, 3.0, 0.0)),
                     "Economy should have truncated the IR, so it should not sound like Full");

    // True stereo: all four convolvers are rebuilt and swapped.
    {
        auto quad = MakeLoadedReverb(trueStereoIR, 3.0, 1.0);
        quad->SetParam("quality", 0.0);
        quad->SetParam("lowLatency", 0.0);
        passed &= Expect(ApplyDeferredRebuild(*quad), "A true-stereo reverb should wait for the message thread too");
        passed &= Expect(SameOutput(*quad, *MakeLoadedReverb(trueStereoIR, 0.0, 0.0)),
                         "A rebuilt true-stereo reverb should sound exactly like one built with its settings");
    }

    // Asking again for what is built, while the rebuild for the change is being built, drops it.
    verb->SetParam(qualityKey, 2.0);
    auto superseded = verb->TakeDeferredRebuild();
    verb->SetParam(qualityKey, 0.0);
    passed &= Expect(superseded != nullptr, "High should wait for the message thread");

    if (superseded)
    {
        superseded->Build();
        verb->CommitDeferredRebuild(*superseded);
    }

    passed &= Expect(verb->GetParam("quality") == 0.0 && !verb->TakeDeferredRebuild(),
                     "A rebuild superseded while it was built should not be installed");
    passed &= Expect(SameOutput(*verb, *MakeLoadedReverb(stereoIR, 0.0, 0.0)),
                     "A superseded rebuild should leave the reverb as it was");

    // A Prepare in between rebuilds with the waiting setting itself, and the late work is dropped.
    verb->SetParam(lowLatencyKey, 1.0);
    auto preempted = verb->TakeDeferredRebuild();
    verb->Prepare(kSampleRate, kBlockSize);

    if (preempted)
    {
        preempted->Build();
        verb->CommitDeferredRebuild(*preempted);
    }

    passed &= Expect(preempted != nullptr && verb->GetParam("lowLatency") == 1.0 && !verb->TakeDeferredRebuild(),
                     "A Prepare should build a waiting setting in");
    passed &= Expect(SameOutput(*verb, *MakeLoadedReverb(stereoIR, 0.0, 1.0)),
                     "A reverb prepared with a setting waiting should sound like one built with it");

    // So does an IR load: the late work, built from the old IR, is dropped.
    verb->SetParam(qualityKey, 1.0);
    auto overtaken = verb->TakeDeferredRebuild();
    passed &= Expect(verb->LoadResources({IRRef(trueStereoIR)}, {trueStereoIR}), "Failed to load another IR");

    if (overtaken)
    {
        overtaken->Build();
        verb->CommitDeferredRebuild(*overtaken);
    }

    passed &= Expect(overtaken != nullptr && verb->GetParam("quality") == 1.0 && !verb->TakeDeferredRebuild(),
                     "An IR load should build a waiting setting in");
    passed &= Expect(SameOutput(*verb, *MakeLoadedReverb(trueStereoIR, 1.0, 1.0)),
                     "A rebuild taken before an IR load should not bring the old IR back");

    std::error_code ignored;
    fs::remove_all(stereoIR.parent_path(), ignored);

    std::cout << (passed ? "IRReverbAutomationRealtimeTests passed" : "IRReverbAutomationRealtimeTests FAILED")
              << std::endl;
    return passed ? 0 : 1;
}
