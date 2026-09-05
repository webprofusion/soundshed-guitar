/**
 * @file SteadyStateProfiler.cpp
 * @brief Samples the real steady-state audio path to show where audio-thread CPU goes.
 *
 * This is a profiler, not a test. It drives the mixer on a dedicated thread doing the
 * same per-block work the host callback does — the global chain in place, the tempo
 * pushed, then Process — and samples that thread's call stack from a second thread
 * (see helpers/StackSampler.h). That needs no elevation, unlike
 * ETW/WPR CPU sampling, and it attributes cost to real function names in the shipping
 * code rather than to a stopwatch someone had to place by hand.
 *
 * --seconds is wall-clock time spent measuring, not simulated audio duration: blocks
 * run back to back rather than at the audio device's pace.
 *
 * Usage:
 *   SteadyStateProfiler [--profile light|baseline|namconv|applive] [--block N] [--sr N]
 *                       [--seconds S] [--diagnostics on|off|both] [--presets N]
 *                       [--repeats N] [--top N] [--callers-of substr] [--csv path]
 */

#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include "helpers/ProfilerPresets.h"
#include "helpers/StackSampler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <dbghelp.h>
    #include <mmsystem.h>
    #pragma comment(lib, "dbghelp.lib")
    #pragma comment(lib, "winmm.lib")
#endif

using namespace guitarfx;
using namespace guitarfx::profiling;

