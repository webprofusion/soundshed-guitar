/**
 * PluginController.cpp — Framework-agnostic plugin orchestration.
 *
 * This file contains the shared business logic that was previously
 * duplicated between GuitarFXPlugin.cpp (iPlug2) and PluginProcessor.cpp (JUCE).
 *
 * Implementation strategy:
 *   The handler methods in this file are direct ports from GuitarFXPlugin.cpp
 *   with all framework-specific calls replaced by IPluginHost interface calls.
 *   When moving handler implementations here, the original code from
 *   GuitarFXPlugin.cpp should be used as the canonical source.
 */

#include "PluginController.h"
#include "MessageDispatcher.h"
#include "controller/DemoPreviewService.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathSanitizer.h"
#include "util/Wav.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <utility>
#include <unordered_set>

#include "NAM/get_dsp.h"

namespace
{
    // ── NAM calibration constants ───────────────────────────────────

    constexpr const char* kNamCalibrationFileName = "calibration/models/index.json";
    constexpr double kNamCalibrationDurationSeconds = 1.0;
    constexpr double kNamCalibrationFrequencyHz = 1000.0;
    constexpr double kMinLinear = 1e-6;

    // ── Metronome constants ─────────────────────────────────────────

    constexpr const char* kMetronomeEnabledSettingKey = "metronome.enabled";
    constexpr const char* kMetronomeBpmSettingKey = "metronome.bpm";
    constexpr const char* kMetronomeVolumeDbSettingKey = "metronome.volumeDb";
    constexpr const char* kMetronomePanSettingKey = "metronome.pan";
    constexpr const char* kMetronomeClickTypeSettingKey = "metronome.clickType";
    constexpr const char* kMetronomeClickConfigSettingKey = "metronome.clickConfig";
    constexpr const char* kMetronomeLegacyBpmKey = "metronomeBpm";
    constexpr const char* kMetronomeLegacyVolumeDbKey = "metronomeVolumeDb";
    constexpr const char* kMetronomeLegacyPanKey = "metronomePan";
    constexpr const char* kMetronomeLegacyClickTypeKey = "metronomeClickType";
    constexpr double kMetronomeDefaultBpm = 120.0;
    constexpr double kMetronomeMinBpm = 30.0;
    constexpr double kMetronomeMaxBpm = 300.0;
    constexpr double kMetronomeMinVolumeDb = -60.0;
    constexpr double kMetronomeMaxVolumeDb = 12.0;
    constexpr double kMetronomeDefaultVolumeDb = -12.0;
    constexpr double kMetronomeDefaultPan = 0.0;
    constexpr int kMetronomeBeatsPerBar = 4;
    constexpr const char* kMetronomeDefaultClickType = "click";
    constexpr double kMetronomeClickSeconds = 0.02;
    constexpr double kMetronomeClickFrequencyHz = 1800.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr const char* kRiffLibraryPathSettingKey = "riffLibrary.path";
    constexpr const char* kRiffLibraryDefaultFolder = "riff-library";
    constexpr const char* kRiffLibraryIndexFile = "riff-library-index.json";
    constexpr const char* kSignalDiagnosticsSettingKey = "diagnostics.signalLevelsEnabled";
    constexpr const char* kInterfaceCalibrationEnabledSettingKey = "audio.interfaceCalibration.enabled";
    constexpr const char* kInterfaceCalibrationReferenceDbuSettingKey = "audio.interfaceCalibration.referenceDbu";
    constexpr double kInterfaceCalibrationDefaultReferenceDbu = 12.0;
    constexpr const char* kSessionLogFileName = "logs/session-log.txt";

    double ToDbFS(double linear)
    {
        if (linear <= kMinLinear) return -120.0;
        return 20.0 * std::log10(linear);
    }

    double HeadroomDbFromPeak(double peak)
    {
        const double peakDb = ToDbFS(peak);
        return std::max(0.0, -peakDb);
    }

        std::string FormatTimestamp()
        {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
    #ifdef _WIN32
        localtime_s(&localTime, &tt);
    #else
        localtime_r(&tt, &localTime);
    #endif
        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return oss.str();
        }

    std::optional<guitarfx::PluginController::NamCalibrationData>
    RunNamCalibration(const std::filesystem::path& modelPath,
                      double sampleRate, int blockSize, std::string& error)
    {
        try
        {
            auto model = ::nam::get_dsp(modelPath);
            if (!model) { error = "Failed to load NAM model"; return std::nullopt; }

            blockSize = std::max(64, blockSize);
            model->Reset(sampleRate, blockSize);

            constexpr double kTwoPi = 6.28318530717958647692;
            const int totalSamples = std::max(1, static_cast<int>(sampleRate * kNamCalibrationDurationSeconds));
            std::vector<NAM_SAMPLE> input(static_cast<size_t>(blockSize));
            std::vector<NAM_SAMPLE> output(static_cast<size_t>(blockSize));

            double inputSumSquares = 0.0, outputSumSquares = 0.0, phase = 0.0;
            const double phaseIncrement = (kTwoPi * kNamCalibrationFrequencyHz) / sampleRate;

            int processed = 0;
            while (processed < totalSamples)
            {
                const int frames = std::min(blockSize, totalSamples - processed);
                for (int i = 0; i < frames; ++i)
                {
                    const double sample = std::sin(phase);
                    phase += phaseIncrement;
                    if (phase >= kTwoPi) phase -= kTwoPi;
                    input[static_cast<size_t>(i)] = static_cast<NAM_SAMPLE>(sample);
                    inputSumSquares += sample * sample;
                }

                NAM_SAMPLE* inputPtr = input.data();
                NAM_SAMPLE* outputPtr = output.data();
                NAM_SAMPLE* inputPtrs[1] = { inputPtr };
                NAM_SAMPLE* outputPtrs[1] = { outputPtr };
                model->process(inputPtrs, outputPtrs, frames);

                for (int i = 0; i < frames; ++i)
                {
                    const double out = static_cast<double>(output[static_cast<size_t>(i)]);
                    outputSumSquares += out * out;
                }
                processed += frames;
            }

            if (processed <= 0) { error = "Calibration produced no samples"; return std::nullopt; }

            const double inputRms = std::sqrt(inputSumSquares / static_cast<double>(processed));
            const double outputRms = std::sqrt(outputSumSquares / static_cast<double>(processed));
            if (!std::isfinite(inputRms) || !std::isfinite(outputRms) || outputRms <= kMinLinear)
            {
                error = "Calibration produced invalid RMS";
                return std::nullopt;
            }

            guitarfx::PluginController::NamCalibrationData data;
            data.inputLevelDb = ToDbFS(inputRms);
            data.outputLevelDb = ToDbFS(outputRms);
            return data;
        }
        catch (const std::exception& ex) { error = ex.what(); return std::nullopt; }
        catch (...) { error = "Unknown calibration error"; return std::nullopt; }
    }

    double LinearFromDb(double db)
    {
        if (!std::isfinite(db))
            return 0.0;
        return std::pow(10.0, db / 20.0);
    }

    double ClampValue(double value, double minimum, double maximum)
    {
        return std::min(maximum, std::max(minimum, value));
    }

    bool HasUnsafeRelativeSegments(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
            return false;

        for (const auto& segment : path)
        {
            if (segment == "..")
                return true;
        }

        return false;
    }

    std::filesystem::path ResolveRiffTakePathForRuntime(const std::filesystem::path& storedPath,
                                                        const std::filesystem::path& libraryPath)
    {
        if (storedPath.empty() || storedPath.is_absolute())
            return storedPath;

        if (HasUnsafeRelativeSegments(storedPath))
            return storedPath;

        return (libraryPath / storedPath).lexically_normal();
    }

    std::filesystem::path BuildRiffTakePathForStorage(const std::filesystem::path& runtimePath,
                                                      const std::filesystem::path& libraryPath)
    {
        if (runtimePath.empty())
            return runtimePath;

        std::error_code ec;
        auto normalizedRuntimePath = std::filesystem::weakly_canonical(runtimePath, ec);
        if (ec)
            normalizedRuntimePath = runtimePath.lexically_normal();

        ec.clear();
        auto normalizedLibraryPath = std::filesystem::weakly_canonical(libraryPath, ec);
        if (ec)
            normalizedLibraryPath = libraryPath.lexically_normal();

        const auto relativePath = normalizedRuntimePath.lexically_relative(normalizedLibraryPath);
        if (!relativePath.empty() && !relativePath.is_absolute() && !HasUnsafeRelativeSegments(relativePath))
            return relativePath;

        return runtimePath;
    }

    std::string BuildUtcIsoTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm utcTime{};
#ifdef _WIN32
        gmtime_s(&utcTime, &tt);
#else
        gmtime_r(&tt, &utcTime);
#endif
        std::ostringstream oss;
        oss << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    void WriteUint16LE(std::ofstream& output, std::uint16_t value)
    {
        const std::array<char, 2> bytes{
            static_cast<char>(value & 0xFFu),
            static_cast<char>((value >> 8u) & 0xFFu)
        };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteUint32LE(std::ofstream& output, std::uint32_t value)
    {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFu),
            static_cast<char>((value >> 8u) & 0xFFu),
            static_cast<char>((value >> 16u) & 0xFFu),
            static_cast<char>((value >> 24u) & 0xFFu)
        };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    bool WriteStereo16BitWav(const std::filesystem::path& path,
                             const std::vector<float>& left,
                             const std::vector<float>& right,
                             int sampleRate)
    {
        if (left.empty() || right.empty() || left.size() != right.size() || sampleRate <= 0)
            return false;

        try
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            if (!output)
                return false;

            const std::uint16_t channels = 2;
            const std::uint16_t bitsPerSample = 16;
            const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8));
            const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate) * blockAlign;
            const std::uint32_t frameCount = static_cast<std::uint32_t>(left.size());
            const std::uint32_t dataSize = frameCount * blockAlign;

            output.write("RIFF", 4);
            WriteUint32LE(output, 36u + dataSize);
            output.write("WAVE", 4);

            output.write("fmt ", 4);
            WriteUint32LE(output, 16u);
            WriteUint16LE(output, 1u);
            WriteUint16LE(output, channels);
            WriteUint32LE(output, static_cast<std::uint32_t>(sampleRate));
            WriteUint32LE(output, byteRate);
            WriteUint16LE(output, blockAlign);
            WriteUint16LE(output, bitsPerSample);

            output.write("data", 4);
            WriteUint32LE(output, dataSize);

            for (std::size_t i = 0; i < left.size(); ++i)
            {
                const float clampedL = static_cast<float>(std::clamp(left[i], -1.0f, 1.0f));
                const float clampedR = static_cast<float>(std::clamp(right[i], -1.0f, 1.0f));
                const auto sampleL = static_cast<std::int16_t>(std::round(clampedL * 32767.0f));
                const auto sampleR = static_cast<std::int16_t>(std::round(clampedR * 32767.0f));
                WriteUint16LE(output, static_cast<std::uint16_t>(sampleL));
                WriteUint16LE(output, static_cast<std::uint16_t>(sampleR));
            }

            return static_cast<bool>(output);
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<std::uint8_t> EncodeStereo16BitWav(const std::vector<float>& left,
                                                   const std::vector<float>& right,
                                                   int sampleRate)
    {
        if (left.empty() || right.empty() || left.size() != right.size() || sampleRate <= 0)
            return {};

        std::vector<std::uint8_t> bytes;
        const std::uint16_t channels = 2;
        const std::uint16_t bitsPerSample = 16;
        const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8));
        const std::uint32_t frameCount = static_cast<std::uint32_t>(left.size());
        const std::uint32_t dataSize = frameCount * blockAlign;
        const std::uint32_t totalSize = 44u + dataSize;
        bytes.reserve(static_cast<std::size_t>(totalSize));

        auto pushChars = [&](const char* data, std::size_t count) {
            bytes.insert(bytes.end(), data, data + count);
        };
        auto pushU16 = [&](std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
        };
        auto pushU32 = [&](std::uint32_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
            bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
        };

        pushChars("RIFF", 4);
        pushU32(36u + dataSize);
        pushChars("WAVE", 4);
        pushChars("fmt ", 4);
        pushU32(16u);
        pushU16(1u);
        pushU16(channels);
        pushU32(static_cast<std::uint32_t>(sampleRate));
        pushU32(static_cast<std::uint32_t>(sampleRate) * blockAlign);
        pushU16(blockAlign);
        pushU16(bitsPerSample);
        pushChars("data", 4);
        pushU32(dataSize);

        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const float clampedL = static_cast<float>(std::clamp(left[i], -1.0f, 1.0f));
            const float clampedR = static_cast<float>(std::clamp(right[i], -1.0f, 1.0f));
            const auto sampleL = static_cast<std::int16_t>(std::round(clampedL * 32767.0f));
            const auto sampleR = static_cast<std::int16_t>(std::round(clampedR * 32767.0f));
            pushU16(static_cast<std::uint16_t>(sampleL));
            pushU16(static_cast<std::uint16_t>(sampleR));
        }

        return bytes;
    }

    nlohmann::json BuildWaveformPeaks(const std::vector<float>& left,
                                      const std::vector<float>& right,
                                      std::size_t bins)
    {
        nlohmann::json peaks = nlohmann::json::array();
        if (left.empty() || right.empty() || left.size() != right.size() || bins == 0)
            return peaks;

        const std::size_t totalSamples = left.size();
        const std::size_t binCount = std::min<std::size_t>(bins, totalSamples);

        for (std::size_t b = 0; b < binCount; ++b)
        {
            const std::size_t start = (b * totalSamples) / binCount;
            const std::size_t end = std::max(start + 1, ((b + 1) * totalSamples) / binCount);
            float peak = 0.0f;
            for (std::size_t i = start; i < end && i < totalSamples; ++i)
            {
                const float p = std::max(std::fabs(left[i]), std::fabs(right[i]));
                if (p > peak)
                    peak = p;
            }
            peaks.push_back(static_cast<double>(std::clamp(peak, 0.0f, 1.0f)));
        }

        return peaks;
    }

    // ── Utility helpers ─────────────────────────────────────────────

    // ── Graph utility ───────────────────────────────────────────────

    std::string MakeUniqueNodeId(const guitarfx::SignalGraph& graph, const std::string& baseId)
    {
        std::string candidate = baseId;
        int suffix = 1;
        while (graph.FindNode(candidate)) candidate = baseId + std::to_string(suffix++);
        return candidate;
    }

    bool IsGraphAcyclic(const guitarfx::SignalGraph& graph)
    {
        std::unordered_map<std::string, int> indegree;
        std::unordered_map<std::string, std::vector<std::string>> outgoing;

        for (const auto& node : graph.nodes)
        {
            indegree.emplace(node.id, 0);
        }

        for (const auto& edge : graph.edges)
        {
            indegree.try_emplace(edge.from, 0);
            indegree.try_emplace(edge.to, 0);
            outgoing[edge.from].push_back(edge.to);
            indegree[edge.to] += 1;
        }

        std::deque<std::string> queue;
        for (const auto& [id, count] : indegree)
        {
            if (count == 0)
                queue.push_back(id);
        }

        size_t visited = 0;
        while (!queue.empty())
        {
            const std::string nodeId = queue.front();
            queue.pop_front();
            visited += 1;

            const auto outIt = outgoing.find(nodeId);
            if (outIt == outgoing.end())
                continue;

            for (const auto& nextId : outIt->second)
            {
                auto indegreeIt = indegree.find(nextId);
                if (indegreeIt == indegree.end())
                    continue;
                indegreeIt->second -= 1;
                if (indegreeIt->second == 0)
                    queue.push_back(nextId);
            }
        }

        return visited == indegree.size();
    }

} // anonymous namespace

namespace guitarfx
{

namespace
{
static std::string GenerateGuidV4String()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis(0, 0xFFFFFFFFu);

    std::uint32_t d0 = dis(gen);
    std::uint32_t d1 = dis(gen);
    std::uint32_t d2 = dis(gen);
    std::uint32_t d3 = dis(gen);

    // Set version to 4 (0100)
    d1 = (d1 & 0xFFFF0FFFu) | 0x00004000u;
    // Set variant to 10xx
    d2 = (d2 & 0x3FFFFFFFu) | 0x80000000u;

    auto hex = [](std::uint32_t value, int width) {
        std::ostringstream oss;
        oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    };

    // UUID layout: 8-4-4-4-12
    const std::string part1 = hex(d0, 8);
    const std::string part2 = hex((d1 >> 16) & 0xFFFFu, 4);
    const std::string part3 = hex(d1 & 0xFFFFu, 4);
    const std::string part4 = hex((d2 >> 16) & 0xFFFFu, 4);
    const std::string part5 = hex(d2 & 0xFFFFu, 4) + hex(d3, 8);
    return part1 + "-" + part2 + "-" + part3 + "-" + part4 + "-" + part5;
}

static std::filesystem::path ResolveEffectLayoutsSettingsPath(const FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / "layouts" / "indexes" / "effect-layouts.json";
}

static nlohmann::json LoadEffectLayoutsSettings(const FileSystem& fileSystem)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    root["associations"] = nlohmann::json::object();

    try
    {
        if (path.empty() || !std::filesystem::exists(path))
            return root;

        std::ifstream input(path);
        if (!input)
            return root;

        nlohmann::json parsed;
        input >> parsed;

        if (!parsed.is_object())
            return root;

        if (!parsed.contains("associations") || !parsed["associations"].is_object())
            parsed["associations"] = nlohmann::json::object();

        if (!parsed.contains("version") || !parsed["version"].is_number())
            parsed["version"] = 1;

        return parsed;
    }
    catch (...)
    {
        return root;
    }
}

static void SaveEffectLayoutsSettings(const FileSystem& fileSystem, const nlohmann::json& root)
{
    const auto path = ResolveEffectLayoutsSettingsPath(fileSystem);
    if (path.empty())
        return;

    try
    {
        const auto dir = path.parent_path();
        [[maybe_unused]] const auto ensured = fileSystem.EnsureDirectory(dir);
        std::ofstream output(path);
        if (output)
            output << root.dump(2);
    }
    catch (...) {}
}

static std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId)
{
    const auto settingsDir = fileSystem.ResolveSettingsDirectory();
    const auto layoutsDir = settingsDir / "layouts" / "content";
    const std::string fileStem = util::SanitizeFilename(layoutId);
    return layoutsDir / (fileStem + ".layout.json");
}
}

// ════════════════════════════════════════════════════════════════════
// Construction / Lifecycle
// ════════════════════════════════════════════════════════════════════

PluginController::PluginController(IPluginHost& host)
    : mHost(host)
{
    mParamValues.fill(0.0);
    RegisterAllEffects();
    mDemoPreview = std::make_unique<DemoPreviewService>(
        mHost,
        mPresetMixer,
        mDSPMutex,
        mSignalTestActive,
        [this](const std::string& message, const std::string& detail) { ReportErrorToUI(message, detail); },
        [this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
}

PluginController::~PluginController() = default;

void PluginController::Initialize()
{
    mResourceRoot = mHost.GetUserDataPath();
    mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";

    std::cout << "[Plugin] Initializing. Resource root: " << mResourceRoot.string() << std::endl;

    // Ensure essential directories exist on first launch
    [[maybe_unused]] const auto ensuredResourceRoot = mFileSystem.EnsureDirectory(mResourceRoot);
    [[maybe_unused]] const auto ensuredSettingsRoot = mFileSystem.EnsureDirectory(mFileSystem.ResolveSettingsDirectory());
    [[maybe_unused]] const auto ensuredUserPresets = mFileSystem.EnsureDirectory(mUserPresetsPath);
    [[maybe_unused]] const auto ensuredResources = mFileSystem.EnsureDirectory(mFileSystem.ResolveSettingsDirectory() / "resources");

    mPresetMixer.SetResourceLibrary(&mResourceLibrary);

    LoadAppSettings();
    ApplyMetronomeSettingsFromAppSettings();
    ApplyDiagnosticsSettingsFromAppSettings();
    ApplyInterfaceCalibrationSettingsFromAppSettings();
    ApplyUiSettingsFromAppSettings();
    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadCompositeLibrary();
    LoadLayoutLibrary();
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }
    LoadLastSessionState();
}

void PluginController::Prepare(double sampleRate, int blockSize)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.Prepare(sampleRate, blockSize);

    if (mHost.IsStandalone())
    {
        mMetronomeSamplesUntilClick = 0.0;
        mMetronomeClickSamplesRemaining = 0;
        mMetronomeClickPhase = 0.0;
        mMetronomeBeatIndex = 0;
        mMetronomeClickSamplePosition = 0;
        mMetronomeClickUseHigh = false;
        mMetronomeResetPending.store(true, std::memory_order_release);
        RefreshMetronomeClickSamples(sampleRate);
    }
}

void PluginController::ActivateRiffGuidance(const RiffCaptureConfig& config, bool forPreview)
{
    if (!mHost.IsStandalone())
        return;

    mRiffGuidanceActive = true;
    mRiffGuidanceForPreview = forPreview;
    mRiffGuidanceBpm = ClampValue(config.tempoBpm > 0.0 ? config.tempoBpm : GetEffectiveTempoBpm(),
                                  kMetronomeMinBpm,
                                  kMetronomeMaxBpm);
    mRiffGuidanceBeatsPerBar = std::max(1, config.timeSigNum);
    mRiffGuidanceBeatScale = 4.0 / static_cast<double>(std::max(1, config.timeSigDen));

    const std::string clickType = config.patternType.empty() ? std::string{kMetronomeDefaultClickType} : config.patternType;
    const auto* clickConfig = FindMetronomeClickType(clickType);
    const double sampleRate = mHost.GetSampleRate();
    if (clickConfig && sampleRate > 0.0)
        mRiffGuidanceClickSamples = BuildMetronomeClickSamples(*clickConfig, sampleRate);
    else
        mRiffGuidanceClickSamples.reset();

    if (!mRiffGuidanceClickSamples)
        mRiffGuidanceClickSamples = std::atomic_load_explicit(&mMetronomeClickSamples, std::memory_order_acquire);

    mMetronomeResetPending.store(true, std::memory_order_release);
}

void PluginController::DeactivateRiffGuidance(bool previewOnly)
{
    if (previewOnly && !mRiffGuidanceForPreview)
        return;

    mRiffGuidanceActive = false;
    mRiffGuidanceForPreview = false;
    mRiffGuidanceBeatScale = 1.0;
    mRiffGuidanceClickSamples.reset();
    mMetronomeResetPending.store(true, std::memory_order_release);
}

void PluginController::Reset()
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.Reset();
    mMetronomeResetPending.store(true, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════
// Audio processing
// ════════════════════════════════════════════════════════════════════

bool PluginController::ProcessAudio(float** inputs, float** outputs, int numSamples)
{
    // Try to acquire the DSP lock without blocking the audio thread.
    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false; // Caller should output silence

    // ARM mode: click is playing, waiting for input signal to trigger recording
    if (mRiffCapture.armed && !mRiffCapture.active && !mRiffCapture.complete)
    {
        const bool hasInput = (inputs && inputs[0] && inputs[1]);
        if (!mRiffCapture.armCountInComplete)
        {
            // Track count-in progress
            mRiffCapture.armCountInIndex += static_cast<std::size_t>(numSamples);
            if (mRiffCapture.armCountInIndex >= mRiffCapture.countInSamples)
                mRiffCapture.armCountInComplete = true;
        }
        else if (hasInput)
        {
            // Count-in done; watch for input signal above threshold
            for (int i = 0; i < numSamples; ++i)
            {
                const float level = std::max(std::abs(inputs[0][i]), std::abs(inputs[1][i]));
                if (level >= mRiffCapture.armThreshold)
                {
                    // Trigger: start recording immediately, skipping count-in
                    mRiffCapture.armed = false;
                    mRiffCapture.active = true;
                    mRiffCapture.writeIndex = mRiffCapture.countInSamples; // already past count-in
                    mRiffCapture.startedAt = std::chrono::steady_clock::now();
                    nlohmann::json startMsg;
                    startMsg["type"] = "riffCaptureStarted";
                    startMsg["takeId"] = mRiffCapture.takeId;
                    startMsg["bars"] = mRiffCapture.config.bars;
                    startMsg["tempoBpm"] = mRiffCapture.config.tempoBpm;
                    startMsg["timeSigNum"] = mRiffCapture.config.timeSigNum;
                    startMsg["timeSigDen"] = mRiffCapture.config.timeSigDen;
                    startMsg["countInBars"] = 0;
                    SendMessageToUI(startMsg.dump());
                    break;
                }
            }
        }
    }

    if (mRiffCapture.active && !mRiffCapture.complete)
    {
        const std::size_t channelsReady = (inputs && inputs[0] && inputs[1]) ? 2u : 0u;
        if (channelsReady == 2u && mRiffCapture.writeIndex < mRiffCapture.targetSamples)
        {
            const std::size_t countInSamples = mRiffCapture.countInSamples;
            const std::size_t bucketSize = std::max<std::size_t>(1, mRiffCapture.livePeakBucketSize);
            for (int i = 0; i < numSamples && mRiffCapture.writeIndex < mRiffCapture.targetSamples; ++i)
            {
                if (mRiffCapture.writeIndex >= countInSamples)
                {
                    const std::size_t captureIndex = mRiffCapture.writeIndex - countInSamples;
                    if (captureIndex < mRiffCapture.left.size() && captureIndex < mRiffCapture.right.size())
                    {
                        mRiffCapture.left[captureIndex] = inputs[0][i];
                        mRiffCapture.right[captureIndex] = inputs[1][i];
                        // Update live waveform peak bucket
                        const float peakVal = std::max(std::abs(inputs[0][i]), std::abs(inputs[1][i]));
                        const std::size_t bucket = captureIndex / bucketSize;
                        if (bucket < mRiffCapture.livePeaks.size())
                            mRiffCapture.livePeaks[bucket] = std::max(mRiffCapture.livePeaks[bucket], peakVal);
                    }
                }
                ++mRiffCapture.writeIndex;
            }

            // Send live progress every ~250 ms
            const std::size_t capturedSoFar = mRiffCapture.writeIndex > countInSamples
                ? mRiffCapture.writeIndex - countInSamples : 0;
            const std::size_t progressInterval = std::max<std::size_t>(1,
                static_cast<std::size_t>(mRiffCapture.sampleRate * 0.25));
            if (capturedSoFar > 0 && capturedSoFar >= mRiffCapture.lastProgressSample + progressInterval)
            {
                mRiffCapture.lastProgressSample = capturedSoFar;
                nlohmann::json progressMsg;
                progressMsg["type"] = "riffCaptureProgress";
                progressMsg["capturedSamples"] = capturedSoFar;
                progressMsg["waveformPeaks"] = mRiffCapture.livePeaks;
                SendMessageToUI(progressMsg.dump());
            }

            if (mRiffCapture.writeIndex >= mRiffCapture.targetSamples)
            {
                const std::size_t capturedFinal = mRiffCapture.left.size();
                mRiffCapture.complete = true;
                mRiffCapture.active = false;
                mRiffCapture.endedAt = std::chrono::steady_clock::now();
                DeactivateRiffGuidance(false);
                const double samplesPerBeat = mRiffCapture.sampleRate
                    * (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm))
                    * (4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen)));
                const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
                const int computedBars = std::max(1, static_cast<int>(
                    std::round(static_cast<double>(capturedFinal) / std::max(1.0, samplesPerBar))));
                nlohmann::json msg;
                msg["type"] = "riffCaptureStopped";
                msg["takeId"] = mRiffCapture.takeId;
                msg["bars"] = computedBars;
                msg["capturedSamples"] = capturedFinal;
                msg["sampleRate"] = mRiffCapture.sampleRate;
                msg["hasAudio"] = capturedFinal > 0;
                msg["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
                SendMessageToUI(msg.dump());
            }
        }
    }

    // Mix in demo audio preview if active
    if (mDemoPreview)
        mDemoPreview->MixIntoInput(inputs, numSamples);

    if (mRiffGuidanceForPreview && (!mDemoPreview || !mDemoPreview->IsPreviewActive()))
    {
        DeactivateRiffGuidance(true);
    }

    // Signal path test tone injection
    if (mSignalTestActive.load(std::memory_order_acquire))
    {
        auto& st = mSignalTestState;
        if (inputs && inputs[0] && inputs[1])
        {
            for (int i = 0; i < numSamples && st.samplesRemaining > 0; ++i, --st.samplesRemaining)
            {
                float sample = static_cast<float>(std::sin(st.phase * 2.0 * 3.14159265358979323846));
                st.phase += st.phaseIncrement;
                if (st.phase >= 1.0) st.phase -= 1.0;
                inputs[0][i] = sample;
                inputs[1][i] = sample;
                st.inputSumSquares += static_cast<double>(sample) * sample;
            }
        }
        if (st.samplesRemaining <= 0)
        {
            mSignalTestActive.store(false, std::memory_order_release);
            mSignalTestResultPending.store(true, std::memory_order_release);
        }
    }

    // Main DSP processing
    mPresetMixer.Process(inputs, outputs, numSamples);

    // Add metronome click on top of processed audio (standalone only)
    RenderMetronome(outputs, numSamples);

    // Collect signal test output
    if (mSignalTestState.samplesRemaining > 0 || mSignalTestResultPending.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (outputs && outputs[0])
                mSignalTestState.outputSumSquares[0] += static_cast<double>(outputs[0][i]) * outputs[0][i];
            if (outputs && outputs[1])
                mSignalTestState.outputSumSquares[1] += static_cast<double>(outputs[1][i]) * outputs[1][i];
        }
    }

    return true;
}

