/**
 * IRBuildOnPrepareTests.cpp — loading a preset builds each IR effect's convolvers once.
 *
 * SignalGraphExecutor loads a new node's resources before it prepares the node (CreateProcessors,
 * then Prepare), and until Prepare an effect has only the default rate and block size. The IR cab
 * and the convolution reverb used to build their convolvers for those in the load, and Prepare then
 * built them again for the host. For a long true-stereo reverb that is a resample, a truncation and
 * the FFT setup of seconds of IR, twice, on the preset-load path. A load before Prepare now keeps
 * the impulses and Prepare does the one build. A load on a prepared, live effect still builds at once.
 *
 * For each effect: a load before Prepare builds nothing, Prepare builds it, a load after Prepare
 * builds at once, and either order renders bit-identical output. It also prints what a preset load
 * through MultiPresetMixer::AddActivePreset costs with a long 48 kHz reverb IR at 48 kHz.
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

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

/// This run's own folder: two runs at once, from two build trees, must not delete each other's IRs.
const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_build_on_prepare_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

/// Exponentially decaying noise, one channel per entry, written as a 32-bit float WAV.
fs::path WriteIR(const std::string& name, double seconds, double sampleRate, int channels, unsigned seed)
{
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> interleaved(frames * static_cast<std::size_t>(channels));
    std::mt19937 rng(seed);
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

    fs::create_directories(TempDir());
    const auto path = TempDir() / name;

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

/// An IR reference as the executor hands it over, stamped with the slot it fills.
ResourceRef IRRef(const fs::path& path, std::size_t slot)
{
    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = path;
    ir.metadata["resourceSlotIndex"] = std::to_string(slot);
    return ir;
}

struct LoadCase
{
    std::string name;
    std::string type;
    std::map<std::string, double> params;
    std::vector<fs::path> irs;
};

std::unique_ptr<EffectProcessor> MakeEffect(const LoadCase& loadCase)
{
    auto effect = EffectRegistry::Instance().Create(loadCase.type);

    for (const auto& [key, value] : loadCase.params)
    {
        effect->SetParam(key, value);
    }

    return effect;
}

bool Load(EffectProcessor& effect, const LoadCase& loadCase)
{
    std::vector<ResourceRef> refs;

    for (std::size_t slot = 0; slot < loadCase.irs.size(); ++slot)
    {
        refs.push_back(IRRef(loadCase.irs[slot], slot));
    }

    return effect.LoadResources(refs, loadCase.irs);
}

/// An impulse, then noise, through `effect` in host-sized blocks; both channels, block by block.
std::vector<float> Render(EffectProcessor& effect, int blocks)
{
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> noise(-0.25f, 0.25f);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);
    std::vector<float> rendered;
    rendered.reserve(static_cast<std::size_t>(blocks) * kBlockSize * 2);

    for (int block = 0; block < blocks; ++block)
    {
        for (std::size_t i = 0; i < inL.size(); ++i)
        {
            const bool impulse = block == 0 && i == 0;
            inL[i] = impulse ? 1.0f : noise(rng);
            inR[i] = impulse ? 1.0f : noise(rng);
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        effect.Process(inputs, outputs, kBlockSize);
        rendered.insert(rendered.end(), outL.begin(), outL.end());
        rendered.insert(rendered.end(), outR.begin(), outR.end());
    }

    return rendered;
}

bool TestLoadOrder(const LoadCase& loadCase)
{
    bool passed = true;

    // A preset's node: params, then the IR (SignalGraphExecutor::CreateProcessors), then Prepare.
    auto loadedFirst = MakeEffect(loadCase);
    passed &= Expect(Load(*loadedFirst, loadCase), loadCase.name + ": the IR should load before Prepare");
    passed &= Expect(!loadedFirst->HasResource(),
                     loadCase.name + ": a load before Prepare should leave the build to Prepare");
    loadedFirst->Prepare(kSampleRate, kBlockSize);
    passed &= Expect(loadedFirst->HasResource(), loadCase.name + ": Prepare should build the IR loaded before it");

    // A live node given its IR.
    auto preparedFirst = MakeEffect(loadCase);
    preparedFirst->Prepare(kSampleRate, kBlockSize);
    passed &= Expect(Load(*preparedFirst, loadCase) && preparedFirst->HasResource(),
                     loadCase.name + ": a load on a prepared effect should build at once");

    passed &= Expect(loadedFirst->GetLatencySamples() == preparedFirst->GetLatencySamples(),
                     loadCase.name + ": both orders should report the same latency");
    passed &= Expect(Render(*loadedFirst, 400) == Render(*preparedFirst, 400),
                     loadCase.name + ": both orders should render bit-identical output");

    std::cout << "  " << (passed ? "PASS" : "FAIL") << "  " << loadCase.name << std::endl;
    return passed;
}

Preset MakeReverbPreset(const std::string& id, const fs::path& irPath)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    GraphNode in{"in", kNodeTypeInput, "", "Input", true};
    GraphNode verb{"verb", EffectGuids::kReverbIr, "reverb", "Convolution Reverb", true};
    verb.params["mix"] = 0.5;
    verb.resources.push_back(IRRef(irPath, 0));
    GraphNode out{"out", kNodeTypeOutput, "", "Output", true};

    preset.graph.nodes = {in, verb, out};
    preset.graph.edges = {GraphEdge{"in", "verb", 0, 0, 1.0}, GraphEdge{"verb", "out", 0, 0, 1.0}};
    return preset;
}

double MsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// What a preset load with this reverb IR costs, split the way the mixer does it. Prints only: the
/// time depends on the machine and the build.
bool ReportPresetLoadCost(const std::string& label, const fs::path& irPath)
{
    ResourceLibrary library;
    const Preset preset = MakeReverbPreset("verb", irPath);

    // MultiPresetMixer::BuildInstance's two steps, timed apart.
    SignalGraphExecutor executor;
    executor.SetResourceLibrary(&library);
    const auto setGraphStart = Clock::now();
    executor.SetGraph(preset.graph);
    const double setGraphMs = MsSince(setGraphStart);
    const EffectProcessor* verb = executor.GetNodeProcessor("verb");
    const bool builtOnLoad = verb && verb->HasResource();
    const auto prepareStart = Clock::now();
    executor.Prepare(kSampleRate, kBlockSize);
    const double prepareMs = MsSince(prepareStart);

    MultiPresetMixer mixer;
    mixer.SetResourceLibrary(&library);
    mixer.Prepare(kSampleRate, kBlockSize);
    const auto addStart = Clock::now();
    const bool added = mixer.AddActivePreset(preset, preset.id, preset.name);
    const double addMs = MsSince(addStart);
    const EffectProcessor* mixerVerb = mixer.GetNodeProcessor(preset.id, "verb");

    std::cout << std::fixed << std::setprecision(1) << "  " << label << ": AddActivePreset " << addMs
              << " ms; executor SetGraph " << setGraphMs << " ms (convolvers " << (builtOnLoad ? "built" : "not built")
              << "), Prepare " << prepareMs << " ms" << std::endl;

    return Expect(added && mixerVerb && mixerVerb->HasResource(), label + ": the preset's reverb should load");
}
} // namespace

int main()
{
    RegisterAllEffects();
    bool passed = true;

    std::cout << "IR effects build their convolvers once, in Prepare" << std::endl;

    // IR rates on both sides of the host's, so the builds resample as well as truncate.
    const fs::path verbQuad = WriteIR("verb_1s_44k_quad.wav", 1.0, 44100.0, 4, 1);
    const fs::path verbStereo = WriteIR("verb_1s_48k_stereo.wav", 1.0, 48000.0, 2, 2);
    const fs::path cabStereo = WriteIR("cab_200ms_44k_stereo.wav", 0.2, 44100.0, 2, 3);
    const fs::path cabMono = WriteIR("cab_100ms_96k_mono.wav", 0.1, 96000.0, 1, 4);

    // Quality and Low Latency set before the IR loads are folded into the one build.
    passed &= TestLoadOrder({"reverb, true stereo",
                             EffectGuids::kReverbIr,
                             {{"mix", 0.6}, {"quality", 1.0}, {"lowLatency", 0.0}},
                             {verbQuad}});
    passed &= TestLoadOrder({"reverb, stereo", EffectGuids::kReverbIr, {{"mix", 1.0}}, {verbStereo}});
    passed &= TestLoadOrder({"cab, slot A", EffectGuids::kCabIr, {{"normalizeIR", 0.0}}, {cabStereo}});
    passed &= TestLoadOrder({"cab, slots A and B",
                             EffectGuids::kCabIr,
                             {{"irBlend", 0.5}, {"quality", 3.0}, {"lowLatency", 0.0}},
                             {cabStereo, cabMono}});

    // A build before Prepare would run at the default 44.1 kHz, so for an IR at the host's 48 kHz it
    // also resampled, which Prepare then threw away. An IR at 44.1 kHz is resampled by Prepare either
    // way, so its line is what the resampler costs a preset load.
    std::cout << "Preset load cost with a long reverb IR, host at 48 kHz / " << kBlockSize << std::endl;
    passed &=
        ReportPresetLoadCost("6 s true-stereo 48 kHz reverb", WriteIR("verb_6s_48k_quad.wav", 6.0, 48000.0, 4, 5));
    passed &=
        ReportPresetLoadCost("6 s true-stereo 44.1 kHz reverb", WriteIR("verb_6s_44k_quad.wav", 6.0, 44100.0, 4, 6));

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    std::cout << (passed ? "All tests passed" : "Some tests FAILED") << std::endl;
    return passed ? 0 : 1;
}