namespace
{
namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;

struct Settings
{
    std::string profile = "baseline";
    double sampleRate = 48000.0;
    int blockSize = 128;
    double seconds = 3.0;
    int presetCount = 1;
    int topCount = 25;
    /// Timing passes per configuration. Configurations are interleaved and the minimum
    /// is kept: on a machine with other work running, run-to-run variance on a heavy
    /// chain is larger than the effect being measured, and the minimum is the estimator
    /// least contaminated by interference.
    int repeats = 3;
    std::string diagnostics = "both";
    std::string csvPath;
    /// When set, print the immediate callers of every sampled function whose name
    /// contains this substring -- what turns "the heap is busy" into "this line allocates".
    std::string callersOf;
};

// ---------------------------------------------------------------------------
// Reporting types
// ---------------------------------------------------------------------------

struct ProfileReport
{
    std::uint64_t sampleCount = 0;
    std::uint64_t blocksProcessed = 0;
    double avgBlockUs = 0.0;
    double p50BlockUs = 0.0;
    double p99BlockUs = 0.0;
    double realtimeBudgetUs = 0.0;
    std::vector<std::pair<std::string, SampleTotals>> functions;
    std::vector<CallerBreakdown> callerBreakdowns;
};

// ---------------------------------------------------------------------------
// Measurement driver
// ---------------------------------------------------------------------------

struct RunConfig
{
    const Settings* settings = nullptr;
    bool diagnosticsEnabled = true;
    // Suspending the DSP thread thousands of times a second destroys the block timings,
    // so a run either times or samples -- never both. RunProfile is called twice and the
    // two halves of the report are merged.
    bool sampling = true;
};

ProfileReport RunProfile(const RunConfig& config, const std::optional<NamConvAssets>& namAssets)
{
    const Settings& settings = *config.settings;

    MultiPresetMixer mixer;
    ResourceLibrary library;
    const fs::path repoRoot = fs::current_path();
    const fs::path resourcesRoot = repoRoot / "resources";

    if (fs::exists(resourcesRoot))
    {
        library.LoadFromDirectory(resourcesRoot);
        mixer.SetResourceLibrary(&library);
    }

    mixer.SetMultiThreadedProcessingEnabled(false);

    // The app always has a global chain either side of the preset graph — a gate and a
    // transpose in front, an EQ and a doubler behind, all disabled by default. Disabled
    // nodes still cost per-block bookkeeping, and the mixer's per-block calls fan out
    // over them, so a profile without them understates the framework's share.
    mixer.SetGlobalChainConfig(GlobalSignalChainConfig::CreateDefault());

    mixer.Prepare(settings.sampleRate, settings.blockSize);
    mixer.SetSignalDiagnosticsEnabled(config.diagnosticsEnabled);

    for (int i = 0; i < settings.presetCount; ++i)
    {
        const std::string id = "preset" + std::to_string(i + 1);
        Preset preset;

        if (settings.profile == "namconv" && namAssets.has_value())
        {
            preset = CreateNamConvPreset(id, *namAssets);
        }
        else if (settings.profile == "applive" && namAssets.has_value())
        {
            preset = CreateAppLivePreset(id, *namAssets);
        }
        else if (settings.profile == "light")
        {
            preset = CreateLightPreset(id);
        }
        else
        {
            preset = CreateBaselinePreset(id);
        }

        if (!mixer.AddActivePreset(preset, id, id))
        {
            std::cerr << "Failed to add preset " << id << "\n";
        }
    }

    // SetSignalDiagnosticsEnabled has to be re-applied: AddActivePreset seeds each new
    // instance's executor from the mixer flag, but the mixer's own flag is what we want
    // authoritative here, and re-asserting it is cheaper than reasoning about ordering.
    mixer.SetSignalDiagnosticsEnabled(config.diagnosticsEnabled);

    const std::size_t blockSamples = static_cast<std::size_t>(settings.blockSize);
    std::vector<float> inL(blockSamples, 0.0f);
    std::vector<float> inR(blockSamples, 0.0f);
    std::vector<float> outL(blockSamples, 0.0f);
    std::vector<float> outR(blockSamples, 0.0f);

    // A guitar-ish signal: silence and pure digital zero take denormal-flush and
    // noise-gate paths that no real input would.
    for (int i = 0; i < settings.blockSize; ++i)
    {
        const double t = static_cast<double>(i) / settings.sampleRate;
        const double value = 0.30 * std::sin(2.0 * kPi * 110.0 * t) + 0.12 * std::sin(2.0 * kPi * 330.0 * t) +
                             0.05 * std::sin(2.0 * kPi * 880.0 * t);
        inL[static_cast<std::size_t>(i)] = static_cast<float>(value);
        inR[static_cast<std::size_t>(i)] = static_cast<float>(value);
    }

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    const double realtimeBudgetUs = (static_cast<double>(settings.blockSize) / settings.sampleRate) * 1.0e6;

    const int warmupBlocks = 200;

    // PluginController::ProcessAudioLocked pushes the tempo before every Process call, so
    // a profile that only calls Process misses whatever that costs. Keep the two together
    // here: this is meant to be the host callback's per-block work, not just the graph's.
    constexpr double kTempoBpm = 120.0;

    const auto processOneBlock = [&]() {
        mixer.SetTempo(kTempoBpm);
        mixer.Process(inputs, outputs, settings.blockSize);
    };

    for (int i = 0; i < warmupBlocks; ++i)
    {
        processOneBlock();
    }

    // Hard ceiling so a pathologically cheap chain cannot run away; the wall-clock
    // deadline in the loop is what normally ends the pass.
    constexpr std::uint64_t kMaxBlocks = 4'000'000;

    std::vector<double> blockTimesUs;
    blockTimesUs.reserve(1u << 20);

    std::atomic<bool> dspReady{false};
    std::atomic<bool> dspDone{false};

#if defined(_WIN32)
    HANDLE dspThreadHandle = nullptr;
#endif

    std::thread dspThread([&]() {
#if defined(_WIN32)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dspThreadHandle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        dspReady.store(true, std::memory_order_release);

        // Blocks run back to back rather than at the audio device's pace, so --seconds is
        // wall-clock time spent measuring, not simulated audio duration. Both passes use
        // the same deadline so the sampling pass gathers a comparable number of samples.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(settings.seconds * 1000.0));

        for (std::uint64_t block = 0; block < kMaxBlocks; ++block)
        {
            if ((block % 64 == 0) && std::chrono::steady_clock::now() > deadline)
            {
                break;
            }

            // Nudge the input so nothing settles into a degenerate steady value.
            const float nudge = static_cast<float>(0.001 * std::sin(static_cast<double>(block) * 0.017));
            inL[0] += nudge;
            inR[0] += nudge;

            const auto start = std::chrono::steady_clock::now();
            processOneBlock();
            const auto end = std::chrono::steady_clock::now();

            blockTimesUs.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }

        dspDone.store(true, std::memory_order_release);
    });

    while (!dspReady.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    ProfileReport report;
    report.realtimeBudgetUs = realtimeBudgetUs;

#if defined(_WIN32)
    std::unordered_map<std::string, SampleTotals> totals;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> callerCounts;

    if (dspThreadHandle != nullptr && config.sampling)
    {
        StackSampler sampler(dspThreadHandle, 200); // 5 kHz
        sampler.Start();
        dspThread.join();
        sampler.Stop();

        const HANDLE process = GetCurrentProcess();
        std::unordered_map<DWORD64, std::string> symbolCache;

        for (const auto& stack : sampler.Stacks())
        {
            if (stack.empty())
            {
                continue;
            }

            ++report.sampleCount;

            std::vector<std::string> names;
            names.reserve(stack.size());

            for (const DWORD64 address : stack)
            {
                names.push_back(ResolveSymbol(process, address, symbolCache));
            }

            totals[names.front()].self += 1;

            // Attribute each matched frame to the frame that called it.
            if (!settings.callersOf.empty())
            {
                for (std::size_t depth = 0; depth < names.size(); ++depth)
                {
                    if (names[depth].find(settings.callersOf) == std::string::npos)
                    {
                        continue;
                    }

                    // Blame the nearest application frame, not the CRT wrapper directly
                    // above: "_malloc_base called RtlAllocateHeap" says nothing about
                    // which line in the audio path asked for memory.
                    std::string caller = "<root>";

                    for (std::size_t up = depth + 1; up < names.size(); ++up)
                    {
                        if (names[up].find("guitarfx::") != std::string::npos)
                        {
                            caller = names[up];
                            break;
                        }

                        if (up == depth + 1)
                        {
                            caller = names[up];
                        }
                    }

                    callerCounts[names[depth]][caller] += 1;
                    break;
                }
            }

            // Inclusive counts a function once per stack even if it recurses.
            std::vector<std::string> unique = names;
            std::sort(unique.begin(), unique.end());
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

            for (const auto& name : unique)
            {
                totals[name].inclusive += 1;
            }
        }

        CloseHandle(dspThreadHandle);
    }
    else
    {
        dspThread.join();

        if (dspThreadHandle != nullptr)
        {
            CloseHandle(dspThreadHandle);
        }
    }

    report.functions.assign(totals.begin(), totals.end());
    std::sort(report.functions.begin(), report.functions.end(),
              [](const auto& a, const auto& b) { return a.second.self > b.second.self; });

    for (auto& [callee, callers] : callerCounts)
    {
        CallerBreakdown breakdown;
        breakdown.callee = callee;
        breakdown.callers.assign(callers.begin(), callers.end());

        for (const auto& [name, count] : breakdown.callers)
        {
            breakdown.total += count;
        }

        std::sort(breakdown.callers.begin(), breakdown.callers.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        report.callerBreakdowns.push_back(std::move(breakdown));
    }

    std::sort(report.callerBreakdowns.begin(), report.callerBreakdowns.end(),
              [](const auto& a, const auto& b) { return a.total > b.total; });
#else
    dspThread.join();
#endif

    report.blocksProcessed = static_cast<std::uint64_t>(blockTimesUs.size());

    if (!blockTimesUs.empty())
    {
        double sum = 0.0;

        for (const double value : blockTimesUs)
        {
            sum += value;
        }

        report.avgBlockUs = sum / static_cast<double>(blockTimesUs.size());

        std::vector<double> sorted = blockTimesUs;
        std::sort(sorted.begin(), sorted.end());
        report.p50BlockUs = sorted[sorted.size() / 2];
        report.p99BlockUs = sorted[static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.99)];
    }

    return report;
}

void PrintReport(const std::string& label, const ProfileReport& report, int topCount)
{
    std::cout << "\n=== " << label << " ===\n";
    std::cout << "  blocks processed : " << report.blocksProcessed << "\n";
    std::cout << "  realtime budget  : " << std::fixed << std::setprecision(1) << report.realtimeBudgetUs
              << " us/block\n";
    std::cout << "  avg block        : " << std::fixed << std::setprecision(2) << report.avgBlockUs << " us ("
              << std::setprecision(2) << (report.avgBlockUs / report.realtimeBudgetUs * 100.0) << "% of realtime)\n";
    std::cout << "  p50 / p99 block  : " << std::setprecision(2) << report.p50BlockUs << " / " << report.p99BlockUs
              << " us\n";
    std::cout << "  stack samples    : " << report.sampleCount << "\n";

    if (report.functions.empty() || report.sampleCount == 0)
    {
        return;
    }

    std::cout << "\n  " << std::left << std::setw(9) << "self%" << std::setw(9) << "incl%" << "function\n";
    std::cout << "  " << std::string(78, '-') << "\n";

    const auto total = static_cast<double>(report.sampleCount);
    int shown = 0;

    for (const auto& [name, counts] : report.functions)
    {
        if (shown >= topCount)
        {
            break;
        }

        std::cout << "  " << std::left << std::fixed << std::setprecision(2) << std::setw(9)
                  << (static_cast<double>(counts.self) / total * 100.0) << std::setw(9)
                  << (static_cast<double>(counts.inclusive) / total * 100.0) << name << "\n";
        ++shown;
    }

    if (report.callerBreakdowns.empty())
    {
        return;
    }

    std::cout << "\n  callers:\n";

    for (const auto& breakdown : report.callerBreakdowns)
    {
        std::cout << "    " << breakdown.callee << "  (" << std::fixed << std::setprecision(2)
                  << (static_cast<double>(breakdown.total) / total * 100.0) << "% of samples)\n";

        int callerShown = 0;

        for (const auto& [caller, count] : breakdown.callers)
        {
            if (callerShown >= 6)
            {
                break;
            }

            std::cout << "      " << std::setw(7) << std::fixed << std::setprecision(2)
                      << (static_cast<double>(count) / static_cast<double>(breakdown.total) * 100.0) << "%  " << caller
                      << "\n";
            ++callerShown;
        }
    }
}

void WriteCsv(const std::string& path, const std::string& label, const ProfileReport& report)
{
    const bool exists = fs::exists(path);
    std::ofstream out(path, std::ios::app);

    if (!out)
    {
        std::cerr << "Could not write CSV: " << path << "\n";
        return;
    }

    if (!exists)
    {
        out << "config,function,self_samples,inclusive_samples,total_samples,avg_block_us,realtime_budget_us\n";
    }

    for (const auto& [name, counts] : report.functions)
    {
        out << label << ",\"" << name << "\"," << counts.self << "," << counts.inclusive << "," << report.sampleCount
            << "," << report.avgBlockUs << "," << report.realtimeBudgetUs << "\n";
    }
}

bool ParseArgs(int argc, char* argv[], Settings& settings)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

        if (arg == "--profile")
        {
            settings.profile = next();
        }
        else if (arg == "--block")
        {
            settings.blockSize = std::stoi(next());
        }
        else if (arg == "--sr")
        {
            settings.sampleRate = std::stod(next());
        }
        else if (arg == "--seconds")
        {
            settings.seconds = std::stod(next());
        }
        else if (arg == "--presets")
        {
            settings.presetCount = std::stoi(next());
        }
        else if (arg == "--top")
        {
            settings.topCount = std::stoi(next());
        }
        else if (arg == "--repeats")
        {
            settings.repeats = std::stoi(next());
        }
        else if (arg == "--diagnostics")
        {
            settings.diagnostics = next();
        }
        else if (arg == "--csv")
        {
            settings.csvPath = next();
        }
        else if (arg == "--callers-of")
        {
            settings.callersOf = next();
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "SteadyStateProfiler [--profile light|baseline|namconv|applive] [--block N] [--sr N]\n"
                      << "                    [--seconds S] [--presets N] [--diagnostics on|off|both]\n"
                      << "                    [--top N] [--csv path]\n";
            return false;
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    Settings settings;

    if (!ParseArgs(argc, argv, settings))
    {
        return 0;
    }

#if defined(_WIN32)
    timeBeginPeriod(1);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) == 0)
    {
        std::cerr << "SymInitialize failed (" << GetLastError() << "); names will be unavailable.\n";
    }

#endif

