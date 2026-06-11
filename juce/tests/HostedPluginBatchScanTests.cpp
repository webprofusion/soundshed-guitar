// Batch-scans installed plugins (VST3 + LV2) and verifies that the hosted
// plugin effect can load each one, instantiate its editor UI while audio is
// being processed concurrently, then close and release it without crashing.
//
// Pass criteria per plugin:
//   - Load either succeeds, or fails gracefully with a non-empty lastError.
//   - If loaded: editor opens (or the plugin reports it has no editor),
//     concurrent Process() calls run clean, editor closes, effect destructs.
// A crash anywhere aborts the process and fails the ctest run.
//
// Environment overrides:
//   GUITARFX_PLUGIN_SCAN_DIRS  - semicolon-separated directories to scan
//                                (replaces the default locations).
//   GUITARFX_PLUGIN_SCAN_LIMIT - max number of plugins to test.

#include "JuceHostedPluginEffect.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr auto kSkipCode = 77;
constexpr auto kFailCode = 1;
constexpr auto kSampleRate = 48000.0;
constexpr auto kBlockSize = 512;
constexpr auto kEditorPumpMs = 400;

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<fs::path> GetScanRoots()
{
    std::vector<fs::path> roots;

    if (const char* overrideDirs = std::getenv("GUITARFX_PLUGIN_SCAN_DIRS"))
    {
        std::string remaining(overrideDirs);
        size_t pos = 0;
        while ((pos = remaining.find(';')) != std::string::npos)
        {
            if (pos > 0)
                roots.emplace_back(remaining.substr(0, pos));
            remaining.erase(0, pos + 1);
        }
        if (!remaining.empty())
            roots.emplace_back(remaining);
        return roots;
    }

    if (const char* commonFiles = std::getenv("COMMONPROGRAMFILES"))
    {
        roots.emplace_back(fs::path(commonFiles) / "VST3");
        roots.emplace_back(fs::path(commonFiles) / "LV2");
    }
    if (const char* appData = std::getenv("APPDATA"))
        roots.emplace_back(fs::path(appData) / "LV2");

    return roots;
}

bool ShouldSkipCandidate(const fs::path& candidate)
{
    // Avoid hosting our own plugin inside the test (recursive WebView/host setup).
    const std::string name = ToLower(candidate.filename().string());
    return name.find("soundshed") != std::string::npos;
}

// Collects top-level plugin entries: *.vst3 files or bundle directories, and
// *.lv2 bundle directories. Does not descend into bundles.
std::vector<fs::path> CollectPluginCandidates(const std::vector<fs::path>& roots)
{
    std::vector<fs::path> candidates;
    for (const auto& root : roots)
    {
        std::error_code ec;
        if (!fs::exists(root, ec))
            continue;

        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            const fs::path& entry = it->path();
            const std::string ext = ToLower(entry.extension().string());

            const bool isVst3 = ext == ".vst3";
            const bool isLv2Bundle = ext == ".lv2" && it->is_directory(ec);
            if ((isVst3 || isLv2Bundle) && !ShouldSkipCandidate(entry))
                candidates.push_back(entry);
        }
    }

    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

size_t GetScanLimit(size_t available)
{
    if (const char* limitVar = std::getenv("GUITARFX_PLUGIN_SCAN_LIMIT"))
    {
        const long parsed = std::strtol(limitVar, nullptr, 10);
        if (parsed > 0)
            return std::min(available, static_cast<size_t>(parsed));
    }
    return available;
}

void PumpMessageLoop(int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(milliseconds);
}

// Runs Process() in a tight loop on a worker thread, mimicking the audio
// device thread that races editor open/close in the real application.
class AudioPumpThread
{
public:
    explicit AudioPumpThread(guitarfx::JuceHostedPluginEffect& effect)
        : mThread([this, &effect]() { Run(effect); })
    {
    }