double PluginController::GetEffectiveTempoBpm() const
{
    if (mHost.IsStandalone())
        return ClampValue(mMetronomeBpm.load(std::memory_order_relaxed), kMetronomeMinBpm, kMetronomeMaxBpm);

    const double hostTempo = mHost.GetHostTempo();
    if (hostTempo > 0.0)
        return ClampValue(hostTempo, kMetronomeMinBpm, kMetronomeMaxBpm);

    return kMetronomeDefaultBpm;
}

void PluginController::RenderMetronome(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1])
        return;

    if (!mHost.IsStandalone())
        return;

    const bool riffGuidanceActive = mRiffGuidanceActive;
    if (!riffGuidanceActive && !mMetronomeEnabled.load(std::memory_order_relaxed))
        return;

    if (mMetronomeResetPending.exchange(false, std::memory_order_acq_rel))
    {
        mMetronomeSamplesUntilClick = 0.0;
        mMetronomeClickSamplesRemaining = 0;
        mMetronomeClickPhase = 0.0;
        mMetronomeBeatIndex = 0;
        mMetronomeClickSamplePosition = 0;
        mMetronomeClickUseHigh = false;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
        return;

    const double bpm = riffGuidanceActive
        ? ClampValue(mRiffGuidanceBpm, kMetronomeMinBpm, kMetronomeMaxBpm)
        : GetEffectiveTempoBpm();
    const int beatsPerBar = std::max(1, riffGuidanceActive ? mRiffGuidanceBeatsPerBar : kMetronomeBeatsPerBar);
    const double beatScale = riffGuidanceActive ? std::max(0.125, mRiffGuidanceBeatScale) : 1.0;
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, bpm)) * beatScale;
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * kMetronomeClickSeconds));
    mMetronomeClickPhaseIncrement = kTwoPi * kMetronomeClickFrequencyHz / sampleRate;

    const double volumeDb = ClampValue(mMetronomeVolumeDb.load(std::memory_order_relaxed),
                                       kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    const double volume = ClampValue(LinearFromDb(volumeDb), 0.0, LinearFromDb(kMetronomeMaxVolumeDb));
    const double pan = ClampValue(mMetronomePan.load(std::memory_order_relaxed), -1.0, 1.0);
    const double panAngle = (pan + 1.0) * (kTwoPi / 8.0);
    const double panLeft = std::cos(panAngle);
    const double panRight = std::sin(panAngle);

    const auto clickSampleSet = riffGuidanceActive
        ? mRiffGuidanceClickSamples
        : std::atomic_load_explicit(&mMetronomeClickSamples, std::memory_order_acquire);
    const bool hasSampleClick = clickSampleSet
        && ((!clickSampleSet->low.empty() && !clickSampleSet->low.front().empty())
            || (!clickSampleSet->high.empty() && !clickSampleSet->high.front().empty()));

    for (int frame = 0; frame < numSamples; ++frame)
    {
        if (mMetronomeSamplesUntilClick <= 0.0)
        {
            if (hasSampleClick)
            {
                const bool useHigh = (mMetronomeBeatIndex % beatsPerBar) == 0;
                const auto& preferred = useHigh ? clickSampleSet->high : clickSampleSet->low;
                const auto& fallback = useHigh ? clickSampleSet->low : clickSampleSet->high;
                const auto& selected = (!preferred.empty() && !preferred.front().empty()) ? preferred : fallback;
                mMetronomeClickSamplesRemaining = selected.empty() ? 0 : static_cast<int>(selected.front().size());
                mMetronomeClickSamplePosition = 0;
                mMetronomeClickUseHigh = useHigh;
                mMetronomeBeatIndex = (mMetronomeBeatIndex + 1) % beatsPerBar;
            }
            else
            {
                mMetronomeClickSamplesRemaining = clickSamples;
            }
            mMetronomeSamplesUntilClick += samplesPerBeat;
        }

        float clickSampleL = 0.0f;
        float clickSampleR = 0.0f;
        if (mMetronomeClickSamplesRemaining > 0)
        {
            if (hasSampleClick)
            {
                const auto& preferred = mMetronomeClickUseHigh ? clickSampleSet->high : clickSampleSet->low;
                const auto& fallback = mMetronomeClickUseHigh ? clickSampleSet->low : clickSampleSet->high;
                const auto& selected = (!preferred.empty() && !preferred.front().empty()) ? preferred : fallback;
                if (!selected.empty() && !selected.front().empty())
                {
                    const int index = mMetronomeClickSamplePosition;
                    if (index >= 0 && static_cast<std::size_t>(index) < selected.front().size())
                    {
                        clickSampleL = selected[0][static_cast<std::size_t>(index)];
                        clickSampleR = selected.size() > 1
                            ? selected[1][static_cast<std::size_t>(index)]
                            : clickSampleL;
                    }
                }
                ++mMetronomeClickSamplePosition;
                --mMetronomeClickSamplesRemaining;
            }
            else
            {
                const double envelope = static_cast<double>(mMetronomeClickSamplesRemaining) / static_cast<double>(clickSamples);
                const float clickSample = static_cast<float>(std::sin(mMetronomeClickPhase) * envelope);
                clickSampleL = clickSample;
                clickSampleR = clickSample;
                mMetronomeClickPhase += mMetronomeClickPhaseIncrement;
                if (mMetronomeClickPhase >= kTwoPi)
                    mMetronomeClickPhase -= kTwoPi;
                --mMetronomeClickSamplesRemaining;
            }
        }

        outputs[0][frame] += clickSampleL * static_cast<float>(volume * panLeft);
        outputs[1][frame] += clickSampleR * static_cast<float>(volume * panRight);
        mMetronomeSamplesUntilClick -= 1.0;
    }
}

void PluginController::ApplyMetronomeSettingsFromAppSettings()
{
    if (!mHost.IsStandalone())
        return;

    auto readNumber = [&](const char* primary, const char* legacy, double fallback, double minVal, double maxVal) {
        if (mAppSettings.contains(primary) && mAppSettings[primary].is_number())
            return ClampValue(mAppSettings[primary].get<double>(), minVal, maxVal);
        if (mAppSettings.contains(legacy) && mAppSettings[legacy].is_number())
            return ClampValue(mAppSettings[legacy].get<double>(), minVal, maxVal);
        return ClampValue(fallback, minVal, maxVal);
    };

    const double bpm = readNumber(kMetronomeBpmSettingKey, kMetronomeLegacyBpmKey,
                                  kMetronomeDefaultBpm, kMetronomeMinBpm, kMetronomeMaxBpm);
    mMetronomeBpm.store(bpm, std::memory_order_release);
    mAppSettings[kMetronomeBpmSettingKey] = bpm;

    mMetronomeEnabled.store(false, std::memory_order_release);
    if (mAppSettings.contains(kMetronomeEnabledSettingKey))
        mAppSettings.erase(kMetronomeEnabledSettingKey);

    const double volumeDb = readNumber(kMetronomeVolumeDbSettingKey, kMetronomeLegacyVolumeDbKey,
                                       kMetronomeDefaultVolumeDb, kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    mMetronomeVolumeDb.store(volumeDb, std::memory_order_release);
    mAppSettings[kMetronomeVolumeDbSettingKey] = volumeDb;

    const double pan = readNumber(kMetronomePanSettingKey, kMetronomeLegacyPanKey,
                                  kMetronomeDefaultPan, -1.0, 1.0);
    mMetronomePan.store(pan, std::memory_order_release);
    mAppSettings[kMetronomePanSettingKey] = pan;

    std::string clickType = kMetronomeDefaultClickType;
    if (mAppSettings.contains(kMetronomeClickTypeSettingKey) && mAppSettings[kMetronomeClickTypeSettingKey].is_string())
        clickType = mAppSettings[kMetronomeClickTypeSettingKey].get<std::string>();
    else if (mAppSettings.contains(kMetronomeLegacyClickTypeKey) && mAppSettings[kMetronomeLegacyClickTypeKey].is_string())
        clickType = mAppSettings[kMetronomeLegacyClickTypeKey].get<std::string>();

    if (!clickType.empty())
        mMetronomeClickType = clickType;
    mAppSettings[kMetronomeClickTypeSettingKey] = mMetronomeClickType;

    UpdateMetronomeClickConfigFromSettings();
    RefreshMetronomeClickSamples(mHost.GetSampleRate());
}

void PluginController::UpdateMetronomeClickConfigFromSettings()
{
    mMetronomeClickConfig.clear();

    auto resolveClickPath = [this](const std::string& rawPath) -> std::filesystem::path {
        if (rawPath.empty())
            return {};

        std::filesystem::path path{rawPath};
        if (path.is_absolute())
            return path;

        std::error_code ec;
        const auto assetsRoot = mHost.GetBundledAssetsPath();
        if (!assetsRoot.empty())
        {
            const auto candidateUi = assetsRoot / "ui" / path;
            if (std::filesystem::exists(candidateUi, ec))
                return candidateUi;
            const auto candidateRoot = assetsRoot / path;
            if (std::filesystem::exists(candidateRoot, ec))
                return candidateRoot;
        }

        if (!mResourceRoot.empty())
        {
            const auto candidateUi = mResourceRoot / "ui" / path;
            if (std::filesystem::exists(candidateUi, ec))
                return candidateUi;
            const auto candidateRoot = mResourceRoot / path;
            if (std::filesystem::exists(candidateRoot, ec))
                return candidateRoot;
        }

        return path;
    };

    const auto configIt = mAppSettings.find(kMetronomeClickConfigSettingKey);
    bool hasValidConfig = false;
    if (configIt != mAppSettings.end() && configIt->is_array())
    {
        for (const auto& entry : *configIt)
        {
            if (!entry.is_object())
                continue;

            const std::string id = entry.value("id", "");
            if (id.empty())
                continue;

            MetronomeClickTypeConfig config;
            config.id = id;
            config.label = entry.value("label", id);
            const std::string lowPath = entry.value("lowPath", "");
            const std::string highPath = entry.value("highPath", "");
            if (!lowPath.empty())
                config.lowPath = resolveClickPath(lowPath);
            if (!highPath.empty())
                config.highPath = resolveClickPath(highPath);

            std::error_code ec;
            const bool lowExists = !config.lowPath.empty() && std::filesystem::exists(config.lowPath, ec);
            const bool highExists = !config.highPath.empty() && std::filesystem::exists(config.highPath, ec);
            if (!lowExists && !highExists)
                continue;

            mMetronomeClickConfig.push_back(std::move(config));
            hasValidConfig = true;
        }
    }

    if (!hasValidConfig)
    {
        const std::array<std::tuple<std::string, std::string, std::string, std::string>, 3> defaults = {
            
            std::make_tuple(std::string{"drum"}, std::string{"Drum"}, std::string{"metronome/kit1/low.wav"}, std::string{"metronome/kit1/high.wav"}),
            //std::make_tuple(std::string{"click"}, std::string{"Click"}, std::string{"metronome/click/Low.wav"}, std::string{"metronome/click/High.wav"}),
            //std::make_tuple(std::string{"electronic"}, std::string{"Electronic"}, std::string{"metronome/digital/Low.wav"}, std::string{"metronome/digital/High.wav"})
        };

        nlohmann::json defaultConfig = nlohmann::json::array();
        for (const auto& entry : defaults)
        {
            const auto& id = std::get<0>(entry);
            const auto& label = std::get<1>(entry);
            const auto& lowPath = std::get<2>(entry);
            const auto& highPath = std::get<3>(entry);
            MetronomeClickTypeConfig config;
            config.id = id;
            config.label = label;
            config.lowPath = resolveClickPath(lowPath);
            config.highPath = resolveClickPath(highPath);
            mMetronomeClickConfig.push_back(config);

            nlohmann::json defaultEntry;
            defaultEntry["id"] = id;
            defaultEntry["label"] = label;
            defaultEntry["lowPath"] = lowPath;
            defaultEntry["highPath"] = highPath;
            defaultConfig.push_back(std::move(defaultEntry));
        }

        mAppSettings[kMetronomeClickConfigSettingKey] = std::move(defaultConfig);
    }

    if (mMetronomeClickConfig.empty())
        return;

    if (mMetronomeClickType.empty())
        mMetronomeClickType = mMetronomeClickConfig.front().id;
}

const PluginController::MetronomeClickTypeConfig*
PluginController::FindMetronomeClickType(const std::string& id) const
{
    for (const auto& config : mMetronomeClickConfig)
    {
        if (config.id == id)
            return &config;
    }
    return mMetronomeClickConfig.empty() ? nullptr : &mMetronomeClickConfig.front();
}

std::shared_ptr<PluginController::MetronomeClickSamples>
PluginController::BuildMetronomeClickSamples(const MetronomeClickTypeConfig& config, double targetSampleRate) const
{
    if (targetSampleRate <= 0.0)
        return nullptr;

    auto samples = std::make_shared<MetronomeClickSamples>();

    auto loadWav = [&](const std::filesystem::path& path, std::vector<std::vector<float>>& target, std::string_view label)
    {
        if (path.empty())
            return;
        if (!std::filesystem::exists(path))
        {
            std::cerr << "[Plugin] Metronome " << label << " sample not found: " << path.generic_string() << std::endl;
            return;
        }

        const auto bytes = util::ReadFileBytes(path);
        if (bytes.empty())
        {
            std::cerr << "[Plugin] Metronome " << label << " sample empty: " << path.generic_string() << std::endl;
            return;
        }

        const auto wavData = util::DecodePcmWav(bytes);
        if (!wavData)
        {
            std::cerr << "[Plugin] Metronome " << label << " sample unsupported WAV: " << path.generic_string() << std::endl;
            return;
        }

        auto resampled = util::ConvertToSampleRate(*wavData, targetSampleRate);
        if (resampled.empty() || resampled.front().empty())
        {
            std::cerr << "[Plugin] Metronome " << label << " sample empty after resample: " << path.generic_string() << std::endl;
            return;
        }

        std::size_t minFrames = resampled.front().size();
        for (const auto& channel : resampled)
        {
            if (channel.empty())
                return;
            minFrames = std::min(minFrames, channel.size());
        }
        for (auto& channel : resampled)
        {
            if (channel.size() > minFrames)
                channel.resize(minFrames);
        }

        target = std::move(resampled);
    };

    loadWav(config.lowPath, samples->low, "low");
    loadWav(config.highPath, samples->high, "high");

    if (samples->low.empty() && samples->high.empty())
        return nullptr;

    return samples;
}

void PluginController::RefreshMetronomeClickSamples(double sampleRate)
{
    if (!mHost.IsStandalone())
        return;

    if (mMetronomeClickConfig.empty())
        UpdateMetronomeClickConfigFromSettings();

    if (sampleRate <= 0.0)
        return;

    const auto* config = FindMetronomeClickType(mMetronomeClickType);
    if (!config)
    {
        std::atomic_store_explicit(&mMetronomeClickSamples, std::shared_ptr<MetronomeClickSamples>{}, std::memory_order_release);
        return;
    }

    if (config->id != mMetronomeClickType)
    {
        mMetronomeClickType = config->id;
        mAppSettings[kMetronomeClickTypeSettingKey] = mMetronomeClickType;
    }

    auto samples = BuildMetronomeClickSamples(*config, sampleRate);
    std::atomic_store_explicit(&mMetronomeClickSamples, std::move(samples), std::memory_order_release);
}

void PluginController::ApplyDiagnosticsSettingsFromAppSettings()
{
    const auto it = mAppSettings.find(kSignalDiagnosticsSettingKey);
    if (it == mAppSettings.end())
        return;

    bool enabled = false;
    if (it->is_boolean())
        enabled = it->get<bool>();
    else if (it->is_number())
        enabled = it->get<double>() != 0.0;

    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
}

void PluginController::ApplyInterfaceCalibrationSettingsFromAppSettings()
{
    bool enabled = true;
    double referenceDbu = kInterfaceCalibrationDefaultReferenceDbu;

    const auto enabledIt = mAppSettings.find(kInterfaceCalibrationEnabledSettingKey);
    if (enabledIt != mAppSettings.end())
    {
        if (enabledIt->is_boolean())
            enabled = enabledIt->get<bool>();
        else if (enabledIt->is_number())
            enabled = enabledIt->get<double>() != 0.0;
    }

    const auto referenceIt = mAppSettings.find(kInterfaceCalibrationReferenceDbuSettingKey);
    if (referenceIt != mAppSettings.end() && referenceIt->is_number())
        referenceDbu = referenceIt->get<double>();

    mPresetMixer.SetNamInterfaceCalibration(enabled, referenceDbu);
}

void PluginController::ApplyUiSettingsFromAppSettings()
{
    mUiSettings = nlohmann::json::object();

    const auto it = mAppSettings.find("uiSettings");
    if (it != mAppSettings.end() && it->is_object())
    {
        mUiSettings = *it;
        return;
    }

    bool hasLegacy = false;
    nlohmann::json legacy = nlohmann::json::object();
    const auto zoomIt = mAppSettings.find("uiZoom");
    if (zoomIt != mAppSettings.end())
    {
        legacy["zoom"] = *zoomIt;
        hasLegacy = true;
    }
    const auto boundsIt = mAppSettings.find("uiBounds");
    if (boundsIt != mAppSettings.end())
    {
        legacy["bounds"] = *boundsIt;
        hasLegacy = true;
    }

    if (hasLegacy)
        mUiSettings = legacy;
}

// ════════════════════════════════════════════════════════════════════
// State serialization
// ════════════════════════════════════════════════════════════════════

std::string PluginController::SerializeState() const
{
    nlohmann::json state = nlohmann::json::object();
    state["version"] = 1;
    if (mActivePreset)
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(*mActivePreset));
    state["presetId"] = mActivePresetId;
    state["appSettings"] = mAppSettings;
    state["uiSettings"] = mUiSettings;
    state["uiViewState"] = mUiViewState;
    state["globalSignalChain"] = mPresetMixer.GetGlobalChainConfig();

    nlohmann::json params = nlohmann::json::array();
    for (const auto value : mParamValues)
        params.push_back(value);
    state["parameters"] = params;

    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();

    nlohmann::json activePresetIds = nlohmann::json::array();
    nlohmann::json presetConfigs = nlohmann::json::object();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        activePresetIds.push_back(id);
        if (const auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            presetConfigs[id] = {
                {"name", cfg->name},
                {"mix", cfg->mix},
                {"pan", cfg->pan},
                {"mute", cfg->mute},
                {"solo", cfg->solo}
            };
        }
    }
    mixer["activePresetIds"] = std::move(activePresetIds);
    mixer["presets"] = std::move(presetConfigs);
    state["mixer"] = std::move(mixer);

    return state.dump();
}

