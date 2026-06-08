#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kDefaultSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr int kWarmupBlocks = 300;
constexpr int kMeasureBlocks = 3000;

constexpr const char* kProfileBaseline = "baseline";
constexpr const char* kProfileNamConv = "namconv";

struct ProcessSnapshot
{
    std::uint64_t cpuTime100ns = 0;
    std::size_t workingSetBytes = 0;
    std::size_t privateBytes = 0;
};

struct BenchmarkResult
{
    std::string profile;
    int presetCount = 0;
    bool multiThreaded = false;
    double wallMs = 0.0;
    double avgBlockUs = 0.0;
    double cpuUtilPercent = 0.0;
    std::size_t workingSetBytes = 0;
    std::size_t privateBytes = 0;
};

struct BenchmarkSettings
{
    std::string profile = kProfileBaseline;
    double sampleRate = kDefaultSampleRate;
    bool monoInput = false;
    std::string csvPath;
};

struct NamConvAssets
{
    fs::path modelA;
    fs::path modelB;
    fs::path irCab;
    fs::path reverbIr;
};

#if defined(_WIN32)
std::uint64_t FileTimeToUInt64(const FILETIME& ft)
{
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | static_cast<std::uint64_t>(ft.dwLowDateTime);
}

ProcessSnapshot CaptureProcessSnapshot()
{
    ProcessSnapshot snapshot;

    FILETIME createTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime) != 0)
    {
        snapshot.cpuTime100ns = FileTimeToUInt64(kernelTime) + FileTimeToUInt64(userTime);
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0)
    {
        snapshot.workingSetBytes = static_cast<std::size_t>(pmc.WorkingSetSize);
        snapshot.privateBytes = static_cast<std::size_t>(pmc.PrivateUsage);
    }

    return snapshot;
}
#else
ProcessSnapshot CaptureProcessSnapshot()
{
    return {};
}
#endif

