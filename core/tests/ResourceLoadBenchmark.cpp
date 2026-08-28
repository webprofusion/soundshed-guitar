/**
 * Resource Load Benchmark
 *
 * Measures the cost of the work that sits on the preset-switch critical path:
 * NAM model construction and IR partition building. Reports the split between
 * file IO + JSON parse and the model/FFT construction itself, so the value of
 * a resource cache can be judged rather than assumed.
 *
 * Labelled "benchmark" in CMakeLists.txt, so it is excluded from default runs.
 */

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/EffectGuids.h"
#include "dsp/IRWavLoader.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace fs = std::filesystem;

namespace nam::factory
{
void ForceFactoryRegistration();
}

namespace
{
using Clock = std::chrono::steady_clock;

// path::string() throws on names the active code page cannot represent, and
// the asset set deliberately contains such names.
std::string DisplayName(const fs::path& path)
{
    try
    {
        return path.filename().string();
    }
    catch (const std::exception&)
    {
        const auto wide = path.filename().wstring();
        std::string ascii;
        ascii.reserve(wide.size());

        for (const wchar_t c : wide)
        {
            ascii.push_back(c < 128 ? static_cast<char>(c) : '?');
        }

        return ascii;
    }
}

double MsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Median of a small sample set; more stable than a mean when the OS steals a slice.
double Median(std::vector<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void PrintRow(const std::string& label, double ms)
{
    std::cout << "  " << std::left << std::setw(46) << label << std::right << std::fixed << std::setprecision(2)
              << std::setw(9) << ms << " ms\n";
}

std::vector<fs::path> CollectFiles(const fs::path& root, const std::string& extension, std::size_t limit)
{
    std::vector<fs::path> found;

    if (!fs::exists(root))
    {
        return found;
    }

    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        if (entry.path().extension() != extension)
        {
            continue;
        }

        found.push_back(entry.path());

        if (found.size() >= limit)
        {
            break;
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

constexpr int kIterations = 5;

void BenchmarkNamModel(const fs::path& path)
{
    const auto sizeKb = static_cast<double>(fs::file_size(path)) / 1024.0;
    std::cout << "\n" << DisplayName(path) << "  (" << std::fixed << std::setprecision(0) << sizeKb << " KB)\n";

    // Warm the OS file cache so we measure parse cost, not first-touch disk IO.
    {
        auto warm = ::nam::get_dsp(path);
        (void)warm;
    }

    std::vector<double> fromFile, fromData, dataCopy;
    ::nam::dspData cached;
    {
        auto probe = ::nam::get_dsp(path, cached);
        (void)probe;
    }

    for (int i = 0; i < kIterations; ++i)
    {
        {
            const auto t = Clock::now();
            auto model = ::nam::get_dsp(path);
            fromFile.push_back(MsSince(t));

            if (!model)
            {
                std::cout << "  (null model)\n";
            }
        }
        {
            // What a dspData cache hit would cost: copy the cached struct (the cache
            // must hand out a copy, since get_dsp takes a non-const reference) and
            // construct from it. No file IO, no JSON parse of the weights blob.
            const auto t = Clock::now();
            ::nam::dspData copy = cached;
            const double copyMs = MsSince(t);
            auto model = ::nam::get_dsp(copy);
            fromData.push_back(MsSince(t));
            dataCopy.push_back(copyMs);
        }
    }

    // Reset() is where NAM prewarms. LoadModelResource() -> ConfigureModelProcessing()
    // calls it once per model, and Prepare() -> ConfigureModelProcessing() calls it again,
    // so measure it separately from construction.
    std::vector<double> resetMs;
    {
        auto model = ::nam::get_dsp(path);

        for (int i = 0; i < kIterations; ++i)
        {
            const auto t = Clock::now();
            model->Reset(48000.0, 512);
            resetMs.push_back(MsSince(t));
        }
    }

    const double file = Median(fromFile);
    const double data = Median(fromData);
    PrintRow("Reset(48k, 512)  [prewarm]", Median(resetMs));
    PrintRow("get_dsp(path)  [current, per model]", file);
    PrintRow("get_dsp(dspData)  [cache hit]", data);
    PrintRow("  of which: dspData struct copy", Median(dataCopy));
    PrintRow("current cost for one node (2x path)", file * 2.0);
    PrintRow("cached cost for one node (2x data)", data * 2.0);
    std::cout << "  -> per-node saving: " << std::fixed << std::setprecision(2) << (file * 2.0 - data * 2.0) << " ms ("
              << std::setprecision(1) << (100.0 * (1.0 - data / std::max(file, 1e-9))) << "% off each model)\n";
}

void BenchmarkIr(const fs::path& path, int blockSize)
{
    std::cout << "\n" << DisplayName(path) << "\n";

    guitarfx::IRWavData data;
    {
        guitarfx::irwav::LoadWavFile(path, data);
    }

    std::vector<double> loadMs, partitionMs;

    for (int i = 0; i < kIterations; ++i)
    {
        {
            guitarfx::IRWavData fresh;
            const auto t = Clock::now();
            guitarfx::irwav::LoadWavFile(path, fresh);
            loadMs.push_back(MsSince(t));
        }
        {
            std::vector<float> mono = data.samples;

            if (data.channels > 1)
            {
                std::vector<float> l, r;
                guitarfx::irwav::SplitToStereo(data, l, r);
                mono = l;
            }

            guitarfx::RealtimeConvolver convolver;
            const auto t = Clock::now();
            convolver.SetImpulse(mono, blockSize);
            partitionMs.push_back(MsSince(t));
        }
    }

    const double partition = Median(partitionMs);
    PrintRow("LoadWavFile (decode)", Median(loadMs));
    PrintRow("SetImpulse (FFT partition build)", partition);
    PrintRow("current cost for one mono node (2x)", partition * 2.0);
    std::cout << "  -> a shared partition table would save " << std::fixed << std::setprecision(2) << partition
              << " ms on the second convolver alone\n";
}

// A realistic live rig: amp + cab + time-based FX. This is what PreparePresetSwap()
// has to build before a switch can commit, so its wall time is the switch latency.
guitarfx::Preset MakeRigPreset(const std::string& id, const fs::path& namPath, const fs::path& irPath)
{
    using namespace guitarfx;

    auto node = [](const std::string& nodeId, const char* type) {
        GraphNode n;
        n.id = nodeId;
        n.type = type;
        n.enabled = true;
        return n;
    };

    GraphNode in = node("in", kNodeTypeInput);
    GraphNode amp = node("amp", EffectGuids::kAmpNamOptimized);
    GraphNode cab = node("cab", EffectGuids::kCabIr);
    GraphNode delay = node("delay", EffectGuids::kDelayDigital);
    GraphNode reverb = node("reverb", EffectGuids::kReverbRoom);
    GraphNode out = node("out", kNodeTypeOutput);

    ResourceRef namRef;
    namRef.filePath = namPath.string();
    amp.resources.push_back(namRef);

    ResourceRef irRef;
    irRef.filePath = irPath.string();
    cab.resources.push_back(irRef);

    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph.nodes = {in, amp, cab, delay, reverb, out};
    preset.graph.edges = {{"in", "amp"}, {"amp", "cab"}, {"cab", "delay"}, {"delay", "reverb"}, {"reverb", "out"}};
    return preset;
}

void BenchmarkPresetBuild(const fs::path& namPath, const fs::path& irPath)
{
    using namespace guitarfx;

    std::cout << "\nRig: input -> NAM(" << DisplayName(namPath) << ") -> IR cab(" << DisplayName(irPath)
              << ") -> delay -> reverb -> output\n";

    RegisterAllEffects();

    MultiPresetMixer mixer;
    mixer.Prepare(48000.0, 512);

    const Preset a = MakeRigPreset("rig-a", namPath, irPath);
    const Preset b = MakeRigPreset("rig-b", namPath, irPath);

    // Warm the OS file cache; the first build also pays one-off registry/FFT-plan costs.
    mixer.PreparePresetSwap(a, "rig-a", "rig-a");
    mixer.CommitPresetSwap();

    std::vector<double> prepareMs, commitMs;

    for (int i = 0; i < kIterations; ++i)
    {
        const Preset& next = (i % 2 == 0) ? b : a;
        const std::string id = next.id;

        const auto t0 = Clock::now();
        mixer.PreparePresetSwap(next, id, id);
        prepareMs.push_back(MsSince(t0));

        const auto t1 = Clock::now();
        mixer.CommitPresetSwap();
        commitMs.push_back(MsSince(t1));
    }

    PrintRow("PreparePresetSwap (off the DSP lock)", Median(prepareMs));
    PrintRow("CommitPresetSwap (under the DSP lock)", Median(commitMs));
    std::cout << "  -> total switch latency: " << std::fixed << std::setprecision(2)
              << (Median(prepareMs) + Median(commitMs)) << " ms\n";

    // Split the build so it is clear how much is resource loading and how much is
    // graph construction / buffer allocation.
    std::vector<double> setGraphMs, executorPrepareMs, namOnlyMs, noResourceMs;

    for (int i = 0; i < kIterations; ++i)
    {
        {
            SignalGraphExecutor executor;
            const auto t = Clock::now();
            executor.SetGraph(a.graph);
            setGraphMs.push_back(MsSince(t));

            const auto t2 = Clock::now();
            executor.Prepare(48000.0, 512);
            executorPrepareMs.push_back(MsSince(t2));
        }
        {
            // Same graph with the resource refs stripped: isolates construction from loading.
            Preset bare = a;

            for (auto& n : bare.graph.nodes)
            {
                n.resources.clear();
            }

            SignalGraphExecutor executor;
            const auto t = Clock::now();
            executor.SetGraph(bare.graph);
            executor.Prepare(48000.0, 512);
            noResourceMs.push_back(MsSince(t));
        }
        {
            // Amp node alone, to attribute the NAM share of SetGraph.
            Preset ampOnly = a;

            for (auto& n : ampOnly.graph.nodes)
            {
                if (n.id != "amp")
                {
                    n.resources.clear();
                }
            }

            SignalGraphExecutor executor;
            const auto t = Clock::now();
            executor.SetGraph(ampOnly.graph);
            namOnlyMs.push_back(MsSince(t));
        }
    }

    std::cout << "\n  Breakdown of one build:\n";
    PrintRow("SetGraph (create processors + load resources)", Median(setGraphMs));
    PrintRow("  same graph, resources stripped", Median(noResourceMs));
    PrintRow("  NAM node only (of SetGraph)", Median(namOnlyMs));
    PrintRow("Prepare (buffer allocation)", Median(executorPrepareMs));
}
} // namespace

int main()
{
    nam::factory::ForceFactoryRegistration(); // Pull the architecture parsers in from the static lib.

    std::cout << std::unitbuf; // Unbuffered: a crash mid-run must not swallow the results so far.

    const fs::path assets = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets";

    std::cout << "=====================================================================\n";
    std::cout << " Resource load cost (preset-switch critical path)\n";
    std::cout << "=====================================================================\n";

    const auto namFiles = CollectFiles(assets / "amps", ".nam", 4);

    if (namFiles.empty())
    {
        std::cout << "\nNo .nam assets found under " << (assets / "amps").string() << "\n";
    }

    for (const auto& path : namFiles)
    {
        try
        {
            BenchmarkNamModel(path);
        }
        catch (const std::exception& e)
        {
            std::cout << "  skipped: " << e.what() << "\n";
        }
    }

    std::cout << "\n---------------------------------------------------------------------\n";
    std::cout << " IR partitioning (block size 512)\n";
    std::cout << "---------------------------------------------------------------------\n";

    const auto irFiles = CollectFiles(assets / "ir", ".wav", 3);

    if (irFiles.empty())
    {
        std::cout << "\nNo .wav assets found under " << (assets / "ir").string() << "\n";
    }

    for (const auto& path : irFiles)
    {
        BenchmarkIr(path, 512);
    }

    std::cout << "\n---------------------------------------------------------------------\n";
    std::cout << " End-to-end preset switch (what the player actually waits for)\n";
    std::cout << "---------------------------------------------------------------------\n";

    if (!namFiles.empty() && !irFiles.empty())
    {
        try
        {
            BenchmarkPresetBuild(namFiles.front(), irFiles.front());
        }
        catch (const std::exception& e)
        {
            std::cout << "  skipped: " << e.what() << "\n";
        }
    }

    std::cout << "\n";
    return 0;
}