void PluginController::DeserializeState(const std::string& json)
{
    try
    {
        auto state = nlohmann::json::parse(json);
        const nlohmann::json* incomingSettings = nullptr;
        if (state.contains("appSettings") && state["appSettings"].is_object())
            incomingSettings = &state["appSettings"];
        else if (state.contains("settings") && state["settings"].is_object())
            incomingSettings = &state["settings"];

        if (incomingSettings != nullptr)
        {
            if (!mAppSettings.is_object())
                mAppSettings = nlohmann::json::object();

            for (auto it = incomingSettings->begin(); it != incomingSettings->end(); ++it)
                mAppSettings[it.key()] = it.value();
        }

        if (state.contains("uiSettings") && state["uiSettings"].is_object())
            mUiSettings = state["uiSettings"];
        else
            ApplyUiSettingsFromAppSettings();

        if (state.contains("uiViewState") && state["uiViewState"].is_object())
            mUiViewState = state["uiViewState"];

        if (state.contains("globalSignalChain") && state["globalSignalChain"].is_object())
        {
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.SetGlobalChainConfig(state["globalSignalChain"].get<GlobalSignalChainConfig>());
        }
        if (state.contains("preset"))
        {
            auto presetOpt = PresetStorage::DeserializeFromJson(state["preset"].dump());
            if (presetOpt)
            {
                mActivePresetId = state.value("presetId", presetOpt->id);
                mActivePreset = *presetOpt;
                mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
                ApplyPreset(*presetOpt);
            }
        }

        if (state.contains("parameters") && state["parameters"].is_array())
        {
            int idx = 0;
            for (const auto& value : state["parameters"])
            {
                if (idx >= kParamCount) break;
                if (value.is_number())
                    OnParamChange(idx, value.get<double>());
                idx++;
            }
        }

        if (state.contains("mixer") && state["mixer"].is_object())
        {
            const auto& mixer = state["mixer"];
            if (mixer.contains("masterGain") && mixer["masterGain"].is_number())
                mPresetMixer.SetMasterGain(mixer["masterGain"].get<double>());
            if (mixer.contains("limiterEnabled") && mixer["limiterEnabled"].is_boolean())
                mPresetMixer.SetLimiterEnabled(mixer["limiterEnabled"].get<bool>());

            // Reset active presets before restoring mixer state
            for (const auto& id : mPresetMixer.GetActivePresetIds())
                mPresetMixer.RemoveActivePreset(id);

            std::vector<std::string> activeIds;
            if (mixer.contains("activePresetIds") && mixer["activePresetIds"].is_array())
            {
                for (const auto& entry : mixer["activePresetIds"])
                {
                    if (entry.is_string())
                        activeIds.push_back(entry.get<std::string>());
                }
            }
            const auto presets = mixer.contains("presets") ? mixer["presets"] : nlohmann::json::object();
            if (activeIds.empty() && presets.is_object())
            {
                for (const auto& [id, _] : presets.items())
                    activeIds.push_back(id);
            }

            for (const auto& id : activeIds)
            {
                const auto presetEntry = presets.is_object() && presets.contains(id) ? presets[id] : nlohmann::json::object();
                const std::string name = presetEntry.value("name", id);

                bool added = false;
                if (mActivePreset && (id == "p1" || id == mActivePresetId))
                {
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);
                }
                if (!added)
                {
                    added = AddActivePresetById(id);
                }
                if (!added && mActivePreset)
                {
                    (void)mPresetMixer.AddActivePreset(*mActivePreset, id, name);
                }

                if (presetEntry.is_object())
                {
                    if (presetEntry.contains("mix") && presetEntry["mix"].is_number())
                        mPresetMixer.SetPresetMix(id, presetEntry["mix"].get<double>());
                    if (presetEntry.contains("pan") && presetEntry["pan"].is_number())
                        mPresetMixer.SetPresetPan(id, presetEntry["pan"].get<double>());
                    if (presetEntry.contains("mute") && presetEntry["mute"].is_boolean())
                        mPresetMixer.SetPresetMute(id, presetEntry["mute"].get<bool>());
                    if (presetEntry.contains("solo") && presetEntry["solo"].is_boolean())
                        mPresetMixer.SetPresetSolo(id, presetEntry["solo"].get<bool>());
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // Ignore malformed state
    }

    mPendingStateBroadcast = true;
}

// ════════════════════════════════════════════════════════════════════
// UI message entry point
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleUIMessage(const std::string& jsonMessage)
{
    // Delegate to the MessageDispatcher which routes by message type.
    MessageDispatcher::Dispatch(*this, jsonMessage);
}

// ════════════════════════════════════════════════════════════════════
// Idle processing
// ════════════════════════════════════════════════════════════════════

void PluginController::OnIdle()
{
    // Broadcast pending state
    if (mPendingStateBroadcast)
    {
        mPendingStateBroadcast = false;
        BroadcastState();
    }

    // Process NAM calibration results
    ProcessNamCalibrationQueue();

    if (mNamCalibrationFuture && mNamCalibrationFuture->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const auto result = mNamCalibrationFuture->get();
        mNamCalibrationFuture.reset();
        mNamCalibrationActiveJob.reset();
        ApplyNamCalibrationResult(result);
        ProcessNamCalibrationQueue();
    }

    // Signal test result
    if (mSignalTestResultPending.load(std::memory_order_acquire))
    {
        mSignalTestResultPending.store(false, std::memory_order_release);
        auto& st = mSignalTestState;
        auto elapsed = std::chrono::steady_clock::now() - st.startTime;
        mSignalTestResult.elapsedSeconds = std::chrono::duration<double>(elapsed).count();
        mSignalTestResult.sampleRate = st.sampleRate;
        mSignalTestResult.frequencyHz = st.frequencyHz;
        mSignalTestResult.durationSeconds = static_cast<double>(st.totalSamples) / st.sampleRate;
        int total = st.totalSamples;
        mSignalTestResult.inputRMS = (total > 0) ? std::sqrt(st.inputSumSquares / total) : 0.0;
        mSignalTestResult.outputRMS[0] = (total > 0) ? std::sqrt(st.outputSumSquares[0] / total) : 0.0;
        mSignalTestResult.outputRMS[1] = (total > 0) ? std::sqrt(st.outputSumSquares[1] / total) : 0.0;
        mSignalTestResult.passed = mSignalTestResult.outputRMS[0] > 0.001 || mSignalTestResult.outputRMS[1] > 0.001;

        nlohmann::json result;
        result["type"] = "signalPathTestResult";
        result["sampleRate"] = mSignalTestResult.sampleRate;
        result["frequency"] = mSignalTestResult.frequencyHz;
        result["duration"] = mSignalTestResult.durationSeconds;
        result["elapsed"] = mSignalTestResult.elapsedSeconds;
        result["inputRMS"] = mSignalTestResult.inputRMS;
        result["outputRMS"] = { mSignalTestResult.outputRMS[0], mSignalTestResult.outputRMS[1] };
        result["passed"] = mSignalTestResult.passed;
        SendMessageToUI(result.dump());
    }

    // Tuner data
    if (mTunerDataPending.load(std::memory_order_acquire))
    {
        mTunerDataPending.store(false, std::memory_order_release);
        TunerData data;
        {
            std::lock_guard<std::mutex> lock(mTunerMutex);
            data = mPendingTunerData;
        }
        nlohmann::json msg;
        msg["type"] = "tunerUpdate";
        msg["noteName"] = data.noteName;
        msg["octave"] = data.octave;
        msg["frequency"] = data.frequency;
        msg["centOffset"] = data.centOffset;
        msg["confidence"] = data.confidence;
        msg["detected"] = data.detected;
        SendMessageToUI(msg.dump());
    }

    // Periodic updates
    mDSPPerformanceUpdateCounter++;
    if (mDSPPerformanceUpdateCounter >= 30) // ~every 500ms at 60fps idle
    {
        mDSPPerformanceUpdateCounter = 0;
        SendPerformanceStatsToUI();
    }

    if (mSignalDiagnosticsEnabled.load(std::memory_order_acquire))
    {
        mSignalDiagnosticsUpdateCounter++;
        if (mSignalDiagnosticsUpdateCounter >= 6) // ~10fps
        {
            mSignalDiagnosticsUpdateCounter = 0;
            SendSignalDiagnosticsToUI();
        }
    }

    if (mDemoPreview)
        mDemoPreview->OnIdle();
}

void PluginController::OnWebContentLoaded()
{
    mUIReady = true;
    mPendingStateBroadcast = true;

    // The UI may not be ready when Initialize() loads/sends the layout library.
    // Resend here so custom layouts are available immediately after startup.
    LoadLayoutLibrary();
}

// ════════════════════════════════════════════════════════════════════
// Parameter bridging
// ════════════════════════════════════════════════════════════════════

void PluginController::OnParamChange(int paramIdx, double value)
{
    if (paramIdx < 0 || paramIdx >= kParamCount)
        return;

    mParamValues[static_cast<size_t>(paramIdx)] = value;

    // Route to mixer
    switch (paramIdx)
    {
    case kParamInputTrim:    mPresetMixer.SetInputTrim(value); break;
    case kParamOutputTrim:   mPresetMixer.SetOutputTrim(value); break;
    case kParamDrive:        mPresetMixer.SetAmpDrive(value); break;
    case kParamTone:         mPresetMixer.SetAmpTone(value); break;
    case kParamGateEnabled:  mPresetMixer.SetGateEnabled(value > 0.5); break;
    case kParamGateThreshold: mPresetMixer.SetGateThreshold(value); break;
    case kParamDoublerEnabled: mPresetMixer.SetDoublerEnabled(value > 0.5); break;
    case kParamDoublerDelay: mPresetMixer.SetDoublerDelay(value); break;
    case kParamTranspose:    mPresetMixer.SetTranspose(static_cast<int>(value)); break;
    case kParamIRQuality:    mPresetMixer.SetIRQuality(value); break;
    case kParamEQEnabled:    mPresetMixer.SetEQEnabled(value > 0.5); break;
    case kParamEQLowGain:    mPresetMixer.SetEQBandGain(0, value); break;
    case kParamEQLowFreq:    mPresetMixer.SetEQBandFrequency(0, value); break;
    case kParamEQLowMidGain: mPresetMixer.SetEQBandGain(1, value); break;
    case kParamEQLowMidFreq: mPresetMixer.SetEQBandFrequency(1, value); break;
    case kParamEQLowMidQ:    mPresetMixer.SetEQBandQ(1, value); break;
    case kParamEQHighMidGain: mPresetMixer.SetEQBandGain(2, value); break;
    case kParamEQHighMidFreq: mPresetMixer.SetEQBandFrequency(2, value); break;
    case kParamEQHighMidQ:   mPresetMixer.SetEQBandQ(2, value); break;
    case kParamEQHighGain:   mPresetMixer.SetEQBandGain(3, value); break;
    case kParamEQHighFreq:   mPresetMixer.SetEQBandFrequency(3, value); break;
    default: break;
    }
}

double PluginController::GetParamValue(int paramIdx) const
{
    if (paramIdx < 0 || paramIdx >= kParamCount)
        return 0.0;
    return mParamValues[static_cast<size_t>(paramIdx)];
}

// ════════════════════════════════════════════════════════════════════
// Multi-preset mixer controls
// ════════════════════════════════════════════════════════════════════

bool PluginController::AddActivePreset(const Preset& preset, const std::string& presetId, const std::string& name)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    return mPresetMixer.AddActivePreset(preset, presetId, name);
}

bool PluginController::AddActivePresetById(const std::string& presetId)
{
    // If the active preset matches, use it directly
    if (mActivePreset && mActivePreset->id == presetId)
    {
        return AddActivePreset(*mActivePreset, presetId, mActivePreset->name);
    }

    // Try loading from user presets directory
    if (!mUserPresetsPath.empty())
    {
        auto userPath = mUserPresetsPath / (presetId + ".json");
        auto presetOpt = PresetStorage::LoadFromFile(userPath);
        if (presetOpt)
        {
            return AddActivePreset(*presetOpt, presetId, presetOpt->name);
        }
    }

    // Try loading from factory presets directory
    auto factoryPath = mResourceRoot / "presets" / "factory" / (presetId + ".json");
    auto presetOpt = PresetStorage::LoadFromFile(factoryPath);
    if (presetOpt)
    {
        return AddActivePreset(*presetOpt, presetId, presetOpt->name);
    }

    ReportErrorToUI("Cannot add preset to mixer", "Preset '" + presetId + "' not found");
    return false;
}

void PluginController::RemoveActivePreset(const std::string& presetId)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.RemoveActivePreset(presetId);
}

void PluginController::SetActivePresetMix(const std::string& presetId, double value)
{
    mPresetMixer.SetPresetMix(presetId, value);
}

void PluginController::SetActivePresetPan(const std::string& presetId, double pan)
{
    mPresetMixer.SetPresetPan(presetId, pan);
}

void PluginController::SetActivePresetMute(const std::string& presetId, bool mute)
{
    mPresetMixer.SetPresetMute(presetId, mute);
}

void PluginController::SetActivePresetSolo(const std::string& presetId, bool solo)
{
    mPresetMixer.SetPresetSolo(presetId, solo);
}

void PluginController::SetMasterGain(double value)
{
    mPresetMixer.SetMasterGain(value);
}

void PluginController::SetLimiterEnabled(bool enabled)
{
    mPresetMixer.SetLimiterEnabled(enabled);
}

bool PluginController::StartSignalPathTest(double frequencyHz, double durationSeconds)
{
    double sr = mHost.GetSampleRate();
    if (sr <= 0.0) return false;

    auto& st = mSignalTestState;
    st.frequencyHz = frequencyHz;
    st.sampleRate = sr;
    st.phase = 0.0;
    st.phaseIncrement = frequencyHz / sr;
    st.totalSamples = static_cast<int>(durationSeconds * sr);
    st.samplesRemaining = st.totalSamples;
    st.inputSumSquares = 0.0;
    st.outputSumSquares = {0.0, 0.0};
    st.startTime = std::chrono::steady_clock::now();

    mSignalTestResult = {};
    mSignalTestActive.store(true, std::memory_order_release);
    return true;
}

// ════════════════════════════════════════════════════════════════════
// Messaging helpers
// ════════════════════════════════════════════════════════════════════

void PluginController::SendMessageToUI(const std::string& jsonMessage)
{
    mHost.SendMessageToUI(jsonMessage);
}

void PluginController::ReportErrorToUI(const std::string& message, const std::string& detail)
{
    nlohmann::json msg;
    msg["type"] = "error";
    msg["message"] = message;
    if (!detail.empty())
        msg["detail"] = detail;
    SendMessageToUI(msg.dump());
}

void PluginController::AppendSessionLog(const std::string& message)
{
    if (message.empty())
        return;

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto logPath = settingsDir / kSessionLogFileName;
    [[maybe_unused]] const auto ensuredLogDir = mFileSystem.EnsureDirectory(logPath.parent_path());

    std::ofstream output(logPath, std::ios::app);
    if (!output)
        return;

    output << FormatTimestamp() << " " << message << "\n";
}

// ════════════════════════════════════════════════════════════════════
// Request handlers
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleStateRequest()
{
    mPendingStateBroadcast = true;
}

void PluginController::HandlePresetLoadRequest(const nlohmann::json& payload)
{
    try
    {
        Preset preset;
        std::optional<Preset> presetOpt;
        if (payload.contains("preset"))
            presetOpt = PresetStorage::DeserializeFromJson(payload["preset"].dump());
        else
            presetOpt = PresetStorage::DeserializeFromJson(payload.dump());

        if (!presetOpt) return;
        preset = std::move(*presetOpt);

        ApplyBlendDefinitions(preset);

        mActivePresetId = payload.value("presetId", preset.id);
        ApplyPreset(preset); // SetGlobalChainConfig is called inside ApplyPreset under mDSPMutex

        mActivePreset = preset;
        mActivePresetJson = PresetStorage::SerializeToJson(preset);
        mPendingStateBroadcast = true;

        // Send explicit "presetLoaded" confirmation to the UI
        {
            nlohmann::json loaded;
            loaded["type"] = "presetLoaded";
            loaded["preset"] = nlohmann::json::parse(mActivePresetJson);
            nlohmann::json activeIds = nlohmann::json::array();
            for (const auto& id : mPresetMixer.GetActivePresetIds())
                activeIds.push_back(id);
            loaded["activePresetIds"] = activeIds;
            SendMessageToUI(loaded.dump());
        }

        // Persist last loaded preset
        mAppSettings["lastPresetId"] = mActivePresetId;
        SaveAppSettings();
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Failed to load preset", e.what());
    }
}

void PluginController::HandleSetParameterRequest(const nlohmann::json& payload)
{
    std::string paramName = payload.value("name", "");
    double value = payload.value("value", 0.0);

    // Map parameter name to index
    // The host adapter should call OnParamChange to sync DAW-visible parameters.
    // For now, route named parameters directly:
    static const std::unordered_map<std::string, int> paramMap = {
        {"inputTrim", kParamInputTrim}, {"outputTrim", kParamOutputTrim},
        {"drive", kParamDrive}, {"tone", kParamTone},
        {"gateEnabled", kParamGateEnabled}, {"gateThreshold", kParamGateThreshold},
        {"mix", kParamMix},
        {"doublerEnabled", kParamDoublerEnabled}, {"doublerDelay", kParamDoublerDelay},
        {"transpose", kParamTranspose}, {"irQuality", kParamIRQuality},
        {"eqEnabled", kParamEQEnabled},
        {"eqLowGain", kParamEQLowGain}, {"eqLowFreq", kParamEQLowFreq},
        {"eqLowMidGain", kParamEQLowMidGain}, {"eqLowMidFreq", kParamEQLowMidFreq},
        {"eqLowMidQ", kParamEQLowMidQ},
        {"eqHighMidGain", kParamEQHighMidGain}, {"eqHighMidFreq", kParamEQHighMidFreq},
        {"eqHighMidQ", kParamEQHighMidQ},
        {"eqHighGain", kParamEQHighGain}, {"eqHighFreq", kParamEQHighFreq},
    };

    auto it = paramMap.find(paramName);
    if (it != paramMap.end())
        OnParamChange(it->second, value);
}

void PluginController::HandleSetGlobalChainParamRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    auto value = payload.value("value", nlohmann::json());

    // Route paramPath strings to the corresponding mixer methods
    if (path == "gate.enabled") mPresetMixer.SetGlobalGateEnabled(value.get<bool>());
    else if (path == "gate.threshold") mPresetMixer.SetGlobalGateThreshold(value.get<double>());
    else if (path == "gate.attack") mPresetMixer.SetGlobalGateAttack(value.get<double>());
    else if (path == "gate.hold") mPresetMixer.SetGlobalGateHold(value.get<double>());
    else if (path == "gate.release") mPresetMixer.SetGlobalGateRelease(value.get<double>());
    else if (path == "transpose.enabled") mPresetMixer.SetGlobalTransposeEnabled(value.get<bool>());
    else if (path == "transpose.semitones") mPresetMixer.SetGlobalTranspose(value.get<int>());
    else if (path == "eq.enabled") mPresetMixer.SetGlobalEQEnabled(value.get<bool>());
    else if (path == "doubler.enabled") mPresetMixer.SetGlobalDoublerEnabled(value.get<bool>());
    else if (path == "doubler.delay") mPresetMixer.SetGlobalDoublerDelay(value.get<double>());
    else if (path == "doubler.mix") mPresetMixer.SetGlobalDoublerMix(value.get<double>());
    else if (path == "doubler.detune") mPresetMixer.SetGlobalDoublerDetune(value.get<double>());
    else if (path == "input.gain") mPresetMixer.SetGlobalInputGain(value.get<double>());
    else if (path == "output.gain") mPresetMixer.SetGlobalOutputGain(value.get<double>());
    else if (path == "limiter.enabled") mPresetMixer.SetLimiterEnabled(value.get<bool>());
    else if (path == "eq.lowGain") mPresetMixer.SetGlobalEQBandGain(0, value.get<double>());
    else if (path == "eq.lowFreq") mPresetMixer.SetGlobalEQBandFrequency(0, value.get<double>());
    else if (path == "eq.lowMidGain") mPresetMixer.SetGlobalEQBandGain(1, value.get<double>());
    else if (path == "eq.lowMidFreq") mPresetMixer.SetGlobalEQBandFrequency(1, value.get<double>());
    else if (path == "eq.lowMidQ") mPresetMixer.SetGlobalEQBandQ(1, value.get<double>());
    else if (path == "eq.highMidGain") mPresetMixer.SetGlobalEQBandGain(2, value.get<double>());
    else if (path == "eq.highMidFreq") mPresetMixer.SetGlobalEQBandFrequency(2, value.get<double>());
    else if (path == "eq.highMidQ") mPresetMixer.SetGlobalEQBandQ(2, value.get<double>());
    else if (path == "eq.highGain") mPresetMixer.SetGlobalEQBandGain(3, value.get<double>());
    else if (path == "eq.highFreq") mPresetMixer.SetGlobalEQBandFrequency(3, value.get<double>());

    // No echo: the UI already owns the values it sent.
    // Full state is pushed via HandleGetGlobalChainRequest / HandleSetGlobalChainRequest.
}

void PluginController::HandleSignalTestRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    double dur = payload.value("duration", 1.0);
    StartSignalPathTest(freq, dur);
}

void PluginController::HandleBrowseModelRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::NAMModel, "Select NAM Model",
        [this](const BrowseFileResult& result)
        {
            if (result.success)
            {
                nlohmann::json payload;
                payload["path"] = result.path.string();
                HandleLoadModelRequest(payload);
            }
        });
}

void PluginController::HandleBrowseIRRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::IRFile, "Select IR File",
        [this](const BrowseFileResult& result)
        {
            if (result.success)
            {
                nlohmann::json payload;
                payload["path"] = result.path.string();
                HandleLoadIRRequest(payload);
            }
        });
}

void PluginController::HandleOpenAudioPreferencesRequest()
{
    mHost.OpenAudioPreferences();
}

void PluginController::HandleTunerRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "");
    if (action == "start")
    {
        if (payload.contains("liveMode"))
            mPresetMixer.SetLiveTunerMode(payload.value("liveMode", true));

        mTunerActive.store(true, std::memory_order_release);
        mPresetMixer.SetTunerEnabled(true);

        if (payload.contains("referenceFrequency"))
            mPresetMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = mPresetMixer.GetTunerReferenceFrequency();
        message["liveMode"] = mPresetMixer.IsLiveTunerMode();
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "stop")
    {
        mTunerActive.store(false, std::memory_order_release);
        mPresetMixer.SetTunerEnabled(false);

        nlohmann::json message;
        message["type"] = "tunerStopped";
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        mPresetMixer.SetLiveTunerMode(liveMode);

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        mPresetMixer.SetTunerReferenceFrequency(freq);

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = mPresetMixer.GetTunerReferenceFrequency();
        SendMessageToUI(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        mTunerActive.store(enabled, std::memory_order_release);
        mPresetMixer.SetTunerEnabled(enabled);

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = mPresetMixer.GetTunerReferenceFrequency();
        reply["liveMode"] = mPresetMixer.IsLiveTunerMode();
        SendMessageToUI(reply.dump());
    }
}

void PluginController::HandleSetInputModeRequest(const nlohmann::json& payload)
{
    if (payload.contains("monoMode"))
        mPresetMixer.SetMonoMode(payload["monoMode"].get<bool>());
    else if (payload.contains("mono"))
        mPresetMixer.SetMonoMode(payload["mono"].get<bool>());

    if (payload.contains("inputChannel"))
        mPresetMixer.SetInputChannel(payload["inputChannel"].get<int>());
    else if (payload.contains("channel"))
        mPresetMixer.SetInputChannel(payload["channel"].get<int>());

    nlohmann::json message;
    message["type"] = "inputModeChanged";
    message["monoMode"] = mPresetMixer.IsMonoMode();
    message["inputChannel"] = mPresetMixer.GetInputChannel();
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAmpCabStateRequest(const nlohmann::json& payload)
{
    bool ampEnabled = true;
    bool cabEnabled = true;
    if (payload.contains("ampEnabled"))
        ampEnabled = payload.value("ampEnabled", true);
    if (payload.contains("cabEnabled"))
        cabEnabled = payload.value("cabEnabled", true);

    nlohmann::json message;
    message["type"] = "ampCabStateChanged";
    message["ampEnabled"] = ampEnabled;
    message["cabEnabled"] = cabEnabled;
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAutoLevelRequest(const nlohmann::json& payload)
{
    if (payload.contains("autoInput"))
        mPresetMixer.SetAutoLevelInput(payload["autoInput"].get<bool>());
    else if (payload.contains("input"))
        mPresetMixer.SetAutoLevelInput(payload["input"].get<bool>());

    if (payload.contains("autoOutput"))
        mPresetMixer.SetAutoLevelOutput(payload["autoOutput"].get<bool>());
    else if (payload.contains("output"))
        mPresetMixer.SetAutoLevelOutput(payload["output"].get<bool>());

    mAppSettings["autoLevelInput"] = mPresetMixer.GetAutoLevelInput();
    mAppSettings["autoLevelOutput"] = mPresetMixer.GetAutoLevelOutput();
    SaveAppSettings();

    nlohmann::json message;
    message["type"] = "autoLevelChanged";
    message["autoInput"] = mPresetMixer.GetAutoLevelInput();
    message["autoOutput"] = mPresetMixer.GetAutoLevelOutput();
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetMetronomeRequest(const nlohmann::json& payload)
{
    if (!mHost.IsStandalone())
        return;

    bool stateChanged = false;
    bool settingsChanged = false;
    bool resetRequired = false;
    const bool wasEnabled = mMetronomeEnabled.load(std::memory_order_relaxed);

    if (payload.contains("bpm") && payload["bpm"].is_number())
    {
        const double bpm = ClampValue(payload.value("bpm", kMetronomeDefaultBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
        mMetronomeBpm.store(bpm, std::memory_order_release);
        mAppSettings[kMetronomeBpmSettingKey] = bpm;
        stateChanged = true;
        settingsChanged = true;
    }

    if (payload.contains("enabled") && payload["enabled"].is_boolean())
    {
        const bool enabled = payload.value("enabled", false);
        mMetronomeEnabled.store(enabled, std::memory_order_release);
        if (mAppSettings.contains(kMetronomeEnabledSettingKey))
            mAppSettings.erase(kMetronomeEnabledSettingKey);
        stateChanged = true;
        resetRequired = enabled && !wasEnabled;
    }

    if (payload.contains("volumeDb") && payload["volumeDb"].is_number())
    {
        const double volumeDb = ClampValue(payload.value("volumeDb", kMetronomeDefaultVolumeDb),
                                           kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
        mMetronomeVolumeDb.store(volumeDb, std::memory_order_release);
        mAppSettings[kMetronomeVolumeDbSettingKey] = volumeDb;
        stateChanged = true;
        settingsChanged = true;
    }

    if (payload.contains("pan") && payload["pan"].is_number())
    {
        const double pan = ClampValue(payload.value("pan", kMetronomeDefaultPan), -1.0, 1.0);
        mMetronomePan.store(pan, std::memory_order_release);
        mAppSettings[kMetronomePanSettingKey] = pan;
        stateChanged = true;
        settingsChanged = true;
    }

    if (payload.contains("clickConfig") && payload["clickConfig"].is_array())
    {
        mAppSettings[kMetronomeClickConfigSettingKey] = payload["clickConfig"];
        UpdateMetronomeClickConfigFromSettings();
        RefreshMetronomeClickSamples(mHost.GetSampleRate());
        stateChanged = true;
        settingsChanged = true;
    }

    if (payload.contains("clickType") && payload["clickType"].is_string())
    {
        const std::string clickType = payload.value("clickType", std::string{kMetronomeDefaultClickType});
        if (!clickType.empty())
        {
            mMetronomeClickType = clickType;
            mAppSettings[kMetronomeClickTypeSettingKey] = clickType;
            RefreshMetronomeClickSamples(mHost.GetSampleRate());
            stateChanged = true;
            settingsChanged = true;
        }
    }

    if (stateChanged)
    {
        if (resetRequired)
            mMetronomeResetPending.store(true, std::memory_order_release);
        mPendingStateBroadcast = true;
    }

    if (settingsChanged)
        SaveAppSettings();
}

void PluginController::HandleLoadModelRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    if (path.empty())
        path = payload.value("filePath", "");
    if (path.empty()) return;

    std::filesystem::path filePath(path);
    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Model file not found", path);
        return;
    }

    if (UpdateResourceForNodeType("amp_nam", "nam", filePath))
    {
        mAppSettings["lastModelPath"] = filePath.parent_path().string();
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = filePath.generic_string();
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleLoadIRRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    if (path.empty())
        path = payload.value("filePath", "");
    if (path.empty()) return;

    std::filesystem::path filePath(path);
    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("IR file not found", path);
        return;
    }

    if (UpdateResourceForNodeType("cab_ir", "ir", filePath))
    {
        mAppSettings["lastIRPath"] = filePath.parent_path().string();
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = filePath.generic_string();
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleSavePresetRequest(const nlohmann::json& payload)
{
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");
    const std::string presetIdOverride = payload.value("presetId", "");
    const bool includeGlobalSignalChain = payload.value("includeGlobalSignalChain", payload.contains("globalSignalChain"));

    std::optional<Preset> payloadPreset;
    if (payload.contains("preset") && payload["preset"].is_object())
    {
        payloadPreset = PresetStorage::DeserializeFromJson(payload["preset"].dump());
    }

    if (presetName.empty())
    {
        ReportErrorToUI("Cannot save preset", "Preset name is required");
        return;
    }

    if (!payloadPreset)
    {
        EnsureBasicGraph();
        if (!mActivePreset)
        {
            ReportErrorToUI("Cannot save preset", "No active preset to save");
            return;
        }
    }

    try
    {
        Preset newPreset = payloadPreset ? *payloadPreset : *mActivePreset;
        EnsurePresetBoundaryGainNodes(newPreset);
        newPreset.id = presetIdOverride.empty()
            ? "user-" + std::to_string(std::time(nullptr))
            : presetIdOverride;
        newPreset.name = presetName;
        newPreset.category = presetCategory;
        newPreset.description = presetDescription;
        newPreset.version = 2;

        newPreset.global.inputTrim = mParamValues[kParamInputTrim];
        newPreset.global.outputTrim = mParamValues[kParamOutputTrim];
        newPreset.global.transpose = static_cast<int>(mParamValues[kParamTranspose]);
        newPreset.global.autoLevelInput = mPresetMixer.GetAutoLevelInput();
        newPreset.global.autoLevelOutput = mPresetMixer.GetAutoLevelOutput();

        if (includeGlobalSignalChain)
        {
            if (payload.contains("globalSignalChain") && payload["globalSignalChain"].is_object())
            {
                newPreset.globalSignalChain = payload["globalSignalChain"].get<GlobalSignalChainConfig>();
            }
            else if (newPreset.globalSignalChain.has_value())
            {
                newPreset.globalSignalChain = newPreset.globalSignalChain.value();
            }
            else
            {
                newPreset.globalSignalChain = mPresetMixer.GetGlobalChainConfig();
            }
        }
        else
        {
            newPreset.globalSignalChain.reset();
        }

        if (mUserPresetsPath.empty())
            mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";
        [[maybe_unused]] const auto ensuredUserPresetPath = mFileSystem.EnsureDirectory(mUserPresetsPath);

        const auto presetPath = mUserPresetsPath / (newPreset.id + ".json");
        if (!PresetStorage::SaveToFile(newPreset, presetPath))
        {
            ReportErrorToUI("Failed to save preset", "Could not write preset file");
            return;
        }

        mActivePreset = newPreset;
        mActivePresetId = newPreset.id;
        mActivePresetJson = PresetStorage::SerializeToJson(newPreset);
        mPendingStateBroadcast = true;
        SaveAppSettings();

        nlohmann::json reply;
        reply["type"] = "presetSaved";
        reply["preset"] = nlohmann::json::parse(mActivePresetJson);
        SendMessageToUI(reply.dump());
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Failed to save preset", e.what());
    }
}

void PluginController::HandleDeletePresetRequest(const nlohmann::json& payload)
{
    const std::string presetId = payload.value("presetId", "");
    if (presetId.empty())
        return;

    if (mUserPresetsPath.empty())
        mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";

    const auto presetPath = mUserPresetsPath / (presetId + ".json");
    if (!std::filesystem::exists(presetPath))
    {
        ReportErrorToUI("Preset not found", presetId);
        return;
    }

    std::error_code ec;
    std::filesystem::remove(presetPath, ec);
    if (ec)
    {
        ReportErrorToUI("Failed to delete preset", presetId);
    }
}

void PluginController::HandleGetPresetByIdRequest(const nlohmann::json& payload)
{
    const std::string presetId = payload.value("presetId", "");
    if (presetId.empty())
        return;

    if (mUserPresetsPath.empty())
        mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";

    const auto userPath = mUserPresetsPath / (presetId + ".json");
    const auto factoryPath = mResourceRoot / "presets" / "factory" / (presetId + ".json");

    std::optional<Preset> presetOpt;
    if (std::filesystem::exists(userPath))
        presetOpt = PresetStorage::LoadFromFile(userPath);
    if (!presetOpt && std::filesystem::exists(factoryPath))
        presetOpt = PresetStorage::LoadFromFile(factoryPath);

    if (!presetOpt)
    {
        ReportErrorToUI("Preset not found", presetId);
        return;
    }

    nlohmann::json msg;
    msg["type"] = "presetData";
    msg["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(*presetOpt));
    SendMessageToUI(msg.dump());
}

// ── Signal path editing handlers ───────────────────────────────────
// These handlers manipulate the signal graph nodes and edges.
// They will be ported from GuitarFXPlugin.cpp as the next step.

void PluginController::HandleUpdateSignalPathNodeParamRequest(const nlohmann::json& payload)
{
    // Updates a single DSP parameter on a graph node by nodeId/paramKey
    std::string nodeId = payload.value("nodeId", "");
    std::string paramKey = payload.value("paramKey", "");
    double value = payload.value("value", 0.0);
    std::string presetId = payload.value("presetId", "p1");

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->params[paramKey] = value;
    mPresetMixer.SetNodeParam(presetId, nodeId, paramKey, value);
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
}

void PluginController::HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    if (payload.contains("bypassed"))
        enabled = !payload.value("bypassed", false);
    std::string presetId = payload.value("presetId", "p1");

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->enabled = enabled;
    mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
    mPendingStateBroadcast = true;
}

void PluginController::HandleUpdateNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty()) return;

    int resourceIndex = payload.value("resourceIndex", -1);
    const std::string exposedResourceId = payload.value("exposedResourceId", "");

    ResourceRef ref;
    if (payload.contains("resourceType"))
        ref.resourceType = payload["resourceType"].get<std::string>();
    if (payload.contains("resourceId"))
        ref.resourceId = payload["resourceId"].get<std::string>();
    if (payload.contains("filePath"))
        ref.filePath = payload["filePath"].get<std::string>();
    if (payload.contains("embeddedId"))
        ref.embeddedId = payload["embeddedId"].get<std::string>();
    if (payload.contains("parameterId"))
        ref.parameterId = payload["parameterId"].get<std::string>();
    if (payload.contains("parameterValue") && payload["parameterValue"].is_number())
        ref.parameterValue = payload["parameterValue"].get<double>();

    if (!exposedResourceId.empty())
    {
        auto* targetGraph = ResolveEditTarget();
        auto* targetNode = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
        if (targetNode && targetNode->type.rfind("composite:", 0) == 0)
        {
            const std::string definitionId = targetNode->type.substr(std::string("composite:").size());
            if (const auto* definition = mCompositeLibrary.GetDefinition(definitionId))
            {
                const auto exposedIt = std::find_if(
                    definition->exposedResources.begin(),
                    definition->exposedResources.end(),
                    [&](const ExposedResource& exposed)
                    {
                        return exposed.resourceId == exposedResourceId;
                    });

                if (exposedIt != definition->exposedResources.end())
                {
                    if (ref.resourceType.empty())
                        ref.resourceType = exposedIt->resourceType;
                    if (ref.parameterId.empty() && !exposedIt->parameterId.empty())
                        ref.parameterId = exposedIt->parameterId;
                    if (!ref.parameterValue.has_value() && exposedIt->parameterValue.has_value())
                        ref.parameterValue = *exposedIt->parameterValue;
                }
            }
        }
    }

    if (!ref.parameterId.empty() && ref.parameterValue.has_value())
        ref.parameters[ref.parameterId] = *ref.parameterValue;

    if (resourceIndex >= 0)
    {
        if (!IsCompositeEditMode())
            EnsureBasicGraph();

        auto* targetGraph = ResolveEditTarget();
        if (!targetGraph) return;

        auto* target = targetGraph->FindNode(nodeId);
        if (!target) return;

        if (static_cast<size_t>(resourceIndex) >= target->resources.size())
            target->resources.resize(static_cast<size_t>(resourceIndex) + 1);

        ResourceRef& slot = target->resources[static_cast<size_t>(resourceIndex)];
        if (!ref.resourceType.empty())
            slot.resourceType = ref.resourceType;
        if (!ref.resourceId.empty())
        {
            slot.resourceId = ref.resourceId;
            slot.filePath.clear();
        }
        if (!ref.filePath.empty())
        {
            slot.filePath = ref.filePath;
            slot.resourceId.clear();
        }
        if (!ref.embeddedId.empty())
            slot.embeddedId = ref.embeddedId;
        if (!ref.parameterId.empty())
            slot.parameterId = ref.parameterId;
        if (ref.parameterValue.has_value())
            slot.parameterValue = ref.parameterValue;
        if (!ref.parameters.empty())
            slot.parameters = ref.parameters;

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
            ApplyPreset(*mActivePreset);
            mPendingStateBroadcast = true;
        }

        if (!IsCompositeEditMode()
            && (target->type == "amp_nam" || target->type == "amp_nam_optimized")
            && !target->resources.empty()
            && target->resources.front().IsValid())
        {
            QueueNamCalibrationForNode(nodeId, target->resources.front());
        }
        return;
    }

    if (!ref.filePath.empty())
    {
        auto* fpGraph = ResolveEditTarget();
        auto* node = fpGraph ? fpGraph->FindNode(nodeId) : nullptr;
        if (node && !node->resources.empty())
        {
            node->resources.clear();
            node->resources.push_back(ref);

            if (IsCompositeEditMode())
            {
                BroadcastCompositeEditState();
            }
            else if (mActivePreset)
            {
                mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
                ApplyPreset(*mActivePreset);
                mPendingStateBroadcast = true;
            }

            if (!IsCompositeEditMode()
                && (node->type == "amp_nam" || node->type == "amp_nam_optimized")
                && !node->resources.empty()
                && node->resources.front().IsValid())
            {
                QueueNamCalibrationForNode(node->id, node->resources.front());
            }
            return;
        }
    }

    UpdateResourceForNodeId(nodeId, ref);

    if (mActivePreset)
    {
        auto* node = mActivePreset->graph.FindNode(nodeId);
        if (node && (node->type == "amp_nam" || node->type == "amp_nam_optimized")
            && !node->resources.empty() && node->resources.front().IsValid())
        {
            QueueNamCalibrationForNode(nodeId, node->resources.front());
        }
    }
}

void PluginController::HandleBrowseNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    std::string resourceType = payload.value("resourceType", "nam");
    const int resourceIndex = payload.value("resourceIndex", -1);
    const std::string exposedResourceId = payload.value("exposedResourceId", "");

    BrowseFileType fileType = BrowseFileType::NAMModel;
    if (resourceType == "ir") fileType = BrowseFileType::IRFile;

    mHost.BrowseFileAsync(fileType, "Select Resource",
        [this, nodeId, resourceType, resourceIndex, exposedResourceId](const BrowseFileResult& result)
        {
            if (result.success)
            {
                nlohmann::json payload;
                payload["nodeId"] = nodeId;
                payload["filePath"] = result.path.string();
                payload["resourceType"] = resourceType;
                if (resourceIndex >= 0)
                    payload["resourceIndex"] = resourceIndex;
                if (!exposedResourceId.empty())
                    payload["exposedResourceId"] = exposedResourceId;
                HandleUpdateNodeResourceRequest(payload);
            }
        });
}

void PluginController::HandleRerunNamCalibrationRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty() || !mActivePreset) return;

    auto* node = mActivePreset->graph.FindNode(nodeId);
    if (!node || node->resources.empty()) return;

    QueueNamCalibrationForNode(nodeId, node->resources[0], true);
}