Preset CreateBaselinePreset(const std::string& presetId)
{
    Preset preset;
    preset.id = presetId;
    preset.name = presetId;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    graph.nodes.push_back({"gain1", "gain", "utility", "Gain 1", true});
    graph.nodes.back().params["gainDb"] = -1.0;

    graph.nodes.push_back({"delay", "delay_digital", "delay", "Delay", true});
    graph.nodes.back().params["timeMs"] = 85.0;
    graph.nodes.back().params["feedback"] = 0.42;
    graph.nodes.back().params["mix"] = 0.28;

    graph.nodes.push_back({"gain2", "gain", "utility", "Gain 2", true});
    graph.nodes.back().params["gainDb"] = 0.8;

    graph.nodes.push_back({"eq", "eq_parametric", "eq", "Parametric EQ", true});

    graph.nodes.push_back({"reverb", "reverb_room", "reverb", "Room Reverb", true});
    graph.nodes.back().params["mix"] = 0.24;
    graph.nodes.back().params["roomSize"] = 0.62;
    graph.nodes.back().params["damping"] = 0.35;

    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});

    graph.edges.push_back({"in", "gain1", 0, 0, 1.0});
    graph.edges.push_back({"gain1", "delay", 0, 0, 1.0});
    graph.edges.push_back({"delay", "gain2", 0, 0, 1.0});
    graph.edges.push_back({"gain2", "eq", 0, 0, 1.0});
    graph.edges.push_back({"eq", "reverb", 0, 0, 1.0});
    graph.edges.push_back({"reverb", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

bool HasExtension(const fs::path& path, const std::string& extLower)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == extLower;
}

NamConvAssets DiscoverNamConvAssets(const fs::path& repoRoot)
{
    NamConvAssets assets;
    const fs::path presetsRoot = repoRoot / "resources" / "metal-presets";

    if (!fs::exists(presetsRoot))
    {
        throw std::runtime_error("Preset assets root does not exist: " + presetsRoot.string());
    }

    std::vector<fs::path> models;
    std::vector<fs::path> irs;
    for (const auto& entry : fs::recursive_directory_iterator(presetsRoot))
    {
        if (!entry.is_regular_file())
            continue;

        const fs::path file = entry.path();
        if (HasExtension(file, ".nam"))
            models.push_back(file);
        else if (HasExtension(file, ".wav"))
            irs.push_back(file);
    }

    if (models.size() < 2)
    {
        throw std::runtime_error("Need at least 2 .nam files under resources/metal-presets for namconv profile");
    }
    if (irs.size() < 2)
    {
        throw std::runtime_error("Need at least 2 .wav IR files under resources/metal-presets for namconv profile");
    }

    std::sort(models.begin(), models.end());
    std::sort(irs.begin(), irs.end());

    assets.modelA = models[0];
    assets.modelB = models[1];
    assets.irCab = irs[0];
    assets.reverbIr = irs[1];
    return assets;
}

Preset CreateNamConvPreset(const std::string& presetId, const NamConvAssets& assets)
{
    Preset preset;
    preset.id = presetId;
    preset.name = presetId;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    GraphNode amp1;
    amp1.id = "amp1";
    amp1.type = "amp_nam_optimized";
    amp1.category = "amp";
    amp1.label = "NAM 1";
    amp1.enabled = true;
    ResourceRef ampRef1;
    ampRef1.resourceType = "nam";
    ampRef1.filePath = assets.modelA;
    amp1.resources.push_back(ampRef1);
    graph.nodes.push_back(std::move(amp1));

    GraphNode amp2;
    amp2.id = "amp2";
    amp2.type = "amp_nam_optimized";
    amp2.category = "amp";
    amp2.label = "NAM 2";
    amp2.enabled = true;
    ResourceRef ampRef2;
    ampRef2.resourceType = "nam";
    ampRef2.filePath = assets.modelB;
    amp2.resources.push_back(ampRef2);
    graph.nodes.push_back(std::move(amp2));

    GraphNode cab;
    cab.id = "cab";
    cab.type = "ir_cab";
    cab.category = "cab";
    cab.label = "IR Cab";
    cab.enabled = true;
    ResourceRef cabRef;
    cabRef.resourceType = "ir";
    cabRef.filePath = assets.irCab;
    cab.resources.push_back(cabRef);
    graph.nodes.push_back(std::move(cab));

    GraphNode reverb;
    reverb.id = "rev";
    reverb.type = "reverb_ir";
    reverb.category = "reverb";
    reverb.label = "Convolution Reverb";
    reverb.enabled = true;
    reverb.params["mix"] = 0.15;
    ResourceRef reverbRef;
    reverbRef.resourceType = "ir";
    reverbRef.filePath = assets.reverbIr;
    reverb.resources.push_back(reverbRef);
    graph.nodes.push_back(std::move(reverb));

    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});

    graph.edges.push_back({"in", "amp1", 0, 0, 1.0});
    graph.edges.push_back({"amp1", "amp2", 0, 0, 1.0});
    graph.edges.push_back({"amp2", "cab", 0, 0, 1.0});
    graph.edges.push_back({"cab", "rev", 0, 0, 1.0});
    graph.edges.push_back({"rev", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

BenchmarkResult RunBenchmark(const BenchmarkSettings& settings, int presetCount, bool multiThreaded)
{
    MultiPresetMixer mixer;
    ResourceLibrary library;
    const fs::path repoRoot = fs::current_path();
    const fs::path resourcesRoot = repoRoot / "resources";
    if (fs::exists(resourcesRoot))
    {
        library.LoadFromDirectory(resourcesRoot);
        mixer.SetResourceLibrary(&library);
    }

    std::optional<NamConvAssets> namConvAssets;
    if (settings.profile == kProfileNamConv)
    {
        namConvAssets = DiscoverNamConvAssets(repoRoot);
        std::cout << "Using NAM/IR assets:\n"
                  << "  modelA=" << namConvAssets->modelA.string() << "\n"
                  << "  modelB=" << namConvAssets->modelB.string() << "\n"
                  << "  irCab=" << namConvAssets->irCab.string() << "\n"
                  << "  reverbIr=" << namConvAssets->reverbIr.string() << "\n";
    }

    mixer.SetMultiThreadedProcessingEnabled(multiThreaded);
    mixer.Prepare(settings.sampleRate, kBlockSize);

    for (int i = 0; i < presetCount; ++i)
    {
        const std::string id = "preset" + std::to_string(i + 1);
        Preset preset;
        if (settings.profile == kProfileNamConv)
            preset = CreateNamConvPreset(id, *namConvAssets);
        else
            preset = CreateBaselinePreset(id);

        if (!mixer.AddActivePreset(preset, id, id))
        {
            throw std::runtime_error("Failed to add benchmark preset: " + id);
        }
    }

    std::vector<float> inL(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> inR(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outL(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(kBlockSize), 0.0f);

    for (int i = 0; i < kBlockSize; ++i)
    {
        const double phase = static_cast<double>(i) * 2.0 * 3.14159265358979323846 / 64.0;
        inL[static_cast<std::size_t>(i)] = static_cast<float>(0.35 * std::sin(phase));
        inR[static_cast<std::size_t>(i)] = settings.monoInput
            ? inL[static_cast<std::size_t>(i)]
            : static_cast<float>(0.35 * std::cos(phase));
    }

    float* inputs[2] = { inL.data(), inR.data() };
    float* outputs[2] = { outL.data(), outR.data() };

    for (int i = 0; i < kWarmupBlocks; ++i)
    {
        mixer.Process(inputs, outputs, kBlockSize);
    }

    const auto startSnapshot = CaptureProcessSnapshot();
    const auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < kMeasureBlocks; ++i)
    {
        const float mod = static_cast<float>(0.001 * std::sin(static_cast<double>(i) * 0.01));
        inL[0] += mod;
        inR[0] += settings.monoInput ? mod : -mod;
        mixer.Process(inputs, outputs, kBlockSize);
    }

    const auto endTime = std::chrono::steady_clock::now();
    const auto endSnapshot = CaptureProcessSnapshot();

    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();
    const double wallMs = static_cast<double>(wallNs) / 1.0e6;
    const double avgBlockUs = static_cast<double>(wallNs) / static_cast<double>(kMeasureBlocks) / 1.0e3;

    const unsigned int logicalCores = std::max(1u, std::thread::hardware_concurrency());
    const std::uint64_t cpuDelta100ns = (endSnapshot.cpuTime100ns >= startSnapshot.cpuTime100ns)
        ? (endSnapshot.cpuTime100ns - startSnapshot.cpuTime100ns)
        : 0;

    const double cpuSeconds = static_cast<double>(cpuDelta100ns) * 1.0e-7;
    const double wallSeconds = static_cast<double>(wallNs) * 1.0e-9;
    const double cpuUtilPercent = (wallSeconds > 0.0)
        ? (cpuSeconds / (wallSeconds * static_cast<double>(logicalCores))) * 100.0
        : 0.0;

    BenchmarkResult result;
    result.profile = settings.profile;
    result.presetCount = presetCount;
    result.multiThreaded = multiThreaded;
    result.wallMs = wallMs;
    result.avgBlockUs = avgBlockUs;
    result.cpuUtilPercent = cpuUtilPercent;
    result.workingSetBytes = endSnapshot.workingSetBytes;
    result.privateBytes = endSnapshot.privateBytes;
    return result;
}

void PrintResult(const BenchmarkResult& result)
{
    std::cout << std::left
              << std::setw(8) << (result.multiThreaded ? "multi" : "single")
              << std::setw(9) << result.presetCount
              << std::setw(14) << std::fixed << std::setprecision(2) << result.wallMs
              << std::setw(14) << std::fixed << std::setprecision(3) << result.avgBlockUs
              << std::setw(12) << std::fixed << std::setprecision(2) << result.cpuUtilPercent
              << std::setw(15) << static_cast<unsigned long long>(result.workingSetBytes / (1024ull * 1024ull))
              << std::setw(15) << static_cast<unsigned long long>(result.privateBytes / (1024ull * 1024ull))
              << '\n';
}

bool ParseArgs(int argc, char* argv[], BenchmarkSettings& settings, bool& showHelp)
{
    showHelp = false;
    settings.profile = kProfileBaseline;
    settings.sampleRate = kDefaultSampleRate;
    settings.monoInput = false;
    settings.csvPath.clear();

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--csv")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing path after --csv\n";
                return false;
            }
            settings.csvPath = argv[++i];
            continue;
        }

        if (arg == "--profile")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value after --profile\n";
                return false;
            }

            settings.profile = argv[++i];
            if (settings.profile != kProfileBaseline && settings.profile != kProfileNamConv)
            {
                std::cerr << "Unsupported profile: " << settings.profile << "\n";
                std::cerr << "Supported: " << kProfileBaseline << ", " << kProfileNamConv << "\n";
                return false;
            }
            continue;
        }

        if (arg == "--sample-rate")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value after --sample-rate\n";
                return false;
            }

            try
            {
                settings.sampleRate = std::stod(argv[++i]);
            }
            catch (...)
            {
                std::cerr << "Invalid sample rate value\n";
                return false;
            }

            if (settings.sampleRate <= 0.0)
            {
                std::cerr << "Sample rate must be > 0\n";
                return false;
            }
            continue;
        }

        if (arg == "--input-mode")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value after --input-mode\n";
                return false;
            }

            const std::string mode = argv[++i];
            if (mode == "mono")
                settings.monoInput = true;
            else if (mode == "stereo")
                settings.monoInput = false;
            else
            {
                std::cerr << "Unsupported input mode: " << mode << "\n";
                std::cerr << "Supported: mono, stereo\n";
                return false;
            }
            continue;
        }

        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: SignalChainThreadingBenchmark [--profile baseline|namconv] [--sample-rate <hz>] [--input-mode mono|stereo] [--csv <path>]\n";
            std::cout << "  --profile <name>  Benchmark profile to run (default: baseline).\n";
            std::cout << "  --sample-rate <hz>  Processing sample rate for benchmark run (default: 48000).\n";
            std::cout << "  --input-mode <mode>  Input signal mode: mono or stereo (default: stereo).\n";
            std::cout << "  --csv <path>  Write results table as CSV to the given file path.\n";
            showHelp = true;
            return false;
        }

        std::cerr << "Unknown argument: " << arg << '\n';
        std::cerr << "Usage: SignalChainThreadingBenchmark [--profile baseline|namconv] [--sample-rate <hz>] [--input-mode mono|stereo] [--csv <path>]\n";
        return false;
    }

    return true;
}