    RegisterAllEffects();

    std::optional<NamConvAssets> namAssets;

    if (settings.profile == "namconv" || settings.profile == "applive")
    {
        namAssets = DiscoverNamConvAssets(fs::current_path());

        if (!namAssets.has_value())
        {
            std::cerr << settings.profile << " profile needs .nam and .wav assets under resources/metal-presets; "
                      << "falling back to baseline.\n";
            settings.profile = "baseline";
        }
        else
        {
            std::cout << "NAM model : " << namAssets->model.filename().string() << "\n"
                      << "IR cab    : " << namAssets->irCab.filename().string() << "\n"
                      << "IR reverb : " << namAssets->reverbIr.filename().string() << "\n";
        }
    }

    std::cout << "profile=" << settings.profile << " sr=" << settings.sampleRate << " block=" << settings.blockSize
              << " presets=" << settings.presetCount << " seconds=" << settings.seconds << "\n";

    std::vector<bool> diagnosticsModes;

    if (settings.diagnostics == "on")
    {
        diagnosticsModes = {true};
    }
    else if (settings.diagnostics == "off")
    {
        diagnosticsModes = {false};
    }
    else
    {
        diagnosticsModes = {true, false};
    }

    std::vector<std::pair<std::string, ProfileReport>> reports;
    std::vector<ProfileReport> best(diagnosticsModes.size());