void PluginController::HandleAddSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string insertAfter = payload.value("insertAfter", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());

    std::string edgeFrom, edgeTo;
    int edgeFromPort = 0, edgeToPort = 0;
    double edgeGain = 1.0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
        edgeFrom = edgeIt->value("from", "");
        edgeTo = edgeIt->value("to", "");
        edgeFromPort = edgeIt->value("fromPort", 0);
        edgeToPort = edgeIt->value("toPort", 0);
        edgeGain = edgeIt->value("gain", 1.0);
    }

    if (effectType.empty() || (insertAfter.empty() && edgeFrom.empty()))
    {
        ReportErrorToUI("Add node failed", "Missing effectType or insertion target (insertAfter/edge)");
        return;
    }

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph)
    {
        ReportErrorToUI("Add node failed", "No active preset or composite");
        return;
    }

    auto& edges = targetGraph->edges;
    const auto originalEdges = edges;
    auto chosenEdgeIt = edges.end();

    if (!edgeFrom.empty() && !edgeTo.empty())
    {
        chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
            [&](const GraphEdge& e) {
                return e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort;
            });
    }
    else
    {
        chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
            [&](const GraphEdge& e) { return e.from == insertAfter && e.fromPort == 0; });
        if (chosenEdgeIt == edges.end())
            chosenEdgeIt = std::find_if(edges.begin(), edges.end(),
                [&](const GraphEdge& e) { return e.from == insertAfter; });
    }

    if (chosenEdgeIt == edges.end())
    {
        ReportErrorToUI("Add node failed", "Could not find target edge for insertion");
        return;
    }

    if (effectType == "splitter")
    {
        auto& graph = *targetGraph;
        const std::string splitterId = MakeUniqueNodeId(graph, "split");
        const std::string mixerId = MakeUniqueNodeId(graph, "mix");

        GraphNode splitter; splitter.id = splitterId; splitter.type = "splitter"; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
        GraphNode mixer; mixer.id = mixerId; mixer.type = "mixer"; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

        const std::string nextNodeId = chosenEdgeIt->to;
        const int preservedToPort = chosenEdgeIt->toPort;
        const double preservedGain = chosenEdgeIt->gain;

        chosenEdgeIt->to = splitterId; chosenEdgeIt->toPort = 0; chosenEdgeIt->gain = 1.0;

        GraphEdge branch0; branch0.from = splitterId; branch0.to = mixerId; branch0.fromPort = 0; branch0.toPort = 0; branch0.gain = 1.0;
        GraphEdge branch1; branch1.from = splitterId; branch1.to = mixerId; branch1.fromPort = 1; branch1.toPort = 1; branch1.gain = 1.0;
        GraphEdge mixToNext; mixToNext.from = mixerId; mixToNext.to = nextNodeId; mixToNext.fromPort = 0; mixToNext.toPort = preservedToPort; mixToNext.gain = preservedGain;

        edges.push_back(branch0); edges.push_back(branch1); edges.push_back(mixToNext);
        graph.nodes.push_back(splitter); graph.nodes.push_back(mixer);

        if (IsCompositeEditMode()) BroadcastCompositeEditState();
        else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
        return;
    }

    // Create new node with default parameters
    GraphNode newNode;
    newNode.id = effectType + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    newNode.type = effectType;
    newNode.enabled = true;

    const auto effectInfoOpt = EffectRegistry::Instance().GetTypeInfo(effectType);
    if (effectInfoOpt)
    {
        newNode.category = effectInfoOpt->category;
        newNode.label = effectInfoOpt->displayName;
        for (const auto& p : effectInfoOpt->parameters)
            newNode.params[p.id] = p.defaultValue;
    }
    else { newNode.category = "utility"; newNode.label = effectType; }

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) newNode.config[entry.key()] = entry.value().get<std::string>();

    if (!labelOverride.empty()) newNode.label = labelOverride;
    if (!categoryOverride.empty()) newNode.category = categoryOverride;

    const std::string nextNodeId = chosenEdgeIt->to;
    const int preservedToPort = chosenEdgeIt->toPort;
    const double preservedGain = chosenEdgeIt->gain;
    (void)edgeGain;

    chosenEdgeIt->to = newNode.id; chosenEdgeIt->toPort = 0; chosenEdgeIt->gain = 1.0;

    GraphEdge newEdge; newEdge.from = newNode.id; newEdge.to = nextNodeId; newEdge.fromPort = 0; newEdge.toPort = preservedToPort; newEdge.gain = preservedGain;
    edges.push_back(newEdge);
    targetGraph->nodes.push_back(newNode);

    if (!IsGraphAcyclic(*targetGraph))
    {
        edges = originalEdges;
        ReportErrorToUI("Reorder node failed", "Operation would create a cycle");
        return;
    }

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleSplitSignalPathEdgeRequest(const nlohmann::json& payload)
{
    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Split failed", "No active preset or composite"); return; }

    const auto edgeIt = payload.find("edge");
    if (edgeIt == payload.end() || !edgeIt->is_object()) { ReportErrorToUI("Split failed", "Missing edge payload"); return; }

    const std::string from = edgeIt->value("from", "");
    const std::string to = edgeIt->value("to", "");
    const int fromPort = edgeIt->value("fromPort", 0);
    const int toPort = edgeIt->value("toPort", 0);
    if (from.empty() || to.empty()) { ReportErrorToUI("Split failed", "Edge is missing from/to"); return; }

    auto& edges = targetGraph->edges;
    auto targetEdgeIt = std::find_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e) { return e.from == from && e.to == to && e.fromPort == fromPort && e.toPort == toPort; });
    if (targetEdgeIt == edges.end()) { ReportErrorToUI("Split failed", "Target edge not found"); return; }

    const std::string splitterId = MakeUniqueNodeId(*targetGraph, "split");
    const std::string mixerId = MakeUniqueNodeId(*targetGraph, "mix");

    GraphNode splitter; splitter.id = splitterId; splitter.type = "splitter"; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
    GraphNode mixer; mixer.id = mixerId; mixer.type = "mixer"; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

    const std::string nextNodeId = targetEdgeIt->to;
    const int preservedToPort = targetEdgeIt->toPort;
    const double preservedGain = targetEdgeIt->gain;

    targetEdgeIt->to = splitterId; targetEdgeIt->toPort = 0; targetEdgeIt->gain = 1.0;

    GraphEdge b0; b0.from = splitterId; b0.to = mixerId; b0.fromPort = 0; b0.toPort = 0; b0.gain = 1.0;
    GraphEdge b1; b1.from = splitterId; b1.to = mixerId; b1.fromPort = 1; b1.toPort = 1; b1.gain = 1.0;
    GraphEdge mtn; mtn.from = mixerId; mtn.to = nextNodeId; mtn.fromPort = 0; mtn.toPort = preservedToPort; mtn.gain = preservedGain;

    edges.push_back(b0); edges.push_back(b1); edges.push_back(mtn);
    targetGraph->nodes.push_back(splitter); targetGraph->nodes.push_back(mixer);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleCollapseSignalPathSplitRequest(const nlohmann::json& payload)
{
    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Collapse split failed", "No active preset or composite"); return; }

    const std::string splitterId = payload.value("splitterId", "");
    const std::string mixerId = payload.value("mixerId", "");
    if (splitterId.empty() || mixerId.empty()) { ReportErrorToUI("Collapse split failed", "Missing splitterId/mixerId"); return; }

    auto& edges = targetGraph->edges;
    std::vector<GraphEdge*> splitterOut;
    GraphEdge* mixerOut = nullptr;
    GraphEdge* splitterIn = nullptr;

    for (auto& e : edges)
    {
        if (e.from == splitterId) splitterOut.push_back(&e);
        if (e.from == mixerId) mixerOut = &e;
        if (e.to == splitterId) splitterIn = &e;
    }

    if (!splitterIn || !mixerOut) { ReportErrorToUI("Collapse split failed", "Split is not connected correctly"); return; }

    const bool branchesEmpty = !splitterOut.empty() && std::all_of(splitterOut.begin(), splitterOut.end(),
        [&](const GraphEdge* e) { return e && e->to == mixerId; });
    if (!branchesEmpty) { ReportErrorToUI("Collapse split failed", "Can only collapse an empty split (remove branch effects first)"); return; }

    splitterIn->to = mixerOut->to;
    splitterIn->toPort = mixerOut->toPort;
    splitterIn->gain = mixerOut->gain;

    edges.erase(std::remove_if(edges.begin(), edges.end(),
        [&](const GraphEdge& e) { return e.from == splitterId || e.from == mixerId || e.to == mixerId; }), edges.end());

    targetGraph->nodes.erase(std::remove_if(targetGraph->nodes.begin(), targetGraph->nodes.end(),
        [&](const GraphNode& n) { return n.id == splitterId || n.id == mixerId; }), targetGraph->nodes.end());

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleReplaceSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    const std::string newEffectType = payload.value("newEffectType", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());

    if (nodeId.empty() || newEffectType.empty()) { ReportErrorToUI("Replace node failed", "Missing nodeId or newEffectType parameter"); return; }

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Replace node failed", "No active preset or composite"); return; }

    GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Replace node failed", "Node not found: " + nodeId); return; }

    const auto oldEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(node->type);
    const auto newEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(newEffectType);
    if (!newEffectInfoOpt) { ReportErrorToUI("Replace node failed", "Unknown effect type: " + newEffectType); return; }

    if (oldEffectInfoOpt && oldEffectInfoOpt->category != newEffectInfoOpt->category)
    { ReportErrorToUI("Replace node failed", "Cannot replace effect with different category"); return; }

    node->type = newEffectType;
    node->label = newEffectInfoOpt->displayName;
    node->category = newEffectInfoOpt->category;
    node->params.clear();
    node->resources.clear();
    node->config.clear();

    for (const auto& p : newEffectInfoOpt->parameters)
        node->params[p.id] = p.defaultValue;

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) node->config[entry.key()] = entry.value().get<std::string>();

    if (!labelOverride.empty()) node->label = labelOverride;
    if (!categoryOverride.empty()) node->category = categoryOverride;

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleReorderSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    const std::string targetNodeId = payload.value("targetNodeId", "");

    std::string edgeFrom, edgeTo;
    int edgeFromPort = 0, edgeToPort = 0;

    const auto edgeIt = payload.find("edge");
    if (edgeIt != payload.end() && edgeIt->is_object())
    {
        edgeFrom = edgeIt->value("from", "");
        edgeTo = edgeIt->value("to", "");
        edgeFromPort = edgeIt->value("fromPort", 0);
        edgeToPort = edgeIt->value("toPort", 0);
    }

    if (nodeId.empty() || (targetNodeId.empty() && edgeFrom.empty())) return;

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Reorder node failed", "No active preset or composite"); return; }

    const GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Reorder node failed", "Node not found"); return; }
    if (node->type == "splitter" || node->type == "mixer") { ReportErrorToUI("Reorder node failed", "Cannot move splitter/mixer nodes"); return; }

    auto& edges = targetGraph->edges;

    auto incomingEdgeIt = std::find_if(edges.begin(), edges.end(), [&](const GraphEdge& e) { return e.to == nodeId; });
    auto outgoingEdgeIt = std::find_if(edges.begin(), edges.end(), [&](const GraphEdge& e) { return e.from == nodeId; });
    if (incomingEdgeIt == edges.end() || outgoingEdgeIt == edges.end()) { ReportErrorToUI("Reorder node failed", "Missing edges"); return; }

    const std::string nextNodeId = outgoingEdgeIt->to;
    const int preservedToPort = outgoingEdgeIt->toPort;
    const double preservedGain = outgoingEdgeIt->gain;

    incomingEdgeIt->to = nextNodeId;
    incomingEdgeIt->toPort = preservedToPort;
    incomingEdgeIt->gain = preservedGain;
    edges.erase(outgoingEdgeIt);

    if (!edgeFrom.empty() && !edgeTo.empty())
    {
        if (edgeFrom == nodeId || edgeTo == nodeId) { ReportErrorToUI("Reorder node failed", "Cannot move node onto itself"); return; }

        auto tgt = std::find_if(edges.begin(), edges.end(),
            [&](const GraphEdge& e) { return e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort; });
        if (tgt == edges.end()) { ReportErrorToUI("Reorder node failed", "Cannot find target edge"); return; }

        const std::string tNextId = tgt->to;
        const int tPort = tgt->toPort;
        const double tGain = tgt->gain;
        tgt->to = nodeId; tgt->toPort = 0; tgt->gain = 1.0;

        GraphEdge ne; ne.from = nodeId; ne.to = tNextId; ne.fromPort = 0; ne.toPort = tPort; ne.gain = tGain;
        edges.push_back(ne);
    }
    else
    {
        const GraphNode* tNode = targetGraph->FindNode(targetNodeId);
        if (!tNode) { ReportErrorToUI("Reorder node failed", "Target node not found"); return; }

        auto tOut = std::find_if(edges.begin(), edges.end(), [&](const GraphEdge& e) { return e.from == targetNodeId; });
        if (tOut == edges.end()) { ReportErrorToUI("Reorder node failed", "Cannot find target position"); return; }

        const std::string afterId = tOut->to;
        const int tPort = tOut->toPort;
        const double tGain = tOut->gain;
        tOut->to = nodeId; tOut->toPort = 0; tOut->gain = 1.0;

        GraphEdge ne; ne.from = nodeId; ne.to = afterId; ne.fromPort = 0; ne.toPort = tPort; ne.gain = tGain;
        edges.push_back(ne);
    }

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleDeleteSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty()) return;

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Delete node failed", "No active preset or composite"); return; }

    const GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Delete node failed", "Node not found: " + nodeId); return; }

    auto& edges = targetGraph->edges;
    auto& nodes = targetGraph->nodes;

    std::vector<GraphEdge*> incomingEdges;
    std::vector<GraphEdge*> outgoingEdges;
    for (auto& edge : edges)
    {
        if (edge.to == nodeId)
            incomingEdges.push_back(&edge);
        if (edge.from == nodeId)
            outgoingEdges.push_back(&edge);
    }

    if (incomingEdges.size() != 1 || outgoingEdges.size() != 1)
    {
        ReportErrorToUI("Delete node failed", "Node has multiple connections. Remove branch effects first.");
        return;
    }

    GraphEdge* inEdge = incomingEdges.front();
    GraphEdge* outEdge = outgoingEdges.front();
    inEdge->to = outEdge->to;
    inEdge->toPort = outEdge->toPort;
    inEdge->gain = outEdge->gain;
    edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const GraphEdge& e) { return e.from == nodeId; }), edges.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) { return n.id == nodeId; }), nodes.end());

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleImportRemoteResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    const std::string name = payload.value("name", resourceId);
    const std::string description = payload.value("description", "");
    const std::string category = payload.value("category", "");
    const std::string provider = payload.value("provider", "remote");
    const std::string subfolder = payload.value("subfolder", "");
    const std::string data = payload.value("data", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string hash = payload.value("hash", "");
    const nlohmann::json metadataPayload = payload.value("metadata", nlohmann::json::object());

    if (resourceType.empty() || resourceId.empty() || data.empty())
    {
        ReportErrorToUI("Import failed", "Missing resource metadata");
        SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Missing resource metadata"}}.dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto sanitizedProvider = util::SanitizePathSegment(provider, true);
    auto targetDir = settingsDir / "resources" / "content" / sanitizedProvider;
    const auto sanitizedSubfolder = util::SanitizeSubfolderPath(subfolder);
    if (!sanitizedSubfolder.empty()) targetDir /= sanitizedSubfolder;
    [[maybe_unused]] const auto ensuredTargetDir = mFileSystem.EnsureDirectory(targetDir);

    std::string resolvedName = fileName.empty() ? resourceId : fileName;
    resolvedName = util::SanitizeFilename(resolvedName);
    if (resolvedName.find('.') == std::string::npos)
        resolvedName += resourceType == "ir" ? ".wav" : ".nam";

    const auto targetPath = targetDir / resolvedName;
    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty())
    {
        ReportErrorToUI("Import failed", "Invalid base64 payload");
        SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Invalid base64 payload"}}.dump());
        return;
    }
    if (!WriteFile(targetPath, bytes))
    {
        ReportErrorToUI("Import failed", "Failed to write file");
        SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Import failed"}, {"detail", "Failed to write file"}}.dump());
        return;
    }

    LibraryResource resource;
    resource.type = resourceType;
    resource.id = resourceId;
    resource.name = name;
    resource.category = category;
    resource.description = description;
    resource.filePath = targetPath;
    resource.hash = hash;
    if (metadataPayload.is_object())
    {
        for (const auto& entry : metadataPayload.items())
        {
            const auto& value = entry.value();
            if (value.is_string()) resource.metadata[entry.key()] = value.get<std::string>();
            else if (value.is_number()) resource.metadata[entry.key()] = value.dump();
            else if (value.is_boolean()) resource.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
        }
    }

    mResourceLibrary.AddResource(resource);
    AppendUserLibraryResource(resource);
    BroadcastState();

    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = resourceType;
    msg["id"] = resourceId;
    msg["name"] = name;
    msg["filePath"] = targetPath.string();
    SendMessageToUI(msg.dump());
    AppendSessionLog("Imported resource " + resourceType + ":" + resourceId + " (" + targetPath.string() + ")");
}

void PluginController::HandleImportToneSharingPackRequest(const nlohmann::json& payload)
{
    const std::string packId = payload.value("packId", "");
    const std::string data = payload.value("data", "");
    std::string fileName = payload.value("fileName", "");

    if (data.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Missing pack data"}}.dump());
        return;
    }

    if (fileName.empty())
    {
        fileName = packId.empty() ? "tone-sharing-pack.zip" : ("tone-sharing-pack-" + packId + ".zip");
    }

    fileName = util::SanitizeFilename(fileName);
    if (fileName.find('.') == std::string::npos)
    {
        fileName += ".zip";
    }

    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Invalid pack payload"}}.dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto importsDir = settingsDir / "imports" / "tone-sharing";
    [[maybe_unused]] const auto ensuredImportsDir = mFileSystem.EnsureDirectory(importsDir);

    auto targetPath = importsDir / fileName;
    if (!WriteFile(targetPath, bytes))
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackImportFailed"}, {"message", "Failed to write imported pack"}}.dump());
        return;
    }

    nlohmann::json result;
    result["type"] = "toneSharingPackImported";
    result["packId"] = packId;
    result["fileName"] = fileName;
    result["path"] = targetPath.generic_string();
    result["byteSize"] = bytes.size();
    SendMessageToUI(result.dump());

    AppendSessionLog("Imported tone sharing pack " + (packId.empty() ? std::string{"(unknown)"} : packId) + " -> " + targetPath.generic_string());
}