bool WriteCsv(const std::string& csvPath,
              const std::vector<BenchmarkResult>& rows,
              double sampleRate,
              bool monoInput,
              double onePresetSpeedup,
              double fourPresetSpeedup)
{
    std::ofstream out(csvPath, std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open CSV output path: " << csvPath << '\n';
        return false;
    }

    out << "profile,sample_rate,input_mode,block_size,warmup_blocks,measure_blocks,mode,presets,wall_ms,avg_block_us,cpu_pct,working_set_mb,private_mb\n";
    for (const auto& row : rows)
    {
        out << row.profile << ','
            << static_cast<int>(std::lround(sampleRate)) << ','
            << (monoInput ? "mono" : "stereo") << ','
            << kBlockSize << ','
            << kWarmupBlocks << ','
            << kMeasureBlocks << ','
            << (row.multiThreaded ? "multi" : "single") << ','
            << row.presetCount << ','
            << std::fixed << std::setprecision(6) << row.wallMs << ','
            << std::fixed << std::setprecision(6) << row.avgBlockUs << ','
            << std::fixed << std::setprecision(6) << row.cpuUtilPercent << ','
            << (row.workingSetBytes / (1024ull * 1024ull)) << ','
            << (row.privateBytes / (1024ull * 1024ull)) << '\n';
    }

    out << "\nmetric,value\n";
    out << "speedup_single_over_multi_1_preset," << std::fixed << std::setprecision(6) << onePresetSpeedup << '\n';
    out << "speedup_single_over_multi_4_presets," << std::fixed << std::setprecision(6) << fourPresetSpeedup << '\n';

    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        BenchmarkSettings settings;
        bool showHelp = false;
        if (!ParseArgs(argc, argv, settings, showHelp))
        {
            return showHelp ? 0 : 1;
        }

        RegisterAllEffects();

        std::cout << "==============================================\n";
        std::cout << "Signal Chain Threading Benchmark\n";
        std::cout << "Profile=" << settings.profile << "\n";
        std::cout << "SampleRate=" << settings.sampleRate << ", BlockSize=" << kBlockSize
                  << ", InputMode=" << (settings.monoInput ? "mono" : "stereo")
                  << ", WarmupBlocks=" << kWarmupBlocks
                  << ", MeasureBlocks=" << kMeasureBlocks << "\n";
        std::cout << "==============================================\n\n";

#if defined(_WIN32)
        std::cout << "CPU% uses process user+kernel time normalized by logical cores.\n";
        std::cout << "Memory columns are process WorkingSet and PrivateUsage snapshots.\n\n";
#else
        std::cout << "Non-Windows build: memory and CPU process snapshots are not collected.\n\n";
#endif

        std::cout << std::left
                  << std::setw(8) << "mode"
                  << std::setw(9) << "presets"
                  << std::setw(14) << "wall_ms"
                  << std::setw(14) << "avg_block_us"
                  << std::setw(12) << "cpu_pct"
                  << std::setw(15) << "working_set_mb"
                  << std::setw(15) << "private_mb"
                  << '\n';

        const auto singleOne = RunBenchmark(settings, 1, false);
        PrintResult(singleOne);
        std::cout << std::flush;

        const auto multiOne = RunBenchmark(settings, 1, true);
        PrintResult(multiOne);
        std::cout << std::flush;

        const auto singleFour = RunBenchmark(settings, 4, false);
        PrintResult(singleFour);
        std::cout << std::flush;

        const auto multiFour = RunBenchmark(settings, 4, true);
        PrintResult(multiFour);
        std::cout << std::flush;

        const std::vector<BenchmarkResult> rows{singleOne, multiOne, singleFour, multiFour};

        const double onePresetSpeedup = (multiOne.wallMs > 0.0) ? (singleOne.wallMs / multiOne.wallMs) : 0.0;
        const double fourPresetSpeedup = (multiFour.wallMs > 0.0) ? (singleFour.wallMs / multiFour.wallMs) : 0.0;

        std::cout << "\nRelative speedup (single/multi):\n";
        std::cout << "  1 preset : " << std::fixed << std::setprecision(3) << onePresetSpeedup << "x\n";
        std::cout << "  4 presets: " << std::fixed << std::setprecision(3) << fourPresetSpeedup << "x\n";

        if (!settings.csvPath.empty())
        {
            if (!WriteCsv(settings.csvPath, rows, settings.sampleRate, settings.monoInput, onePresetSpeedup, fourPresetSpeedup))
            {
                return 1;
            }
            std::cout << "CSV written: " << settings.csvPath << '\n';
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