    // Timing passes, interleaved across configurations so drift in machine state hits
    // every configuration equally rather than only the one that ran last.
    for (int repeat = 0; repeat < std::max(1, settings.repeats); ++repeat)
    {
        for (std::size_t modeIndex = 0; modeIndex < diagnosticsModes.size(); ++modeIndex)
        {
            RunConfig config;
            config.settings = &settings;
            config.diagnosticsEnabled = diagnosticsModes[modeIndex];
            config.sampling = false;

            auto report = RunProfile(config, namAssets);

            if (repeat == 0 || report.avgBlockUs < best[modeIndex].avgBlockUs)
            {
                best[modeIndex] = std::move(report);
            }
        }
    }

    for (std::size_t modeIndex = 0; modeIndex < diagnosticsModes.size(); ++modeIndex)
    {
        const bool diagnostics = diagnosticsModes[modeIndex];
        RunConfig config;
        config.settings = &settings;
        config.diagnosticsEnabled = diagnostics;

        const std::string label =
            settings.profile + "/block" + std::to_string(settings.blockSize) + "/diag-" + (diagnostics ? "on" : "off");

        auto report = std::move(best[modeIndex]);

        // Sampling pass, for the function attribution.
        config.sampling = true;
        auto sampled = RunProfile(config, namAssets);
        report.functions = std::move(sampled.functions);
        report.callerBreakdowns = std::move(sampled.callerBreakdowns);
        report.sampleCount = sampled.sampleCount;

        PrintReport(label, report, settings.topCount);

        if (!settings.csvPath.empty())
        {
            WriteCsv(settings.csvPath, label, report);
        }

        reports.emplace_back(label, std::move(report));
    }

    if (reports.size() == 2 && reports[0].second.avgBlockUs > 0.0)
    {
        const double on = reports[0].second.avgBlockUs;
        const double off = reports[1].second.avgBlockUs;
        std::cout << "\ndiagnostics cost : " << std::fixed << std::setprecision(2) << (on - off) << " us/block ("
                  << std::setprecision(1) << ((on - off) / on * 100.0) << "% of the diag-on block time)\n";
    }

#if defined(_WIN32)
    SymCleanup(GetCurrentProcess());
    timeEndPeriod(1);
#endif

    return 0;
}