void PluginController::HandleDeleteImportedToneSharingPackRequest(const nlohmann::json& payload)
{
    const std::string rawPath = payload.value("path", "");
    if (rawPath.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Missing pack path"}}.dump());
        return;
    }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto importsDir = settingsDir / "imports" / "tone-sharing";
    const auto requestedPath = std::filesystem::path(rawPath);

    std::error_code ec;
    const auto canonicalImports = std::filesystem::weakly_canonical(importsDir, ec);
    if (ec)
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Unable to resolve import directory"}}.dump());
        return;
    }

    ec.clear();
    const auto canonicalRequested = std::filesystem::weakly_canonical(requestedPath, ec);
    if (ec)
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Imported pack path is invalid"}}.dump());
        return;
    }

    auto requestedIt = canonicalRequested.begin();
    bool insideImports = true;
    for (auto importsIt = canonicalImports.begin(); importsIt != canonicalImports.end(); ++importsIt)
    {
        if (requestedIt == canonicalRequested.end() || *requestedIt != *importsIt)
        {
            insideImports = false;
            break;
        }
        ++requestedIt;
    }

    if (!insideImports)
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Refusing to delete outside tone-sharing imports"}}.dump());
        return;
    }

    ec.clear();
    const bool removed = std::filesystem::remove(canonicalRequested, ec);
    if (ec)
    {
        SendMessageToUI(nlohmann::json{{"type", "toneSharingPackDeleteFailed"}, {"message", "Failed to delete imported pack"}}.dump());
        return;
    }

    nlohmann::json result;
    result["type"] = "toneSharingPackDeleted";
    result["path"] = canonicalRequested.generic_string();
    result["removed"] = removed;
    SendMessageToUI(result.dump());

    AppendSessionLog("Deleted imported tone sharing pack -> " + canonicalRequested.generic_string());
}

void PluginController::HandlePreviewRemoteResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string tempResourceId = payload.value("tempResourceId", "");
    const std::string nodeId = payload.value("nodeId", "");
    const int resourceIndex = payload.value("resourceIndex", 0);
    const std::string data = payload.value("data", "");
    const bool isZip = payload.value("isZip", false);

    if (resourceType.empty() || data.empty()) { AppendSessionLog("Preview failed: missing resource type or data"); return; }

    const std::vector<std::uint8_t> bytes = util::DecodeBase64(data);
    if (bytes.empty()) { AppendSessionLog("Preview failed: invalid base64 payload"); return; }

    const auto tempDir = mFileSystem.ResolveSettingsDirectory() / "temp";
    [[maybe_unused]] const auto ensuredTempDir = mFileSystem.EnsureDirectory(tempDir);

    const std::string extension = resourceType == "ir" ? ".wav" : ".nam";
    std::filesystem::path tempPath = tempDir / ("preview_" + std::to_string(std::hash<std::string>{}(tempResourceId)) + extension);

    if (isZip)
    {
        if (!ExtractFirstResourceFromZip(bytes, resourceType, tempPath))
        { AppendSessionLog("Preview failed: no matching resource in zip"); return; }
    }
    else
    {
        if (!WriteFile(tempPath, bytes))
        { AppendSessionLog("Preview failed: could not write temp file"); return; }
    }

    mPreviewState.active = true;
    mPreviewState.nodeId = nodeId;
    mPreviewState.resourceIndex = resourceIndex;
    mPreviewState.resourceType = resourceType;
    mPreviewState.tempFilePath = tempPath;

    if (mActivePreset)
    {
        GraphNode* node = mActivePreset->graph.FindNode(nodeId);
        if (node && resourceIndex >= 0 && static_cast<size_t>(resourceIndex) < node->resources.size())
            mPreviewState.originalResourceRef = node->resources[resourceIndex];
    }

    if (!nodeId.empty())
    {
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = nodeId;
        updatePayload["resourceType"] = resourceType;
        updatePayload["resourceId"] = "";
        updatePayload["filePath"] = tempPath.string();
        updatePayload["resourceIndex"] = resourceIndex;
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    AppendSessionLog("Preview started: " + resourceType + " at " + tempPath.string());
}

void PluginController::HandleCancelPreviewResourceRequest(const nlohmann::json& payload)
{
    (void)payload;
    if (!mPreviewState.active) return;

    if (!mPreviewState.nodeId.empty() && mPreviewState.originalResourceRef.has_value())
    {
        const auto& original = mPreviewState.originalResourceRef.value();
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = mPreviewState.nodeId;
        updatePayload["resourceType"] = mPreviewState.resourceType;
        updatePayload["resourceId"] = original.resourceId;
        updatePayload["filePath"] = original.filePath.string();
        updatePayload["resourceIndex"] = mPreviewState.resourceIndex;
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    if (!mPreviewState.tempFilePath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(mPreviewState.tempFilePath, ec);
    }

    mPreviewState = PreviewState{};
    AppendSessionLog("Preview cancelled");
}

void PluginController::HandleSaveBlendDefinitionRequest(const nlohmann::json& payload)
{
    const nlohmann::json blend = payload.value("blend", nlohmann::json::object());
    if (!blend.is_object()) { ReportErrorToUI("Blend save failed", "Missing blend payload"); return; }

    const std::string id = blend.value("id", "");
    if (id.empty()) { ReportErrorToUI("Blend save failed", "Missing blend id"); return; }

    const std::string category = blend.value("category", "");
    static const std::array<std::string, 5> allowedCategories = {"pedal", "preamp", "amp", "full-rig", "cab"};
    if (!category.empty())
    {
        if (!std::any_of(allowedCategories.begin(), allowedCategories.end(),
            [&](const std::string& e) { return e == category; }))
        { ReportErrorToUI("Blend save failed", "Invalid category"); return; }
    }

    if (!mBlendLibrary.is_array()) mBlendLibrary = nlohmann::json::array();

    nlohmann::json updated = nlohmann::json::array();
    for (const auto& item : mBlendLibrary)
        if (item.value("id", "") != id) updated.push_back(item);
    updated.push_back(blend);
    mBlendLibrary = std::move(updated);

    SaveBlendLibrary();
    BroadcastState();
}

void PluginController::HandleDeleteBlendDefinitionRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("blendId", "");
    if (id.empty()) { ReportErrorToUI("Blend delete failed", "Missing blend id"); return; }

    if (!mBlendLibrary.is_array()) mBlendLibrary = nlohmann::json::array();

    nlohmann::json updated = nlohmann::json::array();
    bool removed = false;
    for (const auto& item : mBlendLibrary)
    {
        if (item.value("id", "") == id)
        {
            removed = true;
            continue;
        }
        updated.push_back(item);
    }

    if (!removed)
    {
        ReportErrorToUI("Blend delete failed", "Blend not found");
        return;
    }

    mBlendLibrary = std::move(updated);
    SaveBlendLibrary();
    BroadcastState();
}

void PluginController::HandleRequestResourceDataRequest(const nlohmann::json& payload)
{
    const std::string requestId = payload.value("requestId", "");
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");

    if (requestId.empty() || resourceType.empty() || resourceId.empty())
    { SendMessageToUI(nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Missing resource request info"}}.dump()); return; }

    ResourceRef ref;
    ref.resourceType = resourceType;
    ref.resourceId = resourceId;
    const auto resolvedPath = ResolveResourceRef(ref);
    if (!resolvedPath || resolvedPath->empty())
    { SendMessageToUI(nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Resource not found"}}.dump()); return; }

    std::ifstream input(*resolvedPath, std::ios::binary);
    if (!input)
    { SendMessageToUI(nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Failed to open resource file"}}.dump()); return; }

    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (data.empty())
    { SendMessageToUI(nlohmann::json{{"type", "resourceDataFailed"}, {"requestId", requestId}, {"message", "Resource file empty"}}.dump()); return; }

    const std::string encoded = util::EncodeBase64(data);
    nlohmann::json response;
    response["type"] = "resourceData";
    response["requestId"] = requestId;
    response["resourceType"] = resourceType;
    response["resourceId"] = resourceId;
    response["fileName"] = resolvedPath->filename().string();
    response["data"] = encoded;
    SendMessageToUI(response.dump());
}

void PluginController::HandleSaveBlendArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "blend.namz");
    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Save Blend Archive", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Save cancelled"}}.dump()); return; }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(result.path, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "blendExportFailed"}, {"message", "Failed to save file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "blendExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Blend export saved: " + result.path.generic_string());
        });
}

void PluginController::HandleSavePresetArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    std::string suggestedName = util::SanitizeFilename(payload.value("fileName", "preset.soundshed.preset"));
    std::string lowerSuggested = suggestedName;
    std::transform(lowerSuggested.begin(), lowerSuggested.end(), lowerSuggested.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const std::string presetSuffix = ".soundshed.preset";
    while (lowerSuggested.size() >= presetSuffix.size() &&
           lowerSuggested.compare(lowerSuggested.size() - presetSuffix.size(), presetSuffix.size(), presetSuffix) == 0)
    {
        suggestedName.erase(suggestedName.size() - presetSuffix.size());
        lowerSuggested.erase(lowerSuggested.size() - presetSuffix.size());
    }
    if (suggestedName.empty())
    {
        suggestedName = "preset";
    }
    suggestedName += presetSuffix;

    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Save Preset Archive", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Save cancelled"}}.dump()); return; }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(result.path, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Failed to save file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "presetExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Preset export saved: " + result.path.generic_string());
        });
}

void PluginController::HandleSaveLibraryArchiveRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "library.soundshed-library.zip");
    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Save Library Export", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Save cancelled"}}.dump()); return; }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(result.path, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "libraryExportFailed"}, {"message", "Failed to save file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "libraryExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Library export saved: " + result.path.generic_string());
        });
}

void PluginController::HandleDeleteLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    const std::string layoutId = payload.value("layoutId", "");
    if (effectType.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing effect type");
        return;
    }

    if (layoutId.empty())
    {
        ReportErrorToUI("Delete layout failed", "Missing layoutId");
        return;
    }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);
    const auto layoutFile = ResolveLayoutFilePath(mFileSystem, layoutId);

    std::error_code ec;
    if (std::filesystem::exists(layoutFile, ec))
    {
        std::filesystem::remove(layoutFile, ec);
        if (ec)
        {
            ReportErrorToUI("Delete layout failed", "Unable to remove layout file");
            return;
        }
        AppendSessionLog("Layout deleted: " + layoutFile.generic_string());
    }

    // Update associations mapping
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        auto& assoc = settings["associations"];
        if (assoc.contains(lookupKey) && assoc[lookupKey].is_object())
        {
            auto& entry = assoc[lookupKey];
            auto ids = entry.value("layoutIds", nlohmann::json::array());
            if (!ids.is_array()) ids = nlohmann::json::array();

            nlohmann::json updated = nlohmann::json::array();
            for (const auto& id : ids)
            {
                if (id.is_string() && id.get<std::string>() == layoutId)
                    continue;
                updated.push_back(id);
            }
            entry["layoutIds"] = updated;

            const std::string currentDefault = entry.value("defaultLayoutId", "");
            if (currentDefault == layoutId)
            {
                if (!updated.empty() && updated[0].is_string())
                    entry["defaultLayoutId"] = updated[0].get<std::string>();
                else
                    entry["defaultLayoutId"] = "";
            }

            // Remove empty association entries
            if (entry.value("layoutIds", nlohmann::json::array()).empty())
            {
                assoc.erase(lookupKey);
            }
        }
    }
    SaveEffectLayoutsSettings(mFileSystem, settings);

    LoadLayoutLibrary();
}

void PluginController::HandleSaveEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string blendId = payload.value("blendId", "");
    std::string layoutId = payload.value("layoutId", "");
    const auto layoutIt = payload.find("layout");

    if (effectType.empty() || layoutIt == payload.end() || !layoutIt->is_object())
    { ReportErrorToUI("Save layout failed", "Missing effect type or layout data"); return; }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    if (layoutId.empty())
        layoutId = GenerateGuidV4String();

    // Persist layout JSON by GUID filename and embed layoutId into the payload for round-trip.
    nlohmann::json layoutJson = *layoutIt;
    layoutJson["layoutId"] = layoutId;
    SaveLayoutToFile(layoutId, layoutJson);

    // Update association mapping
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
    if (!settings.contains("associations") || !settings["associations"].is_object())
        settings["associations"] = nlohmann::json::object();
    if (!settings["associations"].contains(lookupKey) || !settings["associations"][lookupKey].is_object())
    {
        settings["associations"][lookupKey] = nlohmann::json::object({
            {"defaultLayoutId", layoutId},
            {"layoutIds", nlohmann::json::array()}
        });
    }

    auto& assocEntry = settings["associations"][lookupKey];
    auto ids = assocEntry.value("layoutIds", nlohmann::json::array());
    if (!ids.is_array()) ids = nlohmann::json::array();
    bool found = false;
    for (const auto& id : ids)
    {
        if (id.is_string() && id.get<std::string>() == layoutId)
        {
            found = true;
            break;
        }
    }
    if (!found)
        ids.push_back(layoutId);
    assocEntry["layoutIds"] = ids;
    assocEntry["defaultLayoutId"] = layoutId;

    SaveEffectLayoutsSettings(mFileSystem, settings);

    SendMessageToUI(nlohmann::json{
        {"type", "layoutSaved"},
        {"effectType", effectType},
        {"blendId", blendId},
        {"lookupKey", lookupKey},
        {"layoutId", layoutId},
        {"layout", layoutJson}
    }.dump());

    AppendSessionLog("Effect layout saved: " + lookupKey + " -> " + layoutId);

    // Broadcast updated library so UI can select/apply immediately.
    LoadLayoutLibrary();
}

void PluginController::HandleExportEffectLayoutRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "layout.sgfxlayout.zip");

    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Export Effect Layout", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Export cancelled"}}.dump()); return; }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(result.path, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "layoutExportFailed"}, {"message", "Failed to write file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "layoutExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Layout exported: " + result.path.generic_string());
        });
}

void PluginController::HandleBrowseLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string purpose = payload.value("purpose", "");
    const int layerIndex = payload.value("layerIndex", 0);
    const std::string paramKey = payload.value("paramKey", "");

    mHost.BrowseFileAsync(BrowseFileType::ImageFile, "Select Image",
        [this, purpose, layerIndex, paramKey](const BrowseFileResult& result)
        {
            if (!result.success) return;

            const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
            const auto imagesDir = settingsDir / "layouts" / "content" / "images";
            [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

            const auto selectedPath = result.path;
            const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string imageId = selectedPath.stem().string() + "_" + std::to_string(timestamp);
            const std::string destFilename = imageId + selectedPath.extension().string();
            const auto destPath = imagesDir / destFilename;

            try
            {
                std::filesystem::copy_file(selectedPath, destPath, std::filesystem::copy_options::overwrite_existing);

                std::ifstream imageFile(destPath, std::ios::binary);
                if (!imageFile) { ReportErrorToUI("Image import failed", "Failed to read copied image file"); return; }
                std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(imageFile)), std::istreambuf_iterator<char>());
                imageFile.close();

                const std::string base64Data = util::EncodeBase64(imageData);
                std::string mimeType = "image/png";
                const auto ext = selectedPath.extension().string();
                if (ext == ".jpg" || ext == ".jpeg") mimeType = "image/jpeg";
                const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

                SendMessageToUI(nlohmann::json{
                    {"type", "layoutImageSelected"},
                    {"purpose", purpose},
                    {"imageId", imageId},
                    {"fileName", destFilename},
                    {"dataUrl", dataUrl},
                    {"layerIndex", layerIndex},
                    {"paramKey", paramKey}
                }.dump());
            }
            catch (const std::exception& e)
            {
                AppendSessionLog("Failed to copy layout image: " + std::string(e.what()));
                ReportErrorToUI("Image import failed", "Failed to copy image file");
            }
        });
}

void PluginController::HandleSaveLayoutImageRequest(const nlohmann::json& payload)
{
    const std::string imageId = payload.value("imageId", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string dataEncoded = payload.value("data", "");

    if (imageId.empty() || fileName.empty() || dataEncoded.empty())
    { AppendSessionLog("SaveLayoutImage: missing required fields"); return; }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto imagesDir = settingsDir / "layouts" / "content" / "images";
    [[maybe_unused]] const auto ensuredImagesDir = mFileSystem.EnsureDirectory(imagesDir);

    const auto decodedBytes = util::DecodeBase64(dataEncoded);
    if (decodedBytes.empty()) { AppendSessionLog("SaveLayoutImage: failed to decode base64 data for " + imageId); return; }

    const auto destPath = imagesDir / fileName;
    if (WriteFile(destPath, decodedBytes))
        AppendSessionLog("Layout image saved from import: " + destPath.generic_string());
    else
        AppendSessionLog("SaveLayoutImage: failed to write " + destPath.generic_string());
}

void PluginController::HandleCleanupResourceLibraryRequest(const nlohmann::json& payload)
{
    const nlohmann::json resources = payload.value("resources", nlohmann::json::array());
    const std::string scope = payload.value("scope", "all");
    const bool removeFiles = payload.value("removeFiles", true);

    if (!resources.is_array()) { ReportErrorToUI("Cleanup failed", "Missing resource list"); return; }

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto resourcesDir = settingsDir / "resources";
    const auto libraryDir = resourcesDir / "indexes";
    const auto libraryFile = libraryDir / "resources-index.json";
    const auto resourceFilesDir = resourcesDir / "content";

    nlohmann::json entries = nlohmann::json::array();
    if (std::filesystem::exists(libraryFile))
    {
        std::ifstream input(libraryFile);
        if (input)
        {
            nlohmann::json parsed;
            input >> parsed;
            if (parsed.is_array()) entries = std::move(parsed);
        }
    }

    auto makeKey = [](const std::string& type, const std::string& id) { return type + ":" + id; };

    std::unordered_set<std::string> userKeys;
    for (const auto& e : entries)
    {
        const std::string t = e.value("type", ""), i = e.value("id", "");
        if (!t.empty() && !i.empty()) userKeys.insert(makeKey(t, i));
    }

    std::unordered_set<std::string> usedKeys;
    auto addUsedPreset = [&](const Preset& preset) {
        for (const auto& n : preset.graph.nodes)
            for (const auto& r : n.resources)
                if (r.IsLibraryRef()) usedKeys.insert(makeKey(r.resourceType, r.resourceId));
    };

    if (mActivePreset) addUsedPreset(*mActivePreset);
    if (!mUserPresetsPath.empty() && std::filesystem::exists(mUserPresetsPath))
        for (const auto& p : PresetStorage::LoadAllFromDirectory(mUserPresetsPath)) addUsedPreset(p);

    if (mBlendLibrary.is_array())
        for (const auto& blend : mBlendLibrary)
            if (blend.is_object())
                for (const auto& mid : blend.value("models", nlohmann::json::array()))
                    if (mid.is_string()) usedKeys.insert(makeKey("nam", mid.get<std::string>()));

    auto isScopeMatch = [&](const std::string& type) { return scope == "all" || scope == type; };

    auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec); if (ec) return false;
        auto nb = std::filesystem::weakly_canonical(base, ec); if (ec) return false;
        auto bi = nb.begin(); auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci) { if (ci == nc.end() || *bi != *ci) return false; }
        return true;
    };

    std::vector<std::string> removedKeys;
    std::size_t skipped = 0, skippedUsed = 0;

    for (const auto& item : resources)
    {
        if (!item.is_object()) { ++skipped; continue; }
        const std::string t = item.value("type", ""), i = item.value("id", "");
        if (t.empty() || i.empty()) { ++skipped; continue; }
        if (!isScopeMatch(t)) continue;

        const std::string key = makeKey(t, i);
        if (usedKeys.count(key) > 0) { ++skippedUsed; continue; }

        const auto resourceOpt = mResourceLibrary.LookupResource(t, i);
        if (!resourceOpt) { ++skipped; continue; }

        const bool isUserEntry = userKeys.count(key) > 0;
        const bool isUserFile = !resourceOpt->filePath.empty() && isUnderDirectory(resourceOpt->filePath, resourceFilesDir);
        if (!isUserEntry && !isUserFile) { ++skipped; continue; }

        mResourceLibrary.RemoveResource(t, i);
        removedKeys.push_back(key);

        if (removeFiles && isUserFile)
        { std::error_code ec; std::filesystem::remove(resourceOpt->filePath, ec); }
    }

    if (!removedKeys.empty())
    {
        std::unordered_set<std::string> removedSet(removedKeys.begin(), removedKeys.end());
        nlohmann::json updated = nlohmann::json::array();
        for (const auto& e : entries)
        {
            const std::string t = e.value("type", ""), i = e.value("id", "");
            if (!t.empty() && !i.empty() && removedSet.count(makeKey(t, i)) > 0) continue;
            updated.push_back(e);
        }
        [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryDir);
        std::ofstream output(libraryFile);
        if (output) output << updated.dump(2);
    }

    BroadcastState();
    nlohmann::json msg;
    msg["type"] = "resourceCleanupResult";
    msg["requested"] = resources.size();
    msg["removed"] = removedKeys.size();
    msg["skipped"] = skipped;
    msg["skippedUsed"] = skippedUsed;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSaveCompositeDefinitionRequest(const nlohmann::json& payload)
{
    const nlohmann::json defJson = payload.value("definition", nlohmann::json::object());
    if (!defJson.is_object() || defJson.empty())
    { ReportErrorToUI("Composite save failed", "Missing definition payload"); return; }

    CompositeEffectDefinition def;
    try { def = DeserializeCompositeEffectDefinition(defJson); }
    catch (const std::exception& e) { ReportErrorToUI("Composite save failed", std::string("Invalid definition: ") + e.what()); return; }

    if (!def.IsValid()) { ReportErrorToUI("Composite save failed", "Definition is invalid (missing id/name/innerGraph)"); return; }

    const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
    if (!mCompositeLibrary.SaveDefinition(def, userDir))
    { ReportErrorToUI("Composite save failed", "Could not write definition file"); return; }

    mCompositeLibrary.AddDefinition(def);

    nlohmann::json response;
    response["type"] = "compositeDefinitionAdded";
    response["definition"] = SerializeCompositeEffectDefinition(def);
    SendMessageToUI(response.dump());
    BroadcastState();
}

void PluginController::HandleDeleteCompositeDefinitionRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty()) { ReportErrorToUI("Composite delete failed", "Missing definition id"); return; }

    const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
    mCompositeLibrary.DeleteDefinition(id, userDir);

    nlohmann::json response;
    response["type"] = "compositeDefinitionRemoved";
    response["id"] = id;
    SendMessageToUI(response.dump());
    BroadcastState();
}

void PluginController::HandleEnterCompositeEditModeRequest(const nlohmann::json& payload)
{
    const std::string compositeId = payload.value("compositeId", "");
    if (compositeId.empty())
    {
        ReportErrorToUI("Enter composite edit failed", "Missing compositeId");
        return;
    }

    const auto* def = mCompositeLibrary.GetDefinition(compositeId);
    if (!def)
    {
        ReportErrorToUI("Enter composite edit failed", "Composite not found: " + compositeId);
        return;
    }

    mEditingComposite = *def;
    std::cout << "[Plugin] Entered composite edit mode: " << compositeId
              << " (" << def->name << ")" << std::endl;
    BroadcastCompositeEditState();
}

void PluginController::HandleExitCompositeEditModeRequest(const nlohmann::json& payload)
{
    const bool save = payload.value("save", false);

    if (save && mEditingComposite)
    {
        const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites" / "user";
        if (mCompositeLibrary.SaveDefinition(*mEditingComposite, userDir))
        {
            mCompositeLibrary.AddDefinition(*mEditingComposite);

            nlohmann::json response;
            response["type"] = "compositeDefinitionAdded";
            response["definition"] = SerializeCompositeEffectDefinition(*mEditingComposite);
            SendMessageToUI(response.dump());

            std::cout << "[Plugin] Saved composite from edit mode: " << mEditingComposite->id << std::endl;
        }
        else
        {
            ReportErrorToUI("Composite save failed", "Could not write definition file on exit");
        }
    }

    const std::string exitId = mEditingComposite ? mEditingComposite->id : "";
    mEditingComposite.reset();

    std::cout << "[Plugin] Exited composite edit mode" << (save ? " (saved)" : " (cancelled)") << std::endl;

    nlohmann::json exitMsg;
    exitMsg["type"] = "compositeEditModeExited";
    exitMsg["compositeId"] = exitId;
    exitMsg["saved"] = save;
    SendMessageToUI(exitMsg.dump());

    BroadcastState();
}

void PluginController::HandlePreviewDemoRequest(const nlohmann::json& payload)
{
    if (mDemoPreview)
        mDemoPreview->StartPreview(payload);
}

void PluginController::HandleStopDemoRequest()
{
    if (mDemoPreview)
        mDemoPreview->StopPreview();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        DeactivateRiffGuidance(true);
    }
}

void PluginController::HandleGetRiffLibraryRequest()
{
    SendRiffLibraryStateToUI();
}

void PluginController::HandleSetRiffLibraryPathRequest(const nlohmann::json& payload)
{
    const std::string requestedPath = payload.value("path", "");
    if (requestedPath.empty())
    {
        ReportErrorToUI("Riff Library", "Path is required");
        return;
    }

    try
    {
        const std::filesystem::path libraryPath = std::filesystem::path(requestedPath);
        std::filesystem::create_directories(libraryPath);
        mAppSettings[kRiffLibraryPathSettingKey] = libraryPath.string();
        SaveAppSettings();

        {
            std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
            mRiffLibraryIndex = LoadRiffLibraryIndex();
            mRiffLibraryIndex["path"] = libraryPath.string();
            if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
                mRiffLibraryIndex["riffs"] = nlohmann::json::array();
            SaveRiffLibraryIndex(mRiffLibraryIndex);
        }

        SendRiffLibraryStateToUI();
    }
    catch (const std::exception& ex)
    {
        ReportErrorToUI("Riff Library", ex.what());
    }
}