    ~AudioPumpThread()
    {
        mStop.store(true, std::memory_order_release);
        if (mThread.joinable())
            mThread.join();
    }

private:
    void Run(guitarfx::JuceHostedPluginEffect& effect)
    {
        std::vector<float> left(kBlockSize, 0.05f);
        std::vector<float> right(kBlockSize, -0.05f);
        std::vector<float> outLeft(kBlockSize, 0.0f);
        std::vector<float> outRight(kBlockSize, 0.0f);
        std::array<float*, 2> inputs { left.data(), right.data() };
        std::array<float*, 2> outputs { outLeft.data(), outRight.data() };

        while (!mStop.load(std::memory_order_acquire))
        {
            effect.Process(inputs.data(), outputs.data(), kBlockSize);
            // ~real-time pacing for a 512-sample block at 48 kHz.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::atomic<bool> mStop { false };
    std::thread mThread;
};

struct ScanResult
{
    int loaded = 0;
    int gracefulLoadFailures = 0;
    int hardFailures = 0;
    std::vector<std::string> failureMessages;
};

void ExercisePlugin(const fs::path& candidate, ScanResult& result)
{
    std::cout << "[SCAN] " << candidate.string() << std::endl;

    guitarfx::JuceHostedPluginEffect effect;
    effect.Prepare(kSampleRate, kBlockSize);

    const bool loadedOk = effect.LoadResource(candidate);
    if (!loadedOk)
    {
        const std::string error = effect.GetConfig("lastError");
        if (error.empty())
        {
            ++result.hardFailures;
            result.failureMessages.push_back(candidate.string() + ": load failed without setting lastError");
            std::cout << "  [FAIL] load failed with no error message" << std::endl;
        }
        else
        {
            ++result.gracefulLoadFailures;
            std::cout << "  [SKIP] graceful load failure: " << error << std::endl;
        }
        return;
    }

    ++result.loaded;
    std::cout << "  loaded: " << effect.GetConfig("pluginName")
              << " (" << effect.GetConfig("pluginFormat") << ")" << std::endl;

    {
        // Race audio against editor open/close, like the device thread does.
        AudioPumpThread audioThread(effect);

        PumpMessageLoop(50);
        effect.SetConfig("showPluginEditor", "1");
        PumpMessageLoop(kEditorPumpMs);

        if (!effect.IsPluginEditorOpenForTesting())
        {
            const std::string error = effect.GetConfig("lastError");
            if (error.empty())
            {
                ++result.hardFailures;
                result.failureMessages.push_back(candidate.string() + ": editor did not open and no error was reported");
                std::cout << "  [FAIL] editor did not open, no error reported" << std::endl;
            }
            else
            {
                std::cout << "  [INFO] editor unavailable: " << error << std::endl;
            }
        }
        else
        {
            std::cout << "  editor opened" << std::endl;
        }

        if (!effect.ClosePluginEditorForTesting())
        {
            ++result.hardFailures;
            result.failureMessages.push_back(candidate.string() + ": failed to close plugin editor");
            std::cout << "  [FAIL] failed to close editor" << std::endl;
        }

        PumpMessageLoop(100);
        // AudioPumpThread keeps processing until it leaves scope here.
    }

    PumpMessageLoop(50);
    std::cout << "  closed cleanly" << std::endl;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto roots = GetScanRoots();
    auto candidates = CollectPluginCandidates(roots);
    if (candidates.empty())
    {
        std::cout << "Skipping hosted plugin batch scan; no plugin candidates found." << std::endl;
        return kSkipCode;
    }

    const size_t limit = GetScanLimit(candidates.size());
    candidates.resize(limit);

    std::cout << "Batch scanning " << candidates.size() << " plugin candidate(s)" << std::endl;

    ScanResult result;
    for (const auto& candidate : candidates)
        ExercisePlugin(candidate, result);

    std::cout << "\nBatch scan summary: loaded=" << result.loaded
              << ", gracefulLoadFailures=" << result.gracefulLoadFailures
              << ", hardFailures=" << result.hardFailures << std::endl;

    for (const auto& message : result.failureMessages)
        std::cerr << "[HostedPluginBatchScanTests] " << message << std::endl;

    if (result.hardFailures > 0)
        return kFailCode;

    if (result.loaded == 0)
    {
        std::cout << "No plugin loaded successfully; treating as skip." << std::endl;
        return kSkipCode;
    }

    std::cout << "Hosted plugin batch scan completed without crashes" << std::endl;
    return 0;
}