void PluginController::HandleStartRiffCaptureRequest(const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    if (mRiffCapture.active)
    {
        ReportErrorToUI("Riff Capture", "Capture is already running");
        return;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
    {
        ReportErrorToUI("Riff Capture", "Audio device sample rate is unavailable");
        return;
    }

    RiffCaptureConfig config;
    config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    config.bars = std::max(1, payload.value("bars", 1));
    config.countInBars = std::max(0, payload.value("countInBars", 1));
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.presetId = mActivePresetId;
    config.presetName = mActivePreset ? mActivePreset->name : std::string{};

    const double beatScale = 4.0 / static_cast<double>(config.timeSigDen);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, config.tempoBpm)) * beatScale;
    const std::size_t captureSamples = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));
    const std::size_t countInSamples = config.countInBars > 0
        ? std::max<std::size_t>(0,
            static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.countInBars))))
        : 0;

    mRiffCapture = RiffCaptureRuntime{};
    mRiffCapture.active = true;
    mRiffCapture.complete = false;
    mRiffCapture.takeId = BuildRiffTakeId();
    mRiffCapture.config = config;
    mRiffCapture.left.assign(captureSamples, 0.0f);
    mRiffCapture.right.assign(captureSamples, 0.0f);
    mRiffCapture.writeIndex = 0;
    mRiffCapture.targetSamples = captureSamples + countInSamples;
    mRiffCapture.countInSamples = countInSamples;
    mRiffCapture.sampleRate = sampleRate;
    mRiffCapture.bitsPerSample = 16;
    constexpr std::size_t kLivePeakBuckets = 256;
    mRiffCapture.livePeaks.assign(kLivePeakBuckets, 0.0f);
    mRiffCapture.livePeakBucketSize = std::max<std::size_t>(1, captureSamples / kLivePeakBuckets);
    mRiffCapture.lastProgressSample = 0;
    mRiffCapture.startedAt = std::chrono::steady_clock::now();
    ActivateRiffGuidance(config, false);

    nlohmann::json msg;
    msg["type"] = "riffCaptureStarted";
    msg["takeId"] = mRiffCapture.takeId;
    msg["bars"] = config.bars;
    msg["tempoBpm"] = config.tempoBpm;
    msg["timeSigNum"] = config.timeSigNum;
    msg["timeSigDen"] = config.timeSigDen;
    msg["countInBars"] = config.countInBars;
    msg["estimatedSeconds"] = static_cast<double>(captureSamples) / sampleRate;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleArmRiffCaptureRequest(const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    if (mRiffCapture.active || mRiffCapture.armed)
    {
        ReportErrorToUI("Riff Capture", "Capture or arm is already active");
        return;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
    {
        ReportErrorToUI("Riff Capture", "Audio device sample rate is unavailable");
        return;
    }

    RiffCaptureConfig config;
    config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    // ARM mode: no fixed bar count; allocate 16 bars max
    config.bars = 16;
    config.countInBars = std::max(0, payload.value("countInBars", 1));
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.presetId = mActivePresetId;
    config.presetName = mActivePreset ? mActivePreset->name : std::string{};

    const double beatScale = 4.0 / static_cast<double>(config.timeSigDen);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, config.tempoBpm)) * beatScale;
    const std::size_t maxCaptureSamples = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.bars))));
    const std::size_t countInSamples = config.countInBars > 0
        ? std::max<std::size_t>(0,
            static_cast<std::size_t>(std::llround(samplesPerBeat * static_cast<double>(config.timeSigNum * config.countInBars))))
        : 0;

    constexpr std::size_t kLivePeakBuckets = 256;

    mRiffCapture = RiffCaptureRuntime{};
    mRiffCapture.armed = true;
    mRiffCapture.active = false;
    mRiffCapture.complete = false;
    mRiffCapture.takeId = BuildRiffTakeId();
    mRiffCapture.config = config;
    mRiffCapture.left.assign(maxCaptureSamples, 0.0f);
    mRiffCapture.right.assign(maxCaptureSamples, 0.0f);
    mRiffCapture.writeIndex = 0;
    mRiffCapture.targetSamples = maxCaptureSamples + countInSamples;
    mRiffCapture.countInSamples = countInSamples;
    mRiffCapture.sampleRate = sampleRate;
    mRiffCapture.bitsPerSample = 16;
    mRiffCapture.livePeaks.assign(kLivePeakBuckets, 0.0f);
    mRiffCapture.livePeakBucketSize = std::max<std::size_t>(1, maxCaptureSamples / kLivePeakBuckets);
    mRiffCapture.lastProgressSample = 0;
    mRiffCapture.startedAt = std::chrono::steady_clock::now();
    // Start click playing via guidance (count-in pattern), don't start recording yet
    ActivateRiffGuidance(config, false);

    nlohmann::json msg;
    msg["type"] = "riffCaptureArmed";
    msg["takeId"] = mRiffCapture.takeId;
    msg["tempoBpm"] = config.tempoBpm;
    msg["timeSigNum"] = config.timeSigNum;
    msg["timeSigDen"] = config.timeSigDen;
    msg["countInBars"] = config.countInBars;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleStopRiffCaptureRequest(const nlohmann::json& payload)
{
    const bool canceled = payload.value("canceled", false);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    FinalizeRiffCaptureLocked(canceled);
}

void PluginController::HandleImportRiffWavRequest(const nlohmann::json& payload)
{
    const std::string base64 = payload.value("data", std::string{});
    if (base64.empty())
    {
        ReportErrorToUI("Riff Library", "Dropped WAV data is missing");
        return;
    }

    const auto bytes = util::DecodeBase64(base64);
    if (bytes.empty())
    {
        ReportErrorToUI("Riff Library", "Failed to decode dropped WAV data");
        return;
    }

    const auto decodedOpt = util::DecodePcmWav(bytes);
    if (!decodedOpt)
    {
        ReportErrorToUI("Riff Library", "Unsupported WAV file (expected PCM/float WAV)");
        return;
    }

    const auto& decoded = *decodedOpt;
    if (decoded.channelSamples.empty() || decoded.channelSamples.front().empty())
    {
        ReportErrorToUI("Riff Library", "Dropped WAV has no audio samples");
        return;
    }

    const std::size_t frameCount = decoded.channelSamples.front().size();
    if (frameCount == 0)
    {
        ReportErrorToUI("Riff Library", "Dropped WAV has no audio frames");
        return;
    }

    RiffCaptureRuntime imported;
    imported.active = false;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    imported.config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    imported.config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    imported.config.bars = std::max(1, payload.value("bars", 1));
    imported.config.countInBars = 0;
    imported.config.patternType = payload.value("patternType", std::string("click"));
    imported.config.patternId = payload.value("patternId", std::string{});
    imported.config.presetId = mActivePresetId;
    imported.config.presetName = mActivePreset ? mActivePreset->name : std::string{};
    imported.sampleRate = decoded.sampleRate > 0.0 ? decoded.sampleRate : mHost.GetSampleRate();
    imported.bitsPerSample = decoded.bitsPerSample > 0 ? decoded.bitsPerSample : 16;
    imported.left.resize(frameCount, 0.0f);
    imported.right.resize(frameCount, 0.0f);

    const std::size_t rightChannelIndex = decoded.channelSamples.size() > 1 ? 1u : 0u;
    for (std::size_t i = 0; i < frameCount; ++i)
    {
        imported.left[i] = static_cast<float>(std::clamp(decoded.channelSamples[0][i], -1.0, 1.0));
        imported.right[i] = static_cast<float>(std::clamp(decoded.channelSamples[rightChannelIndex][i], -1.0, 1.0));
    }

    imported.writeIndex = frameCount;
    imported.targetSamples = frameCount;
    imported.countInSamples = 0;
    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = std::move(imported);
        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "import";
    SendMessageToUI(msg.dump());
}

void PluginController::HandleTrimCapturedRiffRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff Library", "No captured take available to trim");
            return;
        }

        const std::size_t totalSamples = mRiffCapture.left.size();
        const double startRatioRaw = payload.value("startRatio", 0.0);
        const double endRatioRaw = payload.value("endRatio", 1.0);
        const double startRatio = std::clamp(startRatioRaw, 0.0, 1.0);
        const double endRatio = std::clamp(endRatioRaw, 0.0, 1.0);

        std::size_t startSample = static_cast<std::size_t>(std::floor(startRatio * static_cast<double>(totalSamples)));
        std::size_t endSample = static_cast<std::size_t>(std::ceil(endRatio * static_cast<double>(totalSamples)));
        startSample = std::min(startSample, totalSamples > 0 ? totalSamples - 1 : 0);
        endSample = std::max(endSample, startSample + 1);
        endSample = std::min(endSample, totalSamples);

        if (startSample >= endSample)
        {
            ReportErrorToUI("Riff Library", "Invalid trim markers");
            return;
        }

        std::vector<float> trimmedLeft(mRiffCapture.left.begin() + startSample,
                           mRiffCapture.left.begin() + endSample);
        std::vector<float> trimmedRight(mRiffCapture.right.begin() + startSample,
                        mRiffCapture.right.begin() + endSample);

        mRiffCapture.left = std::move(trimmedLeft);
        mRiffCapture.right = std::move(trimmedRight);
        mRiffCapture.writeIndex = mRiffCapture.left.size();
        mRiffCapture.targetSamples = mRiffCapture.left.size();
        mRiffCapture.countInSamples = 0;
        mRiffCapture.endedAt = std::chrono::steady_clock::now();

        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "trim";
    SendMessageToUI(msg.dump());
}

void PluginController::HandleLoadRiffTakeForEditRequest(const nlohmann::json& payload)
{
    const std::string takeId = payload.value("takeId", std::string{});
    if (takeId.empty())
    {
        ReportErrorToUI("Riff Library", "Missing takeId for edit");
        return;
    }

    const auto take = FindRiffTakeById(takeId);
    if (!take)
    {
        ReportErrorToUI("Riff Library", "Take not found");
        return;
    }

    const std::string filePath = take->value("filePath", std::string{});
    if (filePath.empty() || !std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Riff Library", "Take WAV file is missing");
        return;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input)
    {
        ReportErrorToUI("Riff Library", "Unable to open take WAV file");
        return;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        ReportErrorToUI("Riff Library", "Take WAV file is empty");
        return;
    }

    const auto decodedOpt = util::DecodePcmWav(bytes);
    if (!decodedOpt)
    {
        ReportErrorToUI("Riff Library", "Unable to decode take WAV file");
        return;
    }

    const auto& decoded = *decodedOpt;
    if (decoded.channelSamples.empty() || decoded.channelSamples.front().empty())
    {
        ReportErrorToUI("Riff Library", "Take WAV has no audio samples");
        return;
    }

    const std::size_t frameCount = decoded.channelSamples.front().size();
    const std::size_t rightChannelIndex = decoded.channelSamples.size() > 1 ? 1u : 0u;

    RiffCaptureRuntime imported;
    imported.active = false;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm = ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    imported.config.timeSigNum = std::max(1, take->value("timeSigNum", 4));
    imported.config.timeSigDen = std::max(1, take->value("timeSigDen", 4));
    imported.config.bars = std::max(1, take->value("bars", 1));
    imported.config.countInBars = 0;
    imported.config.patternType = take->value("patternType", std::string("click"));
    imported.config.patternId = take->value("patternId", std::string{});
    imported.config.presetId = take->value("presetId", std::string{});
    imported.config.presetName = take->value("presetName", std::string{});
    imported.sampleRate = decoded.sampleRate > 0.0 ? decoded.sampleRate : mHost.GetSampleRate();
    imported.bitsPerSample = decoded.bitsPerSample > 0 ? decoded.bitsPerSample : 16;
    imported.left.resize(frameCount, 0.0f);
    imported.right.resize(frameCount, 0.0f);
    for (std::size_t i = 0; i < frameCount; ++i)
    {
        imported.left[i] = static_cast<float>(std::clamp(decoded.channelSamples[0][i], -1.0, 1.0));
        imported.right[i] = static_cast<float>(std::clamp(decoded.channelSamples[rightChannelIndex][i], -1.0, 1.0));
    }
    imported.writeIndex = frameCount;
    imported.targetSamples = frameCount;
    imported.countInSamples = 0;
    imported.startedAt = std::chrono::steady_clock::now();
    imported.endedAt = imported.startedAt;

    RiffCaptureRuntime captureSnapshot;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = std::move(imported);
        captureSnapshot = mRiffCapture;
    }

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = captureSnapshot.takeId;
    msg["capturedSamples"] = captureSnapshot.left.size();
    msg["sampleRate"] = captureSnapshot.sampleRate;
    msg["hasAudio"] = !captureSnapshot.left.empty() && !captureSnapshot.right.empty();
    msg["waveformPeaks"] = BuildWaveformPeaks(captureSnapshot.left, captureSnapshot.right, 256);
    msg["bars"] = captureSnapshot.config.bars;
    msg["tempoBpm"] = captureSnapshot.config.tempoBpm;
    msg["timeSigNum"] = captureSnapshot.config.timeSigNum;
    msg["timeSigDen"] = captureSnapshot.config.timeSigDen;
    msg["patternType"] = captureSnapshot.config.patternType;
    if (!captureSnapshot.config.patternId.empty())
        msg["patternId"] = captureSnapshot.config.patternId;
    msg["source"] = "editLoad";
    msg["originalTakeId"] = takeId;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSaveRiffTakeRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime capture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff Library", "No completed take to save");
            return;
        }
        capture = mRiffCapture;
    }

    if (payload.contains("tempoBpm"))
        capture.config.tempoBpm = ClampValue(payload.value("tempoBpm", capture.config.tempoBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
    if (payload.contains("timeSigNum"))
        capture.config.timeSigNum = std::max(1, payload.value("timeSigNum", capture.config.timeSigNum));
    if (payload.contains("timeSigDen"))
        capture.config.timeSigDen = std::max(1, payload.value("timeSigDen", capture.config.timeSigDen));
    if (payload.contains("bars"))
        capture.config.bars = std::max(1, payload.value("bars", capture.config.bars));
    if (payload.contains("patternType") && payload["patternType"].is_string())
        capture.config.patternType = payload.value("patternType", capture.config.patternType);
    if (payload.contains("patternId") && payload["patternId"].is_string())
        capture.config.patternId = payload.value("patternId", std::string{});
    if (payload.contains("presetId") && payload["presetId"].is_string())
        capture.config.presetId = payload.value("presetId", capture.config.presetId);

    const std::string riffId = payload.value("riffId", std::string{}).empty() ? BuildRiffId() : payload.value("riffId", std::string{});
    const std::string baseTitle = payload.value("title", std::string("New Riff"));
    const std::string safeTitle = util::SanitizeFilename(baseTitle.empty() ? "New Riff" : baseTitle);
    const auto libraryPath = ResolveRiffLibraryPath();
    const auto takesDir = libraryPath / "takes" / riffId;
    const auto fileName = safeTitle + "_" + capture.takeId + ".wav";
    const auto wavPath = takesDir / fileName;

    if (!WriteStereo16BitWav(wavPath, capture.left, capture.right, static_cast<int>(std::llround(capture.sampleRate))))
    {
        ReportErrorToUI("Riff Library", "Failed to write WAV file");
        return;
    }

    nlohmann::json takeJson;
    takeJson["id"] = capture.takeId;
    takeJson["filePath"] = wavPath.string();
    takeJson["durationSec"] = capture.sampleRate > 0.0
        ? static_cast<double>(capture.left.size()) / capture.sampleRate
        : 0.0;
    takeJson["bars"] = capture.config.bars;
    takeJson["tempoBpm"] = capture.config.tempoBpm;
    takeJson["timeSigNum"] = capture.config.timeSigNum;
    takeJson["timeSigDen"] = capture.config.timeSigDen;
    takeJson["patternType"] = capture.config.patternType;
    if (!capture.config.patternId.empty())
        takeJson["patternId"] = capture.config.patternId;
    if (!capture.config.presetId.empty())
        takeJson["presetId"] = capture.config.presetId;
    if (!capture.config.presetName.empty())
        takeJson["presetName"] = capture.config.presetName;
    takeJson["sampleRate"] = capture.sampleRate;
    takeJson["bitsPerSample"] = capture.bitsPerSample;
    takeJson["createdAt"] = BuildTimestampUtcIso();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.is_object())
            mRiffLibraryIndex = nlohmann::json::object();

        mRiffLibraryIndex["path"] = libraryPath.string();
        if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            mRiffLibraryIndex["riffs"] = nlohmann::json::array();

        auto& riffs = mRiffLibraryIndex["riffs"];
        nlohmann::json* riffEntry = nullptr;
        for (auto& riff : riffs)
        {
            if (riff.is_object() && riff.value("id", std::string{}) == riffId)
            {
                riffEntry = &riff;
                break;
            }
        }

        if (!riffEntry)
        {
            nlohmann::json newRiff;
            newRiff["id"] = riffId;
            newRiff["title"] = baseTitle;
            newRiff["categories"] = nlohmann::json::array();
            newRiff["tags"] = nlohmann::json::array();
            newRiff["notes"] = "";
            newRiff["favorite"] = payload.value("favorite", false);
            newRiff["used"] = false;
            newRiff["createdAt"] = BuildTimestampUtcIso();
            newRiff["updatedAt"] = newRiff["createdAt"];
            newRiff["takes"] = nlohmann::json::array();
            riffs.push_back(std::move(newRiff));
            riffEntry = &riffs.back();
        }

        if (riffEntry)
        {
            (*riffEntry)["title"] = baseTitle;
            (*riffEntry)["updatedAt"] = BuildTimestampUtcIso();
            if (payload.contains("categories") && payload["categories"].is_array())
                (*riffEntry)["categories"] = payload["categories"];
            if (payload.contains("tags") && payload["tags"].is_array())
                (*riffEntry)["tags"] = payload["tags"];
            if (payload.contains("notes") && payload["notes"].is_string())
                (*riffEntry)["notes"] = payload["notes"];
            if (payload.contains("favorite") && payload["favorite"].is_boolean())
                (*riffEntry)["favorite"] = payload["favorite"];
            if (!(*riffEntry).contains("takes") || !(*riffEntry)["takes"].is_array())
                (*riffEntry)["takes"] = nlohmann::json::array();
            (*riffEntry)["takes"].push_back(takeJson);
            (*riffEntry)["preferredTakeId"] = capture.takeId;
        }

        SaveRiffLibraryIndex(mRiffLibraryIndex);
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = RiffCaptureRuntime{};
    }

    nlohmann::json msg;
    msg["type"] = "riffSaved";
    msg["riffId"] = riffId;
    msg["takeId"] = capture.takeId;
    msg["path"] = wavPath.string();
    SendMessageToUI(msg.dump());
    SendRiffLibraryStateToUI();
}

void PluginController::HandleDeleteRiffRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
    if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        return;

    auto& riffs = mRiffLibraryIndex["riffs"];
    riffs.erase(std::remove_if(riffs.begin(), riffs.end(),
        [&](const nlohmann::json& riff) { return riff.value("id", std::string{}) == riffId; }), riffs.end());
    SaveRiffLibraryIndex(mRiffLibraryIndex);
    SendRiffLibraryStateToUI();
}

void PluginController::HandleSetRiffFavoriteRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool favorite = payload.value("favorite", false);
    std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
    if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        return;

    for (auto& riff : mRiffLibraryIndex["riffs"])
    {
        if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
            continue;
        riff["favorite"] = favorite;
        riff["updatedAt"] = BuildTimestampUtcIso();
        break;
    }

    SaveRiffLibraryIndex(mRiffLibraryIndex);
    SendRiffLibraryStateToUI();
}

void PluginController::HandleMarkRiffUsedRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool used = payload.value("used", false);
    const std::string songTitle = payload.value("songTitle", std::string{});
    std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
    if (!mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        return;

    for (auto& riff : mRiffLibraryIndex["riffs"])
    {
        if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
            continue;
        riff["used"] = used;
        if (used)
        {
            riff["usedSongTitle"] = songTitle;
            riff["usedAt"] = BuildTimestampUtcIso();
        }
        else
        {
            riff.erase("usedSongTitle");
            riff.erase("usedAt");
        }
        riff["updatedAt"] = BuildTimestampUtcIso();
        break;
    }

    SaveRiffLibraryIndex(mRiffLibraryIndex);
    SendRiffLibraryStateToUI();
}

void PluginController::HandlePreviewRiffTakeRequest(const nlohmann::json& payload)
{
    const std::string takeId = payload.value("takeId", "");
    if (takeId.empty())
    {
        ReportErrorToUI("Riff preview", "Missing takeId");
        return;
    }

    const auto take = FindRiffTakeById(takeId);
    if (!take)
    {
        ReportErrorToUI("Riff preview", "Take not found");
        return;
    }

    const std::string filePath = take->value("filePath", std::string{});
    if (filePath.empty() || !std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Riff preview", "Take WAV file is missing");
        return;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input)
    {
        ReportErrorToUI("Riff preview", "Unable to open take WAV file");
        return;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        ReportErrorToUI("Riff preview", "Take WAV file is empty");
        return;
    }

    nlohmann::json preview;
    preview["audio"] = {
        {"id", takeId},
        {"title", take->value("title", std::string("Riff Take"))},
        {"data", util::EncodeBase64(bytes)},
        {"contentType", "audio/wav"}
    };

    RiffCaptureConfig guideConfig;
    guideConfig.tempoBpm = ClampValue(take->value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    guideConfig.timeSigNum = std::max(1, take->value("timeSigNum", 4));
    guideConfig.timeSigDen = std::max(1, take->value("timeSigDen", 4));
    guideConfig.patternType = take->value("patternType", std::string("click"));
    guideConfig.patternId = take->value("patternId", std::string{});

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            ActivateRiffGuidance(guideConfig, true);
        }
        mDemoPreview->StartPreview(preview);
    }
}

void PluginController::HandlePreviewCapturedRiffRequest(const nlohmann::json& payload)
{
    RiffCaptureRuntime capture;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (!mRiffCapture.complete || mRiffCapture.left.empty() || mRiffCapture.right.empty())
        {
            ReportErrorToUI("Riff preview", "No captured take available yet");
            return;
        }
        capture = mRiffCapture;
    }

    std::vector<float> previewLeft = capture.left;
    std::vector<float> previewRight = capture.right;
    if (!previewLeft.empty() && !previewRight.empty())
    {
        const std::size_t totalSamples = previewLeft.size();
        const double startRatioRaw = payload.value("startRatio", 0.0);
        const double endRatioRaw = payload.value("endRatio", 1.0);
        const double startRatio = std::clamp(startRatioRaw, 0.0, 1.0);
        const double endRatio = std::clamp(endRatioRaw, 0.0, 1.0);

        std::size_t startSample = static_cast<std::size_t>(std::floor(startRatio * static_cast<double>(totalSamples)));
        std::size_t endSample = static_cast<std::size_t>(std::ceil(endRatio * static_cast<double>(totalSamples)));
        startSample = std::min(startSample, totalSamples > 0 ? totalSamples - 1 : 0);
        endSample = std::max(endSample, startSample + 1);
        endSample = std::min(endSample, totalSamples);

        previewLeft = std::vector<float>(previewLeft.begin() + startSample,
                         previewLeft.begin() + endSample);
        previewRight = std::vector<float>(previewRight.begin() + startSample,
                          previewRight.begin() + endSample);
    }

    const auto wavBytes = EncodeStereo16BitWav(
        previewLeft,
        previewRight,
        static_cast<int>(std::llround(capture.sampleRate)));
    if (wavBytes.empty())
    {
        ReportErrorToUI("Riff preview", "Unable to encode captured take");
        return;
    }

    nlohmann::json preview;
    preview["audio"] = {
        {"id", capture.takeId.empty() ? std::string("captured-take") : capture.takeId},
        {"title", std::string("Captured Riff")},
        {"data", util::EncodeBase64(wavBytes)},
        {"contentType", "audio/wav"}
    };

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            ActivateRiffGuidance(capture.config, true);
        }
        mDemoPreview->StartPreview(preview);
    }
}

// ── Additional message handlers (from JUCE version) ────────────────

void PluginController::HandleGetSignalDiagnosticsRequest()
{
    SendSignalDiagnosticsToUI();
}

void PluginController::HandleGetPerformanceStatsRequest()
{
    SendPerformanceStatsToUI();
}

void PluginController::HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload)
{
    bool enabled = payload.value("enabled", false);
    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
    mAppSettings[kSignalDiagnosticsSettingKey] = enabled;
    SaveAppSettings();
}

void PluginController::HandleGetEffectCatalogRequest()
{
    SendEffectCatalogToUI();
}

void PluginController::HandleGetPresetListRequest()
{
    SendPresetListToUI();
}

void PluginController::HandleGetPresetFoldersRequest()
{
    const auto payload = LoadUiStorageJson("preset-folders.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "presetFolders";
    msg["folders"] = payload.value("folders", nlohmann::json::array());
    msg["activeFolderId"] = payload.value("activeFolderId", "__all__");
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetFoldersRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["folders"] = payload.value("folders", nlohmann::json::array());
    toStore["activeFolderId"] = payload.value("activeFolderId", "__all__");
    SaveUiStorageJson("preset-folders.json", toStore);
}

void PluginController::HandleGetPresetFavoritesRequest()
{
    const auto payload = LoadUiStorageJson("preset-favorites.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "presetFavorites";
    msg["favorites"] = payload.value("favorites", nlohmann::json::array());
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetFavoritesRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["favorites"] = payload.value("favorites", nlohmann::json::array());
    SaveUiStorageJson("preset-favorites.json", toStore);
}

void PluginController::HandleGetPresetRatingsRequest()
{
    const auto payload = LoadUiStorageJson("preset-ratings.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "presetRatings";
    msg["ratings"] = payload.value("ratings", nlohmann::json::object());
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetPresetRatingsRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["ratings"] = payload.value("ratings", nlohmann::json::object());
    SaveUiStorageJson("preset-ratings.json", toStore);
}

void PluginController::HandleGetSetlistsRequest()
{
    const auto payload = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    nlohmann::json msg;
    msg["type"] = "setlists";
    msg["setlists"] = payload.value("setlists", nlohmann::json::array());
    msg["activeSetlistId"] = payload.value("activeSetlistId", "");
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetSetlistsRequest(const nlohmann::json& payload)
{
    nlohmann::json toStore = nlohmann::json::object();
    toStore["setlists"] = payload.value("setlists", nlohmann::json::array());
    toStore["activeSetlistId"] = payload.value("activeSetlistId", "");
    SaveUiStorageJson("setlists.json", toStore);
}

void PluginController::HandleGetThemeRequest()
{
    std::string theme = "dark";

    const auto appThemeIt = mAppSettings.find("theme");
    if (appThemeIt != mAppSettings.end() && appThemeIt->is_string())
        theme = appThemeIt->get<std::string>();

    nlohmann::json msg;
    msg["type"] = "theme";
    msg["theme"] = theme;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetThemeRequest(const nlohmann::json& payload)
{
    mAppSettings["theme"] = payload.value("theme", "dark");
    SaveAppSettings();
}

void PluginController::HandleGetAppInfoRequest()
{
    nlohmann::json msg;
    msg["type"] = "appInfo";
    msg["version"] = "1.0.1"; // TODO: Get from build system
    
#if defined(_WIN32)
    msg["os"] = "Windows";
#elif defined(__APPLE__)
    msg["os"] = "macOS";
#elif defined(__linux__)
    msg["os"] = "Linux";
#else
    msg["os"] = "Unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    msg["cpu"] = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    msg["cpu"] = "arm64";
#else
    msg["cpu"] = "Unknown";
#endif

    SendMessageToUI(msg.dump());
}

void PluginController::HandleGetGlobalChainRequest()
{
    SendGlobalChainStateToUI();
}

void PluginController::HandleSetGlobalChainRequest(const nlohmann::json& payload)
{
    // Full global chain config replacement
    if (payload.contains("config"))
    {
        auto config = payload["config"].get<GlobalSignalChainConfig>();
        std::lock_guard<std::mutex> dspLock(mDSPMutex);
        mPresetMixer.SetGlobalChainConfig(config);
    }
    SendGlobalChainStateToUI();
}

void PluginController::HandleSetNodeEnabledRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
}

void PluginController::HandleSetNodeParamRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", "");
    double value = payload.value("value", 0.0);
    mPresetMixer.SetNodeParam(presetId, nodeId, key, value);
}

void PluginController::HandleLoadNodeResourceRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    ResourceRef ref;
    if (payload.contains("resourceType")) ref.resourceType = payload["resourceType"].get<std::string>();
    if (payload.contains("resourceId")) ref.resourceId = payload["resourceId"].get<std::string>();
    if (payload.contains("filePath")) ref.filePath = payload["filePath"].get<std::string>();
    mPresetMixer.LoadNodeResource(presetId, nodeId, ref);
}

void PluginController::HandleSetTunerEnabledRequest(const nlohmann::json& payload)
{
    bool enabled = payload.value("enabled", false);
    mTunerActive.store(enabled, std::memory_order_release);
    mPresetMixer.SetTunerEnabled(enabled);
}

void PluginController::HandleSetTunerReferenceRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    mPresetMixer.SetTunerReferenceFrequency(freq);
}

// ════════════════════════════════════════════════════════════════════
// Internal helpers
// ════════════════════════════════════════════════════════════════════

void PluginController::BroadcastState()
{
    if (!mUIReady) return;

    nlohmann::json state;
    state["type"] = "state";

    // Current preset
    if (mActivePreset)
    {
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(*mActivePreset));
        state["activePresetId"] = mActivePresetId;
    }

    // App settings — UI reads "appSettings"
    state["appSettings"] = mAppSettings;

    // UI settings — UI reads "uiSettings"
    state["uiSettings"] = mUiSettings;

    // UI view state — UI reads "uiViewState"
    state["uiViewState"] = mUiViewState;

    // Global chain — UI reads "globalSignalChain"
    auto chainConfig = mPresetMixer.GetGlobalChainConfig();
    state["globalSignalChain"] = chainConfig;

    // Resource library summary + per-type entries for UI rendering
    nlohmann::json libraryInfo = nlohmann::json::object();
    auto allResources = mResourceLibrary.GetAllResources();
    libraryInfo["totalCount"] = allResources.size();

    for (const auto& resource : allResources)
    {
        const std::string type = resource.type;
        if (!libraryInfo.contains(type) || !libraryInfo[type].is_array())
        {
            libraryInfo[type] = nlohmann::json::array();
        }

        nlohmann::json entry;
        entry["id"] = resource.id;
        entry["name"] = resource.name;
        entry["category"] = resource.category;
        entry["description"] = resource.description;
        entry["filePath"] = resource.filePath.empty() ? "" : resource.filePath.string();
        entry["hash"] = resource.hash;
        if (!resource.metadata.empty())
        {
            entry["metadata"] = resource.metadata;
        }
        const bool hasPath = !resource.filePath.empty();
        const bool exists = hasPath && std::filesystem::exists(resource.filePath);
        entry["fileMissing"] = !(hasPath && exists);

        libraryInfo[type].push_back(entry);
    }

    state["resourceLibrary"] = libraryInfo;

    // Active preset IDs — UI reads "activePresetIds" as string array
    nlohmann::json activePresetIds = nlohmann::json::array();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
        activePresetIds.push_back(id);
    state["activePresetIds"] = activePresetIds;

    // Mixer snapshot
    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();
    mixer["activePresetIds"] = activePresetIds;
    nlohmann::json presetConfigs = nlohmann::json::object();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        if (const auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            presetConfigs[id] = {
                {"name", cfg->name},
                {"mix", cfg->mix},
                {"pan", cfg->pan},
                {"mute", cfg->mute},
                {"solo", cfg->solo}
            };
        }
    }
    mixer["presets"] = std::move(presetConfigs);
    state["mixer"] = std::move(mixer);

    // Metronome
    nlohmann::json metronome;
    metronome["bpm"] = GetEffectiveTempoBpm();
    metronome["enabled"] = mMetronomeEnabled.load();
    metronome["editable"] = mHost.IsStandalone();
    metronome["source"] = mHost.IsStandalone() ? "app" : "host";
    metronome["volumeDb"] = mMetronomeVolumeDb.load();
    metronome["pan"] = mMetronomePan.load();
    metronome["clickType"] = mMetronomeClickType;
    nlohmann::json clickTypes = nlohmann::json::array();
    for (const auto& config : mMetronomeClickConfig)
        clickTypes.push_back({ {"id", config.id}, {"label", config.label} });
    metronome["clickTypes"] = std::move(clickTypes);
    state["metronome"] = metronome;

    // Environment
    state["environment"] = { {"standalone", mHost.IsStandalone()} };

    // Blend library
    state["blendLibrary"] = mBlendLibrary;

    // Riff library
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        state["riffLibrary"] = mRiffLibraryIndex;
    }

    SendMessageToUI(state.dump());

    // Also send supplementary data
    SendCompositeLibraryToUI();
    SendEffectCatalogToUI();
}

void PluginController::ApplyPreset(const Preset& preset)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    // Apply global signal chain config under the DSP lock so the audio thread
    // cannot be inside mPreChainExecutor/mPostChainExecutor.Process() while
    // RebuildGlobalChains() tears down and recreates those executors' node states.
    if (normalizedPreset.globalSignalChain.has_value())
        mPresetMixer.SetGlobalChainConfig(*normalizedPreset.globalSignalChain);
    else
        mPresetMixer.SetGlobalChainConfig(mPresetMixer.GetGlobalChainConfig());

    mActivePreset = normalizedPreset;
    mActivePresetJson = PresetStorage::SerializeToJson(normalizedPreset);

    // Remove existing preset instances and add the new one
    for (const auto& id : mPresetMixer.GetActivePresetIds())
        mPresetMixer.RemoveActivePreset(id);

    mPresetMixer.AddActivePreset(normalizedPreset, "p1", normalizedPreset.name);

    // Register tuner callback
    mPresetMixer.SetTunerCallback(
        [this](const MultiPresetMixer::TunerResult& result)
        {
            std::lock_guard<std::mutex> lock(mTunerMutex);
            mPendingTunerData.noteName = result.noteName;
            mPendingTunerData.octave = result.octave;
            mPendingTunerData.frequency = result.frequency;
            mPendingTunerData.centOffset = result.centOffset;
            mPendingTunerData.confidence = result.confidence;
            mPendingTunerData.detected = result.detected;
            mTunerDataPending.store(true, std::memory_order_release);
        });

    // Queue NAM calibrations for nodes that need them
    for (const auto& node : normalizedPreset.graph.nodes)
    {
        if (!node.resources.empty() && (node.type == "amp_nam" || node.type == "amp_nam_optimized"))
            QueueNamCalibrationForNode(node.id, node.resources[0]);
    }

    mHost.NotifyStateChanged();
}

void PluginController::ApplyBlendDefinitions(Preset& preset)
{
    if (!mBlendLibrary.is_array()) return;

    auto findBlend = [&](const std::string& id) -> nlohmann::json {
        for (const auto& blend : mBlendLibrary)
        {
            if (blend.is_object() && blend.value("id", "") == id) return blend;
        }
        return nlohmann::json::object();
    };

    for (auto& node : preset.graph.nodes)
    {
        if (node.type != "amp_nam_blend") continue;

        const auto blendIt = node.config.find("blendId");
        if (blendIt == node.config.end()) continue;

        const std::string blendId = blendIt->second;
        if (blendId.empty()) continue;

        const nlohmann::json blend = findBlend(blendId);
        if (!blend.is_object()) continue;

        const auto mappingsJson = blend.value("modelMappings", nlohmann::json::array());
        const auto modelsJson = blend.value("models", nlohmann::json::array());
        if ((!mappingsJson.is_array() || mappingsJson.empty()) && (!modelsJson.is_array() || modelsJson.empty()))
            continue;

        node.resources.clear();

        if (mappingsJson.is_array() && !mappingsJson.empty())
        {
            const std::size_t count = mappingsJson.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& mapping = mappingsJson[i];
                if (!mapping.is_object()) continue;

                const std::string modelId = mapping.value("id", "");
                if (modelId.empty()) continue;

                ResourceRef ref;
                ref.resourceType = "nam";
                ref.resourceId = modelId;
                const std::string parameterId = mapping.value("parameterId", "");
                if (!parameterId.empty()) ref.parameterId = parameterId;
                if (mapping.contains("parameterValue") && mapping["parameterValue"].is_number())
                    ref.parameterValue = mapping["parameterValue"].get<double>();
                else if (count > 1)
                    ref.parameterValue = static_cast<double>(i) / static_cast<double>(count - 1);

                if (mapping.contains("parameters") && mapping["parameters"].is_object())
                {
                    for (const auto& [key, value] : mapping["parameters"].items())
                    {
                        if (value.is_number()) ref.parameters[key] = value.get<double>();
                    }
                }

                if (ref.parameters.empty() && !ref.parameterId.empty() && ref.parameterValue.has_value())
                    ref.parameters[ref.parameterId] = *ref.parameterValue;
                else
                    ref.parameterValue = 0.0;

                node.resources.push_back(std::move(ref));
            }
        }
        else if (modelsJson.is_array())
        {
            const std::size_t count = modelsJson.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!modelsJson[i].is_string()) continue;

                ResourceRef ref;
                ref.resourceType = "nam";
                ref.resourceId = modelsJson[i].get<std::string>();
                ref.parameterValue = (count > 1) ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
                node.resources.push_back(std::move(ref));
            }
        }

        const std::string blendMode = blend.value("blendMode", "interpolate");
        node.config["blendMode"] = blendMode;
        if (node.label.empty()) node.label = blend.value("name", "");
    }
}

bool PluginController::ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value)
{
    // In the shared core we don't have direct access to framework-level parameter
    // objects (iPlug2 GetParam / JUCE AudioProcessorParameter).  All DSP parameter
    // routing goes through the PresetMixer which applies values during processing.
    // Framework adapters can intercept or supplement this if they also expose
    // host-automatable parameters.

    if (!mActivePresetId.empty())
    {
        mPresetMixer.SetNodeParam(mActivePresetId, node.id, paramKey, value);
        return true;
    }
    return false;
}

bool PluginController::IsCompositeEditMode() const
{
    return mEditingComposite.has_value();
}

SignalGraph* PluginController::ResolveEditTarget()
{
    if (mEditingComposite)
        return &mEditingComposite->innerGraph;
    if (mActivePreset)
        return &mActivePreset->graph;
    return nullptr;
}

void PluginController::BroadcastCompositeEditState()
{
    if (!mEditingComposite) return;

    nlohmann::json msg;
    msg["type"] = "compositeEditState";
    msg["compositeId"] = mEditingComposite->id;
    msg["name"] = mEditingComposite->name;
    msg["category"] = mEditingComposite->category;
    msg["description"] = mEditingComposite->description;
    msg["author"] = mEditingComposite->author;
    msg["tags"] = mEditingComposite->tags;
    msg["definition"] = SerializeCompositeEffectDefinition(*mEditingComposite);

    nlohmann::json graphJson;
    nlohmann::json nodesArr = nlohmann::json::array();
    for (const auto& node : mEditingComposite->innerGraph.nodes)
    {
        nlohmann::json nj;
        nj["id"] = node.id;
        nj["type"] = node.type;
        nj["displayName"] = node.label;
        nj["category"] = node.category;
        nj["bypassed"] = !node.enabled;
        nj["params"] = nlohmann::json::object();
        for (const auto& [k, v] : node.params) nj["params"][k] = v;
        nj["config"] = nlohmann::json::object();
        for (const auto& [k, v] : node.config) nj["config"][k] = v;
        if (!node.resources.empty())
        {
            nlohmann::json resArr = nlohmann::json::array();
            for (const auto& res : node.resources)
            {
                nlohmann::json rj;
                rj["resourceType"] = res.resourceType;
                rj["resourceId"] = res.resourceId;
                rj["filePath"] = res.filePath;
                rj["embeddedId"] = res.embeddedId;
                rj["parameterId"] = res.parameterId;
                if (res.parameterValue)
                {
                    rj["parameterValue"] = *res.parameterValue;
                }
                else
                {
                    rj["parameterValue"] = nullptr;
                }
                resArr.push_back(rj);
            }
            nj["resources"] = resArr;
        }
        nodesArr.push_back(nj);
    }
    graphJson["nodes"] = nodesArr;

    nlohmann::json edgesArr = nlohmann::json::array();
    for (const auto& edge : mEditingComposite->innerGraph.edges)
    {
        nlohmann::json ej;
        ej["from"] = edge.from;
        ej["to"] = edge.to;
        ej["fromPort"] = edge.fromPort;
        ej["toPort"] = edge.toPort;
        ej["gain"] = edge.gain;
        edgesArr.push_back(ej);
    }
    graphJson["edges"] = edgesArr;
    msg["graph"] = graphJson;

    SendMessageToUI(msg.dump());
}

bool PluginController::UpdateResourceForNodeType(const std::string& nodeType,
                                                 const std::string& resourceType,
                                                 const std::filesystem::path& filePath,
                                                 bool applyPreset)
{
    if (!mActivePreset) return false;

    for (auto& node : mActivePreset->graph.nodes)
    {
        if (node.type == nodeType)
        {
            ResourceRef ref;
            ref.resourceType = resourceType;
            ref.filePath = filePath;
            if (node.resources.empty())
                node.resources.push_back(ref);
            else
                node.resources[0] = ref;

            if (applyPreset)
                ApplyPreset(*mActivePreset);

            mPendingStateBroadcast = true;
            return true;
        }
    }
    return false;
}

bool PluginController::UpdateResourceForNodeId(const std::string& nodeId,
                                               const ResourceRef& ref,
                                               bool applyPreset)
{
    auto* graph = ResolveEditTarget();
    if (!graph) return false;

    auto* node = graph->FindNode(nodeId);
    if (!node) return false;

    if (node->resources.empty())
        node->resources.push_back(ref);
    else
        node->resources[0] = ref;

    if (applyPreset && mActivePreset)
    {
        ApplyPreset(*mActivePreset);
        mPendingStateBroadcast = true;
    }

    return true;
}

std::optional<std::filesystem::path> PluginController::ResolveResourceRef(const ResourceRef& ref) const
{
    return mResourceLibrary.ResolveResource(ref);
}

void PluginController::AppendUserLibraryResource(const LibraryResource& resource)
{
    mResourceLibrary.AddResource(resource);

    const auto libraryFile = mFileSystem.ResolveSettingsDirectory() / "resources" / "indexes" / "resources-index.json";
    [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryFile.parent_path());
    mResourceLibrary.SaveToFile(libraryFile);
}

void PluginController::EnsureBasicGraph()
{
    if (!mActivePreset) return;
    if (mActivePreset->graph.nodes.empty())
    {
        // Create a minimal input → output graph
        GraphNode input;
        input.id = "__input__";
        input.type = kNodeTypeInput;
        GraphNode output;
        output.id = "__output__";
        output.type = kNodeTypeOutput;
        mActivePreset->graph.nodes = {input, output};

        GraphEdge edge;
        edge.from = "__input__";
        edge.to = "__output__";
        mActivePreset->graph.edges = {edge};
    }

    EnsurePresetBoundaryGainNodes(mActivePreset->graph);
}

bool PluginController::ExtractFirstResourceFromZip(const std::vector<std::uint8_t>& /*zipData*/,
                                                   const std::string& /*resourceType*/,
                                                   const std::filesystem::path& /*outputPath*/)
{
    // Zip extraction not yet supported — would require adding miniz or similar dependency.
    // Preview only works with non-zip model downloads.
    AppendSessionLog("Preview from zip not supported - select a non-zip model");
    return false;
}

// ── NAM calibration ────────────────────────────────────────────────

void PluginController::QueueNamCalibrationForNode(const std::string& nodeId,
                                                  const ResourceRef& ref,
                                                  bool force)
{
    if (nodeId.empty()) return;

    const auto resolvedPath = ResolveResourceRef(ref);
    if (!resolvedPath) return;

    const std::string hash = mHasher.HashFile(*resolvedPath);
    if (hash.empty()) return;

    if (mActivePreset)
    {
        if (auto* node = mActivePreset->graph.FindNode(nodeId))
        {
            node->config["modelHash"] = hash;
            ClearNamCalibrationParams(*node);
            if (!node->params.count("autoLevelInput")) node->params["autoLevelInput"] = 1.0;
            if (!node->params.count("autoLevelOutput")) node->params["autoLevelOutput"] = 1.0;
            mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
            mPendingStateBroadcast = true;
        }
    }

    if (force) RemoveNamCalibrationFromCache(hash);

    if (!force)
    {
        if (auto cached = GetNamCalibrationFromCache(hash))
        {
            ApplyNamCalibrationToNode(nodeId, hash, *cached);
            SendNamCalibrationStatus(nodeId, "ready");
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mNamCalibrationMutex);
        auto& waiters = mNamCalibrationWaiters[hash];
        if (std::find(waiters.begin(), waiters.end(), nodeId) == waiters.end())
            waiters.push_back(nodeId);

        if (!mNamCalibrationInFlight.count(hash))
        {
            mNamCalibrationQueue.push_back({hash, *resolvedPath, ref.resourceType, ref.resourceId});
            mNamCalibrationInFlight.insert(hash);
        }
    }

    SendNamCalibrationStatus(nodeId, "calibrating");
    AppendSessionLog("NAM calibration started: " + hash);
    ProcessNamCalibrationQueue();
}

void PluginController::ProcessNamCalibrationQueue()
{
    if (mNamCalibrationFuture && mNamCalibrationFuture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    NamCalibrationJob job;
    {
        std::lock_guard<std::mutex> lock(mNamCalibrationMutex);
        if (mNamCalibrationFuture || mNamCalibrationQueue.empty()) return;

        job = mNamCalibrationQueue.front();
        mNamCalibrationQueue.pop_front();
        mNamCalibrationActiveJob = job;
    }

    const double sampleRate = std::max(1.0, mHost.GetSampleRate());
    const int blockSize = std::max(64, mHost.GetBlockSize());
    mNamCalibrationFuture = std::async(std::launch::async, [job, sampleRate, blockSize]() {
        NamCalibrationResult result;
        result.job = job;
        std::string error;
        if (auto data = RunNamCalibration(job.path, sampleRate, blockSize, error))
        {
            result.success = true;
            result.data = *data;
        }
        else
        {
            result.success = false;
            result.error = error;
        }
        return result;
    });
}

void PluginController::ApplyNamCalibrationResult(const NamCalibrationResult& result)
{
    const std::string& hash = result.job.hash;

    std::vector<std::string> waiters;
    {
        std::lock_guard<std::mutex> lock(mNamCalibrationMutex);
        if (auto it = mNamCalibrationWaiters.find(hash); it != mNamCalibrationWaiters.end())
        {
            waiters = std::move(it->second);
            mNamCalibrationWaiters.erase(it);
        }
        mNamCalibrationInFlight.erase(hash);
    }

    if (!result.success)
    {
        AppendSessionLog("NAM calibration failed: " + result.job.hash +
                         (result.error.empty() ? "" : " (" + result.error + ")"));
        for (const auto& nodeId : waiters)
            SendNamCalibrationStatus(nodeId, "failed");
        return;
    }

    AppendSessionLog("NAM calibration complete: " + result.job.hash);
    StoreNamCalibrationInCache(hash, result.data);

    if (!result.job.resourceType.empty() && !result.job.resourceId.empty())
    {
        if (auto resource = mResourceLibrary.LookupResource(result.job.resourceType, result.job.resourceId))
        {
            auto updated = *resource;
            updated.metadata["calibration.inputLevelDb"] = std::to_string(result.data.inputLevelDb);
            updated.metadata["calibration.outputLevelDb"] = std::to_string(result.data.outputLevelDb);
            mResourceLibrary.UpdateResource(result.job.resourceType, result.job.resourceId, updated);
        }
    }

    for (const auto& nodeId : waiters)
    {
        ApplyNamCalibrationToNode(nodeId, hash, result.data);
        SendNamCalibrationStatus(nodeId, "ready");
    }
}

std::optional<PluginController::NamCalibrationData> PluginController::GetNamCalibrationFromCache(const std::string& hash) const
{
    if (hash.empty()) return std::nullopt;

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto filePath = settingsDir / kNamCalibrationFileName;
    if (!std::filesystem::exists(filePath)) return std::nullopt;

    nlohmann::json root;
    std::ifstream input(filePath);
    if (!input) return std::nullopt;
    try { input >> root; } catch (...) { return std::nullopt; }

    if (!root.is_object() || !root.contains("models") || !root["models"].is_object())
        return std::nullopt;

    const auto& models = root["models"];
    if (!models.contains(hash) || !models[hash].is_object()) return std::nullopt;

    const auto& entry = models[hash];
    NamCalibrationData data;
    data.inputLevelDb = entry.value("inputLevelDb", 0.0);
    data.outputLevelDb = entry.value("outputLevelDb", 0.0);
    return data;
}

void PluginController::StoreNamCalibrationInCache(const std::string& hash, const NamCalibrationData& data)
{
    if (hash.empty()) return;

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto filePath = settingsDir / kNamCalibrationFileName;
    [[maybe_unused]] const auto ensuredCalibrationDir = mFileSystem.EnsureDirectory(filePath.parent_path());

    nlohmann::json root = nlohmann::json::object();
    if (std::filesystem::exists(filePath))
    {
        std::ifstream input(filePath);
        if (input) { try { input >> root; } catch (...) { root = nlohmann::json::object(); } }
    }
    if (!root.is_object()) root = nlohmann::json::object();
    if (!root.contains("models") || !root["models"].is_object()) root["models"] = nlohmann::json::object();

    root["models"][hash] = {
        {"hash", hash},
        {"inputLevelDb", data.inputLevelDb},
        {"outputLevelDb", data.outputLevelDb}
    };

    std::ofstream output(filePath);
    if (output) output << root.dump(2);
}

void PluginController::RemoveNamCalibrationFromCache(const std::string& hash)
{
    if (hash.empty()) return;

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto filePath = settingsDir / kNamCalibrationFileName;
    if (!std::filesystem::exists(filePath)) return;

    nlohmann::json root;
    std::ifstream input(filePath);
    if (!input) return;
    try { input >> root; } catch (...) { return; }

    if (!root.is_object() || !root.contains("models") || !root["models"].is_object()) return;

    auto& models = root["models"];
    if (models.contains(hash))
    {
        models.erase(hash);
        std::ofstream output(filePath);
        if (output) output << root.dump(2);
    }
}

void PluginController::ApplyNamCalibrationToNode(const std::string& nodeId, const std::string& hash, const NamCalibrationData& data)
{
    if (!mActivePreset) return;

    GraphNode* node = mActivePreset->graph.FindNode(nodeId);
    if (!node) return;

    const auto hashIt = node->config.find("modelHash");
    if (hashIt != node->config.end() && hashIt->second != hash) return;

    node->params["calibrationInputLevel"] = data.inputLevelDb;
    node->params["calibrationOutputLevel"] = data.outputLevelDb;
    if (!node->params.count("autoLevelInput")) node->params["autoLevelInput"] = 1.0;
    if (!node->params.count("autoLevelOutput")) node->params["autoLevelOutput"] = 1.0;

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

    if (!mActivePresetId.empty())
    {
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "calibrationInputLevel", data.inputLevelDb);
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "calibrationOutputLevel", data.outputLevelDb);
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "autoLevelInput", node->params["autoLevelInput"]);
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "autoLevelOutput", node->params["autoLevelOutput"]);
    }

    nlohmann::json message;
    message["type"] = "namCalibrationApplied";
    message["nodeId"] = nodeId;
    message["params"] = {
        {"calibrationInputLevel", data.inputLevelDb},
        {"calibrationOutputLevel", data.outputLevelDb},
        {"autoLevelInput", node->params["autoLevelInput"]},
        {"autoLevelOutput", node->params["autoLevelOutput"]}
    };
    SendMessageToUI(message.dump());
    mPendingStateBroadcast = true;
}

void PluginController::ClearNamCalibrationParams(GraphNode& node) const
{
    node.params.erase("calibrationInputLevel");
    node.params.erase("calibrationOutputLevel");
}

void PluginController::SendNamCalibrationStatus(const std::string& nodeId, const std::string& status)
{
    nlohmann::json msg;
    msg["type"] = "namCalibrationStatus";
    msg["nodeId"] = nodeId;
    msg["status"] = status;
    SendMessageToUI(msg.dump());
}

// ── Settings persistence ───────────────────────────────────────────

void PluginController::SaveAppSettings() const
{
    const auto settingsPath = mFileSystem.ResolveSettingsFile();
    if (settingsPath.empty()) return;

    try
    {
        [[maybe_unused]] const auto ensuredSettingsParent = mFileSystem.EnsureDirectory(settingsPath.parent_path());
        std::ofstream ofs(settingsPath);
        if (ofs.is_open())
            ofs << mAppSettings.dump(2);
    }
    catch (const std::exception&) {}
}

void PluginController::LoadAppSettings()
{
    const auto settingsPath = mFileSystem.ResolveSettingsFile();

    if (settingsPath.empty())
    {
        std::cerr << "[Plugin] Settings file path is empty" << std::endl;
        return;
    }

    if (!std::filesystem::exists(settingsPath))
    {
        std::cout << "[Plugin] No settings file found at " << settingsPath.string()
                  << ", using defaults" << std::endl;
        mAppSettings = nlohmann::json::object();
        return;
    }

    try
    {
        std::ifstream ifs(settingsPath);
        if (ifs.is_open())
        {
            mAppSettings = nlohmann::json::parse(ifs);
            std::cout << "[Plugin] Loaded app settings from " << settingsPath.string() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to parse settings: " << e.what() << std::endl;
        mAppSettings = nlohmann::json::object();
    }
}

void PluginController::LoadLastSessionState()
{
    // Restore last-used preset from settings if available
    std::string lastPresetId;
    if (mAppSettings.contains("lastPresetId") && mAppSettings["lastPresetId"].is_string())
    {
        lastPresetId = mAppSettings["lastPresetId"].get<std::string>();
    }

    if (!lastPresetId.empty())
    {
        std::cout << "[Plugin] Restoring last preset: " << lastPresetId << std::endl;
        try
        {
            // Try user presets first, then factory
            std::optional<Preset> presetOpt;
            if (!mUserPresetsPath.empty())
            {
                auto userPath = mUserPresetsPath / (lastPresetId + ".json");
                presetOpt = PresetStorage::LoadFromFile(userPath);
            }
            if (!presetOpt)
            {
                auto factoryPath = mResourceRoot / "presets" / "factory" / (lastPresetId + ".json");
                presetOpt = PresetStorage::LoadFromFile(factoryPath);
            }

            if (presetOpt)
            {
                mActivePresetId = lastPresetId;
                mActivePreset = *presetOpt;
                mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
                ApplyPreset(*presetOpt);
                std::cout << "[Plugin] Restored preset: " << presetOpt->name << std::endl;
            }
            else
            {
                std::cerr << "[Plugin] Last preset not found on disk: " << lastPresetId << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Plugin] Failed to restore last preset: " << e.what() << std::endl;
        }
    }

    mPendingStateBroadcast = true;
    std::cout << "[Plugin] Last session state restored" << std::endl;
}

void PluginController::LoadResourceLibraries()
{
    const auto libraryFile = mFileSystem.ResolveSettingsDirectory() / "resources" / "indexes" / "resources-index.json";
    mResourceLibrary.Clear();
    if (!std::filesystem::exists(libraryFile))
    {
        std::cout << "[Plugin] Resource library file not found: " << libraryFile.string() << std::endl;
        return;
    }

    mResourceLibrary.LoadFromFile(libraryFile);
    std::cout << "[Plugin] Loaded resource library from " << libraryFile.string() << std::endl;
}

void PluginController::LoadBlendLibrary()
{
    const auto blendPath = mFileSystem.ResolveSettingsDirectory() / "blends" / "library.json";
    if (std::filesystem::exists(blendPath))
    {
        try
        {
            std::ifstream ifs(blendPath);
            if (ifs.is_open())
                mBlendLibrary = nlohmann::json::parse(ifs);
        }
        catch (const std::exception&)
        {
            mBlendLibrary = nlohmann::json::array();
        }
    }
}

void PluginController::SaveBlendLibrary() const
{
    const auto blendPath = mFileSystem.ResolveSettingsDirectory() / "blends" / "library.json";
    try
    {
        [[maybe_unused]] const auto ensuredBlendParent = mFileSystem.EnsureDirectory(blendPath.parent_path());
        std::ofstream ofs(blendPath);
        if (ofs.is_open())
            ofs << mBlendLibrary.dump(2);
    }
    catch (const std::exception&) {}
}

void PluginController::LoadCompositeLibrary()
{
    try
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryDir = bundledRoot / "ui" / "assets" / "composites";
        if (std::filesystem::exists(factoryDir))
        {
            mCompositeLibrary.LoadFromDirectory(factoryDir);
            std::cout << "[Plugin] Loaded factory composite definitions from "
                      << factoryDir.string() << ": "
                      << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
        }
        else
        {
            // Backward-compatible fallback for older layouts.
            const auto legacyFactoryDir = mResourceRoot / "composites";
            if (std::filesystem::exists(legacyFactoryDir))
            {
                mCompositeLibrary.LoadFromDirectory(legacyFactoryDir);
                std::cout << "[Plugin] Loaded legacy factory composite definitions from "
                          << legacyFactoryDir.string() << ": "
                          << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
            }
        }

        const auto userDir = mFileSystem.ResolveSettingsDirectory() / "composites";
        if (std::filesystem::exists(userDir))
        {
            mCompositeLibrary.LoadFromDirectory(userDir);
            std::cout << "[Plugin] Composite library total definitions: "
                      << mCompositeLibrary.GetAllDefinitions().size() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to load composite library: " << e.what() << std::endl;
    }
}

void PluginController::LoadLayoutLibrary()
{
    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto layoutsDir = settingsDir / "layouts" / "content";

    nlohmann::json library;
    library["byEffectType"] = nlohmann::json::object();
    library["defaults"] = nlohmann::json::object();
    library["images"] = nlohmann::json::array();

    // Load / migrate associations.
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);

    // Migrate legacy effect-named layouts (effectType[--blendId].layout.json) into GUID-based files.
    // This uses effectType/blendId from the layout JSON only during migration.
    bool migrated = false;
    if (std::filesystem::exists(layoutsDir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(layoutsDir))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;

            const auto filename = entry.path().filename().string();
            if (filename.size() < 11 || filename.substr(filename.size() - 11) != ".layout.json")
                continue;

            const auto stem = entry.path().stem().string();
            const bool isLegacyStem = stem.size() >= 7 && stem.compare(stem.size() - 7, 7, ".layout") == 0;
            if (!isLegacyStem)
            {
                // Non-legacy layout file already (likely GUID-based). Leave it.
                continue;
            }

            try
            {
                std::ifstream input(entry.path());
                if (!input)
                    continue;

                nlohmann::json layoutJson;
                input >> layoutJson;

                const std::string effectType = layoutJson.value("effectType", "");
                if (effectType.empty())
                    continue;

                const std::string blendId = layoutJson.value("blendId", "");
                const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);
                std::string layoutId = layoutJson.value("layoutId", "");
                if (layoutId.empty())
                    layoutId = GenerateGuidV4String();

                layoutJson["layoutId"] = layoutId;

                // Write GUID-based file
                SaveLayoutToFile(layoutId, layoutJson);

                // Update association mapping
                if (!settings.contains("associations") || !settings["associations"].is_object())
                    settings["associations"] = nlohmann::json::object();
                if (!settings["associations"].contains(lookupKey) || !settings["associations"][lookupKey].is_object())
                {
                    settings["associations"][lookupKey] = nlohmann::json::object({
                        {"defaultLayoutId", layoutId},
                        {"layoutIds", nlohmann::json::array()}
                    });
                }
                auto& assocEntry = settings["associations"][lookupKey];
                auto ids = assocEntry.value("layoutIds", nlohmann::json::array());
                if (!ids.is_array()) ids = nlohmann::json::array();
                bool found = false;
                for (const auto& id : ids)
                {
                    if (id.is_string() && id.get<std::string>() == layoutId)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    ids.push_back(layoutId);
                assocEntry["layoutIds"] = ids;
                assocEntry["defaultLayoutId"] = layoutId;

                // Remove legacy file to avoid duplicates
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
                migrated = true;
            }
            catch (const std::exception& e)
            {
                AppendSessionLog("Failed to migrate legacy layout file " + entry.path().generic_string() + ": " + e.what());
            }
        }

        if (migrated)
        {
            SaveEffectLayoutsSettings(mFileSystem, settings);
        }

        const auto appendImagesFromDir = [&library](const std::filesystem::path& imagesDir)
        {
            if (!std::filesystem::exists(imagesDir))
                return;

            for (const auto& entry : std::filesystem::directory_iterator(imagesDir))
            {
                if (!entry.is_regular_file())
                    continue;

                const auto ext = entry.path().extension().string();
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
                    continue;

                std::ifstream imageFile(entry.path(), std::ios::binary);
                if (!imageFile)
                    continue;

                std::vector<std::uint8_t> imageData(
                    (std::istreambuf_iterator<char>(imageFile)),
                    std::istreambuf_iterator<char>());
                imageFile.close();

                const std::string base64Data = util::EncodeBase64(imageData);
                std::string mimeType = "image/png";
                if (ext == ".jpg" || ext == ".jpeg") mimeType = "image/jpeg";
                const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

                const std::string imageId = entry.path().stem().string();
                auto& images = library["images"];
                bool replaced = false;
                if (images.is_array())
                {
                    for (auto& existing : images)
                    {
                        if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                        {
                            existing["fileName"] = entry.path().filename().string();
                            existing["dataUrl"] = dataUrl;
                            replaced = true;
                            break;
                        }
                    }
                }

                if (!replaced)
                {
                    nlohmann::json imageRef;
                    imageRef["imageId"] = imageId;
                    imageRef["fileName"] = entry.path().filename().string();
                    imageRef["dataUrl"] = dataUrl;
                    images.push_back(imageRef);
                }
            }
        };

        // Canonical location for layout images.
        appendImagesFromDir(layoutsDir / "images");
    }

    // Build layout library from mapping (not inferred from filenames).
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const std::string lookupKey = it.key();
            const auto& assocEntry = it.value();
            if (!assocEntry.is_object())
                continue;

            const std::string defaultLayoutId = assocEntry.value("defaultLayoutId", "");
            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());
            if (!ids.is_array())
                continue;

            nlohmann::json entries = nlohmann::json::array();
            for (const auto& id : ids)
            {
                if (!id.is_string())
                    continue;
                const std::string layoutId = id.get<std::string>();
                const auto filePath = ResolveLayoutFilePath(mFileSystem, layoutId);
                if (!std::filesystem::exists(filePath))
                    continue;

                try
                {
                    std::ifstream input(filePath);
                    if (!input)
                        continue;
                    nlohmann::json layoutJson;
                    input >> layoutJson;
                    if (!layoutJson.is_object())
                        continue;

                    // Ensure layoutId is embedded for UI round-trip.
                    layoutJson["layoutId"] = layoutId;

                    nlohmann::json layoutEntry;
                    layoutEntry["layout"] = layoutJson;
                    layoutEntry["isDefault"] = (layoutId == defaultLayoutId);
                    layoutEntry["layoutId"] = layoutId;
                    layoutEntry["filePath"] = filePath.generic_string();
                    entries.push_back(layoutEntry);
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to parse layout file " + filePath.generic_string() + ": " + e.what());
                }
            }

            if (!entries.empty())
            {
                library["byEffectType"][lookupKey] = entries;
                if (!defaultLayoutId.empty())
                    library["defaults"][lookupKey] = defaultLayoutId;
            }
        }
    }

    SendMessageToUI(nlohmann::json{
        {"type", "layoutLibraryLoaded"},
        {"layoutLibrary", library}
    }.dump());
}

void PluginController::SaveLayoutToFile(const std::string& effectType, const nlohmann::json& layoutJson)
{
    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const auto layoutsDir = settingsDir / "layouts" / "content";

    [[maybe_unused]] const auto ensuredLayoutsDir = mFileSystem.EnsureDirectory(layoutsDir);

    const std::string fileStem = util::SanitizeFilename(effectType);
    const auto layoutFile = layoutsDir / (fileStem + ".layout.json");
    std::ofstream output(layoutFile);
    if (output)
    {
        output << layoutJson.dump(2);
        output.close();
        AppendSessionLog("Layout file saved: " + layoutFile.generic_string());
    }
    else
    {
        AppendSessionLog("Failed to write layout file: " + layoutFile.generic_string());
    }
}

std::filesystem::path PluginController::ResolveUiStoragePath(const std::string& filename) const
{
    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();

    if (filename == "preset-folders.json" || filename == "preset-ratings.json")
    {
        const auto dir = settingsDir / "presets";
        [[maybe_unused]] const auto ensuredPresetDir = mFileSystem.EnsureDirectory(dir);
        return dir / filename;
    }

    const auto dir = settingsDir / "settings" / "ui";
    [[maybe_unused]] const auto ensuredUiDir = mFileSystem.EnsureDirectory(dir);
    return dir / filename;
}

nlohmann::json PluginController::LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const
{
    const auto path = ResolveUiStoragePath(filename);
    if (path.empty() || !std::filesystem::exists(path))
        return fallback;

    try
    {
        std::ifstream ifs(path);
        if (ifs.is_open())
            return nlohmann::json::parse(ifs);
    }
    catch (const std::exception&) {}

    return fallback;
}

void PluginController::SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const
{
    const auto path = ResolveUiStoragePath(filename);
    if (path.empty())
        return;

    try
    {
        [[maybe_unused]] const auto ensuredUiStorageParent = mFileSystem.EnsureDirectory(path.parent_path());
        std::ofstream ofs(path);
        if (ofs.is_open())
            ofs << payload.dump(2);
    }
    catch (const std::exception&) {}
}

std::filesystem::path PluginController::ResolveRiffLibraryPath() const
{
    if (mAppSettings.contains(kRiffLibraryPathSettingKey) && mAppSettings[kRiffLibraryPathSettingKey].is_string())
    {
        const auto configured = std::filesystem::path(mAppSettings[kRiffLibraryPathSettingKey].get<std::string>());
        if (!configured.empty())
            return configured;
    }

    return mFileSystem.ResolveSettingsDirectory() / kRiffLibraryDefaultFolder;
}

std::filesystem::path PluginController::ResolveRiffLibraryIndexPath() const
{
    return ResolveRiffLibraryPath() / kRiffLibraryIndexFile;
}

nlohmann::json PluginController::LoadRiffLibraryIndex() const
{
    nlohmann::json index = nlohmann::json::object();
    const auto path = ResolveRiffLibraryPath();
    const auto indexPath = ResolveRiffLibraryIndexPath();

    try
    {
        std::filesystem::create_directories(path);
        if (std::filesystem::exists(indexPath))
        {
            std::ifstream input(indexPath);
            if (input)
            {
                index = nlohmann::json::parse(input, nullptr, false);
                if (index.is_discarded() || !index.is_object())
                    index = nlohmann::json::object();
            }
        }
    }
    catch (...)
    {
        index = nlohmann::json::object();
    }

    index["path"] = path.string();
    if (!index.contains("riffs") || !index["riffs"].is_array())
        index["riffs"] = nlohmann::json::array();

    for (auto& riff : index["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
            continue;

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                continue;

            const auto storedPath = std::filesystem::path(take["filePath"].get<std::string>());
            if (storedPath.empty())
                continue;

            const auto resolvedPath = ResolveRiffTakePathForRuntime(storedPath, path);
            const bool resolvedExists = !resolvedPath.empty() && resolvedPath != storedPath && std::filesystem::exists(resolvedPath);
            const bool storedExists = std::filesystem::exists(storedPath);

            if (resolvedExists)
                take["filePath"] = resolvedPath.string();
            else if (!storedExists && !resolvedPath.empty())
                take["filePath"] = resolvedPath.string();
        }
    }

    return index;
}

bool PluginController::SaveRiffLibraryIndex(const nlohmann::json& payload) const
{
    const auto indexPath = ResolveRiffLibraryIndexPath();
    const auto libraryPath = ResolveRiffLibraryPath();
    nlohmann::json normalizedPayload = payload;

    normalizedPayload["path"] = libraryPath.string();
    if (!normalizedPayload.contains("riffs") || !normalizedPayload["riffs"].is_array())
        normalizedPayload["riffs"] = nlohmann::json::array();

    for (auto& riff : normalizedPayload["riffs"])
    {
        if (!riff.is_object() || !riff.contains("takes") || !riff["takes"].is_array())
            continue;

        for (auto& take : riff["takes"])
        {
            if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                continue;

            const auto runtimePath = std::filesystem::path(take["filePath"].get<std::string>());
            const auto storedPath = BuildRiffTakePathForStorage(runtimePath, libraryPath);
            take["filePath"] = storedPath.string();
        }
    }

    try
    {
        std::filesystem::create_directories(indexPath.parent_path());
        std::ofstream output(indexPath);
        if (!output)
            return false;
        output << normalizedPayload.dump(2);
        return static_cast<bool>(output);
    }
    catch (...)
    {
        return false;
    }
}

std::string PluginController::BuildRiffTakeId() const
{
    return "take-" + GenerateGuidV4String();
}

std::string PluginController::BuildRiffId() const
{
    return "riff-" + GenerateGuidV4String();
}

std::string PluginController::BuildTimestampUtcIso() const
{
    return BuildUtcIsoTimestamp();
}

std::optional<nlohmann::json> PluginController::FindRiffTakeById(const std::string& takeId) const
{
    std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
    if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
        return std::nullopt;

    for (const auto& riff : mRiffLibraryIndex["riffs"])
    {
        if (!riff.is_object())
            continue;
        const std::string riffTitle = riff.value("title", std::string("Riff"));
        const auto takes = riff.value("takes", nlohmann::json::array());
        if (!takes.is_array())
            continue;
        for (const auto& take : takes)
        {
            if (!take.is_object() || take.value("id", std::string{}) != takeId)
                continue;
            nlohmann::json result = take;
            result["title"] = riffTitle;
            return result;
        }
    }

    return std::nullopt;
}

void PluginController::FinalizeRiffCaptureLocked(bool canceled)
{
    if (!mRiffCapture.active && !mRiffCapture.armed && !mRiffCapture.complete)
        return;

    if (canceled)
    {
        const std::string takeId = mRiffCapture.takeId;
        mRiffCapture = RiffCaptureRuntime{};
        DeactivateRiffGuidance(false);
        nlohmann::json msg;
        msg["type"] = "riffCaptureCanceled";
        msg["takeId"] = takeId;
        SendMessageToUI(msg.dump());
        return;
    }

    // If still armed (never triggered), cancel instead of producing empty audio
    if (mRiffCapture.armed && !mRiffCapture.active)
    {
        const std::string takeId = mRiffCapture.takeId;
        mRiffCapture = RiffCaptureRuntime{};
        DeactivateRiffGuidance(false);
        nlohmann::json msg;
        msg["type"] = "riffCaptureCanceled";
        msg["takeId"] = takeId;
        SendMessageToUI(msg.dump());
        return;
    }

    const std::size_t written = std::min(mRiffCapture.writeIndex, mRiffCapture.targetSamples);
    const std::size_t captured = written > mRiffCapture.countInSamples ? (written - mRiffCapture.countInSamples) : 0;
    if (captured < mRiffCapture.left.size())
        mRiffCapture.left.resize(captured);
    if (captured < mRiffCapture.right.size())
        mRiffCapture.right.resize(captured);

    mRiffCapture.active = false;
    mRiffCapture.armed = false;
    mRiffCapture.complete = captured > 0;
    mRiffCapture.endedAt = std::chrono::steady_clock::now();
    DeactivateRiffGuidance(false);

    // Compute bars from actual captured length
    const double samplesPerBeat = mRiffCapture.sampleRate
        * (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm))
        * (4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen)));
    const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
    const int computedBars = std::max(1, static_cast<int>(
        std::round(static_cast<double>(captured) / std::max(1.0, samplesPerBar))));

    nlohmann::json msg;
    msg["type"] = "riffCaptureStopped";
    msg["takeId"] = mRiffCapture.takeId;
    msg["bars"] = computedBars;
    msg["capturedSamples"] = captured;
    msg["sampleRate"] = mRiffCapture.sampleRate;
    msg["hasAudio"] = captured > 0;
    msg["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
    SendMessageToUI(msg.dump());
}

// ── Messaging helpers ──────────────────────────────────────────────

void PluginController::SendGlobalChainStateToUI()
{
    nlohmann::json msg;
    msg["type"] = "globalChain";
    msg["config"] = mPresetMixer.GetGlobalChainConfig();
    SendMessageToUI(msg.dump());
}

void PluginController::SendCompositeLibraryToUI()
{
    nlohmann::json msg;
    msg["type"] = "compositeLibrary";
    nlohmann::json defs = nlohmann::json::array();
    for (const auto& def : mCompositeLibrary.GetAllDefinitions())
        defs.push_back(SerializeCompositeEffectDefinition(def));
    msg["definitions"] = defs;
    SendMessageToUI(msg.dump());
}

void PluginController::SendEffectCatalogToUI()
{
    auto& registry = EffectRegistry::Instance();
    auto types = registry.GetAllTypes();

    nlohmann::json msg;
    msg["type"] = "effectCatalog";
    nlohmann::json catalog = nlohmann::json::array();
    for (const auto& info : types)
    {
        nlohmann::json entry;
        entry["type"] = info.type;
        entry["name"] = info.displayName;
        entry["category"] = info.category;
        entry["requiresResource"] = info.requiresResource;
        if (!info.resourceType.empty())
            entry["resourceType"] = info.resourceType;

        nlohmann::json params = nlohmann::json::array();
        for (const auto& p : info.parameters)
        {
            nlohmann::json param;
            param["key"] = p.id;
            param["name"] = p.displayName;
            param["min"] = p.minValue;
            param["max"] = p.maxValue;
            param["default"] = p.defaultValue;
            param["unit"] = p.unit;
            if (!p.group.empty())
                param["group"] = p.group;
            if (p.advanced)
                param["advanced"] = true;
            params.push_back(param);
        }
        entry["parameters"] = params;

        if (info.type.rfind("composite:", 0) == 0)
        {
            const std::string definitionId = info.type.substr(std::string("composite:").size());
            if (const auto* def = mCompositeLibrary.GetDefinition(definitionId))
            {
                nlohmann::json exposedResources = nlohmann::json::array();
                for (const auto& er : def->exposedResources)
                {
                    nlohmann::json resource;
                    resource["resourceId"] = er.resourceId;
                    resource["displayName"] = er.displayName;
                    resource["nodeId"] = er.nodeId;
                    resource["resourceType"] = er.resourceType;
                    resource["resourceIndex"] = er.resourceIndex;
                    resource["allowBrowseFile"] = er.allowBrowseFile;
                    if (!er.parameterId.empty())
                        resource["parameterId"] = er.parameterId;
                    if (er.parameterValue.has_value())
                        resource["parameterValue"] = *er.parameterValue;
                    exposedResources.push_back(resource);
                }
                entry["exposedResources"] = exposedResources;
            }
        }

        catalog.push_back(entry);
    }
    msg["catalog"] = catalog;
    SendMessageToUI(msg.dump());
}

void PluginController::SendPresetListToUI()
{
    // Scan preset directories and send list
    nlohmann::json msg;
    msg["type"] = "presetList";
    nlohmann::json presets = nlohmann::json::array();

    auto factoryPath = mResourceRoot / "presets" / "factory";
    auto userPath = mUserPresetsPath;

    auto scanDir = [&](const std::filesystem::path& dir, const std::string& source)
    {
        if (!std::filesystem::exists(dir)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".json")
            {
                try
                {
                    auto presetOpt = PresetStorage::LoadFromFile(entry.path());
                    if (!presetOpt) continue;
                    auto& preset = *presetOpt;
                    nlohmann::json p;
                    p["id"] = preset.id;
                    p["name"] = preset.name;
                    p["category"] = preset.category;
                    p["source"] = source;
                    presets.push_back(p);
                }
                catch (...) {}
            }
        }
    };

    scanDir(factoryPath, "factory");
    scanDir(userPath, "user");

    msg["presets"] = presets;
    SendMessageToUI(msg.dump());
}

void PluginController::SendSignalDiagnosticsToUI()
{
    auto snapshot = mPresetMixer.GetSignalDiagnosticsSnapshot();
    nlohmann::json msg;
    msg["type"] = "signalLevelDiagnostics";

    auto buildLevelJson = [](const MultiPresetMixer::SignalLevelStats& stats)
    {
        const double peakDb = ToDbFS(stats.peak);
        const double rmsDb = ToDbFS(stats.rms);
        const double headroomDb = HeadroomDbFromPeak(stats.peak);
        const bool clipped = stats.clipCount > 0 || stats.peak >= 1.0;
        return nlohmann::json{
            {"peak", stats.peak},
            {"rms", stats.rms},
            {"peakDbfs", peakDb},
            {"rmsDbfs", rmsDb},
            {"headroomDb", headroomDb},
            {"clipped", clipped},
            {"clipCount", stats.clipCount},
        };
    };

    msg["rawInput"] = buildLevelJson(snapshot.rawInput);
    msg["input"] = buildLevelJson(snapshot.input);
    msg["output"] = buildLevelJson(snapshot.output);

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& n : snapshot.nodes)
    {
        nlohmann::json node;
        node["scope"] = n.scope;
        node["presetId"] = n.presetId;
        node["nodeId"] = n.nodeId;
        node["nodeType"] = n.nodeType;
        node["levels"] = buildLevelJson(n.levels);
        nodes.push_back(node);
    }
    msg["nodes"] = nodes;
    SendMessageToUI(msg.dump());
}

void PluginController::SendPerformanceStatsToUI()
{
    auto stats = mPresetMixer.GetPerformanceStats();
    nlohmann::json statsJson;
    statsJson["totalProcessingTimeUs"] = stats.totalProcessingTimeUs;
    statsJson["realTimeUs"] = stats.realTimeUs;
    statsJson["dspLoadPercent"] = stats.dspLoadPercent;
    nlohmann::json nodeTimes = nlohmann::json::object();
    for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
        nodeTimes[nodeId] = timeUs;
    statsJson["nodeProcessingTimesUs"] = nodeTimes;

    nlohmann::json msg;
    msg["type"] = "dspPerformance";
    msg["stats"] = statsJson;
    msg["sampleRate"] = mHost.GetSampleRate();
    msg["blockSize"] = mHost.GetBlockSize();
    SendMessageToUI(msg.dump());
}

void PluginController::SendMetronomeStateToUI()
{
    nlohmann::json msg;
    msg["type"] = "metronomeState";
    msg["bpm"] = GetEffectiveTempoBpm();
    msg["enabled"] = mMetronomeEnabled.load();
    msg["editable"] = mHost.IsStandalone();
    msg["source"] = mHost.IsStandalone() ? "app" : "host";
    msg["volumeDb"] = mMetronomeVolumeDb.load();
    msg["pan"] = mMetronomePan.load();
    msg["clickType"] = mMetronomeClickType;
    nlohmann::json clickTypes = nlohmann::json::array();
    for (const auto& config : mMetronomeClickConfig)
        clickTypes.push_back({ {"id", config.id}, {"label", config.label} });
    msg["clickTypes"] = std::move(clickTypes);
    SendMessageToUI(msg.dump());
}

void PluginController::SendRiffLibraryStateToUI()
{
    nlohmann::json msg;
    msg["type"] = "riffLibraryState";
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.is_object())
            mRiffLibraryIndex = LoadRiffLibraryIndex();
        msg["library"] = mRiffLibraryIndex;
    }

    nlohmann::json capture;
    capture["active"] = mRiffCapture.active;
    capture["complete"] = mRiffCapture.complete;
    capture["takeId"] = mRiffCapture.takeId;
    capture["bars"] = mRiffCapture.config.bars;
    capture["tempoBpm"] = mRiffCapture.config.tempoBpm;
    capture["timeSigNum"] = mRiffCapture.config.timeSigNum;
    capture["timeSigDen"] = mRiffCapture.config.timeSigDen;
    capture["capturedSamples"] = mRiffCapture.left.size();
    capture["sampleRate"] = mRiffCapture.sampleRate;
    capture["hasAudio"] = !mRiffCapture.left.empty() && !mRiffCapture.right.empty();
    capture["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
    msg["capture"] = capture;

    SendMessageToUI(msg.dump());
}

bool PluginController::WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const
{
    try
    {
        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace guitarfx
