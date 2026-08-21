/**
 * PluginController.cpp — Framework-agnostic plugin orchestration.
 *
 * This file contains the shared business logic that was previously
 * duplicated across host-framework plugin entry points.
 *
 * Implementation strategy:
 *   Handler methods here replace framework-specific code paths by routing
 *   all host operations through the IPluginHost interface.
 */
#include "PluginController.h"
#include "MessageDispatcher.h"
#include "controller/DemoPreviewService.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/LevelTargets.h"
#include "dsp/BlockSincResampler.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/InputAnalyzerEffect.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#if defined(GUITARFX_ENABLE_WASM_EFFECTS)
#include "dsp/effects/WasmEffect.h"
#endif
#include "presets/CompositePresetStorage.h"
#include "presets/CompositePresetTypes.h"
#include "util/AudioDecoder.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathSanitizer.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <unordered_set>

#include "GuitarFXConfig.h"
#include "NAM/get_dsp.h"

namespace
{
    struct FactoryArchiveResourceEntry
    {
        std::string id;
        std::string name;
        std::string category;
        std::string type;
        std::string fileName;
        std::string hash;
        std::vector<std::uint8_t> bytes;
    };

    struct ParsedFactoryPresetArchive
    {
        std::vector<guitarfx::Preset> presets;
        std::vector<nlohmann::json> blends;
        std::vector<FactoryArchiveResourceEntry> resources;
        nlohmann::json presetFolders = nlohmann::json::array();
        std::size_t tone3000ResourceCount = 0;
    };

    constexpr const char* kJamYouTubeApiKeySettingKey = "jam.youtubeApiKey";
    constexpr const char* kBundledJamYouTubeApiKey = "";
    constexpr const char* kFactoryArchiveResourceProvider = "factory-archives";
    constexpr const char* kLocalResourceProvider = "local";
    constexpr const char* kLocalResourceStorageFolder = "local";
    constexpr const char* kHostedPluginStableIdConfigKey = "pluginStableId";
    constexpr const char* kHostedPluginIdentifierConfigKey = "pluginIdentifier";
    constexpr const char* kHostedPluginNameConfigKey = "pluginName";
    constexpr const char* kHostedPluginManufacturerConfigKey = "pluginManufacturer";
    constexpr const char* kHostedPluginFormatConfigKey = "pluginFormat";
    constexpr const char* kHostedPluginLastErrorCodeConfigKey = "lastErrorCode";
    constexpr const char* kFactoryArchiveStateFileName = "factory-archive-state.json";
    constexpr int kFactoryArchiveStateSchemaVersion = 1;
    constexpr const char* kFactoryArchiveLoadingEnabledSettingKey = "factoryPresets.archiveLoadingEnabled";
    constexpr const char* kPresetArchiveSessionRootFolder = "sessions/preset-archive";
    constexpr const char* kPresetArchiveSessionResourceProvider = "preset-archive-session";

    constexpr double kMinLinear = 1e-6;

    /// Maximum number of DSP performance stats messages sent to the UI per second.
    /// Adjust this to trade display update rate against IPC overhead.
    constexpr int kDspPerformanceStatsRateHz = 5;
    constexpr int kSignalDiagnosticsRateHz = 20;

    /// How often the spatialiser's live source position is pushed to the UI. Fast
    /// enough that a moving puck looks continuous, slow enough to be negligible.
    constexpr int kSpatialPositionRateHz = 20;

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
    constexpr const char* kMetronomeBeatPatternSettingKey = "metronome.beatPattern";
    constexpr double kMetronomeClickSeconds = 0.02;

    // Returns 'H' (High/accent), 'L' (Low), or 'S' (Silent) for a beat position.
    // Empty pattern = first beat High, rest Low.
    static char BeatAccent(const std::string& pattern, int beatIndex)
    {
        if (pattern.empty())
            return (beatIndex == 0) ? 'H' : 'L';
        const std::size_t idx = static_cast<std::size_t>(beatIndex) % pattern.size();
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(pattern[idx])));
        if (c == 'H') return 'H';
        if (c == 'S' || c == '-' || c == '.') return 'S';
        return 'L';
    }
    constexpr double kMetronomeClickFrequencyHz = 1800.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr const char* kRiffLibraryPathSettingKey = "riffLibrary.path";
    constexpr const char* kRiffLibraryDefaultFolder = "riff-library";
    constexpr const char* kRiffLibraryIndexFile = "riff-library-index.json";
    constexpr const char* kSignalDiagnosticsSettingKey = "diagnostics.signalLevelsEnabled";
    constexpr const char* kNominalOperatingLevelSettingKey = "audio.dsp.nominalOperatingLevelDbfs";
    constexpr const char* kOutputProtectionCeilingSettingKey = "audio.dsp.outputProtectionCeilingDbfs";
    constexpr const char* kMultiThreadedProcessingSettingKey = "audio.processing.multiThreaded";
    constexpr const char* kGlobalFxSettingsKey = "globalFx.settings";
    constexpr const char* kNamSlimmableSizeSettingKey = "audio.nam.slimmableSize";
    constexpr const char* kNamSlimmableNodeConfigKey = "slimmableSize";
    constexpr const char* kNamInterfaceCalibrationLevelDbuSettingKey = "audio.nam.interfaceCalibrationLevelDbu";
    constexpr double kNamInterfaceCalibrationLevelDbuDefault = 12.0;
    constexpr double kNamInterfaceCalibrationLevelDbuMin = 0.0;
    constexpr double kNamInterfaceCalibrationLevelDbuMax = 24.0;
    constexpr const char* kNamAutoInputCalibrationSettingKey = "audio.nam.autoInputCalibration";
    constexpr const char* kUserInputCalibrationProfilesSettingKey = "audio.userInputCalibration.profiles";
    constexpr const char* kUserInputCalibrationActiveProfileIdSettingKey = "audio.userInputCalibration.activeProfileId";
    constexpr const char* kLegacyInterfaceCalibrationEnabledSettingKey = "audio.interfaceCalibration.enabled";
    constexpr const char* kLegacyInterfaceCalibrationReferenceDbuSettingKey = "audio.interfaceCalibration.referenceDbu";
    constexpr const char* kSessionLogFileName = "logs/session-log.txt";
    constexpr const char* kDebugSnapshotFileName = "logs/debug-state.json";
    constexpr const char* kSharedSyncStateFileName = "settings/shared-sync-state.json";
    constexpr auto kSharedSyncPollInterval = std::chrono::milliseconds(2000);

    bool IsSensitiveDebugKey(std::string_view key)
    {
        if (key.empty())
            return false;

        std::string normalizedKey(key);
        std::transform(normalizedKey.begin(), normalizedKey.end(), normalizedKey.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });

        return normalizedKey.find("token") != std::string::npos
            || normalizedKey.find("api_key") != std::string::npos
            || normalizedKey.find("apikey") != std::string::npos
            || normalizedKey.find("secret") != std::string::npos
            || normalizedKey.find("password") != std::string::npos
            || normalizedKey.find("authorization") != std::string::npos
            || normalizedKey.find("cookie") != std::string::npos
            || normalizedKey.find("credential") != std::string::npos;
    }

    void ScrubSensitiveJson(nlohmann::json& value, std::string_view currentKey = {})
    {
        if (IsSensitiveDebugKey(currentKey))
        {
            value = "<redacted>";
            return;
        }

        if (value.is_object())
        {
            for (auto it = value.begin(); it != value.end(); ++it)
                ScrubSensitiveJson(it.value(), it.key());
            return;
        }

        if (value.is_array())
        {
            for (auto& entry : value)
                ScrubSensitiveJson(entry);
        }
    }

    std::filesystem::path ResolveDebugSnapshotPath(const guitarfx::FileSystem& fileSystem)
    {
        return fileSystem.ResolveSettingsDirectory() / kDebugSnapshotFileName;
    }

    std::filesystem::path ResolveSharedSyncStatePath(const guitarfx::FileSystem& fileSystem)
    {
        return fileSystem.ResolveSettingsDirectory() / kSharedSyncStateFileName;
    }

    double ToDbFS(double linear)
    {
        if (linear <= kMinLinear) return -120.0;
        return 20.0 * std::log10(linear);
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

    int ComputeBarsFromFrames(std::size_t frameCount,
                              double sampleRate,
                              double tempoBpm,
                              int timeSigNum,
                              int timeSigDen)
    {
        if (frameCount == 0)
            return 1;

        const double samplesPerBeat = sampleRate
            * (60.0 / std::max(1.0, tempoBpm))
            * (4.0 / static_cast<double>(std::max(1, timeSigDen)));
        const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, timeSigNum));
        return std::max(1, static_cast<int>(
            std::round(static_cast<double>(frameCount) / std::max(1.0, samplesPerBar))));
    }

    std::string NormalizeHostedPluginIdentityToken(std::string_view value)
    {
        std::string normalized;
        normalized.reserve(value.size());

        bool lastWasSeparator = false;
        for (const char raw : value)
        {
            const unsigned char ch = static_cast<unsigned char>(raw);
            if (std::isalnum(ch))
            {
                normalized.push_back(static_cast<char>(std::tolower(ch)));
                lastWasSeparator = false;
                continue;
            }

            if (!normalized.empty() && !lastWasSeparator)
            {
                normalized.push_back('-');
                lastWasSeparator = true;
            }
        }

        while (!normalized.empty() && normalized.back() == '-')
            normalized.pop_back();

        return normalized;
    }

    std::string BuildHostedPluginStableId(std::string_view manufacturer,
                                          std::string_view pluginName)
    {
        const std::string normalizedManufacturer = NormalizeHostedPluginIdentityToken(manufacturer);
        const std::string normalizedName = NormalizeHostedPluginIdentityToken(pluginName);
        if (!normalizedManufacturer.empty() && !normalizedName.empty())
            return normalizedManufacturer + "." + normalizedName;
        if (!normalizedName.empty())
            return normalizedName;
        return normalizedManufacturer;
    }

    std::string InferPluginFormatFromPath(const std::filesystem::path& path)
    {
        std::string lower = path.string();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });

        if (lower.find(".vst3") != std::string::npos)
            return "vst3";
        if (lower.find(".component") != std::string::npos || lower.find(".appex") != std::string::npos)
            return "au";
        if (lower.find(".lv2") != std::string::npos)
            return "lv2";
        if (lower.find(".clap") != std::string::npos)
            return "clap";
        if (lower.find(".aaxplugin") != std::string::npos)
            return "aax";
        // Note: ".vst3" is matched above, so a bare ".vst" here means VST2.
        if (lower.find(".dll") != std::string::npos || lower.find(".vst") != std::string::npos)
            return "vst2";
        return {};
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

    bool IsNamEffectType(const std::string& type)
    {
        return type == guitarfx::EffectGuids::kAmpNam
            || type == "amp_nam"
            || type == guitarfx::EffectGuids::kAmpNamOptimized
            || type == "amp_nam_optimized"
            || type == guitarfx::EffectGuids::kAmpNamBlend
            || type == "amp_nam_blend"
            || type == guitarfx::EffectGuids::kFxNam
            || type == "fx_nam";
    }

    // NAM types eligible for interface calibration injection (amp + FX).
    // Each calibratable NAM node with a loaded model can receive the shared
    // interface calibration value; per-node useCalibration still governs whether
    // metadata-based auto-gain is actually applied in the DSP effect.
    bool IsNamCalibratableEffectType(const std::string& type)
    {
        return type == guitarfx::EffectGuids::kAmpNam
            || type == "amp_nam"
            || type == guitarfx::EffectGuids::kAmpNamOptimized
            || type == "amp_nam_optimized"
            || type == guitarfx::EffectGuids::kAmpNamBlend
            || type == "amp_nam_blend"
            || type == guitarfx::EffectGuids::kFxNam
            || type == "fx_nam";
    }

    bool HasValidNamResource(const guitarfx::GraphNode& node)
    {
        for (const auto& resource : node.resources)
        {
            if (resource.IsValid())
                return true;
        }
        return false;
    }

    const guitarfx::GraphNode* FindNodeByIdOrType(const guitarfx::SignalGraph& graph,
                                                   const std::string& id,
                                                   const std::string& type)
    {
        for (const auto& node : graph.nodes)
        {
            if (node.id == id)
                return &node;
        }
        for (const auto& node : graph.nodes)
        {
            if (node.type == type)
                return &node;
        }
        return nullptr;
    }

    int GetGlobalTransposeFromChainConfig(const guitarfx::GlobalSignalChainConfig& config)
    {
        const auto* transposeNode = FindNodeByIdOrType(config.preChainGraph, "global_transpose", guitarfx::EffectGuids::kTranspose);
        if (!transposeNode || !transposeNode->enabled)
            return 0;

        const auto semitonesIt = transposeNode->params.find("semitones");
        if (semitonesIt == transposeNode->params.end())
            return 0;

        return static_cast<int>(std::round(std::clamp(semitonesIt->second, -12.0, 12.0)));
    }

    nlohmann::json SerializeGlobalFxSettings(const guitarfx::GlobalSignalChainConfig& config)
    {
        // Serialize global FX (gate, transpose, EQ, doubler) to app settings for persistence.
        // This is per-instance state saved to app.json (standalone) or host state (plugin).
        // Global FX are NEVER saved in presets—presets contain only the signal graph.
        return nlohmann::json{
            {"inputGain", config.inputGain},
            {"outputGain", config.outputGain},
            {"preChainGraph", guitarfx::SerializeSignalGraph(config.preChainGraph)},
            {"postChainGraph", guitarfx::SerializeSignalGraph(config.postChainGraph)}
        };
    }

    bool IsPersistedGlobalFxParam(int paramIdx)
    {
        // Global FX parameters that are persisted to app settings (standalone) or host state (plugin).
        // Limiter is intentionally excluded—it is mixer state, not part of the pre/post FX chain.
        // Note: Global FX are NEVER persisted in presets; they are per-instance state only.
        switch (paramIdx)
        {
        case guitarfx::PluginController::kParamInputTrim:
        case guitarfx::PluginController::kParamOutputTrim:
        case guitarfx::PluginController::kParamGateEnabled:
        case guitarfx::PluginController::kParamGateThreshold:
        case guitarfx::PluginController::kParamDoublerEnabled:
        case guitarfx::PluginController::kParamDoublerDelay:
        case guitarfx::PluginController::kParamTranspose:
        case guitarfx::PluginController::kParamEQEnabled:
        case guitarfx::PluginController::kParamEQLowGain:
        case guitarfx::PluginController::kParamEQLowFreq:
        case guitarfx::PluginController::kParamEQLowMidGain:
        case guitarfx::PluginController::kParamEQLowMidFreq:
        case guitarfx::PluginController::kParamEQLowMidQ:
        case guitarfx::PluginController::kParamEQHighMidGain:
        case guitarfx::PluginController::kParamEQHighMidFreq:
        case guitarfx::PluginController::kParamEQHighMidQ:
        case guitarfx::PluginController::kParamEQHighGain:
        case guitarfx::PluginController::kParamEQHighFreq:
            return true;
        default:
            return false;
        }
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

    std::string BuildFactoryArchiveKey(const std::filesystem::path& archivePath)
    {
        std::string name = archivePath.filename().string();
        constexpr std::array<std::string_view, 4> suffixes = {
            ".soundshed.presets",
            ".soundshed.preset",
            ".presets",
            ".preset",
        };

        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        for (const auto suffix : suffixes)
        {
            if (lowerName.size() >= suffix.size()
                && lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix.data()) == 0)
            {
                name.erase(name.size() - suffix.size());
                break;
            }
        }

        auto sanitized = guitarfx::util::SanitizePathSegment(name, true);
        if (sanitized.empty())
            sanitized = "factory-archive";
        return sanitized;
    }

    std::string BuildScopedFactoryArchiveId(const std::string& archiveKey, const std::string& rawId)
    {
        auto sanitizedRaw = guitarfx::util::SanitizePathSegment(rawId, true);
        if (sanitizedRaw.empty())
            sanitizedRaw = "item";
        return archiveKey + "__" + sanitizedRaw;
    }

    std::string BuildScopedPresetArchiveSessionId(const std::string& archiveKey, const std::string& rawId)
    {
        auto sanitizedRaw = guitarfx::util::SanitizePathSegment(rawId, true);
        if (sanitizedRaw.empty())
            sanitizedRaw = "item";
        return std::string{"preset-archive-session__"} + archiveKey + "__" + sanitizedRaw;
    }

    bool IsFactoryArchiveExtension(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return extension == ".preset" || extension == ".presets";
    }

    std::string NormalizePresetTitle(std::string value)
    {
        const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }).base(), value.end());
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    /// Reads the "gear_type" string from a NAM file's JSON metadata header.
    /// NAM .nam files are a JSON header followed by binary weights. We read the
    /// first 16 KB and extract the gear_type field with a targeted string search
    /// to avoid full JSON parse (which would fail on trailing binary data).
    std::string TryExtractNamGearType(const std::filesystem::path& namFilePath)
    {
        if (!std::filesystem::exists(namFilePath))
            return {};

        try
        {
            std::ifstream file(namFilePath, std::ios::binary);
            if (!file)
                return {};

            std::array<char, 16384> buf{};
            file.read(buf.data(), buf.size());
            const auto len = static_cast<std::size_t>(file.gcount());
            if (len == 0)
                return {};

            const std::string_view content(buf.data(), len);
            constexpr std::string_view needle = R"("gear_type")";
            const auto keyPos = content.find(needle);
            if (keyPos == std::string_view::npos)
                return {};

            auto colonPos = content.find(':', keyPos + needle.size());
            if (colonPos == std::string_view::npos)
                return {};

            auto p = colonPos + 1;
            while (p < len && (content[p] == ' ' || content[p] == '\t' || content[p] == '\n' || content[p] == '\r'))
                ++p;

            if (p >= len)
                return {};

            // String value: "amp_cab"
            if (content[p] == '"')
            {
                ++p;
                const auto endQ = content.find('"', p);
                if (endQ == std::string_view::npos)
                    return {};
                return std::string(content.substr(p, endQ - p));
            }

            // null or other non-string value — not useful for full-rig detection
            return {};
        }
        catch (...)
        {
        }

        return {};
    }

    struct NamFileMetadata
    {
        std::string fileVersion;
        std::string architecture;
        std::string sampleRate;
        std::string namName;
        std::string modeledBy;
        std::string gearMake;
        std::string gearModel;
        std::string gearType;
        std::string toneType;
        std::string inputLevelDbu;
        std::string outputLevelDbu;
        std::string modelDate;
        std::string trainingFinalLoss;
    };

    /// Extracts all recognised metadata fields from a NAM model file header.
    /// NAM .nam files are a JSON header followed by binary weights; we read the
    /// first 64 KB and use targeted string searches to avoid a full JSON parse.
    NamFileMetadata TryExtractNamMetadata(const std::filesystem::path& namFilePath)
    {
        NamFileMetadata result;
        if (!std::filesystem::exists(namFilePath))
            return result;

        try
        {
            std::ifstream file(namFilePath, std::ios::binary);
            if (!file)
                return result;

            constexpr std::size_t kBufSize = 65536;
            std::vector<char> buf(kBufSize);
            file.read(buf.data(), static_cast<std::streamsize>(kBufSize));
            const auto len = static_cast<std::size_t>(file.gcount());
            if (len == 0)
                return result;

            const std::string_view content(buf.data(), len);

            // Extract a JSON string value: find "key" : "value" and return value.
            const auto extractStr = [](const std::string_view sv, const std::string_view key) -> std::string
            {
                const auto needle = std::string("\"").append(key).append("\"");
                const auto kp = sv.find(needle);
                if (kp == std::string_view::npos) return {};
                const auto cp = sv.find(':', kp + needle.size());
                if (cp == std::string_view::npos) return {};
                auto p = cp + 1;
                while (p < sv.size() && std::isspace(static_cast<unsigned char>(sv[p]))) ++p;
                if (p >= sv.size() || sv[p] != '"') return {};
                ++p;
                const auto eq = sv.find('"', p);
                if (eq == std::string_view::npos) return {};
                return std::string(sv.substr(p, eq - p));
            };

            // Extract a JSON number value: find "key" : number and return as string.
            const auto extractNum = [](const std::string_view sv, const std::string_view key) -> std::string
            {
                const auto needle = std::string("\"").append(key).append("\"");
                const auto kp = sv.find(needle);
                if (kp == std::string_view::npos) return {};
                const auto cp = sv.find(':', kp + needle.size());
                if (cp == std::string_view::npos) return {};
                auto p = cp + 1;
                while (p < sv.size() && std::isspace(static_cast<unsigned char>(sv[p]))) ++p;
                if (p >= sv.size()) return {};
                if (!std::isdigit(static_cast<unsigned char>(sv[p])) && sv[p] != '-') return {};
                const auto ns = p;
                while (p < sv.size() && (std::isdigit(static_cast<unsigned char>(sv[p])) ||
                       sv[p] == '.' || sv[p] == '-' || sv[p] == '+' || sv[p] == 'e' || sv[p] == 'E')) ++p;
                return std::string(sv.substr(ns, p - ns));
            };

            // Return the full text of a named JSON sub-object { ... }.
            const auto extractObjContent = [](const std::string_view sv, const std::string_view key) -> std::string_view
            {
                const auto needle = std::string("\"").append(key).append("\"");
                const auto kp = sv.find(needle);
                if (kp == std::string_view::npos) return {};
                auto p = sv.find('{', kp + needle.size());
                if (p == std::string_view::npos) return {};
                int depth = 0;
                bool ins = false, esc = false;
                for (std::size_t i = p; i < sv.size(); ++i)
                {
                    const char c = sv[i];
                    if (esc)       { esc = false; continue; }
                    if (ins)       { if (c == '\\') esc = true; else if (c == '"') ins = false; continue; }
                    if (c == '"')  ins = true;
                    else if (c == '{') ++depth;
                    else if (c == '}') { if (--depth == 0) return sv.substr(p, i - p + 1); }
                }
                return sv.substr(p); // buffer truncated — still usable
            };

            // Top-level fields
            result.fileVersion  = extractStr(content, "version");
            result.architecture = extractStr(content, "architecture");
            result.sampleRate   = extractNum(content, "sample_rate");

            // metadata sub-object
            const auto metaContent = extractObjContent(content, "metadata");
            if (!metaContent.empty())
            {
                result.namName    = extractStr(metaContent, "name");
                result.modeledBy  = extractStr(metaContent, "modeled_by");
                result.gearMake   = extractStr(metaContent, "gear_make");
                result.gearModel  = extractStr(metaContent, "gear_model");
                result.gearType   = extractStr(metaContent, "gear_type");
                result.toneType   = extractStr(metaContent, "tone_type");
                result.inputLevelDbu  = extractNum(metaContent, "input_level_dbu");
                result.outputLevelDbu = extractNum(metaContent, "output_level_dbu");

                // date sub-object → "YYYY-MM-DD"
                const auto dateContent = extractObjContent(metaContent, "date");
                if (!dateContent.empty())
                {
                    const auto yearStr = extractNum(dateContent, "year");
                    if (!yearStr.empty())
                    {
                        try
                        {
                            const int y = std::stoi(yearStr);
                            const auto ms = extractNum(dateContent, "month");
                            const auto ds = extractNum(dateContent, "day");
                            const int m = ms.empty() ? 0 : std::stoi(ms);
                            const int d = ds.empty() ? 0 : std::stoi(ds);
                            char dateBuf[12];
                            std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", y, m, d);
                            result.modelDate = dateBuf;
                        }
                        catch (...) {}
                    }
                }

                // training.final_loss
                const auto trainContent = extractObjContent(metaContent, "training");
                if (!trainContent.empty())
                    result.trainingFinalLoss = extractNum(trainContent, "final_loss");
            }
        }
        catch (...) {}

        return result;
    }

    /// Enriches a NAM LibraryResource's metadata map from the .nam file header.
    /// Uses setIfMissing semantics for all fields except gear_type, which always
    /// takes the file's authoritative value (required for full-rig signal routing).
    void EnrichNamResourceMetadata(guitarfx::LibraryResource& resource, const std::filesystem::path& namFilePath)
    {
        const NamFileMetadata meta = TryExtractNamMetadata(namFilePath);

        const auto setIfMissing = [&](const std::string& key, const std::string& value)
        {
            if (!value.empty() && !resource.metadata.count(key))
                resource.metadata[key] = value;
        };

        // gear_type: always prefer the file's value — it drives full-rig cab routing.
        if (!meta.gearType.empty())
            resource.metadata["gear_type"] = meta.gearType;

        setIfMissing("namFileVersion",     meta.fileVersion);
        setIfMissing("architecture",       meta.architecture);
        setIfMissing("sampleRate",         meta.sampleRate);
        setIfMissing("namName",            meta.namName);
        setIfMissing("modeledBy",          meta.modeledBy);
        setIfMissing("gearMake",           meta.gearMake);
        setIfMissing("gearModel",          meta.gearModel);
        setIfMissing("toneType",           meta.toneType);
        setIfMissing("inputLevelDbu",      meta.inputLevelDbu);
        setIfMissing("outputLevelDbu",     meta.outputLevelDbu);
        setIfMissing("modelDate",          meta.modelDate);
        setIfMissing("trainingFinalLoss",  meta.trainingFinalLoss);
    }

    std::string NormalizeCategoryToken(std::string value)
    {
        const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }).base(), value.end());
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
        {
            if (ch == '_' || ch == ' ')
                return '-';
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::optional<std::string> MapToLibraryCategory(const std::string& rawCategory)
    {
        const std::string category = NormalizeCategoryToken(rawCategory);
        if (category.empty())
            return std::nullopt;

        if (category == "amp")
            return std::string{"amp"};
        if (category == "preamp" || category == "outboard")
            return std::string{"preamp"};
        if (category == "pedal" || category == "stomp" || category == "stompbox" || category == "effect" || category == "fx")
            return std::string{"pedal"};
        if (category == "cab" || category == "cabinet" || category == "ir")
            return std::string{"cab"};
        if (category == "full-rig" || category == "fullrig" || category == "amp-cab" || category == "ampcab"
            || category == "amp+cab" || category == "amp-and-cab")
            return std::string{"full-rig"};

        return std::nullopt;
    }

    std::string ResolveResourceLibraryCategory(const guitarfx::LibraryResource& resource, const std::string& requestedCategory)
    {
        auto metadataValue = [&](const char* key) -> std::string
        {
            const auto it = resource.metadata.find(key);
            return it != resource.metadata.end() ? it->second : std::string{};
        };

        if (resource.type == "nam")
        {
            // Prefer the Tone3000 category context for NAM imports.
            if (auto mapped = MapToLibraryCategory(metadataValue("tone3000Category")); mapped.has_value())
                return *mapped;
            if (auto mapped = MapToLibraryCategory(metadataValue("gear")); mapped.has_value())
                return *mapped;

            // Otherwise use NAM-native metadata from the file header.
            if (auto mapped = MapToLibraryCategory(metadataValue("gear_type")); mapped.has_value())
                return *mapped;

            // Fall back to requested category only when it maps to a supported bucket.
            if (auto mapped = MapToLibraryCategory(requestedCategory); mapped.has_value())
                return *mapped;

            // If no metadata maps cleanly, use the most likely default for NAM.
            return "amp";
        }

        return requestedCategory;
    }

    /// Lightweight WAV header parse used by the folder browser to surface IR/cab
    /// metadata without decoding sample data. Returns false if the file is not a
    /// readable RIFF/WAVE container.
    struct WavHeaderInfo
    {
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bitsPerSample = 0;
        double durationSec = 0.0;
        bool valid = false;
    };

    WavHeaderInfo TryReadWavHeader(const std::filesystem::path& wavFilePath)
    {
        WavHeaderInfo info;
        std::error_code ec;
        if (!std::filesystem::exists(wavFilePath, ec) || ec)
            return info;

        try
        {
            std::ifstream file(wavFilePath, std::ios::binary);
            if (!file)
                return info;

            const auto readU32 = [&file]() -> std::uint32_t {
                unsigned char b[4]{};
                file.read(reinterpret_cast<char*>(b), 4);
                if (file.gcount() != 4) return 0u;
                return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8)
                     | (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
            };
            const auto readU16 = [&file]() -> std::uint16_t {
                unsigned char b[2]{};
                file.read(reinterpret_cast<char*>(b), 2);
                if (file.gcount() != 2) return 0u;
                return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[0]) | (static_cast<std::uint16_t>(b[1]) << 8));
            };

            char riff[4]{};
            file.read(riff, 4);
            if (file.gcount() != 4 || std::memcmp(riff, "RIFF", 4) != 0)
                return info;
            (void)readU32(); // overall size
            char wave[4]{};
            file.read(wave, 4);
            if (file.gcount() != 4 || std::memcmp(wave, "WAVE", 4) != 0)
                return info;

            std::uint32_t byteRate = 0;
            std::uint32_t dataBytes = 0;
            bool haveFmt = false;
            bool haveData = false;

            // Walk chunks until both fmt and data are found, or EOF. Bounded by file size.
            for (int guard = 0; guard < 4096 && file && !(haveFmt && haveData); ++guard)
            {
                char chunkId[4]{};
                file.read(chunkId, 4);
                if (file.gcount() != 4)
                    break;
                const std::uint32_t chunkSize = readU32();
                if (std::memcmp(chunkId, "fmt ", 4) == 0)
                {
                    (void)readU16();              // audio format
                    info.channels = readU16();
                    info.sampleRate = readU32();
                    byteRate = readU32();
                    (void)readU16();              // block align
                    info.bitsPerSample = readU16();
                    haveFmt = true;
                    // Skip any remaining fmt bytes (e.g. extensible header).
                    if (chunkSize > 16)
                        file.seekg(static_cast<std::streamoff>(chunkSize - 16), std::ios::cur);
                    if (chunkSize & 1u)
                        file.seekg(1, std::ios::cur);
                }
                else if (std::memcmp(chunkId, "data", 4) == 0)
                {
                    dataBytes = chunkSize;
                    haveData = true;
                    // Do not read the payload.
                    file.seekg(static_cast<std::streamoff>(chunkSize) + (chunkSize & 1u), std::ios::cur);
                }
                else
                {
                    file.seekg(static_cast<std::streamoff>(chunkSize) + (chunkSize & 1u), std::ios::cur);
                }
            }

            if (haveFmt && info.sampleRate > 0)
            {
                info.valid = true;
                if (byteRate > 0 && dataBytes > 0)
                    info.durationSec = static_cast<double>(dataBytes) / static_cast<double>(byteRate);
            }
        }
        catch (...)
        {
        }

        return info;
    }

    std::filesystem::path ResolvePresetFoldersPath(const guitarfx::FileSystem& fileSystem)
    {
        return fileSystem.ResolveSettingsDirectory() / "presets" / "preset-folders.json";
    }

    std::filesystem::path ResolveFactoryArchiveStatePath(const guitarfx::FileSystem& fileSystem)
    {
        return fileSystem.ResolveSettingsDirectory() / "presets" / kFactoryArchiveStateFileName;
    }

    std::filesystem::path ResolveCustomEffectLibraryPath(const guitarfx::FileSystem& fileSystem)
    {
        return fileSystem.ResolveSettingsDirectory() / "custom-effects" / "indexes" / "custom-effects-index.json";
    }

    nlohmann::json LoadJsonFile(const std::filesystem::path& path, const nlohmann::json& fallback)
    {
        if (path.empty() || !std::filesystem::exists(path))
            return fallback;

        try
        {
            std::ifstream input(path);
            if (input.is_open())
                return nlohmann::json::parse(input);
        }
        catch (const std::exception&) {}

        return fallback;
    }

    void SaveJsonFile(const guitarfx::FileSystem& fileSystem,
                      const std::filesystem::path& path,
                      const nlohmann::json& payload)
    {
        if (path.empty())
            return;

        try
        {
            [[maybe_unused]] const auto ensuredParent = fileSystem.EnsureDirectory(path.parent_path());
            std::ofstream output(path);
            if (output.is_open())
                output << payload.dump(2);
        }
        catch (const std::exception&) {}
    }

    nlohmann::json MakePresetFolderEntry(const std::string& id, const std::string& name)
    {
        return nlohmann::json{
            {"id", id},
            {"name", name},
            {"children", nlohmann::json::array()},
            {"presetIds", nlohmann::json::array()},
        };
    }

    bool IsFactoryArchiveFolderId(const std::string& folderId)
    {
        return folderId.rfind("factory-archive::", 0) == 0
            || folderId.rfind("factory-archive-folder::", 0) == 0;
    }

    std::string BuildFactoryArchiveNestedFolderId(const std::string& archiveKey, const std::string& folderPath)
    {
        const auto sanitizedPath = guitarfx::util::SanitizeSubfolderPath(folderPath);
        std::string sanitized = sanitizedPath.generic_string<char>();
        std::replace(sanitized.begin(), sanitized.end(), '/', '_');
        if (sanitized.empty())
            sanitized = "folder";
        return "factory-archive-folder::" + archiveKey + "::" + sanitized;
    }

    std::string BuildPresetArchiveSessionFolderId(const std::string& archiveKey, const std::string& folderPath)
    {
        const auto sanitizedPath = guitarfx::util::SanitizeSubfolderPath(folderPath);
        std::string sanitized = sanitizedPath.generic_string<char>();
        std::replace(sanitized.begin(), sanitized.end(), '/', '_');
        if (sanitized.empty())
            sanitized = "folder";
        return "preset-archive-session-folder::" + archiveKey + "::" + sanitized;
    }

    nlohmann::json BuildFactoryArchiveFolders(const std::string& archiveKey,
                                              const nlohmann::json& archivePresetFolders,
                                              const std::unordered_map<std::string, std::string>& presetIdMapping)
    {
        std::function<nlohmann::json(const nlohmann::json&, const std::string&)> buildFolders;
        buildFolders = [&](const nlohmann::json& sourceFolders, const std::string& parentPath) -> nlohmann::json
        {
            nlohmann::json result = nlohmann::json::array();
            if (!sourceFolders.is_array())
                return result;

            for (const auto& sourceFolder : sourceFolders)
            {
                if (!sourceFolder.is_object())
                    continue;

                const std::string name = sourceFolder.value("name", "");
                if (name.empty())
                    continue;

                const std::string folderPath = parentPath.empty() ? name : (parentPath + "/" + name);
                nlohmann::json folder = MakePresetFolderEntry(
                    BuildFactoryArchiveNestedFolderId(archiveKey, folderPath),
                    name);

                if (sourceFolder.contains("presetIds") && sourceFolder["presetIds"].is_array())
                {
                    for (const auto& presetIdValue : sourceFolder["presetIds"])
                    {
                        if (!presetIdValue.is_string())
                            continue;
                        const auto mappedIt = presetIdMapping.find(presetIdValue.get<std::string>());
                        if (mappedIt == presetIdMapping.end())
                            continue;
                        folder["presetIds"].push_back(mappedIt->second);
                    }
                }

                folder["children"] = buildFolders(sourceFolder.value("children", nlohmann::json::array()), folderPath);
                result.push_back(std::move(folder));
            }

            return result;
        };

        return buildFolders(archivePresetFolders, std::string{});
    }

    bool IsFactoryArchiveTopLevelFolder(const std::string& archiveKey, const nlohmann::json& folder)
    {
        if (!folder.is_object())
            return false;

        const std::string folderId = folder.value("id", "");
        const std::string expectedPrefix = "factory-archive-folder::" + archiveKey + "::";
        return folderId.rfind(expectedPrefix, 0) == 0;
    }

    void UpdateFactoryPresetFolders(const guitarfx::FileSystem& fileSystem,
                                    const std::string& archiveKey,
                                    const nlohmann::json& archivePresetFolders,
                                    const std::unordered_map<std::string, std::string>& presetIdMapping,
                                    const std::vector<std::string>&)
    {
        auto payload = LoadJsonFile(ResolvePresetFoldersPath(fileSystem), nlohmann::json::object());
        if (!payload.is_object())
            payload = nlohmann::json::object();

        if (!payload.contains("folders") || !payload["folders"].is_array())
            payload["folders"] = nlohmann::json::array();
        if (!payload.contains("activeFolderId") || !payload["activeFolderId"].is_string())
            payload["activeFolderId"] = "__all__";

        nlohmann::json filteredFolders = nlohmann::json::array();
        for (const auto& folder : payload["folders"])
        {
            if (!IsFactoryArchiveTopLevelFolder(archiveKey, folder))
                filteredFolders.push_back(folder);
        }

        auto archiveFolders = BuildFactoryArchiveFolders(
            archiveKey,
            archivePresetFolders,
            presetIdMapping);

        for (const auto& folder : archiveFolders)
            filteredFolders.push_back(folder);

        payload["folders"] = std::move(filteredFolders);

        SaveJsonFile(fileSystem, ResolvePresetFoldersPath(fileSystem), payload);
    }

    nlohmann::json BuildPresetArchiveSessionFolders(const std::string& archiveKey,
                                                    const nlohmann::json& archivePresetFolders,
                                                    const std::unordered_map<std::string, std::string>& presetIdMapping)
    {
        std::function<nlohmann::json(const nlohmann::json&, const std::string&)> buildFolders;
        buildFolders = [&](const nlohmann::json& sourceFolders, const std::string& parentPath) -> nlohmann::json
        {
            nlohmann::json result = nlohmann::json::array();
            if (!sourceFolders.is_array())
                return result;

            for (const auto& sourceFolder : sourceFolders)
            {
                if (!sourceFolder.is_object())
                    continue;

                const std::string name = sourceFolder.value("name", "");
                if (name.empty())
                    continue;

                const std::string folderPath = parentPath.empty() ? name : (parentPath + "/" + name);
                nlohmann::json folder = MakePresetFolderEntry(
                    BuildPresetArchiveSessionFolderId(archiveKey, folderPath),
                    name);

                if (sourceFolder.contains("presetIds") && sourceFolder["presetIds"].is_array())
                {
                    for (const auto& presetIdValue : sourceFolder["presetIds"])
                    {
                        if (!presetIdValue.is_string())
                            continue;
                        const auto mappedIt = presetIdMapping.find(presetIdValue.get<std::string>());
                        if (mappedIt == presetIdMapping.end())
                            continue;
                        folder["presetIds"].push_back(mappedIt->second);
                    }
                }

                folder["children"] = buildFolders(sourceFolder.value("children", nlohmann::json::array()), folderPath);
                result.push_back(std::move(folder));
            }

            return result;
        };

        return buildFolders(archivePresetFolders, std::string{});
    }

    std::optional<std::vector<std::uint8_t>> ExtractZipEntry(const std::vector<std::uint8_t>& zipBytes,
                                                             const std::string& entryName)
    {
        mz_zip_archive archive{};
        if (!mz_zip_reader_init_mem(&archive, zipBytes.data(), zipBytes.size(), 0))
            return std::nullopt;

        const int fileIndex = mz_zip_reader_locate_file(&archive, entryName.c_str(), nullptr, 0);
        if (fileIndex < 0)
        {
            mz_zip_reader_end(&archive);
            return std::nullopt;
        }

        size_t extractedSize = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(fileIndex), &extractedSize, 0);
        if (!extracted)
        {
            mz_zip_reader_end(&archive);
            return std::nullopt;
        }

        std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(extracted),
                                        static_cast<std::uint8_t*>(extracted) + extractedSize);
        mz_free(extracted);
        mz_zip_reader_end(&archive);
        return bytes;
    }

    std::optional<ParsedFactoryPresetArchive> ParseFactoryPresetArchive(const std::filesystem::path& archivePath,
                                                                        const std::vector<std::uint8_t>& zipBytes,
                                                                        std::string& error)
    {
        mz_zip_archive archive{};
        if (!mz_zip_reader_init_mem(&archive, zipBytes.data(), zipBytes.size(), 0))
        {
            error = "Invalid zip archive";
            return std::nullopt;
        }

        auto finishWithError = [&](std::string message) -> std::optional<ParsedFactoryPresetArchive>
        {
            error = std::move(message);
            mz_zip_reader_end(&archive);
            return std::nullopt;
        };

        const int presetIndex = mz_zip_reader_locate_file(&archive, "preset.json", nullptr, 0);
        const int presetsIndex = mz_zip_reader_locate_file(&archive, "presets.json", nullptr, 0);
        if (presetIndex < 0 && presetsIndex < 0)
            return finishWithError("Archive is missing preset.json or presets.json");

        auto extractJsonEntry = [&](int index) -> std::optional<nlohmann::json>
        {
            if (index < 0)
                return std::nullopt;

            size_t extractedSize = 0;
            void* extracted = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &extractedSize, 0);
            if (!extracted)
                return std::nullopt;

            std::string text(static_cast<const char*>(extracted), extractedSize);
            mz_free(extracted);

            try
            {
                return nlohmann::json::parse(text);
            }
            catch (const std::exception& ex)
            {
                error = ex.what();
                return std::nullopt;
            }
        };

        ParsedFactoryPresetArchive parsed;
        const auto archiveJson = extractJsonEntry(presetIndex >= 0 ? presetIndex : presetsIndex);
        if (!archiveJson || !archiveJson->is_object())
            return finishWithError(error.empty() ? "Archive JSON is invalid" : error);

        const nlohmann::json& root = *archiveJson;
        if (root.contains("resources") && root["resources"].is_array())
        {
            for (const auto& resourceJson : root["resources"])
            {
                if (!resourceJson.is_object())
                    continue;

                FactoryArchiveResourceEntry resource;
                resource.id = resourceJson.value("id", "");
                resource.name = resourceJson.value("name", resource.id);
                resource.category = resourceJson.value("category", "");
                resource.type = resourceJson.value("type", "");
                resource.fileName = resourceJson.value("fileName", "");
                resource.hash = resourceJson.value("hash", "");
                if (resource.type.empty() || resource.id.empty() || resource.fileName.empty())
                    continue;

                const auto resourceBytes = ExtractZipEntry(zipBytes, "resources/" + resource.fileName);
                if (!resourceBytes)
                    return finishWithError("Archive resource missing: resources/" + resource.fileName);

                resource.bytes = *resourceBytes;
                parsed.resources.push_back(std::move(resource));
            }
        }

        if (root.contains("blends") && root["blends"].is_array())
        {
            for (const auto& blend : root["blends"])
            {
                if (blend.is_object())
                    parsed.blends.push_back(blend);
            }
        }

        if (root.contains("presetFolders") && root["presetFolders"].is_array())
            parsed.presetFolders = root["presetFolders"];

        if (root.contains("tone3000Resources") && root["tone3000Resources"].is_array())
            parsed.tone3000ResourceCount = root["tone3000Resources"].size();

        auto appendPreset = [&](const nlohmann::json& presetJson) -> bool
        {
            if (!presetJson.is_object())
                return true;
            const auto presetOpt = guitarfx::PresetStorage::DeserializeFromJson(presetJson.dump());
            if (!presetOpt)
            {
                error = "Failed to parse preset JSON from archive " + archivePath.filename().string();
                return false;
            }
            parsed.presets.push_back(*presetOpt);
            return true;
        };

        if (presetIndex >= 0)
        {
            if (!root.contains("preset") || !root["preset"].is_object())
                return finishWithError("Archive has no preset data");
            if (!appendPreset(root["preset"]))
                return finishWithError(error);
        }
        else
        {
            if (!root.contains("presets") || !root["presets"].is_array() || root["presets"].empty())
                return finishWithError("Archive has no presets data");
            for (const auto& presetJson : root["presets"])
            {
                if (!appendPreset(presetJson))
                    return finishWithError(error);
            }
        }

        mz_zip_reader_end(&archive);
        return parsed;
    }

    void RemapPresetGraphResources(guitarfx::SignalGraph& graph,
                                   const std::unordered_map<std::string, std::string>& resourceIdMap,
                                   const std::unordered_map<std::string, std::string>& blendIdMap)
    {
        for (auto& node : graph.nodes)
        {
            const auto blendIt = node.config.find("blendId");
            if (blendIt != node.config.end())
            {
                const auto mappedBlend = blendIdMap.find(blendIt->second);
                if (mappedBlend != blendIdMap.end())
                    blendIt->second = mappedBlend->second;
            }

            for (auto& resource : node.resources)
            {
                if (!resource.IsLibraryRef())
                    continue;
                const auto mappedResource = resourceIdMap.find(resource.resourceId);
                if (mappedResource != resourceIdMap.end())
                    resource.resourceId = mappedResource->second;
            }
        }
    }

    void RemapPresetArchiveReferences(guitarfx::Preset& preset,
                                      const std::unordered_map<std::string, std::string>& resourceIdMap,
                                      const std::unordered_map<std::string, std::string>& blendIdMap)
    {
        RemapPresetGraphResources(preset.graph, resourceIdMap, blendIdMap);
        for (auto& scene : preset.scenes)
            RemapPresetGraphResources(scene.graph, resourceIdMap, blendIdMap);
    }

    std::filesystem::path ResolveFactoryPresetDirectory(const guitarfx::IPluginHost& host,
                                                        const std::filesystem::path& legacyResourceRoot)
    {
        const auto bundledRoot = host.GetBundledAssetsPath();
        if (!bundledRoot.empty())
        {
            const auto bundledUiFactoryDir = bundledRoot / "ui" / "presets" / "factory";
            if (std::filesystem::exists(bundledUiFactoryDir))
                return bundledUiFactoryDir;

            const auto bundledLegacyFactoryDir = bundledRoot / "presets" / "factory";
            if (std::filesystem::exists(bundledLegacyFactoryDir))
                return bundledLegacyFactoryDir;
        }

        return legacyResourceRoot / "presets" / "factory";
    }

    std::filesystem::path NormalizePresetArchiveSavePath(const std::filesystem::path& path)
    {
        const std::string filename = path.filename().string();
        std::string normalized = filename;
        std::string lowerNormalized = normalized;
        std::transform(lowerNormalized.begin(), lowerNormalized.end(), lowerNormalized.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        constexpr std::array<std::string_view, 2> suffixes = {
            ".soundshed.presets",
            ".soundshed.preset",
        };

        for (const auto suffix : suffixes)
        {
            while (lowerNormalized.size() >= suffix.size() * 2
                   && lowerNormalized.compare(lowerNormalized.size() - suffix.size(), suffix.size(), suffix) == 0
                   && lowerNormalized.compare(lowerNormalized.size() - (suffix.size() * 2), suffix.size(), suffix) == 0)
            {
                normalized.erase(normalized.size() - suffix.size());
                lowerNormalized.erase(lowerNormalized.size() - suffix.size());
            }
        }

        if (normalized == filename)
            return path;

        return path.parent_path() / normalized;
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

    struct OfflineRenderBuffer
    {
        std::string id;
        std::string title;
        double sampleRate = 0.0;
        std::vector<std::vector<float>> channelSamples;
    };

    constexpr std::array<int, 6> kDemoRenderSampleRateOptions = {
        44100,
        48000,
        88200,
        96000,
        176400,
        192000,
    };

    bool IsSupportedDemoRenderSampleRate(double sampleRate)
    {
        if (sampleRate <= 0.0 || sampleRate > 192000.0)
            return false;

        const int roundedSampleRate = static_cast<int>(std::llround(sampleRate));
        if (std::abs(sampleRate - static_cast<double>(roundedSampleRate)) >= 1.0)
            return false;

        return std::find(kDemoRenderSampleRateOptions.begin(), kDemoRenderSampleRateOptions.end(), roundedSampleRate)
            != kDemoRenderSampleRateOptions.end();
    }

    double ResolveDemoRenderSampleRate(const nlohmann::json& payload,
                                       double hostSampleRate,
                                       std::string& error)
    {
        if (hostSampleRate <= 0.0)
        {
            error = "Audio device sample rate is unavailable";
            return 0.0;
        }

        const auto sampleRateIter = payload.find("renderSampleRate");
        if (sampleRateIter == payload.end() || sampleRateIter->is_null())
            return hostSampleRate;

        if (!sampleRateIter->is_number())
        {
            error = "Render sample rate is invalid";
            return 0.0;
        }

        const double requestedSampleRate = sampleRateIter->get<double>();
        if (requestedSampleRate <= 0.0)
            return hostSampleRate;

        if (!IsSupportedDemoRenderSampleRate(requestedSampleRate))
        {
            error = "Unsupported render sample rate";
            return 0.0;
        }

        return static_cast<double>(std::llround(requestedSampleRate));
    }

    std::string BuildDemoRenderSuggestedFilename(const std::string& requestedName, double renderSampleRate)
    {
        std::string suggestedName = guitarfx::util::SanitizeFilename(
            requestedName.empty() ? std::string("demo-audio.wav") : requestedName);
        std::string lowerSuggested = suggestedName;
        std::transform(lowerSuggested.begin(), lowerSuggested.end(), lowerSuggested.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });

        if (lowerSuggested.size() >= 4 && lowerSuggested.compare(lowerSuggested.size() - 4, 4, ".wav") == 0)
            suggestedName.erase(suggestedName.size() - 4);

        if (suggestedName.empty())
            suggestedName = "demo-audio";

        const int roundedKilohertz = static_cast<int>(std::llround(renderSampleRate / 1000.0));
        suggestedName += "-" + std::to_string(roundedKilohertz) + ".wav";
        return suggestedName;
    }

    class OfflineRenderMixerPrepareScope
    {
    public:
        OfflineRenderMixerPrepareScope(guitarfx::MultiPresetMixer& mixer,
                                       double renderSampleRate,
                                       int renderBlockSize,
                                       double restoreSampleRate,
                                       int restoreBlockSize)
            : mMixer(mixer)
            , mRestoreSampleRate(restoreSampleRate)
            , mRestoreBlockSize(restoreBlockSize)
        {
            mMixer.Prepare(renderSampleRate, renderBlockSize);
            mMixer.Reset();
        }

        ~OfflineRenderMixerPrepareScope()
        {
            try
            {
                if (mRestoreSampleRate > 0.0 && mRestoreBlockSize > 0)
                {
                    mMixer.Prepare(mRestoreSampleRate, mRestoreBlockSize);
                    mMixer.Reset();
                }
            }
            catch (...)
            {
            }
        }

        OfflineRenderMixerPrepareScope(const OfflineRenderMixerPrepareScope&) = delete;
        OfflineRenderMixerPrepareScope& operator=(const OfflineRenderMixerPrepareScope&) = delete;

    private:
        guitarfx::MultiPresetMixer& mMixer;
        double mRestoreSampleRate = 0.0;
        int mRestoreBlockSize = 0;
    };

    std::size_t FindTrailingAudibleFrameCount(const std::vector<float>& left,
                                              const std::vector<float>& right,
                                              float threshold,
                                              std::size_t requiredQuietFrames)
    {
        const std::size_t frameCount = std::min(left.size(), right.size());
        if (frameCount == 0)
            return 0;

        std::size_t quietFrames = 0;
        for (std::size_t frame = frameCount; frame > 0; --frame)
        {
            const std::size_t index = frame - 1;
            const float peak = std::max(std::abs(left[index]), std::abs(right[index]));
            if (peak <= threshold)
            {
                ++quietFrames;
                continue;
            }

            if (quietFrames >= requiredQuietFrames)
                return frame;

            return frame + quietFrames;
        }

        return 0;
    }

    void TrimOfflineRenderBufferTrailingSilence(OfflineRenderBuffer& buffer,
                                                float threshold,
                                                std::size_t requiredQuietFrames)
    {
        if (buffer.channelSamples.empty() || buffer.channelSamples.front().empty())
            return;

        auto& left = buffer.channelSamples[0];
        auto& right = buffer.channelSamples.size() > 1 ? buffer.channelSamples[1] : buffer.channelSamples[0];
        const std::size_t trimmedFrames = FindTrailingAudibleFrameCount(left, right, threshold, requiredQuietFrames);
        if (trimmedFrames == 0 || trimmedFrames >= left.size())
            return;

        for (auto& channel : buffer.channelSamples)
            channel.resize(trimmedFrames);
    }

    std::optional<OfflineRenderBuffer> PrepareOfflineRenderBuffer(const std::vector<std::uint8_t>& bytes,
                                                                  double targetSampleRate,
                                                                  const std::string& id,
                                                                  const std::string& title,
                                                                  std::string& error)
    {
        const auto wavData = guitarfx::util::DecodeAudioBytes(bytes);
        if (!wavData)
        {
            error = "Unsupported audio format (expected WAV, AIFF, or MP3)";
            return std::nullopt;
        }

        if (targetSampleRate <= 0.0)
        {
            error = "Target sample rate is invalid";
            return std::nullopt;
        }

        auto resampled = guitarfx::util::ConvertToSampleRate(
            *wavData,
            targetSampleRate,
            guitarfx::SampleRateConversionQuality::Highest);
        if (resampled.empty() || resampled.front().empty())
        {
            error = "Audio buffer is empty";
            return std::nullopt;
        }

        std::size_t minFrames = resampled.front().size();
        for (const auto& channel : resampled)
        {
            if (channel.empty())
            {
                error = "Audio buffer is empty";
                return std::nullopt;
            }
            minFrames = std::min(minFrames, channel.size());
        }

        if (minFrames == 0)
        {
            error = "Audio buffer is empty";
            return std::nullopt;
        }

        for (auto& channel : resampled)
        {
            if (channel.size() > minFrames)
                channel.resize(minFrames);
        }

        OfflineRenderBuffer buffer;
        buffer.id = id;
        buffer.title = title;
        buffer.sampleRate = targetSampleRate;
        buffer.channelSamples = std::move(resampled);
        const std::size_t requiredQuietFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(targetSampleRate * 0.05)));
        TrimOfflineRenderBufferTrailingSilence(buffer, 3.0e-4f, requiredQuietFrames);
        return buffer;
    }

    bool RenderBufferThroughMixer(guitarfx::MultiPresetMixer& mixer,
                                  std::mutex& dspMutex,
                                  const OfflineRenderBuffer& source,
                                  int blockSize,
                                  double restoreSampleRate,
                                  int restoreBlockSize,
                                  double tempoBpm,
                                  std::vector<float>& renderedLeft,
                                  std::vector<float>& renderedRight)
    {
        if (source.channelSamples.empty() || source.channelSamples.front().empty())
            return false;

        const int safeBlockSize = std::max(32, blockSize);
        const std::size_t totalFrames = source.channelSamples.front().size();
        constexpr double kMaxTailSeconds = 8.0;
        constexpr double kTailSilenceSeconds = 0.25;
        constexpr float kTailSilencePeak = 7.5e-4f;

        const int requiredSilentBlocks = std::max(2, static_cast<int>(std::ceil((source.sampleRate * kTailSilenceSeconds) / static_cast<double>(safeBlockSize))));
        const int maxTailBlocks = std::max(requiredSilentBlocks, static_cast<int>(std::ceil((source.sampleRate * kMaxTailSeconds) / static_cast<double>(safeBlockSize))));

        renderedLeft.clear();
        renderedRight.clear();
        renderedLeft.reserve(totalFrames + static_cast<std::size_t>(maxTailBlocks * safeBlockSize));
        renderedRight.reserve(totalFrames + static_cast<std::size_t>(maxTailBlocks * safeBlockSize));

        std::vector<float> inputLeft(static_cast<std::size_t>(safeBlockSize), 0.0f);
        std::vector<float> inputRight(static_cast<std::size_t>(safeBlockSize), 0.0f);
        std::vector<float> outputLeft(static_cast<std::size_t>(safeBlockSize), 0.0f);
        std::vector<float> outputRight(static_cast<std::size_t>(safeBlockSize), 0.0f);
        const bool hasRightChannel = source.channelSamples.size() > 1;

        std::size_t frameOffset = 0;
        int tailBlocks = 0;
        int silentBlocks = 0;

        std::lock_guard<std::mutex> lock(dspMutex);
        OfflineRenderMixerPrepareScope renderPrepare(
            mixer,
            source.sampleRate,
            safeBlockSize,
            restoreSampleRate,
            std::max(1, restoreBlockSize));

        while (frameOffset < totalFrames || tailBlocks < maxTailBlocks)
        {
            const bool feedingInput = frameOffset < totalFrames;
            const int framesThisBlock = feedingInput
                ? std::min(safeBlockSize, static_cast<int>(totalFrames - frameOffset))
                : safeBlockSize;

            std::fill(inputLeft.begin(), inputLeft.end(), 0.0f);
            std::fill(inputRight.begin(), inputRight.end(), 0.0f);
            std::fill(outputLeft.begin(), outputLeft.end(), 0.0f);
            std::fill(outputRight.begin(), outputRight.end(), 0.0f);

            if (feedingInput)
            {
                std::copy_n(source.channelSamples[0].begin() + static_cast<std::ptrdiff_t>(frameOffset),
                            framesThisBlock,
                            inputLeft.begin());
                if (hasRightChannel)
                {
                    std::copy_n(source.channelSamples[1].begin() + static_cast<std::ptrdiff_t>(frameOffset),
                                framesThisBlock,
                                inputRight.begin());
                }
                else
                {
                    std::copy_n(inputLeft.begin(), framesThisBlock, inputRight.begin());
                }
            }

            float* inputPtrs[2] = { inputLeft.data(), inputRight.data() };
            float* outputPtrs[2] = { outputLeft.data(), outputRight.data() };

            mixer.SetTempo(tempoBpm);
            mixer.Process(inputPtrs, outputPtrs, framesThisBlock);

            renderedLeft.insert(renderedLeft.end(), outputLeft.begin(), outputLeft.begin() + framesThisBlock);
            renderedRight.insert(renderedRight.end(), outputRight.begin(), outputRight.begin() + framesThisBlock);

            if (feedingInput)
            {
                frameOffset += static_cast<std::size_t>(framesThisBlock);
                continue;
            }

            ++tailBlocks;

            float peak = 0.0f;
            for (int i = 0; i < framesThisBlock; ++i)
            {
                peak = std::max(peak, std::abs(outputLeft[static_cast<std::size_t>(i)]));
                peak = std::max(peak, std::abs(outputRight[static_cast<std::size_t>(i)]));
            }

            silentBlocks = (peak <= kTailSilencePeak) ? (silentBlocks + 1) : 0;
            if (silentBlocks >= requiredSilentBlocks)
                break;
        }

        const std::size_t requiredQuietFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(source.sampleRate * 0.05)));
        const std::size_t trimmedFrames = FindTrailingAudibleFrameCount(renderedLeft, renderedRight, kTailSilencePeak, requiredQuietFrames);
        if (trimmedFrames > 0 && trimmedFrames < renderedLeft.size())
        {
            renderedLeft.resize(trimmedFrames);
            renderedRight.resize(trimmedFrames);
        }

        mixer.Reset();
        return !renderedLeft.empty() && renderedLeft.size() == renderedRight.size();
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

static std::string HashStringForLog(std::string_view value)
{
    constexpr std::uint64_t kFNVOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kFNVPrime = 1099511628211ull;

    std::uint64_t hash = kFNVOffsetBasis;
    for (const unsigned char byte : value)
    {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFNVPrime;
    }

    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

static void ScrubHostedPluginStateForUi(SignalGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
            continue;

        const auto stateIt = node.config.find("pluginStateBase64");
        if (stateIt == node.config.end())
            continue;

        node.config["pluginStateBase64Length"] = std::to_string(stateIt->second.size());
        node.config.erase(stateIt);
    }
}

static nlohmann::json SerializePresetForUi(const Preset& preset)
{
    Preset uiPreset = preset;
    ScrubHostedPluginStateForUi(uiPreset.graph);
    for (auto& scene : uiPreset.scenes)
        ScrubHostedPluginStateForUi(scene.graph);

    return nlohmann::json::parse(PresetStorage::SerializeToJson(uiPreset));
}

static bool GraphHasScrubbedHostedPluginState(const SignalGraph& graph)
{
    for (const auto& node : graph.nodes)
    {
        if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
            continue;

        const auto stateIt = node.config.find("pluginStateBase64");
        const auto lengthIt = node.config.find("pluginStateBase64Length");
        if (lengthIt != node.config.end() && (stateIt == node.config.end() || stateIt->second.empty()))
            return true;
    }

    return false;
}

static bool PresetHasScrubbedHostedPluginState(const Preset& preset)
{
    if (GraphHasScrubbedHostedPluginState(preset.graph))
        return true;

    for (const auto& scene : preset.scenes)
    {
        if (GraphHasScrubbedHostedPluginState(scene.graph))
            return true;
    }

    return false;
}

static void AppendHostedPluginGraphSummary(const SignalGraph& graph,
                                           const std::string& scopeLabel,
                                           std::vector<std::string>& entries)
{
    for (const auto& node : graph.nodes)
    {
        if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
            continue;

        const auto stateIt = node.config.find("pluginStateBase64");
        const auto lengthIt = node.config.find("pluginStateBase64Length");
        const std::string stateLength = stateIt != node.config.end()
            ? std::to_string(stateIt->second.size())
            : std::string{"0"};
        const std::string stateHash = stateIt != node.config.end()
            ? HashStringForLog(stateIt->second)
            : std::string{"<none>"};
        const std::string scrubbedLength = lengthIt != node.config.end()
            ? lengthIt->second
            : std::string{"0"};
        entries.push_back(scopeLabel + "/" + node.id + ":state=" + stateLength + ",hash=" + stateHash + ",scrubbed=" + scrubbedLength);
    }
}

static std::string SummarizeHostedPluginState(const Preset& preset)
{
    std::vector<std::string> entries;
    AppendHostedPluginGraphSummary(preset.graph, "graph", entries);
    for (const auto& scene : preset.scenes)
        AppendHostedPluginGraphSummary(scene.graph, "scene:" + scene.id, entries);

    if (entries.empty())
        return "no hosted plugin nodes";

    std::ostringstream summary;
    for (size_t index = 0; index < entries.size(); ++index)
    {
        if (index > 0)
            summary << "; ";
        summary << entries[index];
    }
    return summary.str();
}

static std::string GenerateUserPresetId()
{
    return "user-" + GenerateGuidV4String();
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

static std::filesystem::path ResolveLayoutDir(const FileSystem& fileSystem, const std::string& layoutId)
{
    const auto settingsDir = fileSystem.ResolveSettingsDirectory();
    const std::string safeStem = util::SanitizeFilename(layoutId);
    return settingsDir / "layouts" / "content" / safeStem;
}

static std::filesystem::path ResolveLayoutFilePath(const FileSystem& fileSystem, const std::string& layoutId)
{
    return ResolveLayoutDir(fileSystem, layoutId) / "layout.json";
}
}

// ════════════════════════════════════════════════════════════════════
// Construction / Lifecycle
// ════════════════════════════════════════════════════════════════════

PluginController::PluginController(IPluginHost& host)
    : mHost(host)
{
    mParamValues.fill(0.0);
    mPendingMidiApply.reserve(kMaxPendingMidiApply);
    mPendingMidiLog.reserve(kMaxPendingMidiLog);
    RegisterAllEffects();
    mDemoPreview = std::make_unique<DemoPreviewService>(
        mHost,
        mPresetMixer,
        mDSPMutex,
        mSignalTestActive,
        [this](const std::string& message, const std::string& detail) { ReportErrorToUI(message, detail); },
        [this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
}

PluginController::~PluginController()
{
    if (mPresetArchiveSession)
    {
        std::error_code ec;
        std::filesystem::remove_all(mPresetArchiveSession->rootPath, ec);
    }

    // Supersede any in-flight folder scans so detached workers bail out
    // promptly, then wait until every outstanding worker has finished before
    // our members are destroyed (workers call SendMessageToUI through mHost and
    // read mFolderScanGeneration, so they must not outlive us).
    mFolderScanGeneration.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mFolderScanDoneMutex);
    mFolderScanDoneCv.wait(lock, [this]() {
        return mActiveFolderScans.load(std::memory_order_relaxed) == 0;
    });
}

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

    // When hosted in a DAW the host controls the input configuration; disable
    // app-side mono folding/channel selection so the input is used as provided.
    mPresetMixer.SetHostControlledInput(!mHost.IsStandalone());

    LoadAppSettings();
    ApplyMetronomeSettingsFromAppSettings();
    ApplyDiagnosticsSettingsFromAppSettings();
    ApplyDspLevelTargetSettingsFromAppSettings();
    ApplyProcessingModeSettingsFromAppSettings();
    ApplyInputModeSettingsFromAppSettings();
    ApplyGlobalFxSettingsFromAppSettings();
    ApplyNamSlimmableSettingsFromAppSettings();
    ApplyNamInterfaceCalibrationFromAppSettings();
    ApplyUserInputCalibrationSettingsFromAppSettings();
    ApplyUiSettingsFromAppSettings();
    if (!IsPresetArchiveSessionActive())
        LoadResourceLibraries();
    if (!IsPresetArchiveSessionActive())
        LoadBlendLibrary();
    LoadCustomEffectLibrary();
    LoadFactoryPresetArchives();
    LoadCompositeLibrary();
    LoadLayoutLibrary();
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }
    LoadLastSessionState();
    ApplyInputModeSettingsFromAppSettings();

    // Initialize automation system
    mAutomationSlots.SetMixer(&mPresetMixer);
    mAutomationSlots.SetEffectRegistry(&EffectRegistry::Instance());
    mAutomationSlots.InitializeRegistry(
        mPresetMixer,
        [this]() { return static_cast<double>(mSetlistCursorIndex); },
        [this](int idx) { ApplySetlistPresetByIndex(idx); },
        [this](int steps) { SetlistBankUp(steps); },
        [this](int steps) { SetlistBankDown(steps); },
        [this]() { return GetSetlistLength(); },
        [this]() { return GetSetlistBankBase(); },
        [this](int bankNumber) { SelectSetlistBank(bankNumber); },
        [this]() { return GetSetlistBankNumber(); },
        [this](int index) { SelectSceneByIndex(index); },
        [this]() { return GetActiveSceneIndex(); });

    // Wire node-param-applied callback so the UI can reflect automation-driven changes.
    mAutomationSlots.SetOnNodeParamApplied(
        [this](const std::string& effectType, const std::string& paramId, double value)
        {
            // Resolve the concrete nodeId from the mixer's runtime graph.
            const auto found = mPresetMixer.FindFirstEnabledNodeOfType(effectType);
            if (!found)
                return;

            const auto& nodeId = found->second;

            // Patch mActivePreset so a subsequent state broadcast is consistent.
            if (mActivePreset)
            {
                auto* node = mActivePreset->graph.FindNode(nodeId);
                if (node)
                    node->params[paramId] = value;
            }

            // Queue a lightweight UI notification (safe from audio or UI thread).
            {
                std::lock_guard<std::mutex> lock(mPendingNodeParamMutex);
                mPendingNodeParamNotifies.push_back({nodeId, paramId, value});
            }
        });

    mAutomationSlots.SetOnNodeBypassApplied(
        [this](const std::string& effectType, bool enabled)
        {
            if (!mActivePreset)
                return;

            const auto resolvedType = EffectRegistry::Instance().Resolve(effectType);
            bool updated = false;
            const auto applyBypassToGraph = [&](SignalGraph& graph)
            {
                for (auto& node : graph.nodes)
                {
                    if (EffectRegistry::Instance().Resolve(node.type) != resolvedType)
                        continue;
                    node.enabled = enabled;
                    updated = true;
                }
            };

            // Keep both the active scene graph and mActivePreset->graph in sync.
            // BroadcastState calls SyncActivePresetSceneGraph(), which copies the
            // active scene graph into mActivePreset->graph.
            const std::string activeSceneId = GetResolvedActiveSceneId();
            if (auto* scene = FindPresetScene(*mActivePreset, activeSceneId))
                applyBypassToGraph(scene->graph);
            applyBypassToGraph(mActivePreset->graph);

            if (!updated)
                return;

            mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
            if (!mActivePresetId.empty())
                mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
            mPendingStateBroadcast = true;

        });

    // Load automation.json
    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());
    if (!automationData.empty())
        mAutomationSlots.LoadFromJson(automationData);

    // Load setlist cursor/bankSize from setlists.json
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    mNextSharedSyncPollAt = std::chrono::steady_clock::now();
}

void PluginController::Prepare(double sampleRate, int blockSize)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.Prepare(sampleRate, blockSize);

    // Report initial latency to the host (e.g. IR cab partition size may be
    // known only after Prepare sets the sample rate).
    UpdateHostLatency();

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

    if (!config.metronomeClickEnabled)
    {
        mRiffGuidanceActive = false;
        mRiffGuidanceForPreview = false;
        mRiffGuidancePreviewWasActive = false;
        mRiffGuidanceBeatScale = 1.0;
        mRiffGuidanceClickSamples.reset();
        mMetronomeResetPending.store(true, std::memory_order_release);
        return;
    }

    mRiffGuidanceActive = true;
    mRiffGuidanceForPreview = forPreview;
    mRiffGuidancePreviewWasActive = false;
    mRiffGuidanceBeatPattern = config.beatPattern;
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

    ProcessAudioLocked(inputs, outputs, numSamples);
    return true;
}

void PluginController::ProcessAudioLocked(float** inputs, float** outputs, int numSamples)
{

    // ARM mode: click is playing, waiting for input signal to trigger recording
    if (mRiffCapture.armed && !mRiffCapture.active && !mRiffCapture.complete)
    {
        const bool hasInput = (inputs && inputs[0]);
        const float* inputR = (inputs && inputs[1]) ? inputs[1] : (inputs ? inputs[0] : nullptr);
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
                const float level = std::max(std::abs(inputs[0][i]), std::abs(inputR[i]));
                if (level >= mRiffCapture.armThreshold)
                {
                    // Compute bar phase at trigger for snapping the trim start to the bar boundary
                    const double beatScaleTrig = 4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen));
                    const double samplesPerBeatTrig = mRiffCapture.sampleRate
                        * (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm)) * beatScaleTrig;
                    const double samplesPerBarTrig = samplesPerBeatTrig
                        * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
                    const std::size_t barSamples = static_cast<std::size_t>(std::max(1.0, samplesPerBarTrig));
                    const std::size_t triggerOffset = mRiffCapture.armPostCountInSamples
                        + static_cast<std::size_t>(i);
                    const std::size_t barAlignOffset = triggerOffset % barSamples;

                    // Trigger: start recording — audio from trigger point is captured
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
                    startMsg["barAlignOffsetSamples"] = barAlignOffset;
                    SendMessageToUI(startMsg.dump());
                    break;
                }
            }
            // Only track detection-phase samples when still waiting (no trigger this block)
            if (mRiffCapture.armed)
                mRiffCapture.armPostCountInSamples += static_cast<std::size_t>(numSamples);
        }
    }

    if (mRiffCapture.active && !mRiffCapture.complete)
    {
        const bool hasInputCh0 = (inputs && inputs[0]);
        const float* capInputR = (inputs && inputs[1]) ? inputs[1] : (hasInputCh0 ? inputs[0] : nullptr);
        if (hasInputCh0 && mRiffCapture.writeIndex < mRiffCapture.targetSamples)
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
                        mRiffCapture.right[captureIndex] = capInputR[i];
                        // Update live waveform peak bucket
                        const float peakVal = std::max(std::abs(inputs[0][i]), std::abs(capInputR[i]));
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

    // Deactivate guidance for preview only once the preview has been active and then stopped.
    // This avoids a race where guidance is deactivated before DemoPreview has loaded the buffer.
    if (mRiffGuidanceForPreview && mDemoPreview)
    {
        if (mDemoPreview->IsPreviewActive())
            mRiffGuidancePreviewWasActive = true;
        else if (mRiffGuidancePreviewWasActive)
        {
            DeactivateRiffGuidance(true);
            mRiffGuidancePreviewWasActive = false;
        }
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

    // Push current tempo to any tempo-aware effect nodes
    mPresetMixer.SetTempo(GetEffectiveTempoBpm());

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

    const std::string& activeBeatPattern = riffGuidanceActive ? mRiffGuidanceBeatPattern : mMetronomeBeatPattern;

    for (int frame = 0; frame < numSamples; ++frame)
    {
        if (mMetronomeSamplesUntilClick <= 0.0)
        {
            const char accent = BeatAccent(activeBeatPattern, mMetronomeBeatIndex);
            const bool useHigh = (accent == 'H');
            const bool silent  = (accent == 'S');

            if (hasSampleClick)
            {
                if (!silent)
                {
                    const auto& preferred = useHigh ? clickSampleSet->high : clickSampleSet->low;
                    const auto& fallback  = useHigh ? clickSampleSet->low  : clickSampleSet->high;
                    const auto& selected  = (!preferred.empty() && !preferred.front().empty()) ? preferred : fallback;
                    mMetronomeClickSamplesRemaining = selected.empty() ? 0 : static_cast<int>(selected.front().size());
                    mMetronomeClickSamplePosition = 0;
                    mMetronomeClickUseHigh = useHigh;
                }
                else
                {
                    mMetronomeClickSamplesRemaining = 0;
                }
                mMetronomeBeatIndex = (mMetronomeBeatIndex + 1) % beatsPerBar;
            }
            else
            {
                mMetronomeClickSamplesRemaining = silent ? 0 : clickSamples;
                if (!silent) mMetronomeBeatIndex = (mMetronomeBeatIndex + 1) % beatsPerBar;
                else         mMetronomeBeatIndex = (mMetronomeBeatIndex + 1) % beatsPerBar;
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

    mMetronomeBeatPattern.clear();
    if (mAppSettings.contains(kMetronomeBeatPatternSettingKey) && mAppSettings[kMetronomeBeatPatternSettingKey].is_string())
        mMetronomeBeatPattern = mAppSettings[kMetronomeBeatPatternSettingKey].get<std::string>();
    mAppSettings[kMetronomeBeatPatternSettingKey] = mMetronomeBeatPattern;

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
    const bool enabled = true;
    mAppSettings[kSignalDiagnosticsSettingKey] = enabled;
    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
}

void PluginController::ApplyDspLevelTargetSettingsFromAppSettings()
{
    bool settingsChanged = false;

    const auto readNumericSetting = [this, &settingsChanged](const char* key, double defaultValue) -> double
    {
        const auto it = mAppSettings.find(key);
        if (it == mAppSettings.end())
            return defaultValue;
        if (it->is_number())
            return it->get<double>();
        if (!it->is_null())
            settingsChanged = true;
        return defaultValue;
    };

    const double nominalLevelDbfs = SanitizeNominalOperatingLevelDbfs(
        readNumericSetting(kNominalOperatingLevelSettingKey, kDefaultNominalOperatingLevelDbfs));
    const double protectionCeilingDbfs = SanitizeOutputProtectionCeilingDbfs(
        readNumericSetting(kOutputProtectionCeilingSettingKey, kDefaultOutputProtectionCeilingDbfs));

    SetNominalOperatingLevelDbfs(nominalLevelDbfs);
    SetOutputProtectionCeilingDbfs(protectionCeilingDbfs);

    const auto updateStoredSetting = [this, &settingsChanged](const char* key, double value)
    {
        const auto it = mAppSettings.find(key);
        if (it == mAppSettings.end() || !it->is_number() || it->get<double>() != value)
        {
            mAppSettings[key] = value;
            settingsChanged = true;
        }
    };

    updateStoredSetting(kNominalOperatingLevelSettingKey, nominalLevelDbfs);
    updateStoredSetting(kOutputProtectionCeilingSettingKey, protectionCeilingDbfs);

    if (settingsChanged)
        SaveAppSettings();
}

void PluginController::ApplyProcessingModeSettingsFromAppSettings()
{
    bool settingsChanged = false;

    bool enabled = true;
    const auto it = mAppSettings.find(kMultiThreadedProcessingSettingKey);
    if (it != mAppSettings.end())
    {
        if (it->is_boolean())
            enabled = it->get<bool>();
        else if (!it->is_null())
            settingsChanged = true;
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetMultiThreadedProcessingEnabled(enabled);
    }

    if (it == mAppSettings.end() || !it->is_boolean() || it->get<bool>() != enabled)
    {
        mAppSettings[kMultiThreadedProcessingSettingKey] = enabled;
        settingsChanged = true;
    }

    if (settingsChanged)
        SaveAppSettings();
}

void PluginController::ApplyInputModeSettingsFromAppSettings()
{
    // Only applies in standalone mode; in plugin mode the DAW owns the input config.
    if (!mHost.IsStandalone())
        return;

    // Key names must match the UI constants in controls.ts:
    //   INPUT_CHANNEL_SETTING  = "inputChannel.mono"
    //   MONO_MODE_SETTING      = "inputChannel.monoMode"
    constexpr auto kMonoModeKey    = "inputChannel.monoMode";
    constexpr auto kInputChanKey   = "inputChannel.mono";

    const auto monoIt = mAppSettings.find(kMonoModeKey);
    const auto chanIt = mAppSettings.find(kInputChanKey);

    const bool storedMonoMode = (monoIt != mAppSettings.end() && monoIt->is_boolean())
        ? monoIt->get<bool>()
        : true; // Default to mono mode so the guitar comes through on startup

    const int storedChannel = (chanIt != mAppSettings.end() && chanIt->is_number_integer())
        ? std::clamp(chanIt->get<int>(), 0, 1)
        : 0;

    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetMonoMode(storedMonoMode);
    mPresetMixer.SetInputChannel(storedChannel);
}

void PluginController::ApplyGlobalFxSettingsFromAppSettings()
{
    if (!mHost.IsStandalone())
        return;

    const auto it = mAppSettings.find(kGlobalFxSettingsKey);
    if (it == mAppSettings.end() || !it->is_object())
        return;

    try
    {
        auto config = it->get<GlobalSignalChainConfig>();
        config.autoLevelInput = false;
        config.autoLevelOutput = false;

        // Build off the lock, install under it — rebuilding the global executors while the
        // audio thread is blocked on mDSPMutex is an audible dropout.
        mPresetMixer.PrepareGlobalChainSwap(config);

        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
            mParamValues[kParamInputTrim] = config.inputGain;
            mParamValues[kParamOutputTrim] = config.outputGain;
            mParamValues[kParamTranspose] = static_cast<double>(GetGlobalTransposeFromChainConfig(config));
        }
        UpdateHostLatency();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to apply global FX settings: " << e.what() << std::endl;
    }
}

void PluginController::PersistGlobalFxSettingsToAppSettings()
{
    mHost.NotifyStateChanged();

    if (!mHost.IsStandalone())
        return;

    mAppSettings[kGlobalFxSettingsKey] = SerializeGlobalFxSettings(mPresetMixer.GetGlobalChainConfig());
    SaveAppSettings();
}

void PluginController::ApplyNamSlimmableSettingsFromAppSettings()
{
    bool settingsChanged = false;

    const auto it = mAppSettings.find(kNamSlimmableSizeSettingKey);
    const double rawValue = (it != mAppSettings.end() && it->is_number())
        ? it->get<double>()
        : kNamSlimmableSizeDefault;

    const double slimmableSize = SanitizeNamSlimmableSize(rawValue);
    if (it == mAppSettings.end() || !it->is_number() || it->get<double>() != slimmableSize)
    {
        mAppSettings[kNamSlimmableSizeSettingKey] = slimmableSize;
        settingsChanged = true;
    }

    SetGlobalNamSlimmableSize(slimmableSize);
    const std::string configValue = std::to_string(slimmableSize);

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeConfigForType(EffectGuids::kAmpNam, kNamSlimmableNodeConfigKey, configValue);
        mPresetMixer.SetNodeConfigForType(EffectGuids::kAmpNamOptimized, kNamSlimmableNodeConfigKey, configValue);
        mPresetMixer.SetNodeConfigForType(EffectGuids::kAmpNamBlend, kNamSlimmableNodeConfigKey, configValue);
        mPresetMixer.SetNodeConfigForType(EffectGuids::kFxNam, kNamSlimmableNodeConfigKey, configValue);
    }

    if (settingsChanged)
        SaveAppSettings();
}

void PluginController::ApplyNamInterfaceCalibrationFromAppSettings()
{
    // Respect the global auto-input-calibration toggle (default: enabled).
    bool autoCalibrationEnabled = true;
    const auto enableIt = mAppSettings.find(kNamAutoInputCalibrationSettingKey);
    if (enableIt != mAppSettings.end() && enableIt->is_boolean())
        autoCalibrationEnabled = enableIt->get<bool>();

    const auto it = mAppSettings.find(kNamInterfaceCalibrationLevelDbuSettingKey);
    double calLevel = std::numeric_limits<double>::quiet_NaN();
    if (autoCalibrationEnabled)
    {
        if (it != mAppSettings.end() && it->is_number())
        {
            const double raw = it->get<double>();
            if (raw >= kNamInterfaceCalibrationLevelDbuMin && raw <= kNamInterfaceCalibrationLevelDbuMax)
                calLevel = raw;
            else
                calLevel = kNamInterfaceCalibrationLevelDbuDefault;
        }
        else
        {
            // Setting absent — use the default, mirroring the UI's fallback behaviour.
            // This ensures calibration is active from the first run even before the user
            // has visited Settings and explicitly saved the value.
            calLevel = kNamInterfaceCalibrationLevelDbuDefault;
        }
    }
    mNamInterfaceCalibrationLevelDbu = calLevel;

    if (!mActivePresetId.empty() && mActivePreset)
    {
        const auto& graph = mActivePreset->graph;
        const bool hasCalibrationValue = std::isfinite(calLevel);
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        std::lock_guard<std::mutex> lock(mDSPMutex);
        for (const auto& node : graph.nodes)
        {
            if (!IsNamCalibratableEffectType(node.type))
                continue;

            const double calibrationToInject =
                hasCalibrationValue ? calLevel : clearValue;
            mPresetMixer.SetNodeParam(mActivePresetId, node.id, "calibrationInputLevel", calibrationToInject);
        }
    }
}

void PluginController::ApplyUserInputCalibrationSettingsFromAppSettings()
{
    bool settingsChanged = false;

    if (mAppSettings.erase(kLegacyInterfaceCalibrationEnabledSettingKey) > 0)
        settingsChanged = true;
    if (mAppSettings.erase(kLegacyInterfaceCalibrationReferenceDbuSettingKey) > 0)
        settingsChanged = true;

    std::string activeProfileId;
    const auto activeIt = mAppSettings.find(kUserInputCalibrationActiveProfileIdSettingKey);
    if (activeIt != mAppSettings.end())
    {
        if (activeIt->is_string())
            activeProfileId = activeIt->get<std::string>();
        else if (!activeIt->is_null())
        {
            mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = nullptr;
            settingsChanged = true;
        }
    }

    double gainDb = 0.0;
    bool foundActiveProfile = activeProfileId.empty();
    const auto profilesIt = mAppSettings.find(kUserInputCalibrationProfilesSettingKey);
    if (profilesIt != mAppSettings.end())
    {
        if (profilesIt->is_array())
        {
            for (const auto& profile : *profilesIt)
            {
                if (!profile.is_object())
                    continue;

                const auto idIt = profile.find("id");
                if (idIt == profile.end() || !idIt->is_string() || idIt->get<std::string>() != activeProfileId)
                    continue;

                foundActiveProfile = true;

                const auto gainIt = profile.find("gainDb");
                if (gainIt != profile.end() && gainIt->is_number())
                {
                    gainDb = gainIt->get<double>();
                }
                else
                {
                    const auto capturedIt = profile.find("capturedPeakDbfs");
                    const auto targetIt = profile.find("targetPeakDbfs");
                    if (capturedIt != profile.end() && capturedIt->is_number()
                        && targetIt != profile.end() && targetIt->is_number())
                    {
                        gainDb = targetIt->get<double>() - capturedIt->get<double>();
                    }
                }
                break;
            }
        }
        else if (!profilesIt->is_null())
        {
            mAppSettings[kUserInputCalibrationProfilesSettingKey] = nlohmann::json::array();
            settingsChanged = true;
        }
    }

    if (!foundActiveProfile)
    {
        mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = nullptr;
        gainDb = 0.0;
        settingsChanged = true;
    }

    if (mUserInputCalibrationTrainingActive)
        gainDb = 0.0;

    mPresetMixer.SetUserInputCalibrationGainDb(gainDb);

    if (settingsChanged)
        SaveAppSettings();
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

bool PluginController::IsFactoryPresetArchiveLoadingEnabled() const
{
    const auto it = mAppSettings.find(kFactoryArchiveLoadingEnabledSettingKey);
    if (it == mAppSettings.end() || !it->is_boolean())
        return true;
    return it->get<bool>();
}

bool PluginController::IsPresetArchiveSessionActive() const
{
    return mPresetArchiveSession.has_value();
}

std::filesystem::path PluginController::GetEffectiveUserPresetDirectory() const
{
    if (mPresetArchiveSession)
        return mPresetArchiveSession->presetDir;
    return mUserPresetsPath;
}

std::filesystem::path PluginController::GetEffectiveSettingsDirectory() const
{
    if (mPresetArchiveSession)
        return mPresetArchiveSession->rootPath;
    return mFileSystem.ResolveSettingsDirectory();
}

std::filesystem::path PluginController::ResolveResourceLibraryIndexPath() const
{
    return GetEffectiveSettingsDirectory() / "resources" / "indexes" / "resources-index.json";
}

void PluginController::RefreshPresetLibraryViews()
{
    HandleGetPresetListRequest();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    HandleGetSetlistsRequest();
}

void PluginController::ClearActivePresetMixerState()
{
    const auto activePresetIds = mPresetMixer.GetActivePresetIds();
    for (const auto& presetId : activePresetIds)
        mPresetMixer.RemoveActivePreset(presetId);
    mMixerPresetJsonCache.clear();
}

void PluginController::SendPresetArchiveSessionStateToUI(const char* messageType,
                                                         const std::string& detail)
{
    nlohmann::json message;
    message["type"] = messageType == nullptr ? "presetArchiveSessionState" : messageType;
    message["active"] = IsPresetArchiveSessionActive();
    if (mPresetArchiveSession)
    {
        message["archiveName"] = mPresetArchiveSession->archiveName;
        message["archiveKey"] = mPresetArchiveSession->archiveKey;
        message["presetCount"] = mPresetArchiveSession->presetCount;
    }
    if (!detail.empty())
        message["detail"] = detail;
    SendMessageToUI(message.dump());
}

void PluginController::StartPresetArchiveSession(const std::string& archiveFileName,
                                                 const std::vector<std::uint8_t>& archiveBytes)
{
    std::string parseError;
    auto parsedOpt = ParseFactoryPresetArchive(std::filesystem::path(archiveFileName), archiveBytes, parseError);
    if (!parsedOpt)
        throw std::runtime_error(parseError.empty() ? "Failed to parse preset archive" : parseError);

    auto parsed = std::move(*parsedOpt);
    if (parsed.presets.empty())
        throw std::runtime_error("Archive contains no presets");
    if (parsed.tone3000ResourceCount > 0)
        throw std::runtime_error("Archive session mode does not support Tone3000-linked resources yet");

    if (IsPresetArchiveSessionActive())
        EndPresetArchiveSession(false);

    const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
    const std::string archiveKeyBase = BuildFactoryArchiveKey(std::filesystem::path(archiveFileName));
    const std::string sessionStamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string archiveKey = archiveKeyBase + "-" + sessionStamp;
    const auto sessionRoot = settingsDir / kPresetArchiveSessionRootFolder / archiveKey;
    const auto presetDir = sessionRoot / "presets" / "user";
    const auto resourceContentDir = sessionRoot / "resources" / "content" / kPresetArchiveSessionResourceProvider / archiveKey;

    std::error_code ec;
    std::filesystem::remove_all(sessionRoot, ec);
    [[maybe_unused]] const auto ensuredPresetDir = mFileSystem.EnsureDirectory(presetDir);
    [[maybe_unused]] const auto ensuredResourceDir = mFileSystem.EnsureDirectory(resourceContentDir);

    std::unordered_map<std::string, std::string> resourceIdMap;
    for (const auto& resource : parsed.resources)
    {
        const std::string scopedResourceId = BuildScopedPresetArchiveSessionId(archiveKey, resource.id);
        std::string resolvedName = resource.fileName.empty() ? resource.id : resource.fileName;
        resolvedName = util::SanitizeFilename(resolvedName);
        if (resolvedName.empty())
            resolvedName = scopedResourceId + (resource.type == "ir" ? ".wav" : ".nam");

        const auto targetPath = resourceContentDir / resolvedName;
        if (!WriteFile(targetPath, resource.bytes))
            throw std::runtime_error("Failed to extract archive resource: " + resolvedName);

        resourceIdMap[resource.id] = scopedResourceId;

        LibraryResource libraryResource;
        libraryResource.type = resource.type;
        libraryResource.id = scopedResourceId;
        libraryResource.name = resource.name.empty() ? resource.id : resource.name;
        libraryResource.category = resource.category;
        libraryResource.description = "Session-only preset archive resource";
        libraryResource.filePath = targetPath;
        libraryResource.hash = resource.hash;
        libraryResource.metadata["provider"] = kPresetArchiveSessionResourceProvider;
        libraryResource.metadata["archiveName"] = archiveFileName;
        libraryResource.metadata["archiveKey"] = archiveKey;
        libraryResource.metadata["originalId"] = resource.id;
        if (resource.type == "nam")
            EnrichNamResourceMetadata(libraryResource, targetPath);
        libraryResource.category = ResolveResourceLibraryCategory(libraryResource, libraryResource.category);
        mResourceLibrary.AddResource(libraryResource);
    }

    std::unordered_map<std::string, std::string> blendIdMap;
    if (!mBlendLibrary.is_array())
        mBlendLibrary = nlohmann::json::array();
    for (auto blend : parsed.blends)
    {
        const std::string originalBlendId = blend.value("id", "");
        if (originalBlendId.empty())
            continue;

        const std::string scopedBlendId = BuildScopedPresetArchiveSessionId(archiveKey, originalBlendId);
        blendIdMap[originalBlendId] = scopedBlendId;
        blend["id"] = scopedBlendId;

        if (blend.contains("models") && blend["models"].is_array())
        {
            for (auto& modelId : blend["models"])
            {
                if (!modelId.is_string())
                    continue;
                const auto mapped = resourceIdMap.find(modelId.get<std::string>());
                if (mapped != resourceIdMap.end())
                    modelId = mapped->second;
            }
        }

        if (blend.contains("modelMappings") && blend["modelMappings"].is_array())
        {
            for (auto& mapping : blend["modelMappings"])
            {
                if (!mapping.is_object())
                    continue;
                const auto mapped = resourceIdMap.find(mapping.value("id", ""));
                if (mapped != resourceIdMap.end())
                    mapping["id"] = mapped->second;
            }
        }

        bool replaced = false;
        for (auto& existing : mBlendLibrary)
        {
            if (existing.is_object() && existing.value("id", "") == scopedBlendId)
            {
                existing = blend;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            mBlendLibrary.push_back(blend);
    }

    std::unordered_map<std::string, std::string> presetIdMap;
    std::optional<Preset> firstPreset;
    for (auto preset : parsed.presets)
    {
        RemapPresetArchiveReferences(preset, resourceIdMap, blendIdMap);
        NormalizePresetScenes(preset);

        const std::string sourcePresetId = preset.id.empty()
            ? (preset.name.empty() ? "preset" : preset.name)
            : preset.id;
        const std::string scopedPresetId = BuildScopedPresetArchiveSessionId(archiveKey, sourcePresetId);
        presetIdMap[sourcePresetId] = scopedPresetId;

        preset.id = scopedPresetId;
        if (preset.category.empty())
            preset.category = "Imported";

        const auto presetPath = presetDir / (preset.id + ".json");
        if (!PresetStorage::SaveToFile(preset, presetPath))
            throw std::runtime_error("Failed to write session preset: " + preset.name);

        if (!firstPreset.has_value())
            firstPreset = preset;
    }

    nlohmann::json presetFoldersPayload = nlohmann::json::object();
    presetFoldersPayload["folders"] = BuildPresetArchiveSessionFolders(archiveKey, parsed.presetFolders, presetIdMap);
    presetFoldersPayload["activeFolderId"] = "__all__";

    mPresetArchiveSession = PresetArchiveSessionState{
        archiveKey,
        std::filesystem::path(archiveFileName).filename().string(),
        sessionRoot,
        presetDir,
        presetIdMap.size(),
    };

    SaveUiStorageJson("preset-folders.json", presetFoldersPayload);

    ClearActivePresetMixerState();
    mActivePreset.reset();
    mActivePresetId.clear();
    mActivePresetJson.clear();
    mActiveSceneId.clear();

    if (firstPreset)
    {
        ApplyBlendDefinitions(*firstPreset);
        if (!SetPresetActiveScene(*firstPreset, "", &mActiveSceneId))
            mActiveSceneId = GetDefaultPresetSceneId(*firstPreset);
        mActivePresetId = firstPreset->id;
        ApplyPreset(*firstPreset);
    }

    InvalidateResourceUsageIndex();
    mPendingStateBroadcast = true;
    BroadcastState();
    RefreshPresetLibraryViews();
    SendPresetArchiveSessionStateToUI("presetArchiveSessionStarted");
}

void PluginController::EndPresetArchiveSession(bool notifyUi)
{
    if (!mPresetArchiveSession)
        return;

    const auto sessionRoot = mPresetArchiveSession->rootPath;
    mPresetArchiveSession.reset();

    std::error_code ec;
    std::filesystem::remove_all(sessionRoot, ec);

    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadFactoryPresetArchives();
    InvalidateResourceUsageIndex();

    ClearActivePresetMixerState();
    mActivePreset.reset();
    mActivePresetId.clear();
    mActivePresetJson.clear();
    mActiveSceneId.clear();
    LoadLastSessionState();
    mPendingStateBroadcast = true;
    BroadcastState();
    RefreshPresetLibraryViews();
    if (notifyUi)
        SendPresetArchiveSessionStateToUI("presetArchiveSessionEnded");
}

// ════════════════════════════════════════════════════════════════════
// State serialization
// ════════════════════════════════════════════════════════════════════

std::string PluginController::SerializeState() const
{
    nlohmann::json state = nlohmann::json::object();
    state["version"] = 1;
    if (mActivePreset)
    {
        Preset presetWithRuntimeState = *mActivePreset;
        CaptureRuntimePluginStates(presetWithRuntimeState, mActivePresetId);
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(presetWithRuntimeState));
    }
    state["presetId"] = mActivePresetId;
    state["activeSceneId"] = GetResolvedActiveSceneId();
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
    mixer["multiThreaded"] = mPresetMixer.IsMultiThreadedProcessingEnabled();

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

    state["automation"] = mAutomationSlots.SaveToJson();

    return state.dump();
}

void PluginController::DeserializeState(const std::string& json)
{
    if (mHost.IsStandalone())
    {
        // Standalone startup should restore from app settings + preset files
        // (LoadLastSessionState), not host-serialized transient preset state.
        return;
    }

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

            ApplyNamSlimmableSettingsFromAppSettings();
            ApplyNamInterfaceCalibrationFromAppSettings();
        }

        if (state.contains("uiSettings") && state["uiSettings"].is_object())
            mUiSettings = state["uiSettings"];
        else
            ApplyUiSettingsFromAppSettings();

        if (state.contains("uiViewState") && state["uiViewState"].is_object())
            mUiViewState = state["uiViewState"];

        if (state.contains("globalSignalChain") && state["globalSignalChain"].is_object())
        {
            // Build off the lock, install under it — see PrepareGlobalChainSwap().
            mPresetMixer.PrepareGlobalChainSwap(state["globalSignalChain"].get<GlobalSignalChainConfig>());
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }
        if (state.contains("preset"))
        {
            auto presetOpt = PresetStorage::DeserializeFromJson(state["preset"].dump());
            if (presetOpt)
            {
                mActivePresetId = state.value("presetId", presetOpt->id);
                mActiveSceneId = state.contains("activeSceneId") && state["activeSceneId"].is_string()
                    ? state["activeSceneId"].get<std::string>()
                    : std::string{};
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
            if (mixer.contains("multiThreaded") && mixer["multiThreaded"].is_boolean())
                mPresetMixer.SetMultiThreadedProcessingEnabled(mixer["multiThreaded"].get<bool>());

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
                    if (added)
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
                }
                if (!added)
                {
                    added = AddActivePresetById(id);
                }
                if (!added && mActivePreset)
                {
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);
                    if (added)
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
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

        if (state.contains("automation") && state["automation"].is_object())
            mAutomationSlots.LoadFromJson(state["automation"]);
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
    try
    {
        const auto msg = nlohmann::json::parse(jsonMessage);
        if (msg.is_object() && msg.value("type", std::string{}) == "uiBootstrapError")
        {
            const auto source = msg.value("source", std::string{"unknown"});
            const auto details = msg.value("details", std::string{"(no details)"});
            AppendSessionLog("UI bootstrap error (" + source + "): " + details);
            return;
        }
    }
    catch (const std::exception&)
    {
        // Defer malformed payload handling to the dispatcher.
    }

    // Delegate to the MessageDispatcher which routes by message type.
    MessageDispatcher::Dispatch(*this, jsonMessage);
}

// ════════════════════════════════════════════════════════════════════
// Idle processing
// ════════════════════════════════════════════════════════════════════

void PluginController::OnIdle()
{
    PollSharedSyncState();

    // MIDI learn capture polling
    if (mAutomationSlots.IsMidiLearnArmed())
    {
        // The learn state (mMidiLearnSlotId/mMidiLearnCapture) is written by the
        // audio thread in HandleMidi under mDSPMutex, so read + commit it under the
        // same lock. Disk I/O and UI sends are done afterwards, outside the lock.
        nlohmann::json captureMsg;
        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            const auto slotId = mAutomationSlots.GetMidiLearnSlot();
            auto captured = mAutomationSlots.PollMidiLearnCapture();
            if (captured.has_value() && !slotId.empty())
            {
                const auto* slot = mAutomationSlots.FindSlot(slotId);
                const bool isDefault = slot && slot->isDefault;
                const auto label = slot ? std::optional<std::string>(slot->label) : std::nullopt;
                const auto address = slot ? std::optional<std::string>(slot->address) : std::nullopt;
                const auto nodeSelector = slot ? std::optional<std::string>(slot->nodeSelector) : std::nullopt;
                const auto keyMaps = slot ? std::optional<std::vector<KeyboardMap>>(slot->keyMaps) : std::nullopt;

                if (isDefault)
                    mAutomationSlots.SetDefaultSlotOverrides(slotId, label, *captured, keyMaps);
                else
                    mAutomationSlots.SetCustomSlot(slotId, label, address, nodeSelector, *captured, keyMaps);

                captureMsg["type"] = "midiLearnCapture";
                captureMsg["slotId"] = slotId;
                captureMsg["eventType"] = static_cast<int>(captured->eventType);
                captureMsg["channel"] = captured->channel;
                captureMsg["controller"] = captured->controller;
                committed = true;
            }
        }
        if (committed)
        {
            SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
            SendMessageToUI(captureMsg.dump());
            HandleGetAutomationRequest();
        }
    }

    // Broadcast pending state. A full request supersedes any preset-only request queued
    // alongside it — the full payload is a superset.
    if (mPendingStateBroadcast)
    {
        mPendingStateBroadcast = false;
        mPendingPresetStateBroadcast = false;
        BroadcastState(StateScope::Full);
    }
    else if (mPendingPresetStateBroadcast)
    {
        mPendingPresetStateBroadcast = false;
        BroadcastState(StateScope::PresetOnly);
    }

    // Drain deferred node-param notifications (from MIDI/keyboard automation)
    {
        std::vector<PendingNodeParamNotify> notifies;
        {
            std::lock_guard<std::mutex> lock(mPendingNodeParamMutex);
            notifies = std::move(mPendingNodeParamNotifies);
            mPendingNodeParamNotifies.clear();
        }
        for (const auto& n : notifies)
        {
            nlohmann::json msg;
            msg["type"] = "signalPathNodeParamUpdated";
            msg["nodeId"] = n.nodeId;
            msg["key"] = n.paramKey;
            msg["value"] = n.value;
            SendMessageToUI(msg.dump());
        }
    }

    // Drain diagnostic MIDI log events (only populated while the UI log panel is
    // open). Building JSON + SendMessageToUI happens here on the idle thread, never
    // on the audio thread.
    if (mMidiLogEnabled.load(std::memory_order_relaxed))
    {
        std::vector<MidiEvent> events;
        {
            std::lock_guard<std::mutex> lock(mPendingMidiLogMutex);
            events.assign(mPendingMidiLog.begin(), mPendingMidiLog.end());
            mPendingMidiLog.clear();
        }
        for (const auto& ev : events)
        {
            const int channel = ev.status & 0x0F;
            const int statusType = (ev.status >> 4) & 0x0F;
            const char* typeName = "Unknown";
            switch (statusType)
            {
            case 0x08: typeName = "NoteOff"; break;
            case 0x09: typeName = ev.data2 > 0 ? "NoteOn" : "NoteOff"; break;
            case 0x0A: typeName = "Aftertouch"; break;
            case 0x0B: typeName = "CC"; break;
            case 0x0C: typeName = "ProgramChange"; break;
            case 0x0D: typeName = "ChanPress"; break;
            case 0x0E: typeName = "PitchBend"; break;
            }

            nlohmann::json logMsg;
            logMsg["type"] = "midiLog";
            logMsg["midiType"] = typeName;
            logMsg["channel"] = channel;
            logMsg["data1"] = static_cast<int>(ev.data1);
            logMsg["data2"] = static_cast<int>(ev.data2);
            SendMessageToUI(logMsg.dump());
        }
    }

    // Drain deferred setlist preset apply / bank change (from automation/MIDI under DSP lock)
    {
        std::optional<int> pendingIndex;
        std::optional<int> pendingBankDelta;
        std::optional<int> pendingBankSelect;
        std::optional<int> pendingSceneIndex;
        {
            std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
            pendingIndex = mPendingSetlistPresetIndex;
            mPendingSetlistPresetIndex.reset();
            pendingBankDelta = mPendingSetlistBankDelta;
            mPendingSetlistBankDelta.reset();
            pendingBankSelect = mPendingSetlistBankSelect;
            mPendingSetlistBankSelect.reset();
            pendingSceneIndex = mPendingSceneIndex;
            mPendingSceneIndex.reset();
        }
        if (pendingIndex.has_value())
            ApplySetlistPresetByIndexDirect(*pendingIndex);
        if (pendingBankDelta.has_value())
            SetlistBankChangeDirect(*pendingBankDelta);
        if (pendingBankSelect.has_value())
            SelectSetlistBankDirect(*pendingBankSelect);
        if (pendingSceneIndex.has_value())
            SelectSceneByIndexDirect(*pendingSceneIndex);
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

    // Periodic updates. The telemetry feeds are the app's largest ongoing cost — signal
    // diagnostics alone is ~6.7 KB at 20 Hz — and they only exist to drive on-screen meters,
    // so they are suppressed entirely while the UI is hidden. The counters keep running so
    // the cadence does not jump when it comes back.
    const bool uiVisible = mUiVisible.load(std::memory_order_acquire);

    mDSPPerformanceUpdateCounter++;
    if (mDSPPerformanceUpdateCounter >= 60 / kDspPerformanceStatsRateHz) // fire at kDspPerformanceStatsRateHz; actual sends are rate-limited below
    {
        mDSPPerformanceUpdateCounter = 0;
        if (uiVisible)
            RequestPerformanceStatsToUI();
    }

    TrySendPendingPerformanceStatsToUI();

    if (mSignalDiagnosticsEnabled.load(std::memory_order_acquire))
    {
        mSignalDiagnosticsUpdateCounter++;
        if (mSignalDiagnosticsUpdateCounter >= 60 / kSignalDiagnosticsRateHz)
        {
            mSignalDiagnosticsUpdateCounter = 0;
            if (uiVisible)
                RequestSignalDiagnosticsToUI();
        }
    }

    TrySendPendingSignalDiagnosticsToUI();

    // The spatial panner's on-screen puck has to match what is being heard: on a
    // 30 second orbit a visual phase drift is glaringly obvious. This is sent on its
    // own schedule rather than piggybacking on the diagnostics feed, which the user
    // has to opt into, and it costs nothing when no spatialiser is in the chain.
    mSpatialPositionUpdateCounter++;
    if (mSpatialPositionUpdateCounter >= 60 / kSpatialPositionRateHz)
    {
        mSpatialPositionUpdateCounter = 0;
        SendSpatialPositionsToUI();
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

void PluginController::ReloadSharedSyncSourcesFromDisk()
{
    LoadAppSettings();
    ApplyMetronomeSettingsFromAppSettings();
    ApplyDiagnosticsSettingsFromAppSettings();
    ApplyDspLevelTargetSettingsFromAppSettings();
    ApplyProcessingModeSettingsFromAppSettings();
    ApplyInputModeSettingsFromAppSettings();
    ApplyGlobalFxSettingsFromAppSettings();
    ApplyNamSlimmableSettingsFromAppSettings();
    ApplyNamInterfaceCalibrationFromAppSettings();
    ApplyUserInputCalibrationSettingsFromAppSettings();
    ApplyUiSettingsFromAppSettings();

    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadCustomEffectLibrary();

    std::vector<std::string> definitionIds;
    definitionIds.reserve(mCompositeLibrary.GetAllDefinitions().size());
    for (const auto& def : mCompositeLibrary.GetAllDefinitions())
        definitionIds.push_back(def.id);
    for (const auto& id : definitionIds)
        mCompositeLibrary.RemoveDefinition(id);
    LoadCompositeLibrary();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }

    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());
    if (!automationData.empty())
        mAutomationSlots.LoadFromJson(automationData);

    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    InvalidateResourceUsageIndex();
}

void PluginController::PollSharedSyncState()
{
    const auto now = std::chrono::steady_clock::now();
    if (now < mNextSharedSyncPollAt)
        return;

    mNextSharedSyncPollAt = now + kSharedSyncPollInterval;

    const auto path = ResolveSharedSyncStatePath(mFileSystem);
    const auto payload = LoadJsonFile(path, nlohmann::json::object());
    if (!payload.is_object())
        return;

    const auto versionIt = payload.find("version");
    if (versionIt == payload.end() || !versionIt->is_number_unsigned())
        return;

    const auto version = versionIt->get<std::uint64_t>();
    if (!mSharedSyncVersionSeenInitialized)
    {
        mSharedSyncVersionSeen = version;
        mSharedSyncVersionSeenInitialized = true;
        return;
    }

    if (version <= mSharedSyncVersionSeen)
        return;

    mSharedSyncVersionSeen = version;
    if (!mUIReady)
        return;

    nlohmann::json msg;
    msg["type"] = "sharedSyncUpdated";
    msg["version"] = version;
    if (payload.contains("domains") && payload["domains"].is_array())
        msg["domains"] = payload["domains"];
    if (payload.contains("updatedAt"))
        msg["updatedAt"] = payload["updatedAt"];
    SendMessageToUI(msg.dump());
}

// ════════════════════════════════════════════════════════════════════
// Parameter bridging
// ════════════════════════════════════════════════════════════════════

void PluginController::OnParamChange(int paramIdx, double value)
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        ApplyParamChangeLocked(paramIdx, value);
    }

    if (IsPersistedGlobalFxParam(paramIdx))
        PersistGlobalFxSettingsToAppSettings();
}

void PluginController::ApplyParamChangeLocked(int paramIdx, double value)
{
    if (paramIdx < 0 || paramIdx >= kParamCount)
        return;

    mParamValues[static_cast<size_t>(paramIdx)] = value;
    const bool latencyMayHaveChanged = (paramIdx == kParamTranspose);

    // Route to mixer
    switch (paramIdx)
    {
    case kParamInputTrim:    mPresetMixer.SetGlobalInputGain(value); break;
    case kParamOutputTrim:   mPresetMixer.SetGlobalOutputGain(value); break;
    case kParamDrive:        mPresetMixer.SetAmpDrive(value); break;
    case kParamTone:         mPresetMixer.SetAmpTone(value); break;
    case kParamGateEnabled:  mPresetMixer.SetGlobalGateEnabled(value > 0.5); break;
    case kParamGateThreshold: mPresetMixer.SetGlobalGateThreshold(value); break;
    case kParamDoublerEnabled: mPresetMixer.SetGlobalDoublerEnabled(value > 0.5); break;
    case kParamDoublerDelay: mPresetMixer.SetGlobalDoublerDelay(value); break;
    case kParamTranspose:    mPresetMixer.SetGlobalTranspose(static_cast<int>(value)); break;
    case kParamIRQuality:    mPresetMixer.SetIRQuality(value); break;
    case kParamEQEnabled:    mPresetMixer.SetGlobalEQEnabled(value > 0.5); break;
    case kParamEQLowGain:    mPresetMixer.SetGlobalEQBandGain(0, value); break;
    case kParamEQLowFreq:    mPresetMixer.SetGlobalEQBandFrequency(0, value); break;
    case kParamEQLowMidGain: mPresetMixer.SetGlobalEQBandGain(1, value); break;
    case kParamEQLowMidFreq: mPresetMixer.SetGlobalEQBandFrequency(1, value); break;
    case kParamEQLowMidQ:    mPresetMixer.SetGlobalEQBandQ(1, value); break;
    case kParamEQHighMidGain: mPresetMixer.SetGlobalEQBandGain(2, value); break;
    case kParamEQHighMidFreq: mPresetMixer.SetGlobalEQBandFrequency(2, value); break;
    case kParamEQHighMidQ:   mPresetMixer.SetGlobalEQBandQ(2, value); break;
    case kParamEQHighGain:   mPresetMixer.SetGlobalEQBandGain(3, value); break;
    case kParamEQHighFreq:   mPresetMixer.SetGlobalEQBandFrequency(3, value); break;
    default: break;
    }

    if (latencyMayHaveChanged)
        UpdateHostLatency();
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
    const bool added = mPresetMixer.AddActivePreset(preset, presetId, name);
    if (added)
    {
        AttachRuntimeConfigCallbacks(presetId, preset);
        try { mMixerPresetJsonCache[presetId] = PresetStorage::SerializeToJson(preset); }
        catch (...) {}
        UpdateHostLatency();
    }
    return added;
}

bool PluginController::AddActivePresetById(const std::string& presetId)
{
    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        ReportErrorToUI("Cannot add preset to mixer", "Factory preset archive loading is disabled in Advanced settings");
        return false;
    }

    // If the active preset matches, use it directly
    if (mActivePreset && mActivePreset->id == resolvedPresetId)
    {
        return AddActivePreset(*mActivePreset, resolvedPresetId, mActivePreset->name);
    }

    // Try loading from user presets directory
    if (!mUserPresetsPath.empty())
    {
        auto userPath = mUserPresetsPath / (resolvedPresetId + ".json");
        auto presetOpt = PresetStorage::LoadFromFile(userPath);
        if (presetOpt)
        {
            return AddActivePreset(*presetOpt, resolvedPresetId, presetOpt->name);
        }
    }

    // Try loading from factory presets directory
    auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
    auto presetOpt = PresetStorage::LoadFromFile(factoryPath);
    if (!presetOpt)
    {
        auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);
        if (archiveIt != mFactoryArchivePresets.end())
            presetOpt = archiveIt->second;
    }
    if (presetOpt)
    {
        return AddActivePreset(*presetOpt, resolvedPresetId, presetOpt->name);
    }

    ReportErrorToUI("Cannot add preset to mixer", "Preset '" + presetId + "' not found");
    return false;
}

bool PluginController::ApplyActivePresetById(const std::string& presetId)
{
    if (presetId.empty())
        return false;

    auto presetOpt = LoadPresetById(presetId);
    if (!presetOpt && mActivePreset
        && (mActivePreset->id == presetId || mActivePresetId == presetId))
    {
        // Unsaved/session-only preset that is already loaded — re-apply what we have.
        presetOpt = *mActivePreset;
    }
    if (!presetOpt)
    {
        ReportErrorToUI("Cannot load preset", "Preset '" + presetId + "' not found");
        return false;
    }

    Preset preset = std::move(*presetOpt);
    NormalizePresetScenes(preset);
    if (!SetPresetActiveScene(preset, std::string{}, &mActiveSceneId))
        mActiveSceneId = GetDefaultPresetSceneId(preset);

    mActivePresetId = presetId;

    // Single-instance swap: ApplyPreset() crossfades the outgoing chain out and the new one
    // in, and sets mActivePreset/mActivePresetJson and the mixer slot cache itself.
    ApplyPreset(preset);

    mPendingPresetStateBroadcast = true;

    if (mActivePreset)
    {
        nlohmann::json loaded;
        loaded["type"] = "presetLoaded";
        loaded["preset"] = SerializePresetForUi(*mActivePreset);
        nlohmann::json activeIds = nlohmann::json::array();
        for (const auto& id : mPresetMixer.GetActivePresetIds())
            activeIds.push_back(id);
        loaded["activePresetIds"] = activeIds;
        loaded["sceneId"] = GetResolvedActiveSceneId();
        SendMessageToUI(loaded.dump());
    }

    if (!IsPresetArchiveSessionActive())
    {
        mAppSettings["lastPresetId"] = mActivePresetId;
        SaveAppSettings();
    }

    return true;
}

void PluginController::RemoveActivePreset(const std::string& presetId)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.RemoveActivePreset(presetId);
    mMixerPresetJsonCache.erase(presetId);
    UpdateHostLatency();
}

void PluginController::FocusMixerPreset(const std::string& presetId)
{
    if (presetId.empty() || presetId == mActivePresetId)
        return;

    const auto it = mMixerPresetJsonCache.find(presetId);
    if (it == mMixerPresetJsonCache.end())
    {
        AppendSessionLog("FocusMixerPreset: no cached preset data for slot=" + presetId);
        return;
    }

    auto presetOpt = PresetStorage::DeserializeFromJson(it->second);
    if (!presetOpt)
    {
        AppendSessionLog("FocusMixerPreset: failed to deserialize cached preset for slot=" + presetId);
        return;
    }

    // Switch the editing/display target only. The DSP mixer instances keep running
    // untouched so audio is unaffected — this just makes graph edits and the
    // broadcast "preset" state target the mixer slot the user is currently viewing.
    NormalizePresetScenes(*presetOpt);
    mActiveSceneId = GetDefaultPresetSceneId(*presetOpt);
    mActivePreset = std::move(presetOpt);
    mActivePresetId = presetId;
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[presetId] = mActivePresetJson;

    mPendingPresetStateBroadcast = true;
}

bool PluginController::ReplaceActiveMixerPresetInPlace(const Preset& preset, const std::string& presetId, const std::string& name)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    const bool replaced = mPresetMixer.ReplaceActivePresetInPlace(preset, presetId, name);
    if (replaced)
    {
        AttachRuntimeConfigCallbacks(presetId, preset);
        try { mMixerPresetJsonCache[presetId] = PresetStorage::SerializeToJson(preset); }
        catch (...) {}
        UpdateHostLatency();
    }
    return replaced;
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

void PluginController::SetMultiThreadedProcessingEnabled(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetMultiThreadedProcessingEnabled(enabled);
    }
    mAppSettings[kMultiThreadedProcessingSettingKey] = enabled;
    SaveAppSettings();
    mPendingStateBroadcast = true;
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

void PluginController::AppendSessionLog(const std::string& message) const
{
    if (message.empty())
        return;

    const auto settingsDir = GetEffectiveSettingsDirectory();
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

void PluginController::HandleGetSharedSyncStateRequest()
{
    // Only act if the shared sync state file has a new version since we last responded.
    const auto syncPath = ResolveSharedSyncStatePath(mFileSystem);
    const auto filePayload = LoadJsonFile(syncPath, nlohmann::json::object());
    std::uint64_t currentVersion = 0;
    if (filePayload.is_object())
    {
        const auto it = filePayload.find("version");
        if (it != filePayload.end() && it->is_number_unsigned())
            currentVersion = it->get<std::uint64_t>();
    }

    if (currentVersion > 0 && currentVersion == mSharedSyncVersionHandled)
        return;

    mSharedSyncVersionHandled = currentVersion;
    if (!mSharedSyncVersionSeenInitialized || currentVersion > mSharedSyncVersionSeen)
    {
        mSharedSyncVersionSeen = currentVersion;
        mSharedSyncVersionSeenInitialized = true;
    }

    ReloadSharedSyncSourcesFromDisk();

    nlohmann::json state;
    state["type"] = "sharedSyncState";
    state["appSettings"] = mAppSettings;
    state["uiSettings"] = mUiSettings;
    state["blendLibrary"] = mBlendLibrary;
    state["presetArchiveSession"] = {
        {"active", IsPresetArchiveSessionActive()}
    };
    if (mPresetArchiveSession)
    {
        state["presetArchiveSession"]["archiveName"] = mPresetArchiveSession->archiveName;
        state["presetArchiveSession"]["archiveKey"] = mPresetArchiveSession->archiveKey;
        state["presetArchiveSession"]["presetCount"] = mPresetArchiveSession->presetCount;
    }

    // Resource library summary + per-type entries for UI rendering
    nlohmann::json libraryInfo = nlohmann::json::object();
    auto allResources = mResourceLibrary.GetAllResources();
    libraryInfo["totalCount"] = allResources.size();

    for (const auto& resource : allResources)
    {
        const std::string type = resource.type;
        if (!libraryInfo.contains(type) || !libraryInfo[type].is_array())
            libraryInfo[type] = nlohmann::json::array();

        nlohmann::json entry;
        entry["id"] = resource.id;
        entry["name"] = resource.name;
        entry["category"] = resource.category;
        entry["description"] = resource.description;
        entry["tags"] = resource.tags;
        entry["filePath"] = resource.filePath.empty() ? "" : util::PathToUtf8(resource.filePath);
        entry["hash"] = resource.hash;
        if (!resource.metadata.empty())
            entry["metadata"] = resource.metadata;
        const bool hasPath = !resource.filePath.empty();
        const bool exists = hasPath && std::filesystem::exists(resource.filePath);
        entry["fileMissing"] = !(hasPath && exists);

        libraryInfo[type].push_back(entry);
    }
    state["resourceLibrary"] = std::move(libraryInfo);

    {
        nlohmann::json customEffects = nlohmann::json::array();
        for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
            customEffects.push_back(SerializeCustomEffectLibraryEntry(entry));
        state["customEffectLibrary"] = std::move(customEffects);
    }

    SendMessageToUI(state.dump());

    // Send auxiliary shared datasets over their existing message contracts.
    HandleGetThemeRequest();
    HandleGetPresetListRequest();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    HandleGetSetlistsRequest();
    HandleGetAutomationRequest();
    SendCompositeLibraryToUI();
    SendCompositePresetListToUI();
    SendRiffLibraryStateToUI();
}

void PluginController::HandleCaptureDebugSnapshotRequest(const nlohmann::json& payload)
{
    const std::string source = payload.value("source", "manual");

    if (!mUIReady)
    {
        HandleDebugReportUiStateRequest(nlohmann::json{
            {"source", source + ":backend-only"},
        });
        return;
    }

    SendMessageToUI(nlohmann::json{
        {"type", "captureDebugSnapshot"},
        {"source", source},
    }.dump());
}

void PluginController::HandleDebugReportUiStateRequest(const nlohmann::json& payload)
{
    try
    {
        const std::string source = payload.value("source", "ui-auto");
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["type"] = "debugSnapshot";
        snapshot["capturedAt"] = FormatTimestamp();
        snapshot["source"] = source;
        snapshot["paths"] = {
            {"sessionLog", (mFileSystem.ResolveSettingsDirectory() / kSessionLogFileName).generic_string()},
            {"snapshot", ResolveDebugSnapshotPath(mFileSystem).generic_string()},
        };
        snapshot["session"] = {
            {"activePresetId", mActivePresetId},
            {"activeSceneId", GetResolvedActiveSceneId()},
            {"uiReady", mUIReady},
            {"pendingStateBroadcast", mPendingStateBroadcast},
            {"activePresetIds", mPresetMixer.GetActivePresetIds()},
        };

        if (payload.contains("snapshot"))
        {
            snapshot["ui"] = payload["snapshot"];
            ScrubSensitiveJson(snapshot["ui"]);
        }

        nlohmann::json backendState = nlohmann::json::parse(SerializeState());
        ScrubSensitiveJson(backendState);
        snapshot["backend"] = std::move(backendState);

        if (mActivePreset)
            snapshot["activePresetSummary"] = SummarizeHostedPluginState(*mActivePreset);

        const auto snapshotPath = ResolveDebugSnapshotPath(mFileSystem);
        SaveJsonFile(mFileSystem, snapshotPath, snapshot);

        SendMessageToUI(nlohmann::json{
            {"type", "debugSnapshotWritten"},
            {"path", snapshotPath.generic_string()},
            {"source", source},
        }.dump());
    }
    catch (const std::exception& e)
    {
        AppendSessionLog("Debug snapshot write failed: " + std::string{e.what()});
    }
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

        const std::string requestedPresetId = payload.value("presetId", preset.id);
        const bool scrubbedHostedState = PresetHasScrubbedHostedPluginState(preset);
        AppendSessionLog("Hosted plugin load request presetId="
            + (requestedPresetId.empty() ? std::string{"<none>"} : requestedPresetId)
            + ", scrubbed=" + std::string{scrubbedHostedState ? "true" : "false"}
            + ", payload=" + SummarizeHostedPluginState(preset));
        if (!requestedPresetId.empty() && scrubbedHostedState)
        {
            if (auto storedPreset = TryLoadStoredPresetById(requestedPresetId))
            {
                AppendSessionLog("Hosted plugin load rehydrated presetId=" + requestedPresetId
                    + " from authoritative source: " + SummarizeHostedPluginState(*storedPreset));
                preset = std::move(*storedPreset);
            }
            else
            {
                AppendSessionLog("Hosted plugin load could not rehydrate presetId=" + requestedPresetId);
            }
        }

        NormalizePresetScenes(preset);

        const std::string requestedSceneId = payload.value("sceneId", "");
        if (!SetPresetActiveScene(preset, requestedSceneId, &mActiveSceneId))
            mActiveSceneId = GetDefaultPresetSceneId(preset);

        ApplyBlendDefinitions(preset);

        AppendSessionLog("Hosted plugin load applying presetId="
            + (requestedPresetId.empty() ? preset.id : requestedPresetId)
            + ", final=" + SummarizeHostedPluginState(preset));

        mActivePresetId = requestedPresetId.empty() ? preset.id : requestedPresetId;

        // If this preset is already one of several active mixer slots (e.g. the user is
        // switching scenes on a preset that's part of a multi-preset mix), rebuild just
        // that slot in place. ApplyPreset()'s PreparePresetSwap()/CommitPresetSwap() swap
        // the *entire* mixer down to a single instance, which would silently drop every
        // other active mixer preset.
        const auto activeMixerIds = mPresetMixer.GetActivePresetIds();
        const bool isActiveMixerSlot = activeMixerIds.size() > 1
            && std::find(activeMixerIds.begin(), activeMixerIds.end(), mActivePresetId) != activeMixerIds.end();

        if (isActiveMixerSlot && ReplaceActiveMixerPresetInPlace(preset, mActivePresetId, preset.name))
        {
            mActivePreset = preset;
            mActivePresetJson = PresetStorage::SerializeToJson(preset);
        }
        else
        {
            ApplyPreset(preset); // SetGlobalChainConfig is called inside ApplyPreset under mDSPMutex
        }

        mPendingPresetStateBroadcast = true;

        // Send explicit "presetLoaded" confirmation to the UI
        {
            nlohmann::json loaded;
            loaded["type"] = "presetLoaded";
            loaded["preset"] = SerializePresetForUi(*mActivePreset);
            nlohmann::json activeIds = nlohmann::json::array();
            for (const auto& id : mPresetMixer.GetActivePresetIds())
                activeIds.push_back(id);
            loaded["activePresetIds"] = activeIds;
            loaded["sceneId"] = GetResolvedActiveSceneId();
            SendMessageToUI(loaded.dump());
        }

        if (!IsPresetArchiveSessionActive())
        {
            mAppSettings["lastPresetId"] = mActivePresetId;
            SaveAppSettings();
        }
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


std::optional<Preset> PluginController::TryLoadStoredPresetById(const std::string& presetId)
{
    if (presetId.empty())
        return std::nullopt;

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (mActivePreset)
    {
        const bool matchesActivePreset = mActivePreset->id == resolvedPresetId
            || (!mActivePresetId.empty() && (mActivePresetId == presetId || mActivePresetId == resolvedPresetId));
        if (matchesActivePreset)
        {
            Preset preset = *mActivePreset;
            CaptureRuntimePluginStates(preset, mActivePresetId.empty() ? resolvedPresetId : mActivePresetId);
            AppendSessionLog("Hosted plugin rehydrate source=active presetId=" + resolvedPresetId
                + ", state=" + SummarizeHostedPluginState(preset));
            return preset;
        }
    }

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
        return std::nullopt;

    const auto presetDirectory = GetEffectiveUserPresetDirectory();
    const auto userPath = presetDirectory / (resolvedPresetId + ".json");
    if (!presetDirectory.empty() && std::filesystem::exists(userPath))
    {
        if (auto presetOpt = PresetStorage::LoadFromFile(userPath))
        {
            AppendSessionLog("Hosted plugin rehydrate source=user-file presetId=" + resolvedPresetId
                + ", path=" + userPath.generic_string() + ", state=" + SummarizeHostedPluginState(*presetOpt));
            return presetOpt;
        }
    }

    if (IsPresetArchiveSessionActive())
        return std::nullopt;

    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
    if (std::filesystem::exists(factoryPath))
    {
        if (auto presetOpt = PresetStorage::LoadFromFile(factoryPath))
        {
            AppendSessionLog("Hosted plugin rehydrate source=factory-file presetId=" + resolvedPresetId
                + ", path=" + factoryPath.generic_string() + ", state=" + SummarizeHostedPluginState(*presetOpt));
            return presetOpt;
        }
    }

    const auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);
    if (archiveIt != mFactoryArchivePresets.end())
    {
        AppendSessionLog("Hosted plugin rehydrate source=factory-archive presetId=" + resolvedPresetId
            + ", state=" + SummarizeHostedPluginState(archiveIt->second));
        return archiveIt->second;
    }

    return std::nullopt;
}
void PluginController::HandleSetGlobalChainParamRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    auto value = payload.value("value", nlohmann::json());
    bool persistGlobalFx = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        // Route paramPath strings to the corresponding mixer methods
        if (path == "gate.enabled") { mPresetMixer.SetGlobalGateEnabled(value.get<bool>()); persistGlobalFx = true; }
        else if (path == "gate.threshold") { mPresetMixer.SetGlobalGateThreshold(value.get<double>()); persistGlobalFx = true; }
        else if (path == "gate.attack") { mPresetMixer.SetGlobalGateAttack(value.get<double>()); persistGlobalFx = true; }
        else if (path == "gate.hold") { mPresetMixer.SetGlobalGateHold(value.get<double>()); persistGlobalFx = true; }
        else if (path == "gate.release") { mPresetMixer.SetGlobalGateRelease(value.get<double>()); persistGlobalFx = true; }
        else if (path == "transpose.enabled")
        {
            const bool enabled = value.get<bool>();
            mPresetMixer.SetGlobalTransposeEnabled(enabled);
            if (!enabled)
                mParamValues[kParamTranspose] = 0.0;
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "transpose.semitones")
        {
            const int semitones = std::clamp(value.get<int>(), -12, 12);
            mPresetMixer.SetGlobalTranspose(semitones);
            mParamValues[kParamTranspose] = static_cast<double>(semitones);
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "eq.enabled") { mPresetMixer.SetGlobalEQEnabled(value.get<bool>()); persistGlobalFx = true; }
        else if (path == "doubler.enabled") { mPresetMixer.SetGlobalDoublerEnabled(value.get<bool>()); persistGlobalFx = true; }
        else if (path == "doubler.delay") { mPresetMixer.SetGlobalDoublerDelay(value.get<double>()); persistGlobalFx = true; }
        else if (path == "doubler.mix") { mPresetMixer.SetGlobalDoublerMix(value.get<double>()); persistGlobalFx = true; }
        else if (path == "doubler.detune") { mPresetMixer.SetGlobalDoublerDetune(value.get<double>()); persistGlobalFx = true; }
        else if (path == "input.gain")
        {
            const double gainDb = value.get<double>();
            mPresetMixer.SetGlobalInputGain(gainDb);
            mParamValues[kParamInputTrim] = gainDb;
            persistGlobalFx = true;
        }
        else if (path == "output.gain")
        {
            const double gainDb = value.get<double>();
            mPresetMixer.SetGlobalOutputGain(gainDb);
            mParamValues[kParamOutputTrim] = gainDb;
            persistGlobalFx = true;
        }
        else if (path == "limiter.enabled") mPresetMixer.SetLimiterEnabled(value.get<bool>());
        else if (path == "eq.lowGain") { mPresetMixer.SetGlobalEQBandGain(0, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.lowFreq") { mPresetMixer.SetGlobalEQBandFrequency(0, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.lowMidGain") { mPresetMixer.SetGlobalEQBandGain(1, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.lowMidFreq") { mPresetMixer.SetGlobalEQBandFrequency(1, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.lowMidQ") { mPresetMixer.SetGlobalEQBandQ(1, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.highMidGain") { mPresetMixer.SetGlobalEQBandGain(2, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.highMidFreq") { mPresetMixer.SetGlobalEQBandFrequency(2, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.highMidQ") { mPresetMixer.SetGlobalEQBandQ(2, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.highGain") { mPresetMixer.SetGlobalEQBandGain(3, value.get<double>()); persistGlobalFx = true; }
        else if (path == "eq.highFreq") { mPresetMixer.SetGlobalEQBandFrequency(3, value.get<double>()); persistGlobalFx = true; }
    }

    if (persistGlobalFx)
        PersistGlobalFxSettingsToAppSettings();

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
                payload["path"] = util::PathToUtf8(result.path);
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
                payload["path"] = util::PathToUtf8(result.path);
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
        mTunerActive.store(true, std::memory_order_release);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            if (payload.contains("liveMode"))
                mPresetMixer.SetLiveTunerMode(payload.value("liveMode", true));

            if (payload.contains("referenceFrequency"))
                mPresetMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());

            mPresetMixer.SetTunerEnabled(true);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = referenceFrequency;
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "stop")
    {
        mTunerActive.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(false);
        }

        nlohmann::json message;
        message["type"] = "tunerStopped";
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetLiveTunerMode(liveMode);
        }

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        double effectiveFrequency = 440.0;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerReferenceFrequency(freq);
            effectiveFrequency = mPresetMixer.GetTunerReferenceFrequency();
        }

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = effectiveFrequency;
        SendMessageToUI(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        mTunerActive.store(enabled, std::memory_order_release);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(enabled);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = referenceFrequency;
        reply["liveMode"] = liveMode;
        SendMessageToUI(reply.dump());
    }
}

void PluginController::HandleSetInputModeRequest(const nlohmann::json& payload)
{
    // In hosted-plugin mode the DAW owns the input configuration; ignore
    // UI overrides and report the effective (host-controlled) state.
    if (mHost.IsStandalone())
    {
        if (payload.contains("monoMode"))
            mPresetMixer.SetMonoMode(payload["monoMode"].get<bool>());
        else if (payload.contains("mono"))
            mPresetMixer.SetMonoMode(payload["mono"].get<bool>());

        if (payload.contains("inputChannel"))
            mPresetMixer.SetInputChannel(payload["inputChannel"].get<int>());
        else if (payload.contains("channel"))
            mPresetMixer.SetInputChannel(payload["channel"].get<int>());
    }
    else
    {
        AppendSessionLog("Ignoring setInputMode request: input is host-controlled in plugin mode");
    }

    nlohmann::json message;
    message["type"] = "inputModeChanged";
    message["monoMode"] = mPresetMixer.IsMonoMode();
    message["inputChannel"] = mPresetMixer.GetInputChannel();
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetProcessingModeRequest(const nlohmann::json& payload)
{
    bool enabled = mPresetMixer.IsMultiThreadedProcessingEnabled();

    if (payload.contains("multiThreaded") && payload["multiThreaded"].is_boolean())
        enabled = payload["multiThreaded"].get<bool>();
    else if (payload.contains("enabled") && payload["enabled"].is_boolean())
        enabled = payload["enabled"].get<bool>();
    else if (payload.contains("mode") && payload["mode"].is_string())
    {
        const std::string mode = payload["mode"].get<std::string>();
        if (mode == "single" || mode == "singleThreaded")
            enabled = false;
        else if (mode == "multi" || mode == "multiThreaded")
            enabled = true;
    }

    SetMultiThreadedProcessingEnabled(enabled);

    nlohmann::json message;
    message["type"] = "processingModeChanged";
    message["multiThreaded"] = enabled;
    message["mode"] = enabled ? "multiThreaded" : "singleThreaded";
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
    (void)payload;

    // Mixer-wide peak auto-leveling is retired in favor of model metadata plus
    // explicit input/output controls. Keep the message for compatibility but
    // force the legacy path off.
    mPresetMixer.SetAutoLevelInput(false);
    mPresetMixer.SetAutoLevelOutput(false);
    mAppSettings["autoLevelInput"] = false;
    mAppSettings["autoLevelOutput"] = false;
    SaveAppSettings();

    nlohmann::json message;
    message["type"] = "autoLevelChanged";
    message["autoInput"] = false;
    message["autoOutput"] = false;
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

    if (payload.contains("beatPattern") && payload["beatPattern"].is_string())
    {
        std::string validated;
        for (const char ch : payload.value("beatPattern", std::string{}))
        {
            const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (upper == 'H' || upper == 'L' || upper == 'S' || upper == '-' || upper == '.')
                validated += upper;
        }
        mMetronomeBeatPattern = validated;
        mAppSettings[kMetronomeBeatPatternSettingKey] = validated;
        stateChanged = true;
        settingsChanged = true;
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

    std::filesystem::path filePath = util::PathFromUtf8(path);
    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Model file not found", path);
        return;
    }

    std::string resourceError;
    const auto savedResource = SaveLocalLibraryResource(
        nlohmann::json{
            {"resourceType", "nam"},
            {"filePath", util::PathToUtf8(filePath)},
            {"name", util::PathToUtf8(filePath.stem())},
            {"category", "Local"},
            {"metadata", nlohmann::json::object({{"provider", kLocalResourceProvider}})}
        },
        resourceError,
        true);
    if (!savedResource)
    {
        ReportErrorToUI("Model load failed", resourceError.empty() ? "Could not register model in the resource library" : resourceError);
        return;
    }

    const bool updatedNamResource =
        UpdateResourceForNodeType(EffectGuids::kAmpNamOptimized, savedResource->type, filePath)
        || UpdateResourceForNodeType(EffectGuids::kAmpNamBlend, savedResource->type, filePath)
        || UpdateResourceForNodeType(EffectGuids::kFxNam, savedResource->type, filePath)
        || UpdateResourceForNodeType(EffectGuids::kAmpNam, savedResource->type, filePath);

    if (updatedNamResource)
    {
        mAppSettings["lastModelPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleLoadIRRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    if (path.empty())
        path = payload.value("filePath", "");
    if (path.empty()) return;

    std::filesystem::path filePath = util::PathFromUtf8(path);
    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("IR file not found", path);
        return;
    }

    if (UpdateResourceForNodeType(EffectGuids::kCabIr, "ir", filePath))
    {
        mAppSettings["lastIRPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleSavePresetRequest(const nlohmann::json& payload)
{
    const std::string presetName = payload.value("name", "");
    const std::string presetCategory = payload.value("category", "User");
    const std::string presetDescription = payload.value("description", "");
    const std::string presetIdOverride = payload.value("presetId", "");
    const std::string saveMode = payload.value("saveMode", "");
    std::string sourcePresetId = payload.value("sourcePresetId", "");
    const bool requireNewPresetId = payload.value("requireNewPresetId", saveMode == "save-as");
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
        NormalizePresetScenes(newPreset);
        EnsurePresetBoundaryGainNodes(newPreset);
        if (sourcePresetId.empty())
            sourcePresetId = mActivePresetId;

        std::string resolvedPresetId = presetIdOverride.empty()
            ? GenerateUserPresetId()
            : presetIdOverride;
        if (requireNewPresetId && !sourcePresetId.empty() && resolvedPresetId == sourcePresetId)
            resolvedPresetId = GenerateUserPresetId();

        newPreset.id = std::move(resolvedPresetId);
        newPreset.name = presetName;
        newPreset.category = presetCategory;
        newPreset.description = presetDescription;
        newPreset.version = 2;

        auto currentChain = mPresetMixer.GetGlobalChainConfig();
        currentChain.autoLevelInput = false;
        currentChain.autoLevelOutput = false;

        newPreset.global.inputTrim = currentChain.inputGain;
        newPreset.global.outputTrim = currentChain.outputGain;
        newPreset.global.transpose = static_cast<int>(mParamValues[kParamTranspose]);
        newPreset.global.autoLevelInput = false;
        newPreset.global.autoLevelOutput = false;

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
                newPreset.globalSignalChain = currentChain;
            }

            if (newPreset.globalSignalChain.has_value())
            {
                newPreset.globalSignalChain->inputGain = currentChain.inputGain;
                newPreset.globalSignalChain->outputGain = currentChain.outputGain;
                newPreset.globalSignalChain->autoLevelInput = false;
                newPreset.globalSignalChain->autoLevelOutput = false;
            }
        }
        else
        {
            newPreset.globalSignalChain.reset();
        }

        const std::string requestedSceneId = payload.value("sceneId", mActiveSceneId);
        if (!SetPresetActiveScene(newPreset, requestedSceneId, &mActiveSceneId))
            mActiveSceneId = GetDefaultPresetSceneId(newPreset);

        const auto presetDirectory = GetEffectiveUserPresetDirectory();
        [[maybe_unused]] const auto ensuredUserPresetPath = mFileSystem.EnsureDirectory(presetDirectory);

        AppendSessionLog("Hosted plugin preset save begin presetId=" + newPreset.id
            + ", sourcePresetId=" + (sourcePresetId.empty() ? std::string{"<none>"} : sourcePresetId)
            + ", beforeCapture=" + SummarizeHostedPluginState(newPreset));

        CaptureRuntimePluginStates(newPreset, sourcePresetId.empty() ? mActivePresetId : sourcePresetId);

        AppendSessionLog("Hosted plugin preset save captured presetId=" + newPreset.id
            + ", afterCapture=" + SummarizeHostedPluginState(newPreset));

        const auto presetPath = presetDirectory / (newPreset.id + ".json");
        if (!PresetStorage::SaveToFile(newPreset, presetPath))
        {
            ReportErrorToUI("Failed to save preset", "Could not write preset file");
            return;
        }

        AppendSessionLog("Hosted plugin preset save wrote presetId=" + newPreset.id
            + ", path=" + presetPath.generic_string() + ", state=" + SummarizeHostedPluginState(newPreset));

        mActivePreset = newPreset;
        mActivePresetId = newPreset.id;
        mActivePresetJson = PresetStorage::SerializeToJson(newPreset);
        mPendingStateBroadcast = true;
        if (!IsPresetArchiveSessionActive())
            SaveAppSettings();
        TouchSharedSyncState({"presetLibrary"});
        InvalidateResourceUsageIndex();

        nlohmann::json reply;
        reply["type"] = "presetSaved";
        reply["preset"] = SerializePresetForUi(newPreset);
        reply["sceneId"] = GetResolvedActiveSceneId();
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

    const auto presetDirectory = GetEffectiveUserPresetDirectory();
    const auto presetPath = presetDirectory / (presetId + ".json");
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
    else
    {
        InvalidateResourceUsageIndex();
        TouchSharedSyncState({"presetLibrary"});
    }
}

void PluginController::HandleStartPresetArchiveSessionRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string archiveFileName = payload.value("fileName", "preset-archive.soundshed.presets");
    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", "Missing archive data"}
        }.dump());
        return;
    }

    const auto archiveBytes = util::DecodeBase64(dataEncoded);
    if (archiveBytes.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", "Invalid archive payload"}
        }.dump());
        return;
    }

    try
    {
        StartPresetArchiveSession(archiveFileName, archiveBytes);
    }
    catch (const std::exception& e)
    {
        ReportErrorToUI("Preset archive session failed", e.what());
        SendMessageToUI(nlohmann::json{
            {"type", "presetArchiveSessionFailed"},
            {"message", e.what()}
        }.dump());
    }
}

void PluginController::HandleEndPresetArchiveSessionRequest()
{
    EndPresetArchiveSession(true);
}

void PluginController::HandleGetPresetByIdRequest(const nlohmann::json& payload)
{
    const std::string presetId = payload.value("presetId", "");
    const std::string requestId = payload.value("requestId", "");
    if (presetId.empty())
        return;

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end() ? aliasIt->second : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
    {
        nlohmann::json msg;
        msg["type"] = "error";
        msg["message"] = "Preset unavailable";
        msg["detail"] = "Factory preset archive loading is disabled in Advanced settings";
        if (!requestId.empty())
            msg["requestId"] = requestId;
        msg["presetId"] = presetId;
        SendMessageToUI(msg.dump());
        return;
    }

    if (mUserPresetsPath.empty())
        mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";

    const auto userPath = mUserPresetsPath / (resolvedPresetId + ".json");
    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");

    std::optional<Preset> presetOpt;
    if (std::filesystem::exists(userPath))
        presetOpt = PresetStorage::LoadFromFile(userPath);
    if (!presetOpt && std::filesystem::exists(factoryPath))
        presetOpt = PresetStorage::LoadFromFile(factoryPath);
    if (!presetOpt)
    {
        auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);
        if (archiveIt != mFactoryArchivePresets.end())
            presetOpt = archiveIt->second;
    }

    if (!presetOpt)
    {
        nlohmann::json msg;
        msg["type"] = "error";
        msg["message"] = "Preset not found";
        msg["detail"] = presetId;
        if (!requestId.empty())
            msg["requestId"] = requestId;
        msg["presetId"] = presetId;
        SendMessageToUI(msg.dump());
        return;
    }

    nlohmann::json msg;
    msg["type"] = "presetData";
    msg["preset"] = SerializePresetForUi(*presetOpt);
    if (!requestId.empty())
        msg["requestId"] = requestId;
    msg["requestedPresetId"] = presetId;
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
    // Prefer an explicit presetId from the payload; fall back to the active preset ID so
    // that effects whose instance is keyed by a real UUID (not "p1") still receive the update.
    std::string presetId = payload.value("presetId", std::string{});
    if (presetId.empty())
        presetId = mActivePresetId.empty() ? "p1" : mActivePresetId;

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->params[paramKey] = value;
    SyncActivePresetSceneGraph();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeParam(presetId, nodeId, paramKey, value);
    }
    // Some parameters (e.g. the convolution low-latency toggle) change a node's
    // processing latency. Re-report total plugin latency so the host updates PDC.
    if (paramKey == "lowLatency" || paramKey == "oversampling" || paramKey == "antiAliasPhase")
        UpdateHostLatency();
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
}

void PluginController::HandleUpdateSignalPathNodeBypassRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    if (payload.contains("bypassed"))
        enabled = !payload.value("bypassed", false);
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    std::string presetId = payload.value("presetId", fallbackId);

    auto* graph = ResolveEditTarget();
    if (!graph) return;

    auto* node = graph->FindNode(nodeId);
    if (!node) return;

    node->enabled = enabled;
    SyncActivePresetSceneGraph();
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    }
    mActivePresetJson = mActivePreset ? PresetStorage::SerializeToJson(*mActivePreset) : "{}";
    mPendingStateBroadcast = true;
}

void PluginController::HandleUpdateSignalPathNodeConfigRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", payload.value("configKey", ""));
    std::string value = payload.value("value", "");
    const bool persist = payload.value("persist", true);
    const bool capture = payload.value("capture", false) || value == "__capture_plugin_state__";
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    const std::string presetId = payload.value("presetId", fallbackId);
    bool notifyStateChanged = false;

    if (nodeId.empty() || key.empty())
        return;

    if (capture)
    {
        AppendSessionLog("Hosted plugin capture requested presetId=" + presetId + ", nodeId=" + nodeId);
        key = "pluginStateBase64";
        value = mPresetMixer.GetNodeConfig(presetId, nodeId, key);
        if (value.empty())
        {
            AppendSessionLog("Hosted plugin capture failed presetId=" + presetId + ", nodeId=" + nodeId + ": empty runtime state");
            ReportErrorToUI("Plugin state capture failed", "No hosted plugin state was available for node " + nodeId);
            return;
        }

        AppendSessionLog("Hosted plugin capture succeeded presetId=" + presetId + ", nodeId=" + nodeId
            + ", stateLength=" + std::to_string(value.size())
            + ", stateHash=" + HashStringForLog(value));
    }

    mPresetMixer.SetNodeConfig(presetId, nodeId, key, value);

    if (persist)
    {
        auto* graph = ResolveEditTarget();
        auto* node = graph ? graph->FindNode(nodeId) : nullptr;
        if (!node)
            return;

        node->config[key] = value;
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
            mMixerPresetJsonCache[presetId] = mActivePresetJson;
            mPendingStateBroadcast = true;
            notifyStateChanged = true;
        }
    }

    if (capture)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "signalPathNodeConfigUpdated"},
            {"presetId", presetId},
            {"nodeId", nodeId},
            {"key", key},
            {"captured", true},
            {"valueLength", value.size()},
            {"persist", persist},
            {"dirty", persist}
        }.dump());
    }

    if (notifyStateChanged)
        mHost.NotifyStateChanged();
}

void PluginController::HandleUpdateNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty()) return;
    const bool openPluginEditorAfterLoad = payload.value("openPluginEditorAfterLoad", false);

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

    const auto requestHostedPluginEditorOpen = [this, openPluginEditorAfterLoad](const std::string& targetNodeId,
                                                                                  const ResourceRef& selectedRef)
    {
        if (!openPluginEditorAfterLoad || selectedRef.resourceType != "plugin")
            return;

        HandleUpdateSignalPathNodeConfigRequest(nlohmann::json{
            {"nodeId", targetNodeId},
            {"key", "showPluginEditor"},
            {"value", "1"},
            {"persist", false}
        });
    };

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

        // A clear is signalled by empty resourceId + empty filePath with no parameterValue update.
        // (Contrast with a blend-value-only update which also sends empty IDs but includes parameterValue.)
        const bool isClearOperation = ref.resourceId.empty() && ref.filePath.empty() && !ref.parameterValue.has_value();
        if (isClearOperation)
        {
            slot.resourceId.clear();
            slot.filePath.clear();
            slot.embeddedId.clear();
            slot.parameters.clear();
            slot.parameterValue.reset();
        }
        else
        {
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
        }
        const ResourceRef selectedRef = slot;

        RefreshWasmNodeDescriptor(*target);
        bool appliedPreset = false;

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
            mPendingStateBroadcast = true;
            appliedPreset = true;
        }

        if (!IsCompositeEditMode()
            && IsNamEffectType(target->type)
            && !target->resources.empty()
            && target->resources.front().IsValid())
        {
            ResetNamNodeLevelState(nodeId);
        }
        if (appliedPreset && ReportHostedPluginResourceLoadFailure(nodeId, selectedRef, resourceIndex))
            DiscardFailedHostedPluginResourceSelection(nodeId, selectedRef, resourceIndex);
        else if (appliedPreset)
        {
            NotifyHostedPluginResourceLoadCompleted(nodeId, selectedRef, resourceIndex);
            requestHostedPluginEditorOpen(nodeId, selectedRef);
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
            const ResourceRef selectedRef = ref;
            RefreshWasmNodeDescriptor(*node);
            bool appliedPreset = false;

            if (IsCompositeEditMode())
            {
                BroadcastCompositeEditState();
            }
            else if (mActivePreset)
            {
                SyncActivePresetSceneGraph();
                ApplyPreset(*mActivePreset);
                mPendingStateBroadcast = true;
                appliedPreset = true;
            }

            if (!IsCompositeEditMode()
                && IsNamEffectType(node->type)
                && !node->resources.empty()
                && node->resources.front().IsValid())
            {
                ResetNamNodeLevelState(node->id);
            }
            if (appliedPreset && ReportHostedPluginResourceLoadFailure(nodeId, selectedRef))
                DiscardFailedHostedPluginResourceSelection(nodeId, selectedRef);
            else if (appliedPreset)
            {
                NotifyHostedPluginResourceLoadCompleted(nodeId, selectedRef);
                requestHostedPluginEditorOpen(nodeId, selectedRef);
            }
            return;
        }
    }

    const bool updatedResource = UpdateResourceForNodeId(nodeId, ref);

    if (auto* targetGraph = ResolveEditTarget())
    {
        if (auto* node = targetGraph->FindNode(nodeId))
            RefreshWasmNodeDescriptor(*node);
    }

    if (mActivePreset)
    {
        auto* node = mActivePreset->graph.FindNode(nodeId);
        if (node && IsNamEffectType(node->type)
            && !node->resources.empty() && node->resources.front().IsValid())
        {
            ResetNamNodeLevelState(nodeId);
        }
    }
    if (updatedResource && ReportHostedPluginResourceLoadFailure(nodeId, ref))
        DiscardFailedHostedPluginResourceSelection(nodeId, ref);
    else if (updatedResource)
    {
        NotifyHostedPluginResourceLoadCompleted(nodeId, ref);
        requestHostedPluginEditorOpen(nodeId, ref);
    }
}

void PluginController::HandleBrowseNodeResourceRequest(const nlohmann::json& payload)
{
    std::string nodeId = payload.value("nodeId", "");
    std::string resourceType = payload.value("resourceType", "nam");
    const int resourceIndex = payload.value("resourceIndex", -1);
    const std::string exposedResourceId = payload.value("exposedResourceId", "");
    const std::string category = payload.value("category", "");
    const bool openPluginEditorAfterLoad = payload.value("openPluginEditorAfterLoad", false);

    BrowseFileType fileType = BrowseFileType::Any;
    if (resourceType == "nam") fileType = BrowseFileType::NAMModel;
    else if (resourceType == "ir") fileType = BrowseFileType::IRFile;
    else if (resourceType == "plugin") fileType = BrowseFileType::PluginFile;

    mHost.BrowseFileAsync(fileType, "Select Resource",
        [this, nodeId, resourceType, resourceIndex, exposedResourceId, category, openPluginEditorAfterLoad](const BrowseFileResult& result)
        {
            if (result.success)
            {
                nlohmann::json payload;
                payload["filePath"] = util::PathToUtf8(result.path);
                payload["resourceType"] = resourceType;
                payload["nodeId"] = nodeId;
                payload["name"] = util::PathToUtf8(result.path.stem());
                payload["category"] = category.empty() ? std::string{"Local"} : category;
                payload["metadata"] = nlohmann::json::object({{"provider", kLocalResourceProvider}});
                if (resourceIndex >= 0)
                    payload["resourceIndex"] = resourceIndex;
                if (!exposedResourceId.empty())
                    payload["exposedResourceId"] = exposedResourceId;
                if (openPluginEditorAfterLoad)
                    payload["openPluginEditorAfterLoad"] = true;
                HandleSaveLocalLibraryResourceRequest(payload);
            }
            else
            {
                // Let the UI know the browse dialog was dismissed so it can
                // clear any pending loading indicators.
                nlohmann::json cancelMsg{
                    {"type", "nodeResourceBrowseCancelled"},
                    {"nodeId", nodeId},
                    {"resourceType", resourceType}
                };
                if (resourceIndex >= 0)
                    cancelMsg["resourceIndex"] = resourceIndex;
                if (!exposedResourceId.empty())
                    cancelMsg["exposedResourceId"] = exposedResourceId;
                SendMessageToUI(cancelMsg.dump());
            }
        });
}

void PluginController::HandleAddSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string insertAfter = payload.value("insertAfter", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());
    const auto paramsPayload = payload.value("params", nlohmann::json::object());
    const auto resourcesPayload = payload.value("resources", nlohmann::json::array());

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

    const std::string resolvedEffectType = EffectRegistry::Instance().Resolve(effectType);
    if (resolvedEffectType == EffectGuids::kSplitter)
    {
        auto& graph = *targetGraph;
        const std::string splitterId = MakeUniqueNodeId(graph, "split");
        const std::string mixerId = MakeUniqueNodeId(graph, "mix");

        GraphNode splitter; splitter.id = splitterId; splitter.type = EffectGuids::kSplitter; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
        GraphNode mixer; mixer.id = mixerId; mixer.type = EffectGuids::kMixer; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

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
        else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
        return;
    }

    // Create new node with default parameters
    GraphNode newNode;
    newNode.id = resolvedEffectType + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    newNode.type = resolvedEffectType;
    newNode.enabled = true;

    const auto effectInfoOpt = EffectRegistry::Instance().GetTypeInfo(resolvedEffectType);
    if (effectInfoOpt)
    {
        newNode.category = effectInfoOpt->category;
        newNode.label = effectInfoOpt->displayName;
        for (const auto& p : effectInfoOpt->parameters)
            newNode.params[p.id] = p.defaultValue;
        // Prefer the preset the effect nominates as its starting point; fall back
        // to the first factory preset for effects that don't mark one.
        auto factoryPreset = std::find_if(
            effectInfoOpt->presets.begin(),
            effectInfoOpt->presets.end(),
            [](const EffectPresetDefinition& preset) { return preset.isFactory && preset.isDefault; });
        if (factoryPreset == effectInfoOpt->presets.end())
        {
            factoryPreset = std::find_if(
                effectInfoOpt->presets.begin(),
                effectInfoOpt->presets.end(),
                [](const EffectPresetDefinition& preset) { return preset.isFactory; });
        }
        if (factoryPreset != effectInfoOpt->presets.end())
        {
            for (const auto& [key, value] : factoryPreset->parameters)
                newNode.params[key] = value;
        }
    }
    else { newNode.category = "utility"; newNode.label = effectType; }

    if (paramsPayload.is_object())
    {
        for (const auto& entry : paramsPayload.items())
        {
            if (entry.value().is_number())
                newNode.params[entry.key()] = entry.value().get<double>();
        }
    }

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) newNode.config[entry.key()] = entry.value().get<std::string>();

    if (resourcesPayload.is_array())
    {
        for (const auto& resourceJson : resourcesPayload)
        {
            if (!resourceJson.is_object())
                continue;
            const auto resource = DeserializeResourceRef(resourceJson);
            if (resource.IsValid())
                newNode.resources.push_back(resource);
        }
    }

    if (!labelOverride.empty()) newNode.label = labelOverride;
    if (!categoryOverride.empty()) newNode.category = categoryOverride;
    RefreshWasmNodeDescriptor(newNode);

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
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
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

    GraphNode splitter; splitter.id = splitterId; splitter.type = EffectGuids::kSplitter; splitter.category = "utility"; splitter.label = "Splitter"; splitter.enabled = true;
    GraphNode mixer; mixer.id = mixerId; mixer.type = EffectGuids::kMixer; mixer.category = "utility"; mixer.label = "Mixer"; mixer.enabled = true;

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
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
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
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
}

void PluginController::HandleReplaceSignalPathNodeRequest(const nlohmann::json& payload)
{
    const std::string nodeId = payload.value("nodeId", "");
    const std::string newEffectType = payload.value("newEffectType", "");
    const std::string labelOverride = payload.value("label", "");
    const std::string categoryOverride = payload.value("category", "");
    const auto configPayload = payload.value("config", nlohmann::json::object());
    const auto paramsPayload = payload.value("params", nlohmann::json::object());
    const auto resourcesPayload = payload.value("resources", nlohmann::json::array());

    if (nodeId.empty() || newEffectType.empty()) { ReportErrorToUI("Replace node failed", "Missing nodeId or newEffectType parameter"); return; }

    SignalGraph* targetGraph = ResolveEditTarget();
    if (!targetGraph) { ReportErrorToUI("Replace node failed", "No active preset or composite"); return; }

    GraphNode* node = targetGraph->FindNode(nodeId);
    if (!node) { ReportErrorToUI("Replace node failed", "Node not found: " + nodeId); return; }

    const auto oldEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(node->type);
    const std::string resolvedNewEffectType = EffectRegistry::Instance().Resolve(newEffectType);
    const auto newEffectInfoOpt = EffectRegistry::Instance().GetTypeInfo(resolvedNewEffectType);
    if (!newEffectInfoOpt) { ReportErrorToUI("Replace node failed", "Unknown effect type: " + newEffectType); return; }

    const std::string oldCategory = oldEffectInfoOpt ? oldEffectInfoOpt->category : node->category;
    const std::string requestedCategory = !categoryOverride.empty() ? categoryOverride : newEffectInfoOpt->category;
    if (!oldCategory.empty() && !requestedCategory.empty() && oldCategory != requestedCategory)
    { ReportErrorToUI("Replace node failed", "Cannot replace effect with different category"); return; }

    node->type = resolvedNewEffectType;
    node->label = newEffectInfoOpt->displayName;
    node->category = newEffectInfoOpt->category;
    node->params.clear();
    node->resources.clear();
    node->config.clear();

    for (const auto& p : newEffectInfoOpt->parameters)
        node->params[p.id] = p.defaultValue;

    if (paramsPayload.is_object())
    {
        for (const auto& entry : paramsPayload.items())
        {
            if (entry.value().is_number())
                node->params[entry.key()] = entry.value().get<double>();
        }
    }

    if (configPayload.is_object())
        for (const auto& entry : configPayload.items())
            if (entry.value().is_string()) node->config[entry.key()] = entry.value().get<std::string>();

    if (resourcesPayload.is_array())
    {
        for (const auto& resourceJson : resourcesPayload)
        {
            if (!resourceJson.is_object())
                continue;
            const auto resource = DeserializeResourceRef(resourceJson);
            if (resource.IsValid())
                node->resources.push_back(resource);
        }
    }

    if (!labelOverride.empty()) node->label = labelOverride;
    if (!categoryOverride.empty()) node->category = categoryOverride;
    RefreshWasmNodeDescriptor(*node);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
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
    if (node->type == "splitter" || node->type == "mixer" || node->type == EffectGuids::kSplitter || node->type == EffectGuids::kMixer) { ReportErrorToUI("Reorder node failed", "Cannot move splitter/mixer nodes"); return; }

    // Everything is validated and resolved before the graph is touched: an early
    // return after a partial splice would leave the node disconnected, which then
    // makes every later reorder of that node fail with a missing-edge error.
    const auto& currentEdges = targetGraph->edges;
    const std::size_t edgeCount = currentEdges.size();

    std::size_t incomingIndex = edgeCount;
    std::size_t outgoingIndex = edgeCount;
    std::size_t incomingCount = 0;
    std::size_t outgoingCount = 0;
    for (std::size_t i = 0; i < edgeCount; ++i)
    {
        if (currentEdges[i].to == nodeId && incomingCount++ == 0) incomingIndex = i;
        if (currentEdges[i].from == nodeId && outgoingCount++ == 0) outgoingIndex = i;
    }

    if (incomingCount == 0 || outgoingCount == 0)
    { ReportErrorToUI("Reorder node failed", "Node is not fully connected (missing input or output connection)"); return; }
    if (incomingCount > 1 || outgoingCount > 1)
    { ReportErrorToUI("Reorder node failed", "Node has multiple connections. Remove branch effects first."); return; }

    std::size_t targetEdgeIndex = edgeCount;
    if (!edgeFrom.empty() && !edgeTo.empty())
    {
        // Dropping a node back onto one of its own connections is a no-op, not an error.
        if (edgeFrom == nodeId || edgeTo == nodeId) return;

        for (std::size_t i = 0; i < edgeCount; ++i)
        {
            const auto& e = currentEdges[i];
            if (e.from == edgeFrom && e.to == edgeTo && e.fromPort == edgeFromPort && e.toPort == edgeToPort)
            { targetEdgeIndex = i; break; }
        }
        if (targetEdgeIndex == edgeCount) { ReportErrorToUI("Reorder node failed", "Cannot find target edge"); return; }
    }
    else
    {
        if (targetNodeId == nodeId) return;
        if (!targetGraph->FindNode(targetNodeId)) { ReportErrorToUI("Reorder node failed", "Target node not found"); return; }

        for (std::size_t i = 0; i < edgeCount; ++i)
        {
            if (currentEdges[i].from == targetNodeId) { targetEdgeIndex = i; break; }
        }
        if (targetEdgeIndex == edgeCount) { ReportErrorToUI("Reorder node failed", "Cannot find target position"); return; }

        // The node already sits directly after the target node.
        if (currentEdges[targetEdgeIndex].to == nodeId) return;
    }

    // Commit-on-success: mutate a copy so a failure can never corrupt the graph.
    auto edges = currentEdges;

    const std::string bypassNextId = edges[outgoingIndex].to;
    const int bypassToPort = edges[outgoingIndex].toPort;
    const double bypassGain = edges[outgoingIndex].gain;

    const std::string reinsertNextId = edges[targetEdgeIndex].to;
    const int reinsertToPort = edges[targetEdgeIndex].toPort;
    const double reinsertGain = edges[targetEdgeIndex].gain;

    edges[incomingIndex].to = bypassNextId;
    edges[incomingIndex].toPort = bypassToPort;
    edges[incomingIndex].gain = bypassGain;

    edges[targetEdgeIndex].to = nodeId;
    edges[targetEdgeIndex].toPort = 0;
    edges[targetEdgeIndex].gain = 1.0;

    edges.erase(edges.begin() + static_cast<std::ptrdiff_t>(outgoingIndex));

    GraphEdge reinserted;
    reinserted.from = nodeId;
    reinserted.to = reinsertNextId;
    reinserted.fromPort = 0;
    reinserted.toPort = reinsertToPort;
    reinserted.gain = reinsertGain;
    edges.push_back(reinserted);

    targetGraph->edges = std::move(edges);

    if (IsCompositeEditMode()) BroadcastCompositeEditState();
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
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
    else if (mActivePreset) { SyncActivePresetSceneGraph(); ApplyPreset(*mActivePreset); BroadcastState(); }
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
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());

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
    if (tagsPayload.is_array())
    {
        for (const auto& tagValue : tagsPayload)
        {
            if (!tagValue.is_string())
                continue;
            const auto tag = tagValue.get<std::string>();
            if (!tag.empty())
                resource.tags.push_back(tag);
        }
    }

    if (resourceType == "nam")
        EnrichNamResourceMetadata(resource, targetPath);

    resource.category = ResolveResourceLibraryCategory(resource, resource.category);

    mResourceLibrary.AddResource(resource);
    AppendUserLibraryResource(resource);
    BroadcastState();

    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = resourceType;
    msg["id"] = resourceId;
    msg["name"] = name;
    msg["filePath"] = util::PathToUtf8(targetPath);
    SendMessageToUI(msg.dump());
    AppendSessionLog("Imported resource " + resourceType + ":" + resourceId + " (" + targetPath.string() + ")");
}

std::optional<LibraryResource> PluginController::SaveLocalLibraryResource(const nlohmann::json& payload,
                                                                          std::string& error,
                                                                          bool allowCreate)
{
    const std::string resourceType = payload.value("resourceType", "");
    std::string resourceId = payload.value("resourceId", "");
    const std::string filePathValue = payload.value("filePath", "");
    const std::string data = payload.value("data", "");
    const std::string fileName = payload.value("fileName", "");
    const std::string name = payload.value("name", "");
    const std::string description = payload.value("description", "");
    const std::string category = payload.value("category", "");
    const std::string subfolder = payload.value("subfolder", "");
    const std::string providedHash = payload.value("hash", "");
    const nlohmann::json metadataPayload = payload.value("metadata", nlohmann::json::object());
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());
    const std::string payloadPluginName = payload.value("pluginName", "");
    const std::string payloadPluginManufacturer = payload.value("pluginManufacturer", "");
    const std::string payloadPluginStableId = payload.value("pluginStableId", "");

    if (resourceType.empty())
    {
        error = "Missing resource type";
        return std::nullopt;
    }

    const bool hasFilePath = !filePathValue.empty();
    const bool hasInlineData = !data.empty();
    if (!hasFilePath && !hasInlineData)
    {
        error = "Missing local file path or file data";
        return std::nullopt;
    }

    auto allResources = mResourceLibrary.GetAllResources();
    std::filesystem::path resolvedPath;
    std::string resolvedHash = providedHash;

    auto upsertMetadata = [&](LibraryResource& resource) {
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
        resource.metadata["provider"] = kLocalResourceProvider;
    };

    auto getMetadataString = [&](const std::string& key) -> std::string
    {
        if (!metadataPayload.is_object() || !metadataPayload.contains(key))
            return {};

        const auto& value = metadataPayload[key];
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number() || value.is_boolean())
            return value.dump();
        return {};
    };

    if (hasFilePath)
    {
        resolvedPath = util::PathFromUtf8(filePathValue);
        if (!std::filesystem::exists(resolvedPath))
        {
            error = "Selected file does not exist";
            return std::nullopt;
        }
        if (resolvedHash.empty())
            resolvedHash = mHasher.HashFile(resolvedPath);
    }
    else
    {
        const auto decodedBytes = util::DecodeBase64(data);
        if (decodedBytes.empty())
        {
            error = "Invalid file data";
            return std::nullopt;
        }

        auto targetDir = GetEffectiveSettingsDirectory() / "resources" / "content" / kLocalResourceStorageFolder;
        if (!subfolder.empty())
        {
            std::filesystem::path sanitizedSubfolder;
            for (const auto& part : std::filesystem::path(subfolder))
            {
                const std::string segment = util::SanitizePathSegment(part.string(), true);
                if (segment.empty() || segment == "." || segment == "..")
                    continue;
                sanitizedSubfolder /= segment;
            }
            if (!sanitizedSubfolder.empty())
                targetDir /= sanitizedSubfolder;
        }
        [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(targetDir);

        const auto defaultExtensionForType = [&](const std::string& type) {
            if (type == "ir") return std::string{".wav"};
            if (type == "wasm") return std::string{".wasm"};
            if (type == "nam") return std::string{".nam"};
            return std::string{".bin"};
        };

        std::string resolvedName = util::SanitizeFilename(fileName.empty() ? (resourceId.empty() ? name : resourceId) : fileName);
        if (resolvedName.empty())
            resolvedName = "resource" + defaultExtensionForType(resourceType);
        if (resolvedName.find('.') == std::string::npos)
            resolvedName += defaultExtensionForType(resourceType);

        resolvedPath = targetDir / resolvedName;

        if (std::filesystem::exists(resolvedPath) && !resolvedHash.empty())
        {
            const std::string existingHash = mHasher.HashFile(resolvedPath);
            if (!existingHash.empty() && existingHash != resolvedHash)
            {
                const std::filesystem::path stem = resolvedPath.stem();
                const std::filesystem::path ext = resolvedPath.extension();
                const std::string hashSuffix = resolvedHash.substr(0, std::min<std::size_t>(12, resolvedHash.size()));
                std::filesystem::path candidate = targetDir / (stem.string() + "-" + hashSuffix + ext.string());
                std::size_t suffix = 2;
                while (std::filesystem::exists(candidate))
                {
                    candidate = targetDir / (stem.string() + "-" + hashSuffix + "-" + std::to_string(suffix++) + ext.string());
                }
                resolvedPath = candidate;
            }
        }

        if (!WriteFile(resolvedPath, decodedBytes))
        {
            error = "Failed to write local resource file";
            return std::nullopt;
        }
        if (resolvedHash.empty())
            resolvedHash = mHasher.HashFile(resolvedPath);
    }

    auto normalizedPathString = resolvedPath.lexically_normal().generic_string();
    auto existingByPath = std::find_if(allResources.begin(), allResources.end(),
        [&](const LibraryResource& resource)
        {
            return resource.type == resourceType
                && !resource.filePath.empty()
                && resource.filePath.lexically_normal().generic_string() == normalizedPathString;
        });

    if (resourceId.empty() && existingByPath != allResources.end())
    {
        const bool canCompareHash = !resolvedHash.empty() && !existingByPath->hash.empty();
        const bool sameHash = canCompareHash && existingByPath->hash == resolvedHash;
        const bool unknownHash = !canCompareHash;
        if (sameHash || unknownHash)
            resourceId = existingByPath->id;
    }

    // For direct local file imports, keep entries path-specific to avoid mutating
    // an existing library item that happens to share a content hash.
    if (resourceId.empty() && !resolvedHash.empty() && !hasFilePath)
    {
        auto existingByHash = std::find_if(allResources.begin(), allResources.end(),
            [&](const LibraryResource& resource)
            {
                return resource.type == resourceType && !resource.hash.empty() && resource.hash == resolvedHash;
            });
        if (existingByHash != allResources.end())
            resourceId = existingByHash->id;
    }

    std::string normalizedPluginStableId;
    if (resourceType == "plugin")
    {
        std::string pluginName = payloadPluginName;
        if (pluginName.empty())
            pluginName = getMetadataString(kHostedPluginNameConfigKey);
        if (pluginName.empty())
            pluginName = resolvedPath.stem().string();

        std::string pluginManufacturer = payloadPluginManufacturer;
        if (pluginManufacturer.empty())
            pluginManufacturer = getMetadataString(kHostedPluginManufacturerConfigKey);

        std::string pluginStableId = payloadPluginStableId;
        if (pluginStableId.empty())
            pluginStableId = getMetadataString(kHostedPluginStableIdConfigKey);
        if (pluginStableId.empty())
            pluginStableId = BuildHostedPluginStableId(pluginManufacturer, pluginName);
        normalizedPluginStableId = NormalizeHostedPluginIdentityToken(pluginStableId);

        if (resourceId.empty() && !normalizedPluginStableId.empty())
        {
            auto existingByStableId = std::find_if(allResources.begin(), allResources.end(),
                [&](const LibraryResource& resource)
                {
                    if (resource.type != "plugin")
                        return false;
                    const auto it = resource.metadata.find(kHostedPluginStableIdConfigKey);
                    if (it == resource.metadata.end())
                        return false;
                    return NormalizeHostedPluginIdentityToken(it->second) == normalizedPluginStableId;
                });
            if (existingByStableId != allResources.end())
                resourceId = existingByStableId->id;
        }
    }

    if (resourceId.empty())
    {
        if (!allowCreate)
        {
            error = "Resource not found";
            return std::nullopt;
        }
        std::string baseId;
        if (resourceType == "plugin" && !normalizedPluginStableId.empty())
        {
            baseId = std::string{kLocalResourceProvider} + ":plugin:" + normalizedPluginStableId;
        }
        else
        {
            baseId = std::string{kLocalResourceProvider} + ":" + util::SanitizePathSegment(resolvedPath.stem().string(), true);
        }
        if (baseId == std::string{kLocalResourceProvider} + ":")
            baseId += "resource";
        const bool allowHashSuffix = !(resourceType == "plugin" && !normalizedPluginStableId.empty());
        if (allowHashSuffix && !resolvedHash.empty())
            baseId += ":" + resolvedHash.substr(0, std::min<std::size_t>(12, resolvedHash.size()));
        resourceId = baseId;
        std::size_t suffix = 2;
        while (mResourceLibrary.HasResource(resourceType, resourceId))
            resourceId = baseId + "-" + std::to_string(suffix++);
    }

    LibraryResource resource;
    if (auto existing = mResourceLibrary.LookupResource(resourceType, resourceId))
        resource = *existing;
    else if (!allowCreate)
    {
        error = "Resource not found";
        return std::nullopt;
    }

    resource.type = resourceType;
    resource.id = resourceId;
    const std::string resolvedName = !name.empty() ? name : (!resource.name.empty() ? resource.name : resolvedPath.stem().string());
    const std::string resolvedCategory = !category.empty() ? category : (!resource.category.empty() ? resource.category : std::string{"Local"});
    resource.name = resolvedName.empty() ? resourceId : resolvedName;
    resource.category = resolvedCategory;
    if (!description.empty() || resource.description.empty())
        resource.description = description;
    resource.filePath = resolvedPath;
    resource.hash = resolvedHash;
    upsertMetadata(resource);
    resource.metadata["sourceFileName"] = resolvedPath.filename().string();
    if (payload.contains("tags"))
    {
        resource.tags.clear();
        if (tagsPayload.is_array())
        {
            for (const auto& tagValue : tagsPayload)
            {
                if (!tagValue.is_string())
                    continue;
                const auto tag = tagValue.get<std::string>();
                if (!tag.empty())
                    resource.tags.push_back(tag);
            }
        }
    }

    // Extract all NAM metadata fields from the model file header.
    if (resourceType == "nam")
        EnrichNamResourceMetadata(resource, resolvedPath);

    resource.category = ResolveResourceLibraryCategory(resource, resource.category);

    if (resourceType == "plugin")
    {
        const std::string pluginName = payloadPluginName.empty()
            ? (resource.metadata.contains(kHostedPluginNameConfigKey)
                   ? resource.metadata[kHostedPluginNameConfigKey]
                   : resolvedPath.stem().string())
            : payloadPluginName;
        if (!pluginName.empty())
            resource.metadata[kHostedPluginNameConfigKey] = pluginName;

        const std::string pluginManufacturer = payloadPluginManufacturer.empty()
            ? (resource.metadata.contains(kHostedPluginManufacturerConfigKey)
                   ? resource.metadata[kHostedPluginManufacturerConfigKey]
                   : std::string{})
            : payloadPluginManufacturer;
        if (!pluginManufacturer.empty())
            resource.metadata[kHostedPluginManufacturerConfigKey] = pluginManufacturer;

        std::string pluginStableId = payloadPluginStableId.empty()
            ? (resource.metadata.contains(kHostedPluginStableIdConfigKey)
                   ? resource.metadata[kHostedPluginStableIdConfigKey]
                   : BuildHostedPluginStableId(pluginManufacturer, pluginName))
            : payloadPluginStableId;
        pluginStableId = NormalizeHostedPluginIdentityToken(pluginStableId);
        if (!pluginStableId.empty())
            resource.metadata[kHostedPluginStableIdConfigKey] = pluginStableId;

        if (!resource.metadata.contains(kHostedPluginFormatConfigKey) || resource.metadata[kHostedPluginFormatConfigKey].empty())
        {
            const std::string inferredFormat = InferPluginFormatFromPath(resolvedPath);
            if (!inferredFormat.empty())
                resource.metadata[kHostedPluginFormatConfigKey] = inferredFormat;
        }
    }

    AppendUserLibraryResource(resource);
    return resource;
}

void PluginController::HandleSaveLocalLibraryResourceRequest(const nlohmann::json& payload)
{
    std::string error;
    auto saved = SaveLocalLibraryResource(payload, error, true);
    if (!saved)
    {
        ReportErrorToUI("Local resource save failed", error);
        SendMessageToUI(nlohmann::json{{"type", "resourceImportFailed"}, {"message", "Local resource save failed"}, {"detail", error}}.dump());
        return;
    }

    if (payload.contains("nodeId") && payload["nodeId"].is_string())
    {
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = payload.value("nodeId", "");
        updatePayload["resourceType"] = saved->type;
        updatePayload["resourceId"] = saved->id;
        if (payload.contains("resourceIndex"))
            updatePayload["resourceIndex"] = payload["resourceIndex"];
        if (payload.contains("exposedResourceId"))
            updatePayload["exposedResourceId"] = payload["exposedResourceId"];
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    if (!mResourceLibrary.HasResource(saved->type, saved->id))
    {
        BroadcastState();
        return;
    }

    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});
    nlohmann::json msg;
    msg["type"] = "resourceImported";
    msg["resourceType"] = saved->type;
    msg["id"] = saved->id;
    msg["name"] = saved->name;
    msg["filePath"] = util::PathToUtf8(saved->filePath);
    SendMessageToUI(msg.dump());
}

void PluginController::HandleRemoveLocalLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    std::string resourceId = payload.value("resourceId", "");
    const std::string filePath = payload.value("filePath", "");

    if (resourceType.empty())
        return;

    // Resolve the id by file path when only a path was provided (e.g. folder browser).
    if (resourceId.empty() && !filePath.empty())
    {
        const auto normalize = [](std::string value) {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };
        const std::string target = normalize(filePath);
        for (const auto& resource : mResourceLibrary.GetAllResources())
        {
            if (resource.type != resourceType)
                continue;
            if (normalize(resource.filePath.string()) == target)
            {
                resourceId = resource.id;
                break;
            }
        }
    }

    if (resourceId.empty() || !mResourceLibrary.HasResource(resourceType, resourceId))
        return;

    RemoveUserLibraryResource(resourceType, resourceId);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});

    SendMessageToUI(nlohmann::json{
        {"type", "resourceRemoved"},
        {"resourceType", resourceType},
        {"id", resourceId}
    }.dump());
}

void PluginController::HandleDeleteLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDeleteFailed"},
            {"message", "Resource delete failed"},
            {"detail", "Missing resource id"}
        }.dump());
        return;
    }

    const auto resourceOpt = mResourceLibrary.LookupResource(resourceType, resourceId);
    if (!resourceOpt)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDeleteFailed"},
            {"message", "Resource delete failed"},
            {"detail", "Resource not found"}
        }.dump());
        return;
    }

    const auto firstUsingPreset = FindFirstPresetUsingResource(resourceType, resourceId);
    if (firstUsingPreset.has_value())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceDeleteFailed"},
            {"message", "Resource is in use"},
            {"detail", "Used by preset: " + *firstUsingPreset},
            {"resourceType", resourceType},
            {"id", resourceId},
            {"presetName", *firstUsingPreset}
        }.dump());
        return;
    }

    const auto settingsResourcesDir = GetEffectiveSettingsDirectory() / "resources" / "content";
    const auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec); if (ec) return false;
        auto nb = std::filesystem::weakly_canonical(base, ec); if (ec) return false;
        auto bi = nb.begin(); auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci) { if (ci == nc.end() || *bi != *ci) return false; }
        return true;
    };

    const std::filesystem::path resourcePath = resourceOpt->filePath;
    const bool shouldDeleteFile = !resourcePath.empty() && isUnderDirectory(resourcePath, settingsResourcesDir);
    if (shouldDeleteFile)
    {
        std::error_code ec;
        std::filesystem::remove(resourcePath, ec);
        if (ec)
        {
            SendMessageToUI(nlohmann::json{
                {"type", "resourceDeleteFailed"},
                {"message", "Resource delete failed"},
                {"detail", "Failed to delete stored file: " + resourcePath.string()}
            }.dump());
            return;
        }
    }

    RemoveUserLibraryResource(resourceType, resourceId);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});

    SendMessageToUI(nlohmann::json{
        {"type", "resourceRemoved"},
        {"resourceType", resourceType},
        {"id", resourceId}
    }.dump());
}

std::optional<std::string> PluginController::FindFirstPresetUsingResource(const std::string& resourceType, const std::string& resourceId) const
{
    const auto graphUsesResource = [&](const SignalGraph& graph) -> bool
    {
        for (const auto& node : graph.nodes)
        {
            for (const auto& ref : node.resources)
            {
                if (ref.IsLibraryRef() && ref.resourceType == resourceType && ref.resourceId == resourceId)
                    return true;
            }
        }
        return false;
    };

    const auto presetUsesResource = [&](const Preset& preset) -> bool
    {
        if (graphUsesResource(preset.graph))
            return true;
        for (const auto& scene : preset.scenes)
        {
            if (graphUsesResource(scene.graph))
                return true;
        }
        return false;
    };

    const auto presetDisplayName = [](const Preset& preset) -> std::string
    {
        if (!preset.name.empty())
            return preset.name;
        if (!preset.id.empty())
            return preset.id;
        return "Unnamed preset";
    };

    // Check active preset
    if (mActivePreset && presetUsesResource(*mActivePreset))
        return presetDisplayName(*mActivePreset);

    // Consult the cached disk/archive index (built once, reused until presets change).
    EnsureResourceUsageDiskIndex();
    const std::string key = resourceType + ":" + resourceId;
    const auto it = mResourceUsageDiskIndex.find(key);
    if (it != mResourceUsageDiskIndex.end())
        return it->second;

    return std::nullopt;
}

void PluginController::EnsureResourceUsageDiskIndex() const
{
    if (mResourceUsageDiskIndexValid)
        return;

    mResourceUsageDiskIndex.clear();

    const auto indexPreset = [this](const Preset& preset)
    {
        std::string displayName = preset.name;
        if (displayName.empty())
            displayName = !preset.id.empty() ? preset.id : "Unnamed preset";

        const auto indexGraph = [&](const SignalGraph& graph)
        {
            for (const auto& node : graph.nodes)
            {
                for (const auto& ref : node.resources)
                {
                    if (!ref.IsLibraryRef())
                        continue;
                    const std::string key = ref.resourceType + ":" + ref.resourceId;
                    // Preserve first-found priority (user > factory > archive).
                    mResourceUsageDiskIndex.emplace(key, displayName);
                }
            }
        };

        indexGraph(preset.graph);
        for (const auto& scene : preset.scenes)
            indexGraph(scene.graph);
    };

    // User presets first so they win ties.
    if (!mUserPresetsPath.empty() && std::filesystem::exists(mUserPresetsPath))
    {
        const auto userPresets = PresetStorage::LoadAllFromDirectory(mUserPresetsPath);
        for (const auto& preset : userPresets)
            indexPreset(preset);
    }

    // Factory presets next.
    {
        const auto factoryDir = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
        if (std::filesystem::exists(factoryDir))
        {
            const auto factoryPresets = PresetStorage::LoadAllFromDirectory(factoryDir);
            for (const auto& preset : factoryPresets)
                indexPreset(preset);
        }
    }

    // Factory archive presets last.
    for (const auto& [_, preset] : mFactoryArchivePresets)
        indexPreset(preset);

    mResourceUsageDiskIndexValid = true;
}

void PluginController::InvalidateResourceUsageIndex()
{
    mResourceUsageDiskIndexValid = false;
    mResourceUsageDiskIndex.clear();
}

void PluginController::HandleQueryResourceUsageRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");

    if (resourceType.empty() || resourceId.empty())
    {
        SendMessageToUI(nlohmann::json{
            {"type", "resourceUsageInfo"},
            {"resourceType", resourceType},
            {"id", resourceId},
            {"inUse", false}
        }.dump());
        return;
    }

    const auto presetName = FindFirstPresetUsingResource(resourceType, resourceId);
    SendMessageToUI(nlohmann::json{
        {"type", "resourceUsageInfo"},
        {"resourceType", resourceType},
        {"id", resourceId},
        {"inUse", presetName.has_value()},
        {"presetName", presetName ? *presetName : ""}
    }.dump());
}

void PluginController::HandleUpdateLibraryResourceRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
    {
        ReportErrorToUI("Resource update failed", "Missing resource id");
        return;
    }

    auto existing = mResourceLibrary.LookupResource(resourceType, resourceId);
    if (!existing)
    {
        ReportErrorToUI("Resource update failed", "Resource not found");
        return;
    }

    LibraryResource updated = *existing;
    const std::string fileNameValue = payload.value("fileName", "");
    const std::string inlineData = payload.value("data", "");
    const auto settingsResourcesDir = GetEffectiveSettingsDirectory() / "resources" / "content";
    const auto isUnderDirectory = [](const std::filesystem::path& candidate, const std::filesystem::path& base) {
        std::error_code ec;
        auto nc = std::filesystem::weakly_canonical(candidate, ec); if (ec) return false;
        auto nb = std::filesystem::weakly_canonical(base, ec); if (ec) return false;
        auto bi = nb.begin(); auto ci = nc.begin();
        for (; bi != nb.end(); ++bi, ++ci) { if (ci == nc.end() || *bi != *ci) return false; }
        return true;
    };
    if (payload.contains("name"))
        updated.name = payload.value("name", updated.name);
    if (payload.contains("category"))
        updated.category = payload.value("category", updated.category);
    if (payload.contains("description"))
        updated.description = payload.value("description", updated.description);
    if (payload.contains("tags"))
    {
        updated.tags.clear();
        if (payload["tags"].is_array())
        {
            for (const auto& tagValue : payload["tags"])
            {
                if (!tagValue.is_string())
                    continue;
                const auto tag = tagValue.get<std::string>();
                if (!tag.empty())
                    updated.tags.push_back(tag);
            }
        }
    }
    if (payload.contains("metadata") && payload["metadata"].is_object())
    {
        updated.metadata.clear();
        for (const auto& entry : payload["metadata"].items())
        {
            const auto& value = entry.value();
            if (value.is_string()) updated.metadata[entry.key()] = value.get<std::string>();
            else if (value.is_number()) updated.metadata[entry.key()] = value.dump();
            else if (value.is_boolean()) updated.metadata[entry.key()] = value.get<bool>() ? "true" : "false";
        }
        if (!updated.metadata.contains("provider"))
            updated.metadata["provider"] = existing->metadata.contains("provider") ? existing->metadata.at("provider") : kLocalResourceProvider;
    }

    if (payload.contains("filePath"))
    {
        const std::string filePathValue = payload.value("filePath", "");
        if (!filePathValue.empty())
        {
            std::filesystem::path updatedPath(filePathValue);
            if (!std::filesystem::exists(updatedPath))
            {
                ReportErrorToUI("Resource update failed", "Selected file does not exist");
                return;
            }
            updated.filePath = updatedPath;
            updated.hash = mHasher.HashFile(updatedPath);
            updated.metadata["sourceFileName"] = updatedPath.filename().string();
        }
    }

    if (!inlineData.empty())
    {
        const std::vector<std::uint8_t> decodedBytes = util::DecodeBase64(inlineData);
        if (decodedBytes.empty())
        {
            ReportErrorToUI("Resource update failed", "Invalid file data");
            return;
        }

        std::filesystem::path targetPath = updated.filePath;
        const bool hasExistingPath = !targetPath.empty();
        if (!hasExistingPath)
        {
            ReportErrorToUI("Resource update failed", "Existing resource file path is missing");
            return;
        }

        const auto extensionForType = [&](const std::string& type) {
            if (type == "ir") return std::string{".wav"};
            if (type == "wasm") return std::string{".wasm"};
            if (type == "nam") return std::string{".nam"};
            return std::string{".bin"};
        };

        if (!fileNameValue.empty())
        {
            std::string resolvedName = util::SanitizeFilename(fileNameValue);
            if (resolvedName.empty())
                resolvedName = "resource" + extensionForType(resourceType);
            if (resolvedName.find('.') == std::string::npos)
                resolvedName += extensionForType(resourceType);
            targetPath = updated.filePath.parent_path() / resolvedName;
        }

        [[maybe_unused]] const auto ensuredTargetDir = mFileSystem.EnsureDirectory(targetPath.parent_path());
        if (!WriteFile(targetPath, decodedBytes))
        {
            ReportErrorToUI("Resource update failed", "Failed to write replacement file");
            return;
        }

        const std::filesystem::path previousPath = updated.filePath;
        updated.filePath = targetPath;
        updated.hash = mHasher.HashFile(targetPath);
        updated.metadata["sourceFileName"] = targetPath.filename().string();

        if (previousPath != targetPath && !previousPath.empty() && isUnderDirectory(previousPath, settingsResourcesDir))
        {
            std::error_code ec;
            std::filesystem::remove(previousPath, ec);
        }
    }

    if (resourceType == "nam")
        EnrichNamResourceMetadata(updated, updated.filePath);

    mResourceLibrary.UpdateResource(resourceType, resourceId, updated);
    AppendUserLibraryResource(updated);
    BroadcastState();
    TouchSharedSyncState({"resourceLibrary"});
    SendMessageToUI(nlohmann::json{{"type", "resourceImported"}, {"resourceType", updated.type}, {"id", updated.id}, {"name", updated.name}, {"filePath", util::PathToUtf8(updated.filePath)}}.dump());
}

void PluginController::HandleBrowseLibraryResourcePathRequest(const nlohmann::json& payload)
{
    const std::string resourceType = payload.value("resourceType", "");
    const std::string resourceId = payload.value("resourceId", "");
    if (resourceType.empty() || resourceId.empty())
        return;

    BrowseFileType fileType = BrowseFileType::Any;
    if (resourceType == "nam") fileType = BrowseFileType::NAMModel;
    else if (resourceType == "ir") fileType = BrowseFileType::IRFile;
    else if (resourceType == "plugin") fileType = BrowseFileType::PluginFile;
    mHost.BrowseFileAsync(fileType, "Select Local Resource",
        [this, payload, resourceType, resourceId](const BrowseFileResult& result)
        {
            if (!result.success)
                return;

            nlohmann::json updatePayload = payload;
            updatePayload["resourceType"] = resourceType;
            updatePayload["resourceId"] = resourceId;
            updatePayload["filePath"] = util::PathToUtf8(result.path);
            HandleUpdateLibraryResourceRequest(updatePayload);
        });
}

void PluginController::HandleBrowseResourceFolderRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::Folder, "Select Resource Folder",
        [this](const BrowseFileResult& result)
        {
            nlohmann::json msg;
            msg["type"] = "resourceFolderPicked";
            std::error_code ec;
            if (result.success && std::filesystem::is_directory(result.path, ec) && !ec)
            {
                msg["success"] = true;
                msg["path"] = result.path.generic_string();
                const auto leaf = result.path.filename();
                msg["name"] = leaf.empty() ? result.path.generic_string() : leaf.string();
            }
            else
            {
                msg["success"] = false;
            }
            SendMessageToUI(msg.dump());
        });
}

void PluginController::HandleListResourceFolderRequest(const nlohmann::json& payload)
{
    const std::string rawPath = payload.value("path", "");

    // Snapshot existing library (filePath -> id) on the message thread. This
    // uses the lightweight path index (two string copies per entry, no metadata
    // maps, no filesystem access, no canonicalization) so even a very large
    // library can never freeze the UI. The worker normalizes/matches off-thread.
    std::vector<std::pair<std::string, std::string>> libraryPaths = mResourceLibrary.GetResourcePathIndex();

    // Supersede any in-flight scan, then spawn a detached worker. We never join
    // on the message thread (which could block on a slow filesystem); detached
    // workers observe the bumped generation and exit quickly, and the
    // destructor waits for all of them via mFolderScanDoneCv.
    const std::uint64_t generation = mFolderScanGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    mActiveFolderScans.fetch_add(1, std::memory_order_relaxed);

    try
    {
        std::thread worker(
            [this, rawPath, libraryPaths = std::move(libraryPaths), generation]() mutable
            {
                const std::string requestedPath = rawPath;
                try
                {
                    ScanResourceFolderWorker(std::move(rawPath), std::move(libraryPaths), generation);
                }
                catch (const std::exception& exception)
                {
                    if (mFolderScanGeneration.load(std::memory_order_relaxed) == generation)
                    {
                        SendMessageToUI(nlohmann::json{
                            {"type", "resourceFolderListingFailed"},
                            {"path", requestedPath},
                            {"message", std::string{"Unable to scan folder: "} + exception.what()}
                        }.dump());
                    }
                }
                catch (...)
                {
                    if (mFolderScanGeneration.load(std::memory_order_relaxed) == generation)
                    {
                        SendMessageToUI(nlohmann::json{
                            {"type", "resourceFolderListingFailed"},
                            {"path", requestedPath},
                            {"message", "Unable to scan folder"}
                        }.dump());
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mFolderScanDoneMutex);
                    mActiveFolderScans.fetch_sub(1, std::memory_order_relaxed);
                }
                mFolderScanDoneCv.notify_all();
            });
        worker.detach();
    }
    catch (const std::exception&)
    {
        // Spawning failed: undo the active-scan bump and report the error so the
        // UI doesn't sit on "Loading…" forever. Never let the exception escape
        // into the WebView native-function callback (which would skip its
        // completion handler and can wedge the message pump).
        {
            std::lock_guard<std::mutex> lock(mFolderScanDoneMutex);
            mActiveFolderScans.fetch_sub(1, std::memory_order_relaxed);
        }
        mFolderScanDoneCv.notify_all();
        SendMessageToUI(nlohmann::json{{"type", "resourceFolderListingFailed"}, {"path", rawPath}, {"message", "Unable to start folder scan"}}.dump());
    }
}

void PluginController::ScanResourceFolderWorker(std::string requestPath,
                                                std::vector<std::pair<std::string, std::string>> libraryPaths,
                                                std::uint64_t generation)
{
    const auto superseded = [this, generation]() {
        return mFolderScanGeneration.load(std::memory_order_relaxed) != generation;
    };

    // Pure-lexical, no-filesystem normalization: lowercase + forward slashes +
    // collapse of "."/".."/redundant separators. Unlike weakly_canonical this
    // never touches the disk, so it can't stall on a slow/disconnected drive.
    const auto normalizePath = [](const std::filesystem::path& p) -> std::string {
        std::string s = util::PathToUtf8(p.lexically_normal());
        if (!s.empty() && s.back() == '/')
            s.pop_back();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const auto classify = [](const std::filesystem::path& p) -> std::string {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".nam") return std::string{"nam"};
        if (ext == ".wav" || ext == ".ir" || ext == ".aif" || ext == ".aiff" || ext == ".flac") return std::string{"ir"};
        return std::string{};
    };

    // Validate the requested path here (off the message thread) so even a slow
    // exists()/is_directory() probe on a bad drive never freezes the UI.
    if (requestPath.empty())
    {
        if (!superseded())
            SendMessageToUI(nlohmann::json{{"type", "resourceFolderListingFailed"}, {"message", "Missing folder path"}}.dump());
        return;
    }

    const std::filesystem::path dir = util::PathFromUtf8(requestPath);
    std::error_code dec;
    if (!std::filesystem::is_directory(dir, dec) || dec)
    {
        if (!superseded())
            SendMessageToUI(nlohmann::json{{"type", "resourceFolderListingFailed"}, {"path", requestPath}, {"message", "Folder not found"}}.dump());
        return;
    }

    // Build the (normalized path -> library id) lookup off-thread from the cheap
    // snapshot captured on the message thread.
    std::map<std::string, std::string> libraryIdByPath;
    for (auto& entry : libraryPaths)
    {
        if (superseded())
            return;
        libraryIdByPath.emplace(normalizePath(std::filesystem::path(entry.first)), std::move(entry.second));
    }

    std::error_code ec;
    constexpr std::size_t kMaxEntries = 5000;
    std::vector<nlohmann::json> dirs;
    std::vector<nlohmann::json> files;
    bool truncated = false;

    // Parallel list of (filesystem path, resourceType) for the second (metadata)
    // pass. Kept separate so the cheap listing can be sent before any file is
    // opened and parsed.
    struct PendingFile { std::filesystem::path path; std::string resourceType; };
    std::vector<PendingFile> pendingMetadata;

    // ── Phase 1: enumerate the immediate level only (no file content reads) ──
    // directory_iterator is intentionally non-recursive: we list just the folder
    // the user navigated into. This is cheap even for large folders, so the UI
    // gets a populated listing almost immediately.
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        const std::string detail = ec.message();
        if (!superseded())
            SendMessageToUI(nlohmann::json{
                {"type", "resourceFolderListingFailed"},
                {"path", requestPath},
                {"message", detail.empty() ? "Unable to read folder" : "Unable to read folder: " + detail}
            }.dump());
        return;
    }

    for (; it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        if (ec)
            break; // Stop on iteration error rather than throwing on a detached thread.
        if (superseded())
            return;

        const auto& entry = *it;
        if (dirs.size() + files.size() >= kMaxEntries)
        {
            truncated = true;
            break;
        }

        std::error_code eec;
        const auto& entryPath = entry.path();
        if (entry.is_directory(eec) && !eec)
        {
            dirs.push_back(nlohmann::json{{"name", util::PathToUtf8(entryPath.filename())}, {"path", util::PathToUtf8(entryPath)}});
            continue;
        }
        if (!entry.is_regular_file(eec) || eec)
            continue;

        const std::string resourceType = classify(entryPath);
        if (resourceType.empty())
            continue;

        nlohmann::json file;
        file["name"] = util::PathToUtf8(entryPath.filename());
        file["path"] = util::PathToUtf8(entryPath);
        file["resourceType"] = resourceType;

        std::error_code sec;
        const auto sizeBytes = std::filesystem::file_size(entryPath, sec);
        file["sizeBytes"] = sec ? 0 : static_cast<std::uint64_t>(sizeBytes);

        const auto libIt = libraryIdByPath.find(normalizePath(entryPath));
        if (libIt != libraryIdByPath.end())
        {
            file["alreadyInLibrary"] = true;
            file["libraryId"] = libIt->second;
        }
        else
        {
            file["alreadyInLibrary"] = false;
        }

        // Metadata is filled in later (Phase 2) so the listing isn't blocked.
        file["metadata"] = nlohmann::json::object();
        file["metadataPending"] = true;
        files.push_back(std::move(file));
        pendingMetadata.push_back({entryPath, resourceType});
    }

    if (superseded())
        return;

    const auto byName = [](const nlohmann::json& a, const nlohmann::json& b) {
        std::string an = a.value("name", "");
        std::string bn = b.value("name", "");
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return an < bn;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    const auto parentPath = dir.parent_path();
    std::string parentStr;
    if (!parentPath.empty() && parentPath != dir)
        parentStr = util::PathToUtf8(parentPath);

    const std::string folderPath = util::PathToUtf8(dir);

    nlohmann::json msg;
    msg["type"] = "resourceFolderListing";
    msg["path"] = folderPath;
    msg["parent"] = parentStr;
    const auto leaf = dir.filename();
    msg["name"] = leaf.empty() ? folderPath : util::PathToUtf8(leaf);
    msg["dirs"] = dirs;
    msg["files"] = files;
    msg["truncated"] = truncated;
    msg["metadataPending"] = !pendingMetadata.empty();

    if (superseded())
        return;
    SendMessageToUI(msg.dump());

    // ── Phase 2: parse per-file metadata and stream it back in batches ──
    // This is the expensive part (each file is opened/parsed). It runs after the
    // listing is already on screen, so badges/details fill in progressively
    // without ever blocking the UI.
    constexpr std::size_t kMetadataBatchSize = 40;
    nlohmann::json batch = nlohmann::json::array();

    const auto flushBatch = [&]() {
        if (batch.empty())
            return true;
        if (superseded())
            return false;
        SendMessageToUI(nlohmann::json{
            {"type", "resourceFolderMetadata"},
            {"path", folderPath},
            {"items", batch}
        }.dump());
        batch = nlohmann::json::array();
        return true;
    };

    for (const auto& pending : pendingMetadata)
    {
        if (superseded())
            return;

        nlohmann::json metadata = nlohmann::json::object();
        if (pending.resourceType == "nam")
        {
            LibraryResource temp;
            EnrichNamResourceMetadata(temp, pending.path);
            for (const auto& [key, value] : temp.metadata)
            {
                if (!value.empty())
                    metadata[key] = value;
            }
        }
        else
        {
            const WavHeaderInfo wav = TryReadWavHeader(pending.path);
            if (wav.valid)
            {
                if (wav.sampleRate > 0) metadata["sampleRate"] = std::to_string(wav.sampleRate);
                if (wav.channels > 0) metadata["channels"] = std::to_string(wav.channels);
                if (wav.bitsPerSample > 0) metadata["bitsPerSample"] = std::to_string(wav.bitsPerSample);
                if (wav.durationSec > 0.0)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.3f", wav.durationSec);
                    metadata["durationSec"] = std::string(buf);
                }
            }
        }

        batch.push_back(nlohmann::json{
            {"path", util::PathToUtf8(pending.path)},
            {"metadata", std::move(metadata)}
        });

        if (batch.size() >= kMetadataBatchSize)
        {
            if (!flushBatch())
                return;
        }
    }

    flushBatch();
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
        updatePayload["filePath"] = util::PathToUtf8(tempPath);
        updatePayload["resourceIndex"] = resourceIndex;
        HandleUpdateNodeResourceRequest(updatePayload);
    }

    AppendSessionLog("Preview started: " + resourceType + " at " + tempPath.string());
}

void PluginController::HandleCancelPreviewResourceRequest(const nlohmann::json& payload)
{
    if (!mPreviewState.active) return;

    const bool restoreOriginal = payload.value("restoreOriginal", true);

    if (restoreOriginal && !mPreviewState.nodeId.empty() && mPreviewState.originalResourceRef.has_value())
    {
        const auto& original = mPreviewState.originalResourceRef.value();
        nlohmann::json updatePayload;
        updatePayload["nodeId"] = mPreviewState.nodeId;
        updatePayload["resourceType"] = mPreviewState.resourceType;
        updatePayload["resourceId"] = original.resourceId;
        updatePayload["filePath"] = util::PathToUtf8(original.filePath);
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

void PluginController::HandleSaveCustomEffectEntryRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Custom Effect save failed", "Custom Effects are not supported in this build");
    return;
#else
    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    if (!entryJson.is_object())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing entry payload");
        return;
    }

    std::string parseError;
    auto entryOpt = DeserializeCustomEffectLibraryEntry(entryJson, &parseError);
    if (!entryOpt)
    {
        ReportErrorToUI("Custom Effect save failed", parseError.empty() ? "Invalid entry" : parseError);
        return;
    }

    auto entry = *entryOpt;
    entry.baseEffectType = EffectRegistry::Instance().Resolve(entry.baseEffectType);
    if (entry.baseEffectType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Custom Effect save failed", "baseEffectType must resolve to wasm_host");
        return;
    }

    if (!mResourceLibrary.HasResource(entry.moduleResourceType, entry.moduleResourceId))
    {
        ReportErrorToUI("Custom Effect save failed", "Referenced module resource was not found in the local library");
        return;
    }

    const auto* existing = mCustomEffectLibrary.GetEntry(entry.id);
    if (entry.category.empty())
        entry.category = existing && !existing->category.empty() ? existing->category : "utility";
    if (entry.createdAt.empty())
        entry.createdAt = existing && !existing->createdAt.empty() ? existing->createdAt : BuildTimestampUtcIso();
    if (entry.updatedAt.empty())
        entry.updatedAt = BuildTimestampUtcIso();
    if (entry.origin.empty() && existing && !existing->origin.empty())
        entry.origin = existing->origin;

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();
    BroadcastState();
#endif
}

void PluginController::HandleSaveCurrentCustomEffectRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Custom Effect save failed", "Custom Effects are not supported in this build");
    return;
#else
    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing node id");
        return;
    }

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!node)
    {
        ReportErrorToUI("Custom Effect save failed", "Selected node was not found");
        return;
    }

    const std::string resolvedType = EffectRegistry::Instance().Resolve(node->type);
    if (resolvedType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Custom Effect save failed", "Selected node is not a Custom Effect");
        return;
    }

    const bool applyToNode = payload.value("applyToNode", false);
    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    if (!entryJson.is_object())
    {
        ReportErrorToUI("Custom Effect save failed", "Missing entry payload");
        return;
    }

    const std::string linkedEntryId = [&]() -> std::string {
        if (const auto it = node->config.find("customEffectId"); it != node->config.end())
            return it->second;
        return {};
    }();
    const auto* linkedEntry = linkedEntryId.empty() ? nullptr : mCustomEffectLibrary.GetEntry(linkedEntryId);

    const auto getOptionalString = [&](const char* key) -> std::optional<std::string> {
        if (!entryJson.contains(key) || !entryJson[key].is_string())
            return std::nullopt;
        return entryJson[key].get<std::string>();
    };

    const std::optional<std::string> requestedNameOpt = getOptionalString("name");
    const std::optional<std::string> requestedCategoryOpt = getOptionalString("category");
    const std::optional<std::string> requestedDescriptionOpt = getOptionalString("description");
    const std::optional<std::string> requestedIdOpt = getOptionalString("id");
    const std::optional<std::string> requestedOriginOpt = getOptionalString("origin");
    const std::optional<std::string> requestedThumbnailOpt = getOptionalString("thumbnailDataUrl");
    const std::optional<std::string> requestedRevisionIdOpt = getOptionalString("latestRevisionId");

    ResourceRef moduleRef;
    if (!node->resources.empty())
        moduleRef = node->resources.front();
    if (moduleRef.resourceType.empty())
        moduleRef.resourceType = "wasm";

    if (moduleRef.resourceType != "wasm")
    {
        ReportErrorToUI("Custom Effect save failed", "The selected node is missing a WASM module resource");
        return;
    }

    std::optional<LibraryResource> moduleResource;
    if (!moduleRef.resourceId.empty())
        moduleResource = mResourceLibrary.LookupResource(moduleRef.resourceType, moduleRef.resourceId);

    if (!moduleResource && !moduleRef.filePath.empty())
    {
        nlohmann::json savePayload;
        savePayload["resourceType"] = moduleRef.resourceType;
        savePayload["filePath"] = util::PathToUtf8(moduleRef.filePath);
        savePayload["name"] = requestedNameOpt && !requestedNameOpt->empty()
            ? *requestedNameOpt
            : (!std::filesystem::path(moduleRef.filePath).stem().string().empty()
                ? std::filesystem::path(moduleRef.filePath).stem().string()
                : "Custom Effect Module");
        savePayload["category"] = "Local";
        savePayload["metadata"] = nlohmann::json::object({{"provider", kLocalResourceProvider}});

        std::string resourceSaveError;
        moduleResource = SaveLocalLibraryResource(savePayload, resourceSaveError, true);
        if (!moduleResource)
        {
            ReportErrorToUI("Custom Effect save failed", resourceSaveError.empty() ? "Failed to save local WASM module" : resourceSaveError);
            return;
        }
    }

    if (!moduleResource)
    {
        if (!moduleRef.embeddedId.empty())
        {
            ReportErrorToUI("Custom Effect save failed", "Embedded module resources are not supported by this save flow yet");
        }
        else
        {
            ReportErrorToUI("Custom Effect save failed", "Select a WASM module before saving this Custom Effect");
        }
        return;
    }

    GraphNode descriptorNode = *node;
    if (descriptorNode.resources.empty())
        descriptorNode.resources.resize(1);
    descriptorNode.resources.front().resourceType = moduleResource->type;
    descriptorNode.resources.front().resourceId = moduleResource->id;
    descriptorNode.resources.front().filePath.clear();
    descriptorNode.resources.front().embeddedId.clear();
    RefreshWasmNodeDescriptor(descriptorNode);

    std::optional<WasmModuleDescriptor> descriptor;
    if (const auto descriptorIt = descriptorNode.config.find(WasmEffect::kDescriptorConfigKey);
        descriptorIt != descriptorNode.config.end())
    {
        std::string parseError;
        descriptor = WasmEffect::ParseDescriptorConfig(descriptorIt->second, &parseError);
        if (!descriptor && !parseError.empty())
            AppendSessionLog("WASM descriptor cache parse failed while saving current Custom Effect " + nodeId + ": " + parseError);
    }

    std::string entryName = requestedNameOpt.value_or("");
    if (entryName.empty() && linkedEntry && !linkedEntry->name.empty())
        entryName = linkedEntry->name;
    if (entryName.empty() && descriptor && !descriptor->displayName.empty())
        entryName = descriptor->displayName;
    if (entryName.empty() && !node->label.empty())
        entryName = node->label;
    if (entryName.empty() && !moduleResource->name.empty())
        entryName = moduleResource->name;
    if (entryName.empty())
        entryName = "Custom Effect";

    std::string entryId = requestedIdOpt.value_or("");
    if (entryId.empty())
        entryId = linkedEntryId;
    if (entryId.empty())
    {
        std::string baseId = util::SanitizePathSegment(entryName, true);
        if (baseId.empty())
            baseId = "custom-effect";
        entryId = baseId;
        std::size_t suffix = 2;
        while (mCustomEffectLibrary.GetEntry(entryId) != nullptr)
            entryId = baseId + "-" + std::to_string(suffix++);
    }

    std::string entryCategory = requestedCategoryOpt.value_or("");
    if (entryCategory.empty() && linkedEntry && !linkedEntry->category.empty())
        entryCategory = linkedEntry->category;
    if (entryCategory.empty() && descriptor && !descriptor->category.empty())
        entryCategory = descriptor->category;
    if (entryCategory.empty() && !node->category.empty())
        entryCategory = node->category;
    if (entryCategory.empty())
        entryCategory = "utility";

    std::string entryDescription = requestedDescriptionOpt.has_value()
        ? *requestedDescriptionOpt
        : std::string{};
    if (!requestedDescriptionOpt.has_value() && linkedEntry && !linkedEntry->description.empty())
        entryDescription = linkedEntry->description;
    if (!requestedDescriptionOpt.has_value() && entryDescription.empty() && descriptor && !descriptor->description.empty())
        entryDescription = descriptor->description;

    nlohmann::json descriptorSummary = linkedEntry && linkedEntry->descriptorSummary.is_object()
        ? linkedEntry->descriptorSummary
        : nlohmann::json::object();
    if (descriptor)
    {
        descriptorSummary = nlohmann::json::object();
        if (!descriptor->displayName.empty())
            descriptorSummary["displayName"] = descriptor->displayName;
        if (!descriptor->version.empty())
            descriptorSummary["version"] = descriptor->version;
        if (!descriptor->category.empty())
            descriptorSummary["category"] = descriptor->category;
        descriptorSummary["parameterCount"] = descriptor->parameters.size();
        descriptorSummary["resourceCount"] = descriptor->exposedResources.size();
    }

    std::vector<std::string> entryTags = linkedEntry ? linkedEntry->tags : std::vector<std::string>{};
    if (entryJson.contains("tags") && entryJson["tags"].is_array())
    {
        entryTags.clear();
        for (const auto& tagValue : entryJson["tags"])
        {
            if (tagValue.is_string())
                entryTags.push_back(tagValue.get<std::string>());
        }
    }

    std::string entryOrigin = requestedOriginOpt.value_or("");
    if (entryOrigin.empty() && linkedEntry && !linkedEntry->origin.empty())
        entryOrigin = linkedEntry->origin;
    if (entryOrigin.empty())
        entryOrigin = "imported";

    std::string latestRevisionId = requestedRevisionIdOpt.value_or("");
    if (latestRevisionId.empty() && linkedEntry && !linkedEntry->latestRevisionId.empty())
        latestRevisionId = linkedEntry->latestRevisionId;
    if (latestRevisionId.empty())
    {
        if (const auto revisionIt = moduleResource->metadata.find("customEffectRevisionId");
            revisionIt != moduleResource->metadata.end())
        {
            latestRevisionId = revisionIt->second;
        }
    }

    std::string thumbnailDataUrl = requestedThumbnailOpt.value_or("");
    if (thumbnailDataUrl.empty() && linkedEntry && !linkedEntry->thumbnailDataUrl.empty())
        thumbnailDataUrl = linkedEntry->thumbnailDataUrl;
    if (thumbnailDataUrl.empty() && descriptor && !descriptor->thumbnailDataUrl.empty())
        thumbnailDataUrl = descriptor->thumbnailDataUrl;

    CustomEffectLibraryEntry entry;
    entry.id = entryId;
    entry.name = entryName;
    entry.category = entryCategory;
    entry.description = entryDescription;
    entry.baseEffectType = EffectGuids::kWasmHost;
    entry.moduleResourceType = moduleResource->type;
    entry.moduleResourceId = moduleResource->id;
    entry.latestRevisionId = latestRevisionId;
    entry.thumbnailDataUrl = thumbnailDataUrl;
    entry.tags = std::move(entryTags);
    entry.defaultParams = descriptorNode.params;
    entry.descriptorSummary = std::move(descriptorSummary);
    entry.origin = entryOrigin;
    entry.createdAt = linkedEntry && !linkedEntry->createdAt.empty() ? linkedEntry->createdAt : BuildTimestampUtcIso();
    entry.updatedAt = BuildTimestampUtcIso();

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();

    if (applyToNode)
    {
        if (node->resources.empty())
            node->resources.resize(1);
        node->resources.front().resourceType = entry.moduleResourceType;
        node->resources.front().resourceId = entry.moduleResourceId;
        node->resources.front().filePath.clear();
        node->resources.front().embeddedId.clear();
        node->config["customEffectId"] = entry.id;
        node->label = entry.name;
        node->category = entry.category;
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
        }
    }

    BroadcastState();
    SendMessageToUI(nlohmann::json{
        {"type", "customEffectSaved"},
        {"id", entry.id},
        {"name", entry.name},
        {"applyToNode", applyToNode},
        {"nodeId", nodeId},
    }.dump());
#endif
}

void PluginController::HandleImportGeneratedCustomEffectRequest(const nlohmann::json& payload)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)payload;
    ReportErrorToUI("Generated Custom Effect import failed", "Custom Effects are not supported in this build");
    return;
#else
    const std::string nodeId = payload.value("nodeId", "");
    if (nodeId.empty())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Missing node id");
        return;
    }

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!node)
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Selected node was not found");
        return;
    }

    const std::string resolvedType = EffectRegistry::Instance().Resolve(node->type);
    if (resolvedType != EffectGuids::kWasmHost)
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Selected node is not a Custom Effect");
        return;
    }

    const nlohmann::json entryJson = payload.value("entry", nlohmann::json::object());
    const nlohmann::json moduleJson = payload.value("module", nlohmann::json::object());
    if (!entryJson.is_object() || !moduleJson.is_object())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Missing entry or module payload");
        return;
    }

    const std::string moduleData = moduleJson.value("data", "");
    if (moduleData.empty())
    {
        ReportErrorToUI("Generated Custom Effect import failed", "Generated module data is missing");
        return;
    }

    const bool applyToNode = payload.value("applyToNode", false);
    const auto getOptionalString = [&](const nlohmann::json& json, const char* key) -> std::optional<std::string> {
        if (!json.contains(key) || !json[key].is_string())
            return std::nullopt;
        return json[key].get<std::string>();
    };

    const std::string linkedEntryId = [&]() -> std::string {
        if (const auto it = node->config.find("customEffectId"); it != node->config.end())
            return it->second;
        return {};
    }();
    const auto* linkedEntry = linkedEntryId.empty() ? nullptr : mCustomEffectLibrary.GetEntry(linkedEntryId);

    const std::optional<std::string> requestedIdOpt = getOptionalString(entryJson, "id");
    const std::optional<std::string> requestedNameOpt = getOptionalString(entryJson, "name");
    const std::optional<std::string> requestedCategoryOpt = getOptionalString(entryJson, "category");
    const std::optional<std::string> requestedDescriptionOpt = getOptionalString(entryJson, "description");
    const std::optional<std::string> requestedOriginOpt = getOptionalString(entryJson, "origin");
    const std::optional<std::string> requestedThumbnailOpt = getOptionalString(entryJson, "thumbnailDataUrl");
    const std::optional<std::string> requestedRevisionIdOpt = getOptionalString(entryJson, "latestRevisionId");
    const std::string descriptorText = moduleJson.value("descriptorText", std::string{});
    const std::string specText = moduleJson.value("specText", std::string{});
    const nlohmann::json manifestJson = moduleJson.value("manifest", nlohmann::json::object());

    nlohmann::json savePayload = nlohmann::json::object();
    savePayload["resourceType"] = "wasm";
    savePayload["data"] = moduleData;
    savePayload["fileName"] = moduleJson.value("fileName", std::string{});
    savePayload["resourceId"] = moduleJson.value("resourceId", std::string{});
    savePayload["name"] = moduleJson.value("name", requestedNameOpt.value_or("Generated Custom Effect"));
    savePayload["category"] = moduleJson.value("category", std::string{"Custom Effects"});
    savePayload["subfolder"] = moduleJson.value("subfolder", std::string{});
    savePayload["metadata"] = moduleJson.value("metadata", nlohmann::json::object());
    if (!requestedRevisionIdOpt.value_or("").empty())
        savePayload["metadata"]["customEffectRevisionId"] = *requestedRevisionIdOpt;
    if (!payload.value("sessionId", std::string{}).empty())
        savePayload["metadata"]["customEffectSessionId"] = payload.value("sessionId", std::string{});
    savePayload["metadata"]["customEffectOrigin"] = requestedOriginOpt.value_or("generated");

    std::string resourceSaveError;
    auto moduleResource = SaveLocalLibraryResource(savePayload, resourceSaveError, true);
    if (!moduleResource)
    {
        ReportErrorToUI("Generated Custom Effect import failed", resourceSaveError.empty() ? "Failed to save generated WASM module" : resourceSaveError);
        return;
    }

    const auto writeTextArtifact = [&](const std::filesystem::path& targetPath, const std::string& content) {
        const std::vector<std::uint8_t> bytes(content.begin(), content.end());
        return WriteFile(targetPath, bytes);
    };

    const auto persistArtifactPath = [&](const char* metadataKey, const std::filesystem::path& path) {
        moduleResource->metadata[metadataKey] = path.lexically_normal().generic_string();
    };

    const auto bundleDir = moduleResource->filePath.parent_path();
    if (!bundleDir.empty())
    {
        if (!descriptorText.empty())
        {
            const auto descriptorPath = bundleDir / "descriptor.txt";
            if (!writeTextArtifact(descriptorPath, descriptorText))
            {
                ReportErrorToUI("Generated Custom Effect import failed", "Failed to write generated descriptor artifact");
                return;
            }
            persistArtifactPath("customEffectDescriptorPath", descriptorPath);
        }

        if (!specText.empty())
        {
            const auto specPath = bundleDir / "spec.txt";
            if (!writeTextArtifact(specPath, specText))
            {
                ReportErrorToUI("Generated Custom Effect import failed", "Failed to write generated implementation spec artifact");
                return;
            }
            persistArtifactPath("customEffectSpecPath", specPath);
        }

        if (manifestJson.is_object() && !manifestJson.empty())
        {
            const auto manifestPath = bundleDir / "manifest.json";
            if (!writeTextArtifact(manifestPath, manifestJson.dump(2)))
            {
                ReportErrorToUI("Generated Custom Effect import failed", "Failed to write generated manifest artifact");
                return;
            }
            persistArtifactPath("customEffectManifestPath", manifestPath);

            const auto validationIt = manifestJson.find("validation");
            if (validationIt != manifestJson.end() && validationIt->is_object())
            {
                const auto validationPath = bundleDir / "validation-report.json";
                if (!writeTextArtifact(validationPath, validationIt->dump(2)))
                {
                    ReportErrorToUI("Generated Custom Effect import failed", "Failed to write generated validation artifact");
                    return;
                }
                persistArtifactPath("customEffectValidationPath", validationPath);
            }

        }
    }

    mResourceLibrary.UpdateResource(moduleResource->type, moduleResource->id, *moduleResource);
    AppendUserLibraryResource(*moduleResource);

    GraphNode descriptorNode = *node;
    if (descriptorNode.resources.empty())
        descriptorNode.resources.resize(1);
    descriptorNode.resources.front().resourceType = moduleResource->type;
    descriptorNode.resources.front().resourceId = moduleResource->id;
    descriptorNode.resources.front().filePath.clear();
    descriptorNode.resources.front().embeddedId.clear();

    std::map<std::string, double> requestedDefaultParams;
    if (entryJson.contains("defaultParams") && entryJson["defaultParams"].is_object())
    {
        for (const auto& item : entryJson["defaultParams"].items())
        {
            if (item.value().is_number())
                requestedDefaultParams[item.key()] = item.value().get<double>();
        }
    }
    if (!requestedDefaultParams.empty())
        descriptorNode.params = requestedDefaultParams;

    RefreshWasmNodeDescriptor(descriptorNode);

    std::optional<WasmModuleDescriptor> descriptor;
    if (const auto descriptorIt = descriptorNode.config.find(WasmEffect::kDescriptorConfigKey);
        descriptorIt != descriptorNode.config.end())
    {
        std::string parseError;
        descriptor = WasmEffect::ParseDescriptorConfig(descriptorIt->second, &parseError);
        if (!descriptor && !parseError.empty())
            AppendSessionLog("WASM descriptor cache parse failed while importing generated Custom Effect " + nodeId + ": " + parseError);
    }

    std::string entryName = requestedNameOpt.value_or("");
    if (entryName.empty() && linkedEntry && !linkedEntry->name.empty())
        entryName = linkedEntry->name;
    if (entryName.empty() && descriptor && !descriptor->displayName.empty())
        entryName = descriptor->displayName;
    if (entryName.empty() && !moduleResource->name.empty())
        entryName = moduleResource->name;
    if (entryName.empty())
        entryName = "Custom Effect";

    std::string entryId = requestedIdOpt.value_or("");
    if (entryId.empty())
        entryId = linkedEntryId;
    if (entryId.empty())
    {
        std::string baseId = util::SanitizePathSegment(entryName, true);
        if (baseId.empty())
            baseId = "custom-effect";
        entryId = baseId;
        std::size_t suffix = 2;
        while (mCustomEffectLibrary.GetEntry(entryId) != nullptr)
            entryId = baseId + "-" + std::to_string(suffix++);
    }

    std::string entryCategory = requestedCategoryOpt.value_or("");
    if (entryCategory.empty() && linkedEntry && !linkedEntry->category.empty())
        entryCategory = linkedEntry->category;
    if (entryCategory.empty() && descriptor && !descriptor->category.empty())
        entryCategory = descriptor->category;
    if (entryCategory.empty() && !node->category.empty())
        entryCategory = node->category;
    if (entryCategory.empty())
        entryCategory = "utility";

    std::string entryDescription = requestedDescriptionOpt.value_or("");
    if (entryDescription.empty() && linkedEntry && !linkedEntry->description.empty())
        entryDescription = linkedEntry->description;
    if (entryDescription.empty() && descriptor && !descriptor->description.empty())
        entryDescription = descriptor->description;

    nlohmann::json descriptorSummary = entryJson.contains("descriptorSummary") && entryJson["descriptorSummary"].is_object()
        ? entryJson["descriptorSummary"]
        : nlohmann::json::object();
    if (descriptorSummary.empty() && linkedEntry && linkedEntry->descriptorSummary.is_object())
        descriptorSummary = linkedEntry->descriptorSummary;
    if (descriptor)
    {
        descriptorSummary = nlohmann::json::object();
        if (!descriptor->displayName.empty())
            descriptorSummary["displayName"] = descriptor->displayName;
        if (!descriptor->version.empty())
            descriptorSummary["version"] = descriptor->version;
        if (!descriptor->category.empty())
            descriptorSummary["category"] = descriptor->category;
        descriptorSummary["parameterCount"] = descriptor->parameters.size();
        descriptorSummary["resourceCount"] = descriptor->exposedResources.size();
    }

    std::vector<std::string> entryTags = linkedEntry ? linkedEntry->tags : std::vector<std::string>{};
    if (entryJson.contains("tags") && entryJson["tags"].is_array())
    {
        entryTags.clear();
        for (const auto& tagValue : entryJson["tags"])
        {
            if (tagValue.is_string())
                entryTags.push_back(tagValue.get<std::string>());
        }
    }

    std::string entryOrigin = requestedOriginOpt.value_or("");
    if (entryOrigin.empty() && linkedEntry && !linkedEntry->origin.empty())
        entryOrigin = linkedEntry->origin;
    if (entryOrigin.empty())
        entryOrigin = "generated";

    std::string latestRevisionId = requestedRevisionIdOpt.value_or("");
    if (latestRevisionId.empty() && linkedEntry && !linkedEntry->latestRevisionId.empty())
        latestRevisionId = linkedEntry->latestRevisionId;
    if (latestRevisionId.empty())
    {
        if (const auto revisionIt = moduleResource->metadata.find("customEffectRevisionId");
            revisionIt != moduleResource->metadata.end())
        {
            latestRevisionId = revisionIt->second;
        }
    }

    std::string thumbnailDataUrl = requestedThumbnailOpt.value_or("");
    if (thumbnailDataUrl.empty() && linkedEntry && !linkedEntry->thumbnailDataUrl.empty())
        thumbnailDataUrl = linkedEntry->thumbnailDataUrl;
    if (thumbnailDataUrl.empty() && descriptor && !descriptor->thumbnailDataUrl.empty())
        thumbnailDataUrl = descriptor->thumbnailDataUrl;

    CustomEffectLibraryEntry entry;
    entry.id = entryId;
    entry.name = entryName;
    entry.category = entryCategory;
    entry.description = entryDescription;
    entry.baseEffectType = EffectGuids::kWasmHost;
    entry.moduleResourceType = moduleResource->type;
    entry.moduleResourceId = moduleResource->id;
    entry.latestRevisionId = latestRevisionId;
    entry.thumbnailDataUrl = thumbnailDataUrl;
    entry.tags = std::move(entryTags);
    entry.defaultParams = !requestedDefaultParams.empty() ? requestedDefaultParams : descriptorNode.params;
    entry.descriptorSummary = std::move(descriptorSummary);
    entry.origin = entryOrigin;
    entry.createdAt = linkedEntry && !linkedEntry->createdAt.empty() ? linkedEntry->createdAt : BuildTimestampUtcIso();
    entry.updatedAt = BuildTimestampUtcIso();

    mCustomEffectLibrary.UpsertEntry(entry);
    SaveCustomEffectLibrary();

    if (applyToNode)
    {
        if (node->resources.empty())
            node->resources.resize(1);
        node->resources.front().resourceType = entry.moduleResourceType;
        node->resources.front().resourceId = entry.moduleResourceId;
        node->resources.front().filePath.clear();
        node->resources.front().embeddedId.clear();
        node->config["customEffectId"] = entry.id;
        node->label = entry.name;
        node->category = entry.category;
        if (!entry.defaultParams.empty())
            node->params = entry.defaultParams;
        RefreshWasmNodeDescriptor(*node);

        if (IsCompositeEditMode())
        {
            BroadcastCompositeEditState();
        }
        else if (mActivePreset)
        {
            SyncActivePresetSceneGraph();
            ApplyPreset(*mActivePreset);
        }
    }

    BroadcastState();
    SendMessageToUI(nlohmann::json{
        {"type", "customEffectSaved"},
        {"id", entry.id},
        {"name", entry.name},
        {"applyToNode", applyToNode},
        {"nodeId", nodeId},
    }.dump());
#endif
}

void PluginController::HandleExportGeneratedCustomEffectBundleRequest(const nlohmann::json& payload)
{
    const std::string dataEncoded = payload.value("data", "");
    const std::string suggestedName = payload.value("fileName", "custom-effect.custom-effect.zip");

    if (dataEncoded.empty())
    {
        SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"}, {"message", "Missing export data"}}.dump());
        return;
    }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Download Custom Effect Bundle", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            {
                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"}, {"message", "Export cancelled"}}.dump());
                return;
            }

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            {
                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"}, {"message", "Invalid export data"}}.dump());
                return;
            }

            if (!WriteFile(result.path, decodedBytes))
            {
                SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportFailed"}, {"message", "Failed to write file"}}.dump());
                return;
            }

            SendMessageToUI(nlohmann::json{{"type", "generatedCustomEffectBundleExportSaved"}, {"path", result.path.generic_string()}}.dump());
            AppendSessionLog("Generated Custom Effect bundle exported: " + result.path.generic_string());
        });
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

void PluginController::HandleDeleteCustomEffectEntryRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty())
    {
        ReportErrorToUI("Custom Effect delete failed", "Missing entry id");
        return;
    }

    if (!mCustomEffectLibrary.RemoveEntry(id))
    {
        ReportErrorToUI("Custom Effect delete failed", "Entry not found");
        return;
    }

    SaveCustomEffectLibrary();
    BroadcastState();
}

// ════════════════════════════════════════════════════════════════════
// Composite (Multi-Rig) Preset handlers
// ════════════════════════════════════════════════════════════════════

void PluginController::SendCompositePresetListToUI()
{
    const auto dir = mResourceRoot / CompositePresetStorage::kSubdir;
    const auto presets = CompositePresetStorage::ListAll(dir);
    nlohmann::json msg;
    msg["type"] = "compositePresetList";
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& cp : presets)
        arr.push_back(nlohmann::json(cp));
    msg["compositePresets"] = std::move(arr);
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSaveCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string name = payload.value("name", "");
    if (name.empty()) { ReportErrorToUI("Save Multi-Rig failed", "A name is required"); return; }

    const std::string description = payload.value("description", "");
    const nlohmann::json tagsPayload = payload.value("tags", nlohmann::json::array());

    // Build CompositePreset from current mixer state
    CompositePreset cp;
    cp.name = name;
    cp.description = description;
    cp.masterGain = mPresetMixer.GetMasterGain();
    cp.limiterEnabled = mPresetMixer.IsLimiterEnabled();

    for (const auto& pid : mPresetMixer.GetActivePresetIds())
    {
        const auto cfgOpt = mPresetMixer.GetPresetConfig(pid);
        if (!cfgOpt) continue;
        CompositePresetSlot slot;
        slot.slotId = cfgOpt->id;
        slot.presetId = pid;
        slot.mix = cfgOpt->mix;
        slot.pan = cfgOpt->pan;
        slot.mute = cfgOpt->mute;
        slot.solo = cfgOpt->solo;
        cp.slots.push_back(std::move(slot));
    }

    if (cp.slots.empty()) { ReportErrorToUI("Save Multi-Rig failed", "No active presets in mixer"); return; }

    if (tagsPayload.is_array())
    {
        for (const auto& tagValue : tagsPayload)
        {
            if (!tagValue.is_string())
                continue;
            std::string tag = tagValue.get<std::string>();
            const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
            tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(), [&](char ch)
            {
                return !isSpace(static_cast<unsigned char>(ch));
            }));
            tag.erase(std::find_if(tag.rbegin(), tag.rend(), [&](char ch)
            {
                return !isSpace(static_cast<unsigned char>(ch));
            }).base(), tag.end());
            if (!tag.empty())
                cp.tags.push_back(tag);
        }
    }

    // Assign id and timestamps
    const std::string existingId = payload.value("id", "");
    if (!existingId.empty())
    {
        cp.id = existingId;
    }
    else
    {
        // Generate a simple id from name + timestamp
        const auto now = std::chrono::system_clock::now();
        const auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        cp.id = guitarfx::util::SanitizeFilename(name) + "_" + std::to_string(ts);
    }

    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    const std::string ts = oss.str();

    if (cp.createdAt.empty()) cp.createdAt = ts;
    cp.modifiedAt = ts;

    const auto dir = mResourceRoot / CompositePresetStorage::kSubdir;
    if (!CompositePresetStorage::SaveToFile(cp, dir))
    {
        ReportErrorToUI("Save Multi-Rig failed", "Could not write file");
        return;
    }

    // Confirm save to UI and send updated list
    SendMessageToUI(nlohmann::json{{"type", "compositePresetSaved"}, {"id", cp.id}, {"name", cp.name}}.dump());
    SendCompositePresetListToUI();
}

void PluginController::HandleLoadCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty()) { ReportErrorToUI("Load Multi-Rig failed", "Missing preset id"); return; }

    const auto dir = mResourceRoot / CompositePresetStorage::kSubdir;
    const auto cpOpt = CompositePresetStorage::LoadById(id, dir);
    if (!cpOpt) { ReportErrorToUI("Load Multi-Rig failed", "Preset not found: " + id); return; }

    const auto& cp = *cpOpt;

    // Clear existing mixer slots
    for (const auto& pid : mPresetMixer.GetActivePresetIds())
        RemoveActivePreset(pid);

    // Load each slot
    for (const auto& slot : cp.slots)
    {
        if (!AddActivePresetById(slot.presetId)) continue;
        SetActivePresetMix(slot.presetId, slot.mix);
        SetActivePresetPan(slot.presetId, slot.pan);
        SetActivePresetMute(slot.presetId, slot.mute);
        SetActivePresetSolo(slot.presetId, slot.solo);
    }

    // Restore master settings
    SetMasterGain(cp.masterGain);
    SetLimiterEnabled(cp.limiterEnabled);

    // Notify UI
    SendMessageToUI(nlohmann::json{{"type", "compositePresetLoaded"}, {"id", cp.id}, {"name", cp.name}}.dump());
    BroadcastState();
}

void PluginController::HandleGetCompositePresetListRequest()
{
    SendCompositePresetListToUI();
}

void PluginController::HandleRemoveCompositePresetRequest(const nlohmann::json& payload)
{
    const std::string id = payload.value("id", "");
    if (id.empty()) { ReportErrorToUI("Remove Multi-Rig failed", "Missing preset id"); return; }

    const auto dir = mResourceRoot / CompositePresetStorage::kSubdir;
    const bool removed = CompositePresetStorage::DeleteById(id, dir);
    if (!removed) { ReportErrorToUI("Remove Multi-Rig failed", "Preset not found: " + id); return; }

    SendCompositePresetListToUI();
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
    const std::string presetsSuffix = ".soundshed.presets";

    const auto hasSuffix = [&lowerSuggested](const std::string& suffix) -> bool
    {
        return lowerSuggested.size() >= suffix.size() &&
               lowerSuggested.compare(lowerSuggested.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    const std::string selectedSuffix = hasSuffix(presetsSuffix) ? presetsSuffix : presetSuffix;

    while (hasSuffix(presetSuffix) || hasSuffix(presetsSuffix))
    {
        const std::string& suffixToTrim = hasSuffix(presetsSuffix) ? presetsSuffix : presetSuffix;
        suggestedName.erase(suggestedName.size() - suffixToTrim.size());
        lowerSuggested.erase(lowerSuggested.size() - suffixToTrim.size());
    }

    if (suggestedName.empty())
    {
        suggestedName = "preset";
    }
    suggestedName += selectedSuffix;

    if (dataEncoded.empty())
    { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Missing export data"}}.dump()); return; }

    mHost.SaveFileAsync(BrowseFileType::ArchiveFile, "Save Preset Archive", suggestedName,
        [this, dataEncoded](const BrowseFileResult& result)
        {
            if (!result.success)
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Save cancelled"}}.dump()); return; }

            const auto normalizedPath = NormalizePresetArchiveSavePath(result.path);

            const auto decodedBytes = util::DecodeBase64(dataEncoded);
            if (decodedBytes.empty())
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Invalid export data"}}.dump()); return; }

            if (!WriteFile(normalizedPath, decodedBytes))
            { SendMessageToUI(nlohmann::json{{"type", "presetExportFailed"}, {"message", "Failed to save file"}}.dump()); return; }

            SendMessageToUI(nlohmann::json{{"type", "presetExportSaved"}, {"path", normalizedPath.generic_string()}}.dump());
            AppendSessionLog("Preset export saved: " + normalizedPath.generic_string());
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
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);

    std::error_code ec;
    if (std::filesystem::exists(layoutDir, ec))
    {
        std::filesystem::remove_all(layoutDir, ec);
        if (ec)
        {
            ReportErrorToUI("Delete layout failed", "Unable to remove layout directory");
            return;
        }
        AppendSessionLog("Layout deleted: " + layoutDir.generic_string());
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
    const bool isNewLayout = payload.value("isNewLayout", false);
    const auto layoutIt = payload.find("layout");

    if (effectType.empty() || layoutIt == payload.end() || !layoutIt->is_object())
    { ReportErrorToUI("Save layout failed", "Missing effect type or layout data"); return; }

    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

    if (layoutId.empty())
        layoutId = GenerateGuidV4String();

    // Persist layout JSON in its own subdirectory.
    nlohmann::json layoutJson = *layoutIt;
    layoutJson["layoutId"] = layoutId;
    SaveLayoutToFile(layoutId, layoutJson);

    // When saving a new layout (first time or forked from factory), copy any referenced
    // images from wherever they currently live into this layout's images/ directory so
    // the layout is self-contained.
    if (isNewLayout)
    {
        const auto destImagesDir = ResolveLayoutDir(mFileSystem, layoutId) / "images";
        [[maybe_unused]] const auto ensuredDest = mFileSystem.EnsureDirectory(destImagesDir);

        const auto referencedIt = payload.find("referencedImageIds");
        if (referencedIt != payload.end() && referencedIt->is_array())
        {
            const auto settingsDir = mFileSystem.ResolveSettingsDirectory();
            const auto userLayoutsRoot = settingsDir / "layouts" / "content";

            for (const auto& idVal : *referencedIt)
            {
                if (!idVal.is_string()) continue;
                const std::string imageId = idVal.get<std::string>();

                // Search: first in other user layout image dirs, then legacy fallback dir.
                std::filesystem::path sourcePath;
                std::error_code ec;

                if (std::filesystem::exists(userLayoutsRoot, ec))
                {
                    for (const auto& layoutDir : std::filesystem::directory_iterator(userLayoutsRoot, ec))
                    {
                        if (!layoutDir.is_directory()) continue;
                        const auto imagesDir = layoutDir.path() / "images";
                        if (!std::filesystem::exists(imagesDir)) continue;
                        for (const auto& imgEntry : std::filesystem::directory_iterator(imagesDir, ec))
                        {
                            if (!imgEntry.is_regular_file()) continue;
                            if (imgEntry.path().stem().string() == imageId)
                            {
                                sourcePath = imgEntry.path();
                                break;
                            }
                        }
                        if (!sourcePath.empty()) break;
                    }
                }

                if (sourcePath.empty()) continue; // image not found, skip

                const auto destPath = destImagesDir / sourcePath.filename();
                if (destPath != sourcePath)
                {
                    std::filesystem::copy_file(sourcePath, destPath,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec)
                        AppendSessionLog("Failed to copy layout image " + sourcePath.generic_string() + ": " + ec.message());
                }
            }
        }
    }

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
    const std::string layoutId = payload.value("layoutId", "");

    mHost.BrowseFileAsync(BrowseFileType::ImageFile, "Select Image",
        [this, purpose, layerIndex, paramKey, layoutId](const BrowseFileResult& result)
        {
            if (!result.success) return;

            const auto imagesDir = layoutId.empty()
                ? mFileSystem.ResolveSettingsDirectory() / "layouts" / "content" / "images"
                : ResolveLayoutDir(mFileSystem, layoutId) / "images";
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
    const std::string layoutId = payload.value("layoutId", "");

    if (imageId.empty() || fileName.empty() || dataEncoded.empty())
    { AppendSessionLog("SaveLayoutImage: missing required fields"); return; }

    const auto imagesDir = layoutId.empty()
        ? mFileSystem.ResolveSettingsDirectory() / "layouts" / "content" / "images"
        : ResolveLayoutDir(mFileSystem, layoutId) / "images";
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
        TouchSharedSyncState({"resourceLibrary"});
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
    TouchSharedSyncState({"composites"});

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
    TouchSharedSyncState({"composites"});

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
            TouchSharedSyncState({"composites"});

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

void PluginController::HandleRenderDemoAudioRequest(const nlohmann::json& payload)
{
    auto sendRenderFailure = [this](const std::string& message)
    {
        SendMessageToUI(nlohmann::json{
            {"type", "demoAudioRenderFailed"},
            {"message", message}
        }.dump());
    };

    const double hostSampleRate = mHost.GetSampleRate();
    std::string sampleRateError;
    const double renderSampleRate = ResolveDemoRenderSampleRate(payload, hostSampleRate, sampleRateError);
    if (renderSampleRate <= 0.0)
    {
        sendRenderFailure(sampleRateError.empty() ? "Render sample rate is invalid" : sampleRateError);
        return;
    }

    const std::string suggestedName = BuildDemoRenderSuggestedFilename(
        payload.value("suggestedName", std::string("demo-audio.wav")),
        renderSampleRate);

    const nlohmann::json payloadCopy = payload;
    mHost.SaveFileAsync(BrowseFileType::AudioFile, "Render Demo Audio", suggestedName,
        [this, payloadCopy, sendRenderFailure, renderSampleRate](const BrowseFileResult& result)
        {
            if (!result.success)
            {
                sendRenderFailure("Save cancelled");
                return;
            }

            if (mSignalTestActive.load(std::memory_order_acquire))
            {
                sendRenderFailure("Signal path test is currently running");
                return;
            }

            const double restoreSampleRate = mHost.GetSampleRate();
            if (restoreSampleRate <= 0.0)
            {
                sendRenderFailure("Audio device sample rate is unavailable");
                return;
            }
            const int hostBlockSize = std::max(1, mHost.GetBlockSize());

            OfflineRenderBuffer source;
            std::string error;
            if (payloadCopy.contains("takeId") && payloadCopy["takeId"].is_string())
            {
                const std::string takeId = payloadCopy.value("takeId", std::string{});
                const auto take = FindRiffTakeById(takeId);
                if (!take)
                {
                    sendRenderFailure("Take not found");
                    return;
                }

                const std::string filePath = take->value("filePath", std::string{});
                if (filePath.empty() || !std::filesystem::exists(filePath))
                {
                    sendRenderFailure("Take WAV file is missing");
                    return;
                }

                std::ifstream input(filePath, std::ios::binary);
                if (!input)
                {
                    sendRenderFailure("Unable to open take WAV file");
                    return;
                }

                std::vector<std::uint8_t> bytes(
                    (std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                if (bytes.empty())
                {
                    sendRenderFailure("Take WAV file is empty");
                    return;
                }

                auto prepared = PrepareOfflineRenderBuffer(
                    bytes,
                    renderSampleRate,
                    takeId,
                    payloadCopy.value("title", take->value("title", std::string("Riff Take"))),
                    error);
                if (!prepared)
                {
                    sendRenderFailure(error.empty() ? "Unable to prepare riff take audio" : error);
                    return;
                }
                source = std::move(*prepared);
            }
            else
            {
                const auto audioIter = payloadCopy.find("audio");
                if (audioIter == payloadCopy.end() || !audioIter->is_object())
                {
                    sendRenderFailure("Audio payload is missing");
                    return;
                }

                const std::string dataEncoded = audioIter->value("data", "");
                if (dataEncoded.empty())
                {
                    sendRenderFailure("Audio payload did not include data");
                    return;
                }

                const auto decodedBytes = util::DecodeBase64(dataEncoded);
                if (decodedBytes.empty())
                {
                    sendRenderFailure("Unable to decode audio data");
                    return;
                }

                auto prepared = PrepareOfflineRenderBuffer(
                    decodedBytes,
                    renderSampleRate,
                    audioIter->value("id", std::string{}),
                    payloadCopy.value("title", audioIter->value("title", std::string("Demo Audio"))),
                    error);
                if (!prepared)
                {
                    sendRenderFailure(error.empty() ? "Unable to prepare demo audio" : error);
                    return;
                }
                source = std::move(*prepared);
            }

            if (mDemoPreview)
                mDemoPreview->StopPreview();
            {
                std::lock_guard<std::mutex> lock(mDSPMutex);
                DeactivateRiffGuidance(true);
            }

            std::vector<float> renderedLeft;
            std::vector<float> renderedRight;
            if (!RenderBufferThroughMixer(
                    mPresetMixer,
                    mDSPMutex,
                    source,
                    hostBlockSize,
                    restoreSampleRate,
                    hostBlockSize,
                    GetEffectiveTempoBpm(),
                    renderedLeft,
                    renderedRight))
            {
                sendRenderFailure("Failed to render demo audio");
                return;
            }

            if (!WriteStereo16BitWav(result.path, renderedLeft, renderedRight, static_cast<int>(std::llround(renderSampleRate))))
            {
                sendRenderFailure("Failed to write WAV file");
                return;
            }

            SendMessageToUI(nlohmann::json{
                {"type", "demoAudioRenderSaved"},
                {"path", result.path.generic_string()},
                {"sampleRate", renderSampleRate}
            }.dump());
            AppendSessionLog("Demo audio rendered (" + std::to_string(static_cast<int>(std::llround(renderSampleRate)))
                + " Hz): " + result.path.generic_string());
        });
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
        const std::filesystem::path libraryPath = util::PathFromUtf8(requestedPath);
        std::filesystem::create_directories(libraryPath);
            mAppSettings[kRiffLibraryPathSettingKey] = util::PathToUtf8(libraryPath);
        SaveAppSettings();

        {
            std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
            mRiffLibraryIndex = LoadRiffLibraryIndex();
            mRiffLibraryIndex["path"] = util::PathToUtf8(libraryPath);
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
    config.metronomeClickEnabled = payload.value("metronomeClickEnabled", true);
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.beatPattern = payload.value("beatPattern", mMetronomeBeatPattern); // use UI value or fall back to global
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
    msg["metronomeClickEnabled"] = config.metronomeClickEnabled;
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
    config.bars = std::max(1, std::min(64, payload.value("bars", 16)));
    config.countInBars = std::max(0, payload.value("countInBars", 1));
    config.metronomeClickEnabled = payload.value("metronomeClickEnabled", true);
    config.patternType = payload.value("patternType", std::string("click"));
    config.patternId = payload.value("patternId", std::string{});
    config.beatPattern = payload.value("beatPattern", mMetronomeBeatPattern);
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
    mRiffCapture.armPostCountInSamples = 0;
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
    msg["bars"] = config.bars;
    msg["metronomeClickEnabled"] = config.metronomeClickEnabled;
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
        ReportErrorToUI("Riff Library", "Dropped audio data is missing");
        return;
    }

    const auto bytes = util::DecodeBase64(base64);
    if (bytes.empty())
    {
            ReportErrorToUI("Riff Library", "Failed to decode dropped audio data");
        return;
    }

        const auto decodedOpt = util::DecodeAudioBytes(bytes);
    if (!decodedOpt)
    {
            ReportErrorToUI("Riff Library", "Unsupported audio format (expected WAV, AIFF, or MP3)");
        return;
    }

    const auto& decoded = *decodedOpt;
    if (decoded.channelSamples.empty() || decoded.channelSamples.front().empty())
    {
            ReportErrorToUI("Riff Library", "Dropped audio file has no audio samples");
        return;
    }

    const std::size_t frameCount = decoded.channelSamples.front().size();
    if (frameCount == 0)
    {
            ReportErrorToUI("Riff Library", "Dropped audio file has no audio frames");
        return;
    }

    RiffCaptureRuntime imported;
    imported.active = false;
    imported.complete = true;
    imported.takeId = BuildRiffTakeId();
    imported.config.tempoBpm = ClampValue(payload.value("tempoBpm", GetEffectiveTempoBpm()), kMetronomeMinBpm, kMetronomeMaxBpm);
    imported.config.timeSigNum = std::max(1, payload.value("timeSigNum", 4));
    imported.config.timeSigDen = std::max(1, payload.value("timeSigDen", 4));
    imported.config.countInBars = 0;
    imported.config.patternType = payload.value("patternType", std::string("click"));
    imported.config.patternId = payload.value("patternId", std::string{});
    imported.config.presetId = mActivePresetId;
    imported.config.presetName = mActivePreset ? mActivePreset->name : std::string{};
    imported.sampleRate = decoded.sampleRate > 0.0 ? decoded.sampleRate : mHost.GetSampleRate();
    imported.bitsPerSample = decoded.bitsPerSample > 0 ? decoded.bitsPerSample : 16;
    imported.config.bars = payload.contains("bars")
        ? std::max(1, payload.value("bars", 1))
        : ComputeBarsFromFrames(frameCount,
                                imported.sampleRate,
                                imported.config.tempoBpm,
                                imported.config.timeSigNum,
                                imported.config.timeSigDen);
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
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
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
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
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
    imported.config.metronomeClickEnabled = take->value("metronomeClickEnabled", true);
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
    msg["metronomeClickEnabled"] = captureSnapshot.config.metronomeClickEnabled;
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
    nlohmann::json updatedLibrary = nlohmann::json::object();
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
    if (payload.contains("metronomeClickEnabled"))
        capture.config.metronomeClickEnabled = payload.value("metronomeClickEnabled", capture.config.metronomeClickEnabled);
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
    takeJson["filePath"] = util::PathToUtf8(wavPath);
    takeJson["durationSec"] = capture.sampleRate > 0.0
        ? static_cast<double>(capture.left.size()) / capture.sampleRate
        : 0.0;
    takeJson["bars"] = capture.config.bars;
    takeJson["tempoBpm"] = capture.config.tempoBpm;
    takeJson["timeSigNum"] = capture.config.timeSigNum;
    takeJson["timeSigDen"] = capture.config.timeSigDen;
    takeJson["metronomeClickEnabled"] = capture.config.metronomeClickEnabled;
    takeJson["patternType"] = capture.config.patternType;
    if (!capture.config.patternId.empty())
        takeJson["patternId"] = capture.config.patternId;
    if (!capture.config.beatPattern.empty())
        takeJson["beatPattern"] = capture.config.beatPattern;
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

        mRiffLibraryIndex["path"] = util::PathToUtf8(libraryPath);
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
        updatedLibrary = LoadRiffLibraryIndex();
        mRiffLibraryIndex = updatedLibrary;
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mRiffCapture = RiffCaptureRuntime{};
    }

    nlohmann::json msg;
    msg["type"] = "riffSaved";
    msg["riffId"] = riffId;
    msg["takeId"] = capture.takeId;
    msg["path"] = util::PathToUtf8(wavPath);
    msg["library"] = updatedLibrary;
    SendMessageToUI(msg.dump());
    SendRiffLibraryStateToUI();
}

void PluginController::HandleDeleteRiffRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    std::vector<std::filesystem::path> takeFiles;
    std::filesystem::path takesDirToRemove;

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        if (!mRiffLibraryIndex.is_object() || !mRiffLibraryIndex.contains("riffs") || !mRiffLibraryIndex["riffs"].is_array())
            return;

        auto& riffs = mRiffLibraryIndex["riffs"];
        for (const auto& riff : riffs)
        {
            if (!riff.is_object() || riff.value("id", std::string{}) != riffId)
                continue;

            const auto takes = riff.value("takes", nlohmann::json::array());
            if (takes.is_array())
            {
                for (const auto& take : takes)
                {
                    if (!take.is_object() || !take.contains("filePath") || !take["filePath"].is_string())
                        continue;
                    const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());
                    if (!runtimePath.empty())
                        takeFiles.push_back(runtimePath);
                }
            }
            break;
        }

        riffs.erase(std::remove_if(riffs.begin(), riffs.end(),
            [&](const nlohmann::json& riff) { return riff.value("id", std::string{}) == riffId; }), riffs.end());
        SaveRiffLibraryIndex(mRiffLibraryIndex);
        takesDirToRemove = ResolveRiffLibraryPath() / "takes" / riffId;
    }

    for (const auto& path : takeFiles)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    if (!takesDirToRemove.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(takesDirToRemove, ec);
    }

    SendRiffLibraryStateToUI();
}

void PluginController::HandleSetRiffFavoriteRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool favorite = payload.value("favorite", false);
    {
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
    }
    SendRiffLibraryStateToUI();
}

void PluginController::HandleMarkRiffUsedRequest(const nlohmann::json& payload)
{
    const std::string riffId = payload.value("riffId", "");
    if (riffId.empty())
        return;

    const bool used = payload.value("used", false);
    const std::string songTitle = payload.value("songTitle", std::string{});
    {
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
    }
    SendRiffLibraryStateToUI();
}

void PluginController::HandlePreviewRiffTakeRequest(const nlohmann::json& payload)
{
    const std::string takeId = payload.value("takeId", "");
    const bool enableGuidance = payload.value("enableGuidance", true);
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
    guideConfig.metronomeClickEnabled = take->value("metronomeClickEnabled", true);
    guideConfig.patternType = take->value("patternType", std::string("click"));
    guideConfig.patternId = take->value("patternId", std::string{});
    guideConfig.beatPattern = take->value("beatPattern", mMetronomeBeatPattern);

    if (mDemoPreview)
    {
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            if (enableGuidance)
                ActivateRiffGuidance(guideConfig, true);
            else
                DeactivateRiffGuidance(true);
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
    // The UI only asks for this on startup or after a reload, when it has no roster to
    // resolve frames against, so always re-send the roster alongside the next frame.
    mSignalDiagnosticsRosterDirty = true;
    RequestSignalDiagnosticsToUI();
}

void PluginController::HandleGetPerformanceStatsRequest()
{
    RequestPerformanceStatsToUI();
}

void PluginController::HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload)
{
    (void)payload;
    const bool enabled = true;
    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPresetMixer.SetSignalDiagnosticsEnabled(enabled);
    mSignalDiagnosticsRosterDirty = true;
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
    nlohmann::json folders = payload.value("folders", nlohmann::json::array());
    std::string activeFolderId = payload.value("activeFolderId", "__all__");

    if (!IsPresetArchiveSessionActive() && !IsFactoryPresetArchiveLoadingEnabled() && folders.is_array())
    {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& folder : folders)
        {
            if (!folder.is_object())
                continue;

            const std::string folderId = folder.value("id", "");
            if (IsFactoryArchiveFolderId(folderId))
            {
                continue;
            }

            filtered.push_back(folder);
        }

        if (IsFactoryArchiveFolderId(activeFolderId))
        {
            activeFolderId = "__all__";
        }

        folders = std::move(filtered);
    }

    nlohmann::json msg;
    msg["type"] = "presetFolders";
    msg["folders"] = std::move(folders);
    msg["activeFolderId"] = activeFolderId;
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
    toStore["bankSize"] = payload.value("bankSize", 8);
    toStore["cursorIndex"] = payload.value("cursorIndex", 0);
    mSetlistBankSize = payload.value("bankSize", 8);
    mSetlistCursorIndex = payload.value("cursorIndex", 0);
    SaveUiStorageJson("setlists.json", toStore);
}

// ── Per-effect user presets ─────────────────────────────────────────────────
// User-saved parameter snapshots for a single effect type, stored as
// { "byEffectType": { "<effectType>": [ { id, name, parameters } ] } }.
// Factory presets stay in the effect registry (EffectTypeInfo::presets); these
// are the "custom" half of that same vocabulary, kept in UI storage so they are
// shared across presets rather than baked into any one of them.

namespace
{
constexpr const char* kEffectPresetsFile = "effect-presets.json";

nlohmann::json NormalizeEffectPresetsDocument(nlohmann::json document)
{
    if (!document.is_object())
        document = nlohmann::json::object();
    if (!document.contains("byEffectType") || !document["byEffectType"].is_object())
        document["byEffectType"] = nlohmann::json::object();
    return document;
}
} // namespace

void PluginController::BroadcastEffectPresets()
{
    const auto document = NormalizeEffectPresetsDocument(
        LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    nlohmann::json msg;
    msg["type"] = "effectPresets";
    msg["byEffectType"] = document["byEffectType"];
    SendMessageToUI(msg.dump());
}

void PluginController::HandleGetEffectPresetsRequest()
{
    BroadcastEffectPresets();
}

void PluginController::HandleSaveEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string name = payload.value("name", "");
    if (effectType.empty() || name.empty())
        return;

    const auto parameters = payload.value("parameters", nlohmann::json::object());
    if (!parameters.is_object())
        return;

    auto document = NormalizeEffectPresetsDocument(
        LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));

    auto& presets = document["byEffectType"][effectType];
    if (!presets.is_array())
        presets = nlohmann::json::array();

    // Saving under an existing name overwrites that entry in place, keeping its id
    // so any UI selection pointing at it stays valid.
    for (auto& existing : presets)
    {
        if (existing.value("name", "") == name)
        {
            existing["parameters"] = parameters;
            SaveUiStorageJson(kEffectPresetsFile, document);
            BroadcastEffectPresets();
            return;
        }
    }

    nlohmann::json entry;
    entry["id"] = "efp-" + GenerateGuidV4String();
    entry["name"] = name;
    entry["parameters"] = parameters;
    presets.push_back(std::move(entry));

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}

void PluginController::HandleDeleteEffectPresetRequest(const nlohmann::json& payload)
{
    const std::string effectType = payload.value("effectType", "");
    const std::string presetId = payload.value("presetId", "");
    if (effectType.empty() || presetId.empty())
        return;

    auto document = NormalizeEffectPresetsDocument(
        LoadUiStorageJson(kEffectPresetsFile, nlohmann::json::object()));
    if (!document["byEffectType"].contains(effectType))
        return;

    auto& presets = document["byEffectType"][effectType];
    if (!presets.is_array())
        return;

    nlohmann::json remaining = nlohmann::json::array();
    for (const auto& entry : presets)
    {
        if (entry.value("id", "") != presetId)
            remaining.push_back(entry);
    }

    if (remaining.size() == presets.size())
        return; // nothing matched — leave the file untouched

    if (remaining.empty())
        document["byEffectType"].erase(effectType);
    else
        presets = std::move(remaining);

    SaveUiStorageJson(kEffectPresetsFile, document);
    BroadcastEffectPresets();
}

// ════════════════════════════════════════════════════════════════════════
// Automation & MIDI mapping
// ════════════════════════════════════════════════════════════════════════

void PluginController::HandleGetAutomationRequest()
{
    nlohmann::json msg;
    msg["type"] = "automation";
    msg["slots"] = mAutomationSlots.GetSlotsJson();

    nlohmann::json registry = nlohmann::json::array();
    for (const auto& info : mAutomationSlots.GetRegistryInfo())
    {
        registry.push_back({
            {"address", info.address}, {"label", info.label}, {"unit", info.unit},
            {"min", info.minValue}, {"max", info.maxValue},
            {"isStepped", info.isStepped}, {"isTrigger", info.isTrigger}
        });
    }
    msg["registry"] = std::move(registry);
    msg["maxCustomSlots"] = kMaxCustomSlots;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetAutomationSlotRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    if (slotId.empty())
        return;

    const auto* existing = mAutomationSlots.FindSlot(slotId);
    const bool isDefault = existing && existing->isDefault;

    std::optional<std::string> label;
    if (payload.contains("label") && payload["label"].is_string())
        label = payload["label"].get<std::string>();

    std::optional<std::string> address;
    if (payload.contains("address") && payload["address"].is_string())
        address = payload["address"].get<std::string>();

    std::optional<std::string> nodeSelector;
    if (payload.contains("nodeSelector") && payload["nodeSelector"].is_string())
        nodeSelector = payload["nodeSelector"].get<std::string>();

    std::optional<MidiControlMap> midiMap;
    bool clearMidiMap = false;
    if (payload.contains("midiMap") && payload["midiMap"].is_object())
    {
        const auto& mm = payload["midiMap"];
        MidiControlMap m;
        m.eventType = static_cast<MidiControlMap::EventType>(mm.value("eventType", 0));
        m.channel = mm.value("channel", 0);
        m.controller = mm.value("controller", 0);
        m.mode = static_cast<MidiControlMap::Mode>(mm.value("mode", 0));
        m.sensitivity = mm.value("sensitivity", 0.1f);
        m.pickupRange = mm.value("pickupRange", 0.1f);
        midiMap = m;
    }
    else if (payload.contains("midiMap") && payload["midiMap"].is_null())
        clearMidiMap = true;

    std::optional<std::vector<KeyboardMap>> keyMaps;
    bool clearKeyMap = false;
    if (payload.contains("keyMap") && payload["keyMap"].is_array())
    {
        std::vector<KeyboardMap> kms;
        for (const auto& k : payload["keyMap"])
        {
            KeyboardMap km;
            km.key = k.value("key", "");
            km.mode = static_cast<KeyboardMap::Mode>(k.value("mode", 0));
            km.value = k.value("value", 0.0f);
            kms.push_back(std::move(km));
        }
        keyMaps = std::move(kms);
    }
    else if (payload.contains("keyMap") && payload["keyMap"].is_null())
        clearKeyMap = true;

    {
        // Lock the DSP mutex around structural slot mutations so we never
        // realloc/erase mSlots while the audio thread iterates it in HandleMidi.
        // Keep the critical section short: no disk I/O or UI sends under the lock.
        std::lock_guard<std::mutex> lock(mDSPMutex);
        if (isDefault)
            mAutomationSlots.SetDefaultSlotOverrides(slotId, label, midiMap, keyMaps);
        else
            mAutomationSlots.SetCustomSlot(slotId, label, address, nodeSelector, midiMap, keyMaps);

        if (clearMidiMap)
        {
            auto* slot = mAutomationSlots.FindSlot(slotId);
            if (slot) slot->midiMap.reset();
        }
        if (clearKeyMap)
        {
            auto* slot = mAutomationSlots.FindSlot(slotId);
            if (slot) slot->keyMaps.clear();
        }
    }

    SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
    HandleGetAutomationRequest();
}

void PluginController::HandleRemoveAutomationSlotRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mAutomationSlots.RemoveCustomSlot(slotId);
    }
    SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
    HandleGetAutomationRequest();
}

void PluginController::HandleSetAutomationValueRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    const float value = payload.value("value", 0.0f);
    const std::string sourceStr = payload.value("source", "ui");
    auto src = sourceStr == "keyboard" ? AutomationSource::Keyboard : AutomationSource::UI;

    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ApplyAutomationLocked(slotId, value, src);
}

void PluginController::ApplyAutomationFromDAW(const std::string& slotId, float normalized)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ApplyAutomationLocked(slotId, normalized, AutomationSource::DAW);
}

float PluginController::GetAutomationSlotValue(const std::string& slotId) const
{
    const auto* slot = mAutomationSlots.FindSlot(slotId);
    return slot ? slot->value.load() : 0.0f;
}

void PluginController::HandleArmMidiLearnRequest(const nlohmann::json& payload)
{
    const std::string slotId = payload.value("slotId", "");
    // mMidiLearnSlotId is read by the audio thread (under mDSPMutex) in HandleMidi.
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ArmMidiLearn(slotId);
}

void PluginController::HandleCancelMidiLearnRequest()
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mAutomationSlots.ArmMidiLearn("");
}

void PluginController::EnqueueMidi(const MidiEvent& ev)
{
    // Audio thread. Must never block or allocate: both vectors are pre-reserved
    // and capped, so push_back never reallocates. Events dropped past the cap are
    // a deliberate safety valve against pathological message-thread stalls.

    if (mMidiLogEnabled.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(mPendingMidiLogMutex);
        if (mPendingMidiLog.size() < kMaxPendingMidiLog)
            mPendingMidiLog.push_back(ev);
    }

    if (mPendingMidiApply.size() < kMaxPendingMidiApply)
        mPendingMidiApply.push_back(ev);
}

void PluginController::ProcessQueuedMidi()
{
    // Audio thread. Drain queued MIDI events under the DSP lock without ever
    // blocking: if the lock is held (e.g. a preset load on the message thread),
    // leave the events queued and retry on the next block.
    if (mPendingMidiApply.empty())
        return;

    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    for (const auto& ev : mPendingMidiApply)
        mAutomationSlots.HandleMidi(ev);
    mPendingMidiApply.clear();
}

void PluginController::SetMidiLogEnabled(bool enabled)
{
    mMidiLogEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
    {
        std::lock_guard<std::mutex> lock(mPendingMidiLogMutex);
        mPendingMidiLog.clear();
    }
}

void PluginController::ApplySetlistPresetByIndex(int index)
{
    // This method may be called from the audio thread (via automation/MIDI apply,
    // already holding mDSPMutex) or from the UI thread (not holding the lock).
    // ApplyActivePresetById needs to acquire mDSPMutex, so when we're already
    // holding it we must defer the actual preset swap to OnIdle.
    //
    // We detect this by trying to lock mDSPMutex non-blocking. If it fails,
    // we're on the audio thread (or another locked context) and must defer.

    if (mDSPMutex.try_lock())
    {
        // We got the lock — not currently held, safe to proceed directly.
        mDSPMutex.unlock();
        ApplySetlistPresetByIndexDirect(index);
    }
    else
    {
        // Lock is held (audio thread under DSP lock) — defer to OnIdle.
        std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
        mPendingSetlistPresetIndex = index;
    }
}

void PluginController::ApplySetlistPresetByIndexDirect(int index)
{
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
        return;

    // Resolve the active setlist by activeSetlistId (first setlist as fallback)
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    const nlohmann::json* activeSlots = nullptr;
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            activeSlots = &sl["slots"];
            break;
        }
    }
    if (!activeSlots || !activeSlots->is_array())
        return;

    if (index < 0 || index >= static_cast<int>(activeSlots->size()))
        return;

    const auto& slot = (*activeSlots)[index];
    const std::string presetId = slot.value("presetId", "");
    if (presetId.empty())
        return;

    mSetlistCursorIndex = index;

    // Persist cursor
    auto toStore = setlistsData;
    toStore["cursorIndex"] = index;
    SaveUiStorageJson("setlists.json", toStore);

    // Change to the preset. A setlist step is a *switch*, not a Multi-Rig add: it must swap
    // the mixer down to this one preset (gapless, via ApplyPreset's crossfade) rather than
    // stacking another instance on top of whatever is already playing.
    ApplyActivePresetById(presetId);

    // Notify the UI that the setlist cursor changed so it can update its display
    // and load the preset into the main preset chooser.
    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = activeSetlistId;
    msg["cursorIndex"] = index;
    msg["presetId"] = presetId;
    SendMessageToUI(msg.dump());
}

void PluginController::SetlistBankUp(int steps)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SetlistBankChangeDirect(steps);
    }
    else
    {
        std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
        mPendingSetlistBankDelta = mPendingSetlistBankDelta.value_or(0) + steps;
    }
}

int PluginController::GetActiveSceneIndex() const
{
    if (!mActivePreset)
        return -1;

    const std::string activeSceneId = GetResolvedActiveSceneId();
    for (std::size_t i = 0; i < mActivePreset->scenes.size(); ++i)
    {
        if (mActivePreset->scenes[i].id == activeSceneId)
            return static_cast<int>(i);
    }
    return -1;
}

void PluginController::SelectSceneByIndex(int index)
{
    // Same threading contract as ApplySetlistPresetByIndex: reachable from the
    // audio thread via automation/MIDI apply (already holding mDSPMutex) or from
    // the UI thread. SelectSceneByIndexDirect ends up in ApplyPreset, which takes
    // mDSPMutex itself, so defer to OnIdle when the lock is already held.
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SelectSceneByIndexDirect(index);
    }
    else
    {
        std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
        mPendingSceneIndex = index;
    }
}

void PluginController::SelectSceneByIndexDirect(int index)
{
    if (!mActivePreset)
        return;

    NormalizePresetScenes(*mActivePreset);

    // Out-of-range is a no-op rather than a clamp: a footswitch mapped to scene 4
    // should do nothing on a two-scene preset, not silently jump to scene 2.
    if (index < 0 || index >= static_cast<int>(mActivePreset->scenes.size()))
        return;

    const std::string targetSceneId = mActivePreset->scenes[static_cast<std::size_t>(index)].id;
    if (targetSceneId == GetResolvedActiveSceneId())
        return;

    if (!SetPresetActiveScene(*mActivePreset, targetSceneId, &mActiveSceneId))
        return;

    SyncActivePresetSceneGraph();
    ApplyPreset(*mActivePreset);

    // Report the switch on the same "presetLoaded" channel a UI-driven scene change
    // uses, so an open editor tracks the change and a closed one simply misses a
    // message it was never going to receive.
    nlohmann::json loaded;
    loaded["type"] = "presetLoaded";
    loaded["preset"] = SerializePresetForUi(*mActivePreset);
    nlohmann::json activeIds = nlohmann::json::array();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
        activeIds.push_back(id);
    loaded["activePresetIds"] = activeIds;
    loaded["sceneId"] = GetResolvedActiveSceneId();
    SendMessageToUI(loaded.dump());
}

void PluginController::SetlistBankDown(int steps)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SetlistBankChangeDirect(-steps);
    }
    else
    {
        std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
        mPendingSetlistBankDelta = mPendingSetlistBankDelta.value_or(0) - steps;
    }
}

void PluginController::SetlistBankChangeDirect(int delta)
{
    if (delta == 0)
        return;

    // A "bank" is a whole setlist. Bank up/down moves the active setlist to the
    // next/previous one in UI list order, clamped at the first/last setlist.
    auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    const int count = static_cast<int>(setlists.size());
    if (count == 0)
        return;

    // Resolve the current active setlist index (by id), defaulting to the first.
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    int currentIndex = 0;
    if (!activeSetlistId.empty())
    {
        for (int i = 0; i < count; ++i)
        {
            if (setlists[i].value("id", "") == activeSetlistId)
            {
                currentIndex = i;
                break;
            }
        }
    }

    const int newIndex = std::clamp(currentIndex + delta, 0, count - 1);
    if (newIndex == currentIndex)
        return;

    const std::string newId = setlists[newIndex].value("id", "");

    // Switch the active setlist ("bank"). Reset the preset cursor to the first
    // slot. No preset is loaded on a bank change — preset selection is a
    // separate MIDI action.
    mSetlistCursorIndex = 0;

    setlistsData["activeSetlistId"] = newId;
    setlistsData["cursorIndex"] = 0;
    SaveUiStorageJson("setlists.json", setlistsData);

    // Notify the UI so the setlist (bank) list highlights the new active setlist
    // and shows its slots.
    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = newId;
    msg["cursorIndex"] = 0;
    SendMessageToUI(msg.dump());
}

void PluginController::SelectSetlistBank(int bankNumber)
{
    if (mDSPMutex.try_lock())
    {
        mDSPMutex.unlock();
        SelectSetlistBankDirect(bankNumber);
    }
    else
    {
        std::lock_guard<std::mutex> lock(mPendingSetlistMutex);
        mPendingSetlistBankSelect = bankNumber;
    }
}

void PluginController::SelectSetlistBankDirect(int bankNumber)
{
    // Select the setlist whose `bank` number matches `bankNumber` and make it
    // the active setlist ("bank"). No-op (with a log) if no setlist claims it.
    auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
        return;

    std::string targetId;
    for (const auto& sl : setlists)
    {
        if (sl.contains("bank") && sl["bank"].is_number_integer()
            && sl["bank"].get<int>() == bankNumber)
        {
            targetId = sl.value("id", "");
            break;
        }
    }

    if (targetId.empty())
    {
        AppendSessionLog("[Automation] Select Bank " + std::to_string(bankNumber)
                         + ": no setlist mapped to this bank number");
        return;
    }

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    if (targetId == activeSetlistId)
        return;

    // Switch the active setlist. Reset the preset cursor to the first slot.
    // No preset is loaded — preset selection is a separate MIDI action.
    mSetlistCursorIndex = 0;

    setlistsData["activeSetlistId"] = targetId;
    setlistsData["cursorIndex"] = 0;
    SaveUiStorageJson("setlists.json", setlistsData);

    nlohmann::json msg;
    msg["type"] = "setlistCursorChanged";
    msg["activeSetlistId"] = targetId;
    msg["cursorIndex"] = 0;
    SendMessageToUI(msg.dump());
}

int PluginController::GetSetlistLength() const
{
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
        return 0;

    // Resolve the active setlist by activeSetlistId (first setlist as fallback)
    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            const auto& slots = sl.value("slots", nlohmann::json::array());
            return static_cast<int>(slots.size());
        }
    }
    return 0;
}

int PluginController::GetSetlistBankBase() const
{
    // A "bank" is a whole setlist, so MIDI preset slots 1..N map directly onto
    // the active setlist's slots starting at index 0.
    return 0;
}

int PluginController::GetSetlistBankNumber() const
{
    // Return the bank number of the active setlist, or 0 if none/unassigned.
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
        return 0;

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            if (sl.contains("bank") && sl["bank"].is_number_integer())
                return sl["bank"].get<int>();
            return 0;
        }
    }
    return 0;
}

std::string PluginController::GetSetlistSlotPresetId(int index) const
{
    if (index < 0)
        return "";

    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    const auto setlists = setlistsData.value("setlists", nlohmann::json::array());
    if (setlists.empty())
        return "";

    const std::string activeSetlistId = setlistsData.value("activeSetlistId", "");
    for (const auto& sl : setlists)
    {
        if (activeSetlistId.empty() || sl.value("id", "") == activeSetlistId)
        {
            const auto& slots = sl.value("slots", nlohmann::json::array());
            if (index >= static_cast<int>(slots.size()))
                return "";
            return slots[index].value("presetId", "");
        }
    }
    return "";
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
    msg["version"] = GUITARFX_APP_VERSION;
    
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

        // Build off the lock, install under it — see PrepareGlobalChainSwap().
        mPresetMixer.PrepareGlobalChainSwap(config);

        {
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
            mParamValues[kParamInputTrim] = config.inputGain;
            mParamValues[kParamOutputTrim] = config.outputGain;
            mParamValues[kParamTranspose] = static_cast<double>(GetGlobalTransposeFromChainConfig(config));
        }
        PersistGlobalFxSettingsToAppSettings();
    }
    SendGlobalChainStateToUI();
}

void PluginController::HandleSetNodeEnabledRequest(const nlohmann::json& payload)
{
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    std::string presetId = payload.value("presetId", fallbackId);
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    UpdateHostLatency();
}

void PluginController::HandleSetNodeParamRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", "");
    double value = payload.value("value", 0.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeParam(presetId, nodeId, key, value);
    UpdateHostLatency();
}

void PluginController::HandleLoadNodeResourceRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    ResourceRef ref;
    if (payload.contains("resourceType")) ref.resourceType = payload["resourceType"].get<std::string>();
    if (payload.contains("resourceId")) ref.resourceId = payload["resourceId"].get<std::string>();
    if (payload.contains("filePath")) ref.filePath = payload["filePath"].get<std::string>();
    const bool loaded = mPresetMixer.LoadNodeResource(presetId, nodeId, ref);
    if (!loaded && ReportHostedPluginResourceLoadFailure(nodeId, ref))
        DiscardFailedHostedPluginResourceSelection(nodeId, ref);
    else if (loaded)
        NotifyHostedPluginResourceLoadCompleted(nodeId, ref);
    UpdateHostLatency();
}

void PluginController::HandleSetTunerEnabledRequest(const nlohmann::json& payload)
{
    bool enabled = payload.value("enabled", false);
    mTunerActive.store(enabled, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerEnabled(enabled);
}

void PluginController::HandleSetTunerReferenceRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerReferenceFrequency(freq);
}

// ════════════════════════════════════════════════════════════════════
// Internal helpers
// ════════════════════════════════════════════════════════════════════

void PluginController::BroadcastState(StateScope scope)
{
    if (!mUIReady) return;

    const bool full = scope == StateScope::Full;

    nlohmann::json state;
    state["type"] = "state";

    // Current preset
    if (mActivePreset)
    {
        SyncActivePresetSceneGraph();
        state["preset"] = SerializePresetForUi(*mActivePreset);
        state["activePresetId"] = mActivePresetId;
        state["activeSceneId"] = GetResolvedActiveSceneId();
    }

    // App settings — UI reads "appSettings". Applying them re-runs the demo-audio, recents,
    // tone-sharing and update-check passes and echoes an input-mode message back to us, so
    // this is deliberately not resent on a preset switch.
    if (full)
        state["appSettings"] = mAppSettings;

    // Always sent: the UI clears its archive-session state when this key is absent.
    state["presetArchiveSession"] = {
        {"active", IsPresetArchiveSessionActive()}
    };
    if (mPresetArchiveSession)
    {
        state["presetArchiveSession"]["archiveName"] = mPresetArchiveSession->archiveName;
        state["presetArchiveSession"]["archiveKey"] = mPresetArchiveSession->archiveKey;
        state["presetArchiveSession"]["presetCount"] = mPresetArchiveSession->presetCount;
    }

    // UI settings — UI reads "uiSettings"
    if (full)
        state["uiSettings"] = mUiSettings;

    // UI view state — UI reads "uiViewState"
    if (full)
        state["uiViewState"] = mUiViewState;

    // Global chain — UI reads "globalSignalChain". Always sent: the UI asks for it with a
    // round trip when the key is missing, which would cost more than including it.
    auto chainConfig = mPresetMixer.GetGlobalChainConfig();
    state["globalSignalChain"] = chainConfig;

    // Resource library summary + per-type entries for UI rendering. By far the largest
    // section, and it stats every file on disk to fill in "fileMissing", so it is only
    // built for a full broadcast.
    if (full)
    {
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
            entry["tags"] = resource.tags;
            entry["filePath"] = resource.filePath.empty() ? "" : util::PathToUtf8(resource.filePath);
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

        state["resourceLibrary"] = std::move(libraryInfo);
    }

    // Active preset IDs — UI reads "activePresetIds" as string array
    nlohmann::json activePresetIds = nlohmann::json::array();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
        activePresetIds.push_back(id);
    state["activePresetIds"] = activePresetIds;

    // Mixer snapshot
    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();
    mixer["multiThreaded"] = mPresetMixer.IsMultiThreadedProcessingEnabled();
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

    // Full preset graphs so the UI can display the signal chain for every mixer slot.
    nlohmann::json presetGraphs = nlohmann::json::object();
    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        auto it = mMixerPresetJsonCache.find(id);
        if (it != mMixerPresetJsonCache.end())
        {
            try { presetGraphs[id] = nlohmann::json::parse(it->second); }
            catch (...) {}
        }
    }
    mixer["presetGraphs"] = std::move(presetGraphs);
    state["mixer"] = std::move(mixer);

    // Everything below is app-wide state that a preset or scene switch cannot change.
    if (full)
    {
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
        state["metronome"] = std::move(metronome);

        // Environment
        state["environment"] = {
        {"standalone", mHost.IsStandalone()},
        {"version", GUITARFX_APP_VERSION},
    #if defined(_WIN32)
        {"os", "Windows"},
    #elif defined(__APPLE__)
        {"os", "macOS"},
    #elif defined(__linux__)
        {"os", "Linux"},
    #else
        {"os", "Unknown"},
    #endif
    #if defined(__x86_64__) || defined(_M_X64)
        {"cpu", "x64"},
    #elif defined(__aarch64__) || defined(_M_ARM64)
        {"cpu", "arm64"},
    #else
        {"cpu", "Unknown"},
    #endif
        };

        // Blend library
        state["blendLibrary"] = mBlendLibrary;

        // Saved custom effects library
        {
            nlohmann::json customEffects = nlohmann::json::array();
            for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
                customEffects.push_back(SerializeCustomEffectLibraryEntry(entry));
            state["customEffectLibrary"] = std::move(customEffects);
        }

        // Riff library
        {
            std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
            state["riffLibrary"] = mRiffLibraryIndex;
        }

        // Automation slots
        state["automation"] = mAutomationSlots.GetSlotsJson();
    }

    SendMessageToUI(state.dump());

    // Also send supplementary data. Both are static libraries, not preset state.
    if (full)
    {
        SendCompositeLibraryToUI();
        SendEffectCatalogToUI();
    }

    // Notify the host of any latency change now that the graph is settled.
    UpdateHostLatency();
}

void PluginController::UpdateHostLatency()
{
    const int latency = mPresetMixer.GetTotalLatencySamples();
    if (latency == mLastReportedLatency)
        return;
    mLastReportedLatency = latency;
    mHost.NotifyLatencyChanged(latency);
}

void PluginController::AttachRuntimeConfigCallbacks(const std::string& presetId, const Preset& preset)
{
    if (presetId.empty())
        return;

    for (const auto& node : preset.graph.nodes)
    {
        if (auto* processor = mPresetMixer.GetNodeProcessor(presetId, node.id))
        {
            processor->SetRuntimeConfigChangedCallback(
                [this, presetId, nodeId = node.id](const std::string& key, const std::string& value)
                {
                    mHost.RunOnMainThread([this, presetId, nodeId, key, value]()
                    {
                        HandleRuntimeNodeConfigChanged(presetId, nodeId, key, value);
                    });
                });
        }
    }
}

bool PluginController::ReportHostedPluginResourceLoadFailure(const std::string& nodeId,
                                                             const ResourceRef& ref,
                                                             int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin" || !mActivePreset)
        return false;

    const std::string presetId = !mActivePresetId.empty() ? mActivePresetId : mActivePreset->id;
    if (presetId.empty())
        return false;

    auto* processor = mPresetMixer.GetNodeProcessor(presetId, nodeId);
    if (!processor)
        return false;

    const std::string lastError = processor->GetConfig("lastError");
    if (lastError.empty())
        return false;
    const std::string lastErrorCode = processor->GetConfig(kHostedPluginLastErrorCodeConfigKey);

    nlohmann::json message{
        {"type", "hostedPluginResourceLoadFailed"},
        {"nodeId", nodeId},
        {"resourceType", "plugin"},
        {"message", lastError}
    };
    if (!lastErrorCode.empty())
        message["errorCode"] = lastErrorCode;
    if (resourceIndex >= 0)
        message["resourceIndex"] = resourceIndex;
    if (!ref.resourceId.empty())
        message["resourceId"] = ref.resourceId;
    if (!ref.filePath.empty())
        message["filePath"] = util::PathToUtf8(ref.filePath);
    else if (auto resolvedPath = ResolveResourceRef(ref))
        message["filePath"] = util::PathToUtf8(*resolvedPath);

    SendMessageToUI(message.dump());
    return true;
}

void PluginController::NotifyHostedPluginResourceLoadCompleted(const std::string& nodeId,
                                                               const ResourceRef& ref,
                                                               int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin")
        return;

    nlohmann::json message{
        {"type", "hostedPluginResourceLoadCompleted"},
        {"nodeId", nodeId},
        {"resourceType", "plugin"}
    };
    if (resourceIndex >= 0)
        message["resourceIndex"] = resourceIndex;
    if (!ref.resourceId.empty())
        message["resourceId"] = ref.resourceId;

    SendMessageToUI(message.dump());
}

void PluginController::DiscardFailedHostedPluginResourceSelection(const std::string& nodeId,
                                                                  const ResourceRef& ref,
                                                                  int resourceIndex)
{
    if (nodeId.empty() || ref.resourceType != "plugin")
        return;

    if (!ref.resourceId.empty())
    {
        if (auto resource = mResourceLibrary.LookupResource("plugin", ref.resourceId))
        {
            const auto providerIt = resource->metadata.find("provider");
            const bool isLocalResource = providerIt != resource->metadata.end()
                && providerIt->second == kLocalResourceProvider;
            mResourceLibrary.RemoveResource("plugin", ref.resourceId);
            if (isLocalResource)
            {
                const auto libraryFile = ResolveResourceLibraryIndexPath();
                [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryFile.parent_path());
                mResourceLibrary.SaveToFile(libraryFile);
            }
        }
    }

    auto* targetGraph = ResolveEditTarget();
    auto* target = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!target)
        return;

    const auto clearResourceSlot = [](ResourceRef& slot)
    {
        slot = ResourceRef{};
        slot.resourceType = "plugin";
    };

    if (resourceIndex >= 0)
    {
        if (static_cast<std::size_t>(resourceIndex) < target->resources.size())
            clearResourceSlot(target->resources[static_cast<std::size_t>(resourceIndex)]);
    }
    else
    {
        for (auto& slot : target->resources)
        {
            if (slot.resourceType == "plugin")
            {
                clearResourceSlot(slot);
                break;
            }
        }
    }

    if (IsCompositeEditMode())
    {
        BroadcastCompositeEditState();
        return;
    }

    if (mActivePreset)
    {
        SyncActivePresetSceneGraph();
        ApplyPreset(*mActivePreset);
        mPendingStateBroadcast = true;
    }
}

void PluginController::HandleRuntimeNodeConfigChanged(const std::string& presetId,
                                                      const std::string& nodeId,
                                                      const std::string& key,
                                                      const std::string& value)
{
    if (presetId.empty() || nodeId.empty() || key.empty() || !mActivePreset || IsCompositeEditMode())
        return;

    if (!mActivePresetId.empty() && presetId != mActivePresetId)
        return;

    auto* targetGraph = ResolveEditTarget();
    auto* node = targetGraph ? targetGraph->FindNode(nodeId) : nullptr;
    if (!node)
        node = mActivePreset->graph.FindNode(nodeId);
    if (!node)
        return;

    const auto existingIt = node->config.find(key);
    if ((value.empty() && existingIt == node->config.end())
        || (existingIt != node->config.end() && existingIt->second == value))
    {
        return;
    }

    if (value.empty())
        node->config.erase(key);
    else
        node->config[key] = value;

    PersistHostedPluginResourceMetadata(*node, key, value);

    SyncActivePresetSceneGraph();
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mMixerPresetJsonCache[presetId] = mActivePresetJson;

    if (key == "pluginStateBase64")
    {
        SendMessageToUI(nlohmann::json{
            {"type", "signalPathNodeConfigUpdated"},
            {"presetId", presetId},
            {"nodeId", nodeId},
            {"key", key},
            {"captured", true},
            {"valueLength", value.size()},
            {"persist", true},
            {"dirty", true},
            {"silent", true}
        }.dump());
    }

    mPendingStateBroadcast = true;
    mHost.NotifyStateChanged();
}

void PluginController::ApplyPreset(const Preset& preset)
{
    // === Phase 1: Normalize and prepare preset data — no DSP lock needed. ===
    // All work here modifies local copies only; the audio thread is unaffected.
    Preset normalizedPreset = preset;
    NormalizePresetScenes(normalizedPreset);
    std::string resolvedSceneId = mActiveSceneId;
    if (!SetPresetActiveScene(normalizedPreset, resolvedSceneId, &resolvedSceneId))
        resolvedSceneId = GetDefaultPresetSceneId(normalizedPreset);

    for (auto& node : normalizedPreset.graph.nodes)
    {
        RefreshWasmNodeDescriptor(node);
    }

    for (auto& node : normalizedPreset.graph.nodes)
    {
        if (!IsNamEffectType(node.type))
            continue;

        // Strip legacy NAM level params; replaced by single useCalibration toggle.
        node.params.erase("autoLevelInput");
        node.params.erase("autoLevelOutput");
        node.params.erase("useNamInputMetadata");
        node.params.erase("clampAutoGain");
        node.params.erase("useAutoLevel");
        ClearNamCalibrationParams(node);
        if (!node.params.contains("useCalibration"))
            node.params["useCalibration"] = 1.0;
    }

    // Hydrate blend node resources (model refs + blendMode) from the blend library.
    // This must run before the executor is built so that CreateProcessors -> LoadResources
    // can resolve NAM model paths for MultiModelNAMAmpEffect. Without it, blend nodes added
    // or replaced via the signal path UI have empty resources, leaving the effect in
    // passthrough mode (no models loaded -> input/output gain and blend selection have no effect).
    ApplyBlendDefinitions(normalizedPreset);

    TryRemapHostedPluginResources(normalizedPreset);
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    // Global settings (gate, transpose, EQ, doubler, limiter) must never come from presets.
    // They are per-instance state and come from app settings (standalone) or host state (plugin).
    // Preserve current global FX state when loading a preset—ignore any preset-level overrides.
    auto chainConfig = mPresetMixer.GetGlobalChainConfig();
    const double inputGainDb = chainConfig.inputGain;
    const double outputGainDb = chainConfig.outputGain;

    chainConfig.inputGain = inputGainDb;
    chainConfig.outputGain = outputGainDb;
    chainConfig.autoLevelInput = false;
    chainConfig.autoLevelOutput = false;
    if (mHost.IsStandalone())
    {
        constexpr auto kMonoModeKey  = "inputChannel.monoMode";
        constexpr auto kInputChanKey = "inputChannel.mono";

        const auto monoIt = mAppSettings.find(kMonoModeKey);
        const auto chanIt = mAppSettings.find(kInputChanKey);

        chainConfig.monoMode = (monoIt != mAppSettings.end() && monoIt->is_boolean())
            ? monoIt->get<bool>()
            : true;
        chainConfig.inputChannel = (chanIt != mAppSettings.end() && chanIt->is_number_integer())
            ? std::clamp(chanIt->get<int>(), 0, 1)
            : 0;
    }

    normalizedPreset.global.inputTrim = inputGainDb;
    normalizedPreset.global.outputTrim = outputGainDb;
    normalizedPreset.global.autoLevelInput = false;
    normalizedPreset.global.autoLevelOutput = false;
    normalizedPreset.globalSignalChain = chainConfig;

    const std::string initialSlotId = normalizedPreset.id.empty() ? "p1" : normalizedPreset.id;
    const std::string newPresetJson = PresetStorage::SerializeToJson(normalizedPreset);

    // === Phase 2: Build the new executors off the DSP lock. ===
    // This is the expensive step: effect processors are created, resources loaded
    // (e.g. NAM model weights read from disk), and Prepare() called on each node.
    // The audio thread continues processing the current preset uninterrupted.
    mPresetMixer.PreparePresetSwap(normalizedPreset, initialSlotId, normalizedPreset.name);

    // Global chains are staged the same way. This is almost always a no-op: global FX are
    // per-instance state that never comes from a preset, so chainConfig normally matches the
    // running config exactly and no rebuild is staged at all.
    mPresetMixer.PrepareGlobalChainSwap(chainConfig);

    // === Phase 3: Atomic swap under the DSP lock (fast). ===
    // The lock is held only for lightweight state updates and the instance swap.
    // No allocations or I/O occur inside this block.
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        mActiveSceneId = resolvedSceneId;
        mParamValues[kParamInputTrim] = inputGainDb;
        mParamValues[kParamOutputTrim] = outputGainDb;
        mParamValues[kParamTranspose] = static_cast<double>(GetGlobalTransposeFromChainConfig(chainConfig));

        // Install the global chains staged above. Construction already happened off the
        // lock; this is a pointer-level swap plus the scalar input/output settings.
        mPresetMixer.CommitGlobalChainSwap();
        mPresetMixer.SetAutoLevelInput(false);
        mPresetMixer.SetAutoLevelOutput(false);

        mActivePreset = normalizedPreset;
        mActivePresetJson = newPresetJson;

        // Use the real preset ID so the UI can map the mixer tab to the presetCache entry.
        // Fall back to "p1" only for presets without an id (should not happen in practice).
        mMixerPresetJsonCache.clear();
        mPresetMixer.CommitPresetSwap(); // Fast: swap mPendingInstance into mInstances + schedule fade-in
        mMixerPresetJsonCache[initialSlotId] = mActivePresetJson;
        AttachRuntimeConfigCallbacks(initialSlotId, normalizedPreset);

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

        // Apply global interface calibration level to all calibratable NAM
        // effect nodes (overrides preset params; calibrationInputLevel is not
        // stored in preset data). We inject even when a model is not currently
        // resolved so the value is already present once the model loads.
        const bool hasCalibrationValue = std::isfinite(mNamInterfaceCalibrationLevelDbu);
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        for (const auto& node : normalizedPreset.graph.nodes)
        {
            if (!IsNamCalibratableEffectType(node.type))
                continue;

            const double calibrationToInject =
                hasCalibrationValue ? mNamInterfaceCalibrationLevelDbu : clearValue;
            mPresetMixer.SetNodeParam(initialSlotId, node.id, "calibrationInputLevel", calibrationToInject);
        }
    }

    mHost.NotifyStateChanged();
}

void PluginController::TryRemapHostedPluginResources(Preset& preset) const
{
    TryRemapHostedPluginResourcesInGraph(preset.graph);
    for (auto& scene : preset.scenes)
        TryRemapHostedPluginResourcesInGraph(scene.graph);
}

void PluginController::TryRemapHostedPluginResourcesInGraph(SignalGraph& graph) const
{
    const auto pluginResources = mResourceLibrary.GetResourcesByType("plugin");
    if (pluginResources.empty())
        return;

    for (auto& node : graph.nodes)
    {
        if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
            continue;

        auto resourceIt = std::find_if(node.resources.begin(), node.resources.end(), [](const ResourceRef& ref)
        {
            return ref.resourceType == "plugin" && ref.IsLibraryRef();
        });

        if (resourceIt == node.resources.end())
            continue;
        if (mResourceLibrary.HasResource("plugin", resourceIt->resourceId))
            continue;

        const auto stableConfigIt = node.config.find(kHostedPluginStableIdConfigKey);
        const auto identifierConfigIt = node.config.find(kHostedPluginIdentifierConfigKey);
        const auto nameConfigIt = node.config.find(kHostedPluginNameConfigKey);
        const auto manufacturerConfigIt = node.config.find(kHostedPluginManufacturerConfigKey);
        const auto formatConfigIt = node.config.find(kHostedPluginFormatConfigKey);

        const std::string normalizedStableId =
            stableConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(stableConfigIt->second) : std::string{};
        const std::string normalizedIdentifier =
            identifierConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(identifierConfigIt->second) : std::string{};
        const std::string normalizedName =
            nameConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(nameConfigIt->second) : std::string{};
        const std::string normalizedManufacturer =
            manufacturerConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(manufacturerConfigIt->second) : std::string{};
        const std::string normalizedFormat =
            formatConfigIt != node.config.end() ? NormalizeHostedPluginIdentityToken(formatConfigIt->second) : std::string{};

        if (normalizedStableId.empty() && normalizedIdentifier.empty() && normalizedName.empty())
            continue;

        std::vector<const LibraryResource*> candidates;
        candidates.reserve(pluginResources.size());

        for (const auto& libraryResource : pluginResources)
        {
            const auto& metadata = libraryResource.metadata;
            const std::string candidateStableId = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginStableIdConfigKey) ? metadata.at(kHostedPluginStableIdConfigKey) : std::string{});
            const std::string candidateIdentifier = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginIdentifierConfigKey) ? metadata.at(kHostedPluginIdentifierConfigKey) : std::string{});
            const std::string candidateName = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginNameConfigKey) ? metadata.at(kHostedPluginNameConfigKey) : libraryResource.name);
            const std::string candidateManufacturer = NormalizeHostedPluginIdentityToken(
                metadata.contains(kHostedPluginManufacturerConfigKey) ? metadata.at(kHostedPluginManufacturerConfigKey) : std::string{});

            bool match = false;
            if (!normalizedStableId.empty() && !candidateStableId.empty() && normalizedStableId == candidateStableId)
                match = true;
            else if (!normalizedIdentifier.empty() && !candidateIdentifier.empty() && normalizedIdentifier == candidateIdentifier)
                match = true;
            else if (!normalizedName.empty() && normalizedName == candidateName)
            {
                if (normalizedManufacturer.empty() || candidateManufacturer.empty() || normalizedManufacturer == candidateManufacturer)
                    match = true;
            }

            if (match)
                candidates.push_back(&libraryResource);
        }

        if (candidates.empty())
            continue;

        const LibraryResource* selected = nullptr;
        if (candidates.size() == 1)
        {
            selected = candidates.front();
        }
        else if (!normalizedFormat.empty())
        {
            for (const auto* candidate : candidates)
            {
                const auto formatIt = candidate->metadata.find(kHostedPluginFormatConfigKey);
                if (formatIt != candidate->metadata.end()
                    && NormalizeHostedPluginIdentityToken(formatIt->second) == normalizedFormat)
                {
                    selected = candidate;
                    break;
                }
            }
        }

        if (!selected)
            continue;

        AppendSessionLog("Hosted plugin resource remapped nodeId=" + node.id
            + ", missingId=" + resourceIt->resourceId
            + ", resolvedId=" + selected->id);
        resourceIt->resourceId = selected->id;
        resourceIt->filePath.clear();
    }
}

void PluginController::PersistHostedPluginResourceMetadata(const GraphNode& node,
                                                           const std::string& key,
                                                           const std::string& value)
{
    if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
        return;

    if (key != kHostedPluginStableIdConfigKey
        && key != kHostedPluginIdentifierConfigKey
        && key != kHostedPluginNameConfigKey
        && key != kHostedPluginManufacturerConfigKey
        && key != kHostedPluginFormatConfigKey)
    {
        return;
    }

    const auto resourceIt = std::find_if(node.resources.begin(), node.resources.end(), [](const ResourceRef& ref)
    {
        return ref.resourceType == "plugin" && ref.IsLibraryRef();
    });
    if (resourceIt == node.resources.end())
        return;

    auto resource = mResourceLibrary.LookupResource("plugin", resourceIt->resourceId);
    if (!resource)
        return;

    auto updated = *resource;
    const auto existingIt = updated.metadata.find(key);
    if (value.empty())
    {
        if (existingIt == updated.metadata.end())
            return;
        updated.metadata.erase(existingIt);
    }
    else
    {
        if (existingIt != updated.metadata.end() && existingIt->second == value)
            return;
        updated.metadata[key] = value;
    }

    mResourceLibrary.UpdateResource("plugin", updated.id, updated);
    const auto libraryFile = ResolveResourceLibraryIndexPath();
    [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryFile.parent_path());
    mResourceLibrary.SaveToFile(libraryFile);
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
        if (node.type != EffectGuids::kAmpNamBlend) continue;

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

void PluginController::CaptureRuntimePluginStates(Preset& preset, const std::string& presetId) const
{
    const auto findExistingState = [&](const std::string& nodeId) -> std::string
    {
        if (!mActivePreset)
            return {};

        const auto findInGraph = [&](const SignalGraph& graph) -> std::string
        {
            if (const auto* existingNode = graph.FindNode(nodeId))
            {
                const auto it = existingNode->config.find("pluginStateBase64");
                if (it != existingNode->config.end())
                    return it->second;
            }
            return {};
        };

        if (const auto state = findInGraph(mActivePreset->graph); !state.empty())
            return state;
        for (const auto& scene : mActivePreset->scenes)
        {
            if (const auto state = findInGraph(scene.graph); !state.empty())
                return state;
        }
        return {};
    };

    const auto captureRuntimeState = [&](const std::string& nodeId) -> std::string
    {
        if (!presetId.empty())
        {
            if (const auto state = mPresetMixer.GetNodeConfig(presetId, nodeId, "pluginStateBase64"); !state.empty())
                return state;
        }

        if (!mActivePresetId.empty() && mActivePresetId != presetId)
        {
            if (const auto state = mPresetMixer.GetNodeConfig(mActivePresetId, nodeId, "pluginStateBase64"); !state.empty())
                return state;
        }

        for (const auto& activeId : mPresetMixer.GetActivePresetIds())
        {
            if (activeId == presetId || activeId == mActivePresetId)
                continue;

            if (const auto state = mPresetMixer.GetNodeConfig(activeId, nodeId, "pluginStateBase64"); !state.empty())
                return state;
        }

        return {};
    };

    const auto captureGraph = [&](SignalGraph& graph)
    {
        for (auto& node : graph.nodes)
        {
            if (EffectRegistry::Instance().Resolve(node.type) != EffectGuids::kPluginHost)
                continue;

            node.config.erase("pluginStateBase64Length");

            std::string state = captureRuntimeState(node.id);
            std::string source = "runtime";
            if (state.empty())
            {
                state = findExistingState(node.id);
                source = "existing";
            }
            if (!state.empty())
            {
                node.config["pluginStateBase64"] = state;
                AppendSessionLog("Hosted plugin runtime state selected presetId="
                    + (presetId.empty() ? std::string{"<none>"} : presetId)
                    + ", nodeId=" + node.id + ", source=" + source
                    + ", length=" + std::to_string(state.size())
                    + ", hash=" + HashStringForLog(state));
            }
            else
            {
                node.config.erase("pluginStateBase64");
                AppendSessionLog("Hosted plugin state capture unavailable for node " + node.id + " while saving preset " + preset.id);
            }
        }
    };

    captureGraph(preset.graph);
    for (auto& scene : preset.scenes)
        captureGraph(scene.graph);
}

bool PluginController::ApplyNodeParameter(const GraphNode& node, const std::string& paramKey, double value)
{
    // In the shared core we don't have direct access to framework-level parameter
    // objects. All DSP parameter
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
    {
        NormalizePresetScenes(*mActivePreset);
        if (auto* scene = FindPresetScene(*mActivePreset, GetResolvedActiveSceneId()))
            return &scene->graph;
        return &mActivePreset->graph;
    }
    return nullptr;
}

std::string PluginController::GetResolvedActiveSceneId() const
{
    if (mActivePreset)
    {
        if (FindPresetScene(*mActivePreset, mActiveSceneId))
            return mActiveSceneId;
        return GetDefaultPresetSceneId(*mActivePreset);
    }
    return mActiveSceneId;
}

void PluginController::SyncActivePresetSceneGraph()
{
    if (!mActivePreset)
        return;

    NormalizePresetScenes(*mActivePreset);
    const std::string resolvedSceneId = GetResolvedActiveSceneId();
    if (auto* scene = FindPresetScene(*mActivePreset, resolvedSceneId))
    {
        EnsurePresetBoundaryGainNodes(scene->graph);
        mActivePreset->graph = scene->graph;
        EnsurePresetBoundaryGainNodes(mActivePreset->graph);
        mActiveSceneId = resolvedSceneId;
        return;
    }

    (void)SetPresetActiveScene(*mActivePreset, resolvedSceneId, &mActiveSceneId);
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

            const auto normalizePath = [](const std::filesystem::path& value) {
                std::string normalized = value.lexically_normal().generic_string();
                std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return normalized;
            };
            const auto normalizedFilePath = normalizePath(filePath);
            for (const auto& resource : mResourceLibrary.GetResourcesByType(resourceType))
            {
                if (normalizePath(resource.filePath) == normalizedFilePath)
                {
                    ref.resourceId = resource.id;
                    ref.filePath.clear();
                    break;
                }
            }

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

void PluginController::RefreshWasmNodeDescriptor(GraphNode& node)
{
#if !defined(GUITARFX_ENABLE_WASM_EFFECTS)
    (void)node;
    return;
#else
    auto& registry = EffectRegistry::Instance();
    if (registry.Resolve(node.type) != EffectGuids::kWasmHost)
        return;

    std::optional<WasmModuleDescriptor> previousDescriptor;
    if (const auto existingDescriptorIt = node.config.find(WasmEffect::kDescriptorConfigKey);
        existingDescriptorIt != node.config.end())
    {
        std::string parseError;
        previousDescriptor = WasmEffect::ParseDescriptorConfig(existingDescriptorIt->second, &parseError);
        if (!previousDescriptor && !parseError.empty())
            AppendSessionLog("WASM descriptor cache parse failed for node " + node.id + ": " + parseError);
    }

    const auto typeInfo = registry.GetTypeInfo(EffectGuids::kWasmHost);
    const auto labelIsDefault = [&]() {
        return node.label.empty()
            || (typeInfo && node.label == typeInfo->displayName)
            || (previousDescriptor && !previousDescriptor->displayName.empty() && node.label == previousDescriptor->displayName);
    };
    const auto categoryIsDefault = [&]() {
        return node.category.empty()
            || (typeInfo && node.category == typeInfo->category)
            || (previousDescriptor && !previousDescriptor->category.empty() && node.category == previousDescriptor->category);
    };

    const auto clearDescriptorState = [&]() {
        if (previousDescriptor)
        {
            for (const auto& oldParam : previousDescriptor->parameters)
                node.params.erase(oldParam.definition.id);
        }

        node.config.erase(WasmEffect::kDescriptorConfigKey);
        if (typeInfo && labelIsDefault())
            node.label = typeInfo->displayName;
        if (typeInfo && categoryIsDefault())
            node.category = typeInfo->category;
    };

    if (node.resources.empty() || !node.resources.front().IsValid())
    {
        clearDescriptorState();
        return;
    }

    const auto modulePath = ResolveResourceRef(node.resources.front());
    if (!modulePath)
        return;

    std::string readError;
    const auto descriptor = WasmEffect::InspectModuleFile(*modulePath, &readError);
    if (!descriptor)
    {
        clearDescriptorState();
        if (!readError.empty())
            AppendSessionLog("WASM descriptor read failed for node " + node.id + ": " + readError);
        return;
    }

    if (descriptor->entries.empty())
    {
        clearDescriptorState();
        return;
    }

    node.config[WasmEffect::kDescriptorConfigKey] = WasmEffect::SerializeDescriptorConfig(descriptor->entries);
    if (labelIsDefault() && !descriptor->displayName.empty())
        node.label = descriptor->displayName;
    if (categoryIsDefault() && !descriptor->category.empty())
        node.category = descriptor->category;

    std::unordered_set<std::string> currentGuestParamIds;
    for (const auto& guestParam : descriptor->parameters)
    {
        currentGuestParamIds.insert(guestParam.definition.id);
        if (node.params.count(guestParam.definition.id) > 0)
            continue;

        double initialValue = guestParam.definition.defaultValue;
        if (!previousDescriptor.has_value())
        {
            const std::string legacyParamKey = "param" + std::to_string(guestParam.slot + 1);
            if (const auto legacyIt = node.params.find(legacyParamKey); legacyIt != node.params.end())
                initialValue = legacyIt->second;
        }

        node.params[guestParam.definition.id] = initialValue;
    }

    if (previousDescriptor)
    {
        for (const auto& oldParam : previousDescriptor->parameters)
        {
            if (currentGuestParamIds.count(oldParam.definition.id) == 0)
                node.params.erase(oldParam.definition.id);
        }
    }
#endif
}

std::optional<std::filesystem::path> PluginController::ResolveResourceRef(const ResourceRef& ref) const
{
    if (auto resolved = mResourceLibrary.ResolveResource(ref))
        return resolved;
    if (!ref.filePath.empty())
        return ref.filePath;
    return std::nullopt;
}

void PluginController::AppendUserLibraryResource(const LibraryResource& resource)
{
    mResourceLibrary.AddResource(resource);

    const auto libraryFile = ResolveResourceLibraryIndexPath();
    [[maybe_unused]] const auto ensuredLibraryDir = mFileSystem.EnsureDirectory(libraryFile.parent_path());
    mResourceLibrary.SaveToFile(libraryFile);
}

void PluginController::RemoveUserLibraryResource(const std::string& type, const std::string& id)
{
    mResourceLibrary.RemoveResource(type, id);

    const auto libraryFile = ResolveResourceLibraryIndexPath();
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

// ── NAM level-state normalization ─────────────────────────────────

void PluginController::ResetNamNodeLevelState(const std::string& nodeId)
{
    if (nodeId.empty() || !mActivePreset) return;

    auto* node = mActivePreset->graph.FindNode(nodeId);
    if (!node || !IsNamEffectType(node->type)) return;

    ClearNamCalibrationParams(*node);
    if (!node->params.contains("useCalibration"))
        node->params["useCalibration"] = 1.0;
    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mPendingStateBroadcast = true;

    if (!mActivePresetId.empty())
    {
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        const auto useCalibrationIt = node->params.find("useCalibration");
        const double useCalibrationValue =
            (useCalibrationIt != node->params.end() && useCalibrationIt->second <= 0.5) ? 0.0 : 1.0;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "useCalibration", useCalibrationValue);

        // Re-inject current interface calibration level for this NAM node.
        // Keep this value resident even if no model is currently resolved so
        // calibration takes effect immediately when a model loads.
        const double calLevelToInject =
            std::isfinite(mNamInterfaceCalibrationLevelDbu)
                ? mNamInterfaceCalibrationLevelDbu
                : clearValue;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "calibrationInputLevel", calLevelToInject);
    }
}

void PluginController::ClearNamCalibrationParams(GraphNode& node) const
{
    node.params.erase("calibrationInputLevel");
    node.params.erase("calibrationOutputLevel");
}

// ── Settings persistence ───────────────────────────────────────────

void PluginController::TouchSharedSyncState(const std::vector<std::string>& domains) const
{
    if (domains.empty())
        return;

    const auto syncPath = ResolveSharedSyncStatePath(mFileSystem);
    if (syncPath.empty())
        return;

    std::uint64_t nextVersion = 1;
    nlohmann::json previous = LoadJsonFile(syncPath, nlohmann::json::object());
    if (previous.is_object())
    {
        const auto versionIt = previous.find("version");
        if (versionIt != previous.end() && versionIt->is_number_unsigned())
            nextVersion = versionIt->get<std::uint64_t>() + 1;
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["version"] = nextVersion;
    payload["updatedAt"] = BuildUtcIsoTimestamp();
    payload["domains"] = nlohmann::json::array();
    for (const auto& domain : domains)
    {
        if (domain.empty())
            continue;
        payload["domains"].push_back(domain);
    }

    const auto instanceIdIt = mAppSettings.find("app.instanceId");
    if (instanceIdIt != mAppSettings.end() && instanceIdIt->is_string())
        payload["writerInstanceId"] = instanceIdIt->get<std::string>();

    try
    {
        [[maybe_unused]] const auto ensuredSyncParent = mFileSystem.EnsureDirectory(syncPath.parent_path());

        const auto tempPath = syncPath.parent_path() / (syncPath.filename().string() + ".tmp");
        {
            std::ofstream ofs(tempPath);
            if (!ofs.is_open())
                return;
            ofs << payload.dump(2);
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, syncPath, ec);
        if (ec)
        {
            std::filesystem::copy_file(tempPath, syncPath,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tempPath, ec);
        }
        if (ec)
            return;

        mSharedSyncVersionSeen = nextVersion;
        mSharedSyncVersionSeenInitialized = true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] TouchSharedSyncState failed: " << e.what() << std::endl;
    }
}

void PluginController::SaveAppSettings() const
{
    const auto settingsPath = mFileSystem.ResolveSettingsFile();
    if (settingsPath.empty()) return;

    bool wrote = false;
    try
    {
        [[maybe_unused]] const auto ensuredSettingsParent = mFileSystem.EnsureDirectory(settingsPath.parent_path());

        // Write to a temp file first, then atomically rename over the real file.
        // This prevents a partial write (crash, exception, lock) from truncating
        // app.json and losing the instanceId or other persistent settings.
        const auto tempPath = settingsPath.parent_path() / (settingsPath.filename().string() + ".tmp");
        {
            std::ofstream ofs(tempPath);
            if (!ofs.is_open())
            {
                std::cerr << "[Plugin] SaveAppSettings: could not open temp file " << tempPath.string() << std::endl;
                return;
            }
            ofs << mAppSettings.dump(2);
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, settingsPath, ec);
        if (ec)
        {
            // rename failed (e.g. cross-device) – fall back to copy+delete
            std::filesystem::copy_file(tempPath, settingsPath,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tempPath, ec);
        }
        wrote = !ec;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] SaveAppSettings failed: " << e.what() << std::endl;
    }

    if (wrote)
        TouchSharedSyncState({"appSettings"});
}

bool PluginController::CleanupLegacyAppSettingsOnLoad()
{
    if (!mAppSettings.is_object())
        return false;

    bool settingsChanged = false;
    const auto eraseKeyIfPresent = [&](const char* key)
    {
        if (mAppSettings.erase(key) > 0)
            settingsChanged = true;
    };

    // Legacy/dead keys no longer read by startup/runtime paths.
    eraseKeyIfPresent("appSettings");
    eraseKeyIfPresent("audioSettings");
    eraseKeyIfPresent("lastPresetJson");
    eraseKeyIfPresent("parameters");
    eraseKeyIfPresent("metronomeEnabled");
    eraseKeyIfPresent("performancePads.open");
    eraseKeyIfPresent("toneSharing.apiBase");
    eraseKeyIfPresent("ui.experimentalFeaturesEnabled");
    eraseKeyIfPresent("audio.processing.namMonoOnly");
    eraseKeyIfPresent("app.lastUpdateCheck");

    // Legacy global chain app setting was superseded by globalFx.settings.
    if (mAppSettings.contains(kGlobalFxSettingsKey))
        eraseKeyIfPresent("globalSignalChain");

    // Prune legacy metronome aliases after canonical keys are present.
    if (mAppSettings.contains(kMetronomeBpmSettingKey))
        eraseKeyIfPresent(kMetronomeLegacyBpmKey);
    if (mAppSettings.contains(kMetronomeVolumeDbSettingKey))
        eraseKeyIfPresent(kMetronomeLegacyVolumeDbKey);
    if (mAppSettings.contains(kMetronomePanSettingKey))
        eraseKeyIfPresent(kMetronomeLegacyPanKey);
    if (mAppSettings.contains(kMetronomeClickTypeSettingKey))
        eraseKeyIfPresent(kMetronomeLegacyClickTypeKey);

    return settingsChanged;
}

void PluginController::LoadAppSettings()
{
    const auto settingsPath = mFileSystem.ResolveSettingsFile();

    if (settingsPath.empty())
    {
        std::cerr << "[Plugin] Settings file path is empty" << std::endl;
        return;
    }

    const auto applyBundledDefaults = [this]()
    {
        if (!mAppSettings.is_object())
            mAppSettings = nlohmann::json::object();

        if (std::strlen(kBundledJamYouTubeApiKey) > 0)
            mAppSettings[kJamYouTubeApiKeySettingKey] = std::string{kBundledJamYouTubeApiKey};
    };

    if (!std::filesystem::exists(settingsPath))
    {
        std::cout << "[Plugin] No settings file found at " << settingsPath.string()
                  << ", using defaults" << std::endl;
        mAppSettings = nlohmann::json::object();
        applyBundledDefaults();
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
        applyBundledDefaults();

        if (CleanupLegacyAppSettingsOnLoad())
            SaveAppSettings();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Plugin] Failed to parse settings: " << e.what() << std::endl;
        // Back up the corrupt file so the instanceId and other data are not silently lost.
        std::error_code ec;
        const auto backupPath = settingsPath.parent_path() / (settingsPath.filename().string() + ".corrupt");
        std::filesystem::copy_file(settingsPath, backupPath,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::cerr << "[Plugin] Corrupt settings backed up to " << backupPath.string() << std::endl;
        mAppSettings = nlohmann::json::object();
        applyBundledDefaults();
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
        if (lastPresetId.rfind("preset-archive-session__", 0) == 0)
        {
            lastPresetId.clear();
        }
    }

    if (!lastPresetId.empty())
    {
        if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(lastPresetId))
        {
            std::cout << "[Plugin] Skipping last factory archive preset restore because archive loading is disabled" << std::endl;
            mPendingStateBroadcast = true;
            std::cout << "[Plugin] Last session state restored" << std::endl;
            return;
        }

        std::cout << "[Plugin] Restoring last preset: " << lastPresetId << std::endl;
        try
        {
            const auto aliasIt = mFactoryArchivePresetAliases.find(lastPresetId);
            const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end()
                ? aliasIt->second
                : lastPresetId;

            // Try user presets first, then factory
            std::optional<Preset> presetOpt;
            if (!mUserPresetsPath.empty())
            {
                auto userPath = mUserPresetsPath / (resolvedPresetId + ".json");
                presetOpt = PresetStorage::LoadFromFile(userPath);
            }
            if (!presetOpt)
            {
                auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
                presetOpt = PresetStorage::LoadFromFile(factoryPath);
            }
            if (!presetOpt)
            {
                auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);
                if (archiveIt != mFactoryArchivePresets.end())
                    presetOpt = archiveIt->second;
            }

            if (presetOpt)
            {
                mActivePresetId = resolvedPresetId;
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

    if (lastPresetId.empty())
        TryLoadConfiguredDefaultPreset();

    mPendingStateBroadcast = true;
    std::cout << "[Plugin] Last session state restored" << std::endl;
}

std::optional<Preset> PluginController::LoadPresetById(const std::string& presetId) const
{
    if (presetId.empty())
        return std::nullopt;

    const auto aliasIt = mFactoryArchivePresetAliases.find(presetId);
    const std::string resolvedPresetId = aliasIt != mFactoryArchivePresetAliases.end()
        ? aliasIt->second
        : presetId;

    if (!IsFactoryPresetArchiveLoadingEnabled() && mTrackedFactoryArchivePresetIds.contains(resolvedPresetId))
        return std::nullopt;

    std::optional<Preset> presetOpt;
    const auto presetDirectory = GetEffectiveUserPresetDirectory();
    if (!presetDirectory.empty())
    {
        const auto userPath = presetDirectory / (resolvedPresetId + ".json");
        presetOpt = PresetStorage::LoadFromFile(userPath);
    }
    if (IsPresetArchiveSessionActive())
        return presetOpt;
    if (!presetOpt)
    {
        const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot) / (resolvedPresetId + ".json");
        presetOpt = PresetStorage::LoadFromFile(factoryPath);
    }
    if (!presetOpt)
    {
        const auto archiveIt = mFactoryArchivePresets.find(resolvedPresetId);
        if (archiveIt != mFactoryArchivePresets.end())
            presetOpt = archiveIt->second;
    }

    return presetOpt;
}

std::optional<std::string> PluginController::FindPresetIdByTitle(const std::string& presetTitle) const
{
    const std::string normalizedTitle = NormalizePresetTitle(presetTitle);
    if (normalizedTitle.empty())
        return std::nullopt;
    const bool factoryArchiveLoadingEnabled = IsFactoryPresetArchiveLoadingEnabled();

    auto matchesTitle = [&](const Preset& preset) -> bool
    {
        return NormalizePresetTitle(preset.name) == normalizedTitle;
    };

    const auto presetDirectory = GetEffectiveUserPresetDirectory();
    if (!presetDirectory.empty() && std::filesystem::exists(presetDirectory))
    {
        for (const auto& entry : std::filesystem::directory_iterator(presetDirectory))
        {
            if (entry.path().extension() != ".json")
                continue;
            const auto presetOpt = PresetStorage::LoadFromFile(entry.path());
            if (presetOpt && !factoryArchiveLoadingEnabled && mTrackedFactoryArchivePresetIds.contains(presetOpt->id))
                continue;
            if (presetOpt && matchesTitle(*presetOpt))
                return presetOpt->id;
        }
    }

    if (IsPresetArchiveSessionActive())
        return std::nullopt;

    const auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
    if (std::filesystem::exists(factoryPath))
    {
        for (const auto& entry : std::filesystem::directory_iterator(factoryPath))
        {
            if (entry.path().extension() != ".json")
                continue;
            const auto presetOpt = PresetStorage::LoadFromFile(entry.path());
            if (presetOpt && matchesTitle(*presetOpt))
                return presetOpt->id;
        }
    }

    if (factoryArchiveLoadingEnabled)
    {
        for (const auto& [presetId, preset] : mFactoryArchivePresets)
        {
            if (matchesTitle(preset))
                return presetId;
        }
    }

    return std::nullopt;
}

bool PluginController::TryLoadConfiguredDefaultPreset()
{
    const std::string configuredTitle = guitarfx::config::kDefaultStartupPresetTitle;
    if (NormalizePresetTitle(configuredTitle).empty())
        return false;

    const auto presetId = FindPresetIdByTitle(configuredTitle);
    if (!presetId)
    {
        std::cerr << "[Plugin] Configured default preset title not found: " << configuredTitle << std::endl;
        return false;
    }

    const auto presetOpt = LoadPresetById(*presetId);
    if (!presetOpt)
    {
        std::cerr << "[Plugin] Configured default preset could not be loaded: " << configuredTitle << std::endl;
        return false;
    }

    mActivePresetId = *presetId;
    mActivePreset = *presetOpt;
    mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
    ApplyPreset(*presetOpt);
    std::cout << "[Plugin] Loaded configured default preset: " << presetOpt->name << std::endl;
    return true;
}

void PluginController::LoadResourceLibraries()
{
    const auto libraryFile = ResolveResourceLibraryIndexPath();
    mResourceLibrary.Clear();
    if (!std::filesystem::exists(libraryFile))
    {
        std::cout << "[Plugin] Resource library file not found: " << libraryFile.string() << std::endl;
        return;
    }

    mResourceLibrary.LoadFromFile(libraryFile);
    CleanupResourceLibraryCategoriesOnStartup();
    std::cout << "[Plugin] Loaded resource library from " << libraryFile.string() << std::endl;
}

void PluginController::CleanupResourceLibraryCategoriesOnStartup()
{
    const auto libraryFile = ResolveResourceLibraryIndexPath();
    auto allResources = mResourceLibrary.GetAllResources();
    std::size_t updatedCount = 0;

    for (auto& resource : allResources)
    {
        if (resource.type != "nam")
            continue;

        // Backfill NAM metadata before category resolution so older entries can
        // be reassigned from file-native metadata (e.g. gear_type).
        EnrichNamResourceMetadata(resource, resource.filePath);

        const std::string resolvedCategory = ResolveResourceLibraryCategory(resource, resource.category);
        if (resolvedCategory.empty() || resolvedCategory == resource.category)
            continue;

        resource.category = resolvedCategory;
        mResourceLibrary.UpdateResource(resource.type, resource.id, resource);
        ++updatedCount;
    }

    if (updatedCount > 0)
    {
        mResourceLibrary.SaveToFile(libraryFile);
        AppendSessionLog("Normalized resource categories at startup: " + std::to_string(updatedCount));
    }
}

void PluginController::LoadFactoryPresetArchives()
{
    mFactoryArchivePresets.clear();
    mFactoryArchiveBlendIds.clear();
    mFactoryArchivePresetIds.clear();
    mTrackedFactoryArchivePresetIds.clear();
    mFactoryArchivePresetAliases.clear();

    auto factoryArchiveState = LoadJsonFile(ResolveFactoryArchiveStatePath(mFileSystem), nlohmann::json::object());
    if (!factoryArchiveState.is_object())
        factoryArchiveState = nlohmann::json::object();
    factoryArchiveState["schemaVersion"] = kFactoryArchiveStateSchemaVersion;
    if (!factoryArchiveState.contains("archives") || !factoryArchiveState["archives"].is_object())
        factoryArchiveState["archives"] = nlohmann::json::object();

    for (const auto& archiveEntry : factoryArchiveState["archives"].items())
    {
        const auto& mappings = archiveEntry.value().value("presetMappings", nlohmann::json::object());
        if (!mappings.is_object())
            continue;
        for (const auto& mapping : mappings.items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            mTrackedFactoryArchivePresetIds.insert(importedId);
        }
    }

    if (!IsFactoryPresetArchiveLoadingEnabled())
    {
        AppendSessionLog("Factory preset archive loading disabled by app setting");
        return;
    }

    for (const auto& archiveEntry : factoryArchiveState["archives"].items())
    {
        const auto& mappings = archiveEntry.value().value("presetMappings", nlohmann::json::object());
        if (!mappings.is_object())
            continue;
        for (const auto& mapping : mappings.items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            mFactoryArchivePresetAliases[mapping.key()] = importedId;
            mFactoryArchivePresetIds.insert(importedId);
            mTrackedFactoryArchivePresetIds.insert(importedId);
        }
    }

    const auto factoryDir = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
    if (!std::filesystem::exists(factoryDir))
        return;

    const auto extractedRoot = mFileSystem.ResolveSettingsDirectory() / "resources" / "content" / kFactoryArchiveResourceProvider;
    [[maybe_unused]] const auto ensuredExtractedRoot = mFileSystem.EnsureDirectory(extractedRoot);

    std::unordered_set<std::string> occupiedPresetIds;
    if (!mUserPresetsPath.empty() && std::filesystem::exists(mUserPresetsPath))
    {
        for (const auto& entry : std::filesystem::directory_iterator(mUserPresetsPath))
        {
            if (entry.path().extension() == ".json")
                occupiedPresetIds.insert(entry.path().stem().string());
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(factoryDir))
    {
        if (entry.path().extension() == ".json")
            occupiedPresetIds.insert(entry.path().stem().string());
    }

    if (!mBlendLibrary.is_array())
        mBlendLibrary = nlohmann::json::array();

    for (const auto& entry : std::filesystem::directory_iterator(factoryDir))
    {
        if (!entry.is_regular_file() || !IsFactoryArchiveExtension(entry.path()))
            continue;

        const auto zipBytes = util::ReadFileBytes(entry.path());
        if (zipBytes.empty())
        {
            AppendSessionLog("Factory preset archive skipped (empty or unreadable): " + entry.path().string());
            continue;
        }

        std::string parseError;
        auto parsedOpt = ParseFactoryPresetArchive(entry.path(), zipBytes, parseError);
        if (!parsedOpt)
        {
            AppendSessionLog("Factory preset archive skipped (" + entry.path().filename().string() + "): " + parseError);
            continue;
        }

        auto parsed = std::move(*parsedOpt);
        const std::string archiveKey = BuildFactoryArchiveKey(entry.path());
        const std::string archiveHash = mHasher.HashFile(entry.path());
        auto archiveState = factoryArchiveState["archives"].contains(archiveKey)
            && factoryArchiveState["archives"][archiveKey].is_object()
            ? factoryArchiveState["archives"][archiveKey]
            : nlohmann::json::object();
        if (!archiveState.contains("presetMappings") || !archiveState["presetMappings"].is_object())
            archiveState["presetMappings"] = nlohmann::json::object();

        std::unordered_set<std::string> trackedPresetIds;
        for (const auto& mapping : archiveState["presetMappings"].items())
        {
            if (!mapping.value().is_string())
                continue;
            const std::string importedId = mapping.value().get<std::string>();
            if (importedId.empty())
                continue;
            trackedPresetIds.insert(importedId);
            mFactoryArchivePresetAliases[mapping.key()] = importedId;
            mFactoryArchivePresetIds.insert(importedId);
        }

        const bool archiveChanged = archiveHash.empty() || archiveState.value("hash", "") != archiveHash;
        std::unordered_map<std::string, std::string> resourceIdMap;
        std::unordered_map<std::string, std::string> blendIdMap;

        for (const auto& resource : parsed.resources)
        {
            const std::string scopedResourceId = BuildScopedFactoryArchiveId(archiveKey, resource.id);
            std::string resolvedName = resource.fileName.empty() ? resource.id : resource.fileName;
            resolvedName = util::SanitizeFilename(resolvedName);
            if (resolvedName.empty())
                resolvedName = scopedResourceId + (resource.type == "ir" ? ".wav" : ".nam");

            const auto archiveExtractDir = extractedRoot / archiveKey;
            [[maybe_unused]] const auto ensuredArchiveDir = mFileSystem.EnsureDirectory(archiveExtractDir);
            const auto targetPath = archiveExtractDir / resolvedName;
            const bool needsWrite = archiveChanged || !std::filesystem::exists(targetPath);
            if (needsWrite && !WriteFile(targetPath, resource.bytes))
            {
                AppendSessionLog("Factory preset archive resource write failed: " + targetPath.string());
                continue;
            }
            if (!std::filesystem::exists(targetPath))
            {
                AppendSessionLog("Factory preset archive resource missing after import: " + targetPath.string());
                continue;
            }

            resourceIdMap[resource.id] = scopedResourceId;

            LibraryResource libraryResource;
            libraryResource.type = resource.type;
            libraryResource.id = scopedResourceId;
            libraryResource.name = resource.name.empty() ? resource.id : resource.name;
            libraryResource.category = resource.category;
            libraryResource.description = "Bundled factory archive resource";
            libraryResource.filePath = targetPath;
            libraryResource.hash = resource.hash;
            libraryResource.metadata["provider"] = kFactoryArchiveResourceProvider;
            libraryResource.metadata["archive"] = entry.path().filename().string();
            libraryResource.metadata["factoryArchiveKey"] = archiveKey;
            libraryResource.metadata["factoryArchiveHash"] = archiveHash;
            libraryResource.metadata["originalId"] = resource.id;
            if (needsWrite || !mResourceLibrary.HasResource(libraryResource.type, libraryResource.id))
                AppendUserLibraryResource(libraryResource);
            else
                mResourceLibrary.AddResource(libraryResource);
        }

        for (auto blend : parsed.blends)
        {
            const std::string originalBlendId = blend.value("id", "");
            if (originalBlendId.empty())
                continue;

            const std::string scopedBlendId = BuildScopedFactoryArchiveId(archiveKey, originalBlendId);
            blendIdMap[originalBlendId] = scopedBlendId;
            blend["id"] = scopedBlendId;

            if (blend.contains("models") && blend["models"].is_array())
            {
                for (auto& modelId : blend["models"])
                {
                    if (!modelId.is_string())
                        continue;
                    const auto mapped = resourceIdMap.find(modelId.get<std::string>());
                    if (mapped != resourceIdMap.end())
                        modelId = mapped->second;
                }
            }

            if (blend.contains("modelMappings") && blend["modelMappings"].is_array())
            {
                for (auto& mapping : blend["modelMappings"])
                {
                    if (!mapping.is_object())
                        continue;
                    const auto mapped = resourceIdMap.find(mapping.value("id", ""));
                    if (mapped != resourceIdMap.end())
                        mapping["id"] = mapped->second;
                }
            }

            mFactoryArchiveBlendIds.insert(scopedBlendId);

            bool replaced = false;
            for (auto& existing : mBlendLibrary)
            {
                if (existing.is_object() && existing.value("id", "") == scopedBlendId)
                {
                    existing = blend;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                mBlendLibrary.push_back(blend);
        }

        std::unordered_map<std::string, std::string> presetIdMapping;
        std::vector<std::string> importedPresetIds;
        for (auto preset : parsed.presets)
        {
            RemapPresetArchiveReferences(preset, resourceIdMap, blendIdMap);
            NormalizePresetScenes(preset);

            const std::string sourcePresetId = preset.id.empty()
                ? BuildScopedFactoryArchiveId(archiveKey, preset.name.empty() ? "preset" : preset.name)
                : preset.id;

            std::string uniquePresetId = archiveState["presetMappings"].value(sourcePresetId, std::string{});
            if (uniquePresetId.empty())
            {
                const std::string basePresetId = BuildScopedFactoryArchiveId(archiveKey, sourcePresetId.empty() ? "preset" : sourcePresetId);
                std::size_t suffix = 2;
                uniquePresetId = basePresetId;
                while ((occupiedPresetIds.contains(uniquePresetId) || mFactoryArchivePresets.contains(uniquePresetId))
                       && !trackedPresetIds.contains(uniquePresetId))
                {
                    uniquePresetId = basePresetId + "-" + std::to_string(suffix++);
                }
            }

            archiveState["presetMappings"][sourcePresetId] = uniquePresetId;
            presetIdMapping[sourcePresetId] = uniquePresetId;
            importedPresetIds.push_back(uniquePresetId);
            mFactoryArchivePresetAliases[sourcePresetId] = uniquePresetId;
            mFactoryArchivePresetIds.insert(uniquePresetId);
            mTrackedFactoryArchivePresetIds.insert(uniquePresetId);
            occupiedPresetIds.insert(uniquePresetId);

            preset.id = uniquePresetId;
            preset.category = "Factory";

            const auto presetPath = mUserPresetsPath / (preset.id + ".json");
            if ((archiveChanged || !std::filesystem::exists(presetPath))
                && !PresetStorage::SaveToFile(preset, presetPath))
            {
                AppendSessionLog("Factory preset archive preset write failed: " + presetPath.string());
            }

            mFactoryArchivePresets[preset.id] = std::move(preset);
        }

        UpdateFactoryPresetFolders(mFileSystem,
                       archiveKey,
                       parsed.presetFolders,
                       presetIdMapping,
                       importedPresetIds);

        archiveState["hash"] = archiveHash;
        archiveState["fileName"] = entry.path().filename().string();
        factoryArchiveState["archives"][archiveKey] = archiveState;

        if (parsed.tone3000ResourceCount > 0)
        {
            AppendSessionLog("Factory preset archive contains tone3000 resource references that are not auto-imported at startup: "
                             + entry.path().filename().string());
        }
    }

    SaveJsonFile(mFileSystem, ResolveFactoryArchiveStatePath(mFileSystem), factoryArchiveState);
    InvalidateResourceUsageIndex();
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

void PluginController::LoadCustomEffectLibrary()
{
    mCustomEffectLibrary.LoadFromFile(ResolveCustomEffectLibraryPath(mFileSystem));
}

void PluginController::SaveBlendLibrary() const
{
    const auto blendPath = mFileSystem.ResolveSettingsDirectory() / "blends" / "library.json";
    bool wrote = false;
    try
    {
        [[maybe_unused]] const auto ensuredBlendParent = mFileSystem.EnsureDirectory(blendPath.parent_path());
        std::ofstream ofs(blendPath);
        if (ofs.is_open())
        {
            nlohmann::json persisted = nlohmann::json::array();
            if (mBlendLibrary.is_array())
            {
                for (const auto& blend : mBlendLibrary)
                {
                    const std::string id = blend.value("id", "");
                    if (!id.empty() && mFactoryArchiveBlendIds.contains(id))
                        continue;
                    persisted.push_back(blend);
                }
            }
            ofs << persisted.dump(2);
            wrote = true;
        }
    }
    catch (const std::exception&) {}

    if (wrote)
        TouchSharedSyncState({"blends"});
}

void PluginController::SaveCustomEffectLibrary() const
{
    mCustomEffectLibrary.SaveToFile(ResolveCustomEffectLibraryPath(mFileSystem));
    TouchSharedSyncState({"customEffects"});
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
    nlohmann::json library;
    library["byEffectType"] = nlohmann::json::object();
    library["defaults"] = nlohmann::json::object();
    // Layout images (base64-encoded backgrounds) are intentionally omitted here to keep
    // app startup lightweight; they are loaded on demand via BuildLayoutImages() when the
    // layout designer/manager requests them. Per-layout thumbnailDataUrl values remain
    // embedded in each layout entry below since they are used for signal-path node avatars.
    library["images"] = nlohmann::json::array();

    // Build user layout library from the associations index.
    // Each layout lives in its own subfolder: layouts/content/<layoutId>/layout.json
    // Images live in: layouts/content/<layoutId>/images/
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
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
                    if (!input) continue;
                    nlohmann::json layoutJson;
                    input >> layoutJson;
                    if (!layoutJson.is_object()) continue;

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

    // Load factory layouts from the bundled assets directory.
    // Structure: ui/assets/layouts/<folder-name>/layout.json  (+ images/ subfolder)
    // Each folder represents one exported layout. Factory layouts are read-only and are
    // added as defaults only when no user-defined default already exists for that key.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = bundledRoot / "ui" / "assets" / "layouts";
        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                    continue;

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";
                if (!std::filesystem::exists(layoutJsonPath))
                    continue;

                try
                {
                    std::ifstream input(layoutJsonPath);
                    if (!input)
                        continue;

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("layout") || !archive["layout"].is_object())
                        continue;

                    nlohmann::json layoutJson = archive["layout"];

                    const std::string effectType = layoutJson.value("effectType", "");
                    if (effectType.empty())
                        continue;

                    const std::string blendId = layoutJson.value("blendId", "");
                    const std::string lookupKey = blendId.empty() ? effectType : (effectType + "::" + blendId);

                    // Use embedded layoutId or derive a stable one from the folder name.
                    std::string layoutId = layoutJson.value("layoutId", "");
                    if (layoutId.empty())
                    {
                        layoutId = "factory::" + layoutFolder.path().filename().string();
                        layoutJson["layoutId"] = layoutId;
                    }

                    // Load images referenced in the manifest and add them to the library image list.
                    const auto imagesDir = layoutFolder.path() / "images";
                    (void)imagesDir; // Factory images are loaded on demand via BuildLayoutImages().

                    // Build the library entry and prepend it so user layouts (added below)
                    // can override/supplement without losing the factory entry.
                    nlohmann::json factoryEntry;
                    factoryEntry["layout"] = layoutJson;
                    factoryEntry["isDefault"] = false; // resolved after user entries are built
                    factoryEntry["layoutId"] = layoutId;
                    factoryEntry["isFactory"] = true;
                    factoryEntry["filePath"] = layoutJsonPath.generic_string();

                    if (!library["byEffectType"].contains(lookupKey))
                        library["byEffectType"][lookupKey] = nlohmann::json::array();

                    // Prepend so factory entries appear first; user entries appended later.
                    library["byEffectType"][lookupKey].insert(
                        library["byEffectType"][lookupKey].begin(), factoryEntry);
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout from "
                        + layoutFolder.path().generic_string() + ": " + e.what());
                }
            }
        }
    }

    // Resolve defaults: for each key without a user-defined default, use the first
    // factory layout found (if any).
    for (auto& [key, entries] : library["byEffectType"].items())
    {
        if (!library["defaults"].contains(key) || library["defaults"][key].get<std::string>().empty())
        {
            for (const auto& entry : entries)
            {
                if (entry.value("isFactory", false))
                {
                    const std::string fid = entry.value("layoutId", "");
                    if (!fid.empty())
                    {
                        library["defaults"][key] = fid;
                        break;
                    }
                }
            }
        }
        // Stamp isDefault on each entry.
        const std::string defaultId = library["defaults"].value(key, "");
        for (auto& entry : entries)
        {
            entry["isDefault"] = (!defaultId.empty() && entry.value("layoutId", "") == defaultId);
        }
    }

    SendMessageToUI(nlohmann::json{
        {"type", "layoutLibraryLoaded"},
        {"layoutLibrary", library}
    }.dump());
}

nlohmann::json PluginController::BuildLayoutImages()
{
    nlohmann::json images = nlohmann::json::array();

    // Helper: load all images from a directory into the image list.
    const auto appendImagesFromDir = [&images](const std::filesystem::path& imagesDir)
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
            if (!imageFile) continue;

            std::vector<std::uint8_t> imageData(
                (std::istreambuf_iterator<char>(imageFile)),
                std::istreambuf_iterator<char>());
            imageFile.close();

            const std::string base64Data = util::EncodeBase64(imageData);
            std::string mimeType = "image/png";
            if (ext == ".jpg" || ext == ".jpeg") mimeType = "image/jpeg";
            const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

            const std::string imageId = entry.path().stem().string();
            bool replaced = false;
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

    // User layout images: one images/ folder per associated layoutId.
    nlohmann::json settings = LoadEffectLayoutsSettings(mFileSystem);
    if (settings.contains("associations") && settings["associations"].is_object())
    {
        for (auto it = settings["associations"].begin(); it != settings["associations"].end(); ++it)
        {
            const auto& assocEntry = it.value();
            if (!assocEntry.is_object())
                continue;

            const auto ids = assocEntry.value("layoutIds", nlohmann::json::array());
            if (!ids.is_array())
                continue;

            for (const auto& id : ids)
            {
                if (!id.is_string())
                    continue;
                const std::string layoutId = id.get<std::string>();
                appendImagesFromDir(ResolveLayoutDir(mFileSystem, layoutId) / "images");
            }
        }
    }

    // Factory layout images: referenced via each layout's manifest.
    {
        const auto bundledRoot = mHost.GetBundledAssetsPath();
        const auto factoryLayoutsDir = bundledRoot / "ui" / "assets" / "layouts";
        if (std::filesystem::exists(factoryLayoutsDir))
        {
            for (const auto& layoutFolder : std::filesystem::directory_iterator(factoryLayoutsDir))
            {
                if (!layoutFolder.is_directory())
                    continue;

                const auto layoutJsonPath = layoutFolder.path() / "layout.json";
                if (!std::filesystem::exists(layoutJsonPath))
                    continue;

                try
                {
                    std::ifstream input(layoutJsonPath);
                    if (!input)
                        continue;

                    nlohmann::json archive;
                    input >> archive;

                    if (!archive.is_object() || !archive.contains("images") || !archive["images"].is_array())
                        continue;

                    const auto imagesDir = layoutFolder.path() / "images";
                    if (!std::filesystem::exists(imagesDir))
                        continue;

                    for (const auto& imgRef : archive["images"])
                    {
                        if (!imgRef.is_object())
                            continue;
                        const std::string imageId = imgRef.value("imageId", "");
                        const std::string fileName = imgRef.value("fileName", "");
                        if (imageId.empty() || fileName.empty())
                            continue;

                        const auto imgPath = imagesDir / fileName;
                        if (!std::filesystem::exists(imgPath))
                            continue;

                        std::ifstream imgFile(imgPath, std::ios::binary);
                        if (!imgFile)
                            continue;

                        std::vector<std::uint8_t> imgData(
                            (std::istreambuf_iterator<char>(imgFile)),
                            std::istreambuf_iterator<char>());
                        imgFile.close();

                        const std::string base64Data = util::EncodeBase64(imgData);
                        const auto ext = imgPath.extension().string();
                        std::string mimeType = "image/png";
                        if (ext == ".jpg" || ext == ".jpeg") mimeType = "image/jpeg";
                        const std::string dataUrl = "data:" + mimeType + ";base64," + base64Data;

                        bool replaced = false;
                        for (auto& existing : images)
                        {
                            if (existing.is_object() && existing.value("imageId", std::string{}) == imageId)
                            {
                                existing["fileName"] = fileName;
                                existing["dataUrl"] = dataUrl;
                                replaced = true;
                                break;
                            }
                        }
                        if (!replaced)
                        {
                            nlohmann::json imageRef;
                            imageRef["imageId"] = imageId;
                            imageRef["fileName"] = fileName;
                            imageRef["dataUrl"] = dataUrl;
                            images.push_back(imageRef);
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    AppendSessionLog("Failed to load factory layout images from "
                        + layoutFolder.path().generic_string() + ": " + e.what());
                }
            }
        }
    }

    return images;
}

void PluginController::HandleRequestLayoutImagesRequest()
{
    SendMessageToUI(nlohmann::json{
        {"type", "layoutImagesLoaded"},
        {"images", BuildLayoutImages()}
    }.dump());
}

void PluginController::SaveLayoutToFile(const std::string& layoutId, const nlohmann::json& layoutJson)
{
    const auto layoutDir = ResolveLayoutDir(mFileSystem, layoutId);
    [[maybe_unused]] const auto ensuredDir = mFileSystem.EnsureDirectory(layoutDir);

    const auto layoutFile = layoutDir / "layout.json";
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
    const auto settingsDir = GetEffectiveSettingsDirectory();

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

    bool wrote = false;
    try
    {
        [[maybe_unused]] const auto ensuredUiStorageParent = mFileSystem.EnsureDirectory(path.parent_path());

        // Write to a temp file first, then atomically rename over the real file,
        // matching SaveAppSettings. A partial write here would corrupt setlists.json,
        // automation.json or the preset metadata files and lose the user's data.
        const auto tempPath = path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::ofstream ofs(tempPath);
            if (!ofs.is_open())
                return;
            ofs << payload.dump(2);
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, path, ec);
        if (ec)
        {
            // rename failed (e.g. cross-device) – fall back to copy+delete
            std::filesystem::copy_file(tempPath, path,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tempPath, ec);
        }
        wrote = !ec;
    }
    catch (const std::exception&) {}

    if (!wrote)
        return;

    std::vector<std::string> domains;
    if (filename == "automation.json")
        domains.push_back("automation");
    else if (filename == "setlists.json")
        domains.push_back("setlists");
    else if (filename == "preset-folders.json"
             || filename == "preset-favorites.json"
             || filename == "preset-ratings.json")
        domains.push_back("presetMetadata");
    else
        domains.push_back("uiStorage");

    TouchSharedSyncState(domains);
}

std::filesystem::path PluginController::ResolveRiffLibraryPath() const
{
    if (mAppSettings.contains(kRiffLibraryPathSettingKey) && mAppSettings[kRiffLibraryPathSettingKey].is_string())
    {
        const auto configured = util::PathFromUtf8(mAppSettings[kRiffLibraryPathSettingKey].get<std::string>());
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

    index["path"] = util::PathToUtf8(path);
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

            const auto storedPath = util::PathFromUtf8(take["filePath"].get<std::string>());
            if (storedPath.empty())
                continue;

            const auto resolvedPath = ResolveRiffTakePathForRuntime(storedPath, path);
            const bool resolvedExists = !resolvedPath.empty() && resolvedPath != storedPath && std::filesystem::exists(resolvedPath);
            const bool storedExists = std::filesystem::exists(storedPath);

            if (resolvedExists)
                take["filePath"] = util::PathToUtf8(resolvedPath);
            else if (!storedExists && !resolvedPath.empty())
                take["filePath"] = util::PathToUtf8(resolvedPath);
        }
    }

    return index;
}

bool PluginController::SaveRiffLibraryIndex(const nlohmann::json& payload) const
{
    const auto indexPath = ResolveRiffLibraryIndexPath();
    const auto libraryPath = ResolveRiffLibraryPath();
    nlohmann::json normalizedPayload = payload;

    normalizedPayload["path"] = util::PathToUtf8(libraryPath);
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

            const auto runtimePath = util::PathFromUtf8(take["filePath"].get<std::string>());
            const auto storedPath = BuildRiffTakePathForStorage(runtimePath, libraryPath);
            take["filePath"] = util::PathToUtf8(storedPath);
        }
    }

    try
    {
        std::filesystem::create_directories(indexPath.parent_path());
        std::ofstream output(indexPath);
        if (!output)
            return false;
        output << normalizedPayload.dump(2);
        const bool ok = static_cast<bool>(output);
        if (ok)
            TouchSharedSyncState({"riffLibrary"});
        return ok;
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
    msg["metronomeClickEnabled"] = mRiffCapture.config.metronomeClickEnabled;
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

void PluginController::SendCustomEffectLibraryToUI()
{
    nlohmann::json msg;
    msg["type"] = "customEffectLibrary";
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
        entries.push_back(SerializeCustomEffectLibraryEntry(entry));
    msg["entries"] = std::move(entries);
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
        if (!info.resourceFilterHint.empty())
            entry["resourceFilterHint"] = info.resourceFilterHint;

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
            if (p.step != 0.0)
                param["step"] = p.step;
            if (!p.labels.empty())
                param["labels"] = p.labels;
            params.push_back(param);
        }
        entry["parameters"] = params;

        if (!info.presets.empty())
        {
            nlohmann::json presets = nlohmann::json::array();
            for (const auto& preset : info.presets)
            {
                presets.push_back({
                    {"id", preset.id},
                    {"name", preset.displayName},
                    {"source", preset.isFactory ? "factory" : "custom"},
                    {"parameters", preset.parameters},
                    {"parameterOrder", preset.parameterOrder},
                });
            }
            entry["presets"] = std::move(presets);
        }

        if (!info.exposedResources.empty())
        {
            nlohmann::json exposedResources = nlohmann::json::array();
            for (const auto& er : info.exposedResources)
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
        else if (info.type.rfind("composite:", 0) == 0)
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
    const bool factoryArchiveLoadingEnabled = IsFactoryPresetArchiveLoadingEnabled();
    const bool archiveSessionActive = IsPresetArchiveSessionActive();

    auto factoryPath = ResolveFactoryPresetDirectory(mHost, mResourceRoot);
    auto userPath = GetEffectiveUserPresetDirectory();

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
                    if (!factoryArchiveLoadingEnabled && mTrackedFactoryArchivePresetIds.contains(preset.id))
                        continue;
                    nlohmann::json p;
                    p["id"] = preset.id;
                    p["name"] = preset.name;
                    p["category"] = preset.category;
                    p["source"] = mFactoryArchivePresetIds.contains(preset.id) ? "factory" : source;
                    presets.push_back(p);
                }
                catch (...) {}
            }
        }
    };

    if (!archiveSessionActive)
        scanDir(factoryPath, "factory");
    scanDir(userPath, "user");

    std::unordered_set<std::string> seenPresetIds;
    for (const auto& preset : presets)
    {
        if (preset.is_object())
            seenPresetIds.insert(preset.value("id", ""));
    }
    for (const auto& [presetId, preset] : mFactoryArchivePresets)
    {
        if (archiveSessionActive || !factoryArchiveLoadingEnabled)
            continue;
        if (seenPresetIds.contains(presetId))
            continue;
        nlohmann::json p;
        p["id"] = preset.id;
        p["name"] = preset.name;
        p["category"] = preset.category;
        p["source"] = "factory";
        presets.push_back(p);
    }

    if (archiveSessionActive)
    {
        for (auto& preset : presets)
            preset["source"] = "session";
    }

    msg["presets"] = presets;
    SendMessageToUI(msg.dump());
}

void PluginController::RequestSignalDiagnosticsToUI()
{
    mPendingSignalDiagnosticsUpdate = true;
    TrySendPendingSignalDiagnosticsToUI();
}

void PluginController::TrySendPendingSignalDiagnosticsToUI()
{
    if (!mPendingSignalDiagnosticsUpdate)
        return;

    constexpr auto kMinSignalDiagnosticsInterval = std::chrono::milliseconds(1000 / kSignalDiagnosticsRateHz);
    const auto now = std::chrono::steady_clock::now();
    if (mLastSignalDiagnosticsUpdateSentAt.time_since_epoch().count() != 0
        && (now - mLastSignalDiagnosticsUpdateSentAt) < kMinSignalDiagnosticsInterval)
    {
        return;
    }

    mPendingSignalDiagnosticsUpdate = false;
    mLastSignalDiagnosticsUpdateSentAt = now;
    SendSignalDiagnosticsToUI();
}

void PluginController::SendSignalDiagnosticsToUI()
{
    auto snapshot = mPresetMixer.GetSignalDiagnosticsSnapshot();

    // Levels travel as bare numeric tuples, so the 20 Hz frame carries no repeated keys or
    // node ids. Values are rounded to 0.1 dB: the UI renders one decimal place, and full
    // double precision was costing ~14 characters per value for digits nothing displays.
    const auto roundDb = [](double db) { return std::round(db * 10.0) / 10.0; };
    const auto buildLevelTuple = [&roundDb](const MultiPresetMixer::SignalLevelStats& stats)
    {
        // `clipped` is carried explicitly rather than derived UI-side from the rounded peak:
        // a peak of 0.999 rounds to -0.0 dBFS, which would otherwise read as clipping.
        const bool clipped = stats.clipCount > 0 || stats.peak >= 1.0;
        return nlohmann::json::array({
            roundDb(ToDbFS(stats.peak)),
            roundDb(ToDbFS(stats.rms)),
            stats.clipCount,
            clipped ? 1 : 0,
        });
    };

    std::vector<SignalDiagnosticsRosterEntry> roster;
    roster.reserve(snapshot.nodes.size());
    for (const auto& n : snapshot.nodes)
    {
        roster.push_back(SignalDiagnosticsRosterEntry{
            n.scope, n.presetId, n.nodeId, n.nodeType, n.channelCount, n.analyzer.has_value() });
    }

    if (mSignalDiagnosticsRosterDirty || roster != mSignalDiagnosticsRoster)
    {
        mSignalDiagnosticsRoster = roster;
        mSignalDiagnosticsRosterDirty = false;
        ++mSignalDiagnosticsRosterSeq;

        nlohmann::json rosterNodes = nlohmann::json::array();
        for (const auto& entry : mSignalDiagnosticsRoster)
        {
            rosterNodes.push_back(nlohmann::json::array({
                entry.scope, entry.presetId, entry.nodeId, entry.nodeType,
                entry.channelCount, entry.hasAnalyzer ? 1 : 0 }));
        }

        // The analyzer display ranges are compile-time constants, so they ride along with
        // the roster instead of being repeated in every analyzer payload.
        nlohmann::json rosterMsg;
        rosterMsg["type"] = "sldRoster";
        rosterMsg["seq"] = mSignalDiagnosticsRosterSeq;
        rosterMsg["nodes"] = std::move(rosterNodes);
        rosterMsg["spectrogramRange"] = nlohmann::json::array({
            InputAnalyzerEffect::kSpectrogramMinDbfs, InputAnalyzerEffect::kSpectrogramMaxDbfs,
            InputAnalyzerEffect::kSpectrogramMinFrequencyHz, InputAnalyzerEffect::kSpectrogramMaxFrequencyHz });
        rosterMsg["barkRange"] = nlohmann::json::array({
            InputAnalyzerEffect::kBarkMinDbfs, InputAnalyzerEffect::kBarkMaxDbfs,
            InputAnalyzerEffect::kBarkMinFrequencyHz, InputAnalyzerEffect::kBarkMaxFrequencyHz });
        SendMessageToUI(rosterMsg.dump());
    }

    nlohmann::json frameLevels = nlohmann::json::array();
    for (const auto& n : snapshot.nodes)
    {
        for (const auto& value : buildLevelTuple(n.levels))
            frameLevels.push_back(value);
    }

    nlohmann::json frame;
    frame["type"] = "sld";
    frame["seq"] = mSignalDiagnosticsRosterSeq;
    frame["r"] = buildLevelTuple(snapshot.rawInput);
    frame["i"] = buildLevelTuple(snapshot.input);
    frame["o"] = buildLevelTuple(snapshot.output);
    frame["d"] = std::move(frameLevels);
    SendMessageToUI(frame.dump());

    // Analyzer telemetry is an order of magnitude larger than a level tuple, so it moves out
    // of the frame into its own message rather than bloating every node entry. Band values are
    // rounded to whole dB — the spectrogram heatmap and bark bar graph cannot resolve finer.
    // NOTE: this still goes out for every analyzer node on every frame. The analyzer refreshes
    // on each processed block, so there is no cheap staleness check to skip on; the remaining
    // win here is only sending it for the node whose analyzer panel is actually open.
    for (const auto& n : snapshot.nodes)
    {
        if (!n.analyzer)
            continue;

        const auto& analyzer = *n.analyzer;
        const auto quantiseBands = [](const std::vector<float>& values)
        {
            nlohmann::json out = nlohmann::json::array();
            for (const float value : values)
                out.push_back(std::isfinite(value) ? static_cast<int>(std::lround(value)) : -120);
            return out;
        };

        nlohmann::json analyzerMsg;
        analyzerMsg["type"] = "sldA";
        analyzerMsg["seq"] = mSignalDiagnosticsRosterSeq;
        analyzerMsg["id"] = n.nodeId;
        analyzerMsg["t"] = analyzer.generatedAtMs;
        // Percent-of-full-scale values are not rounded to 0.1 like the dB fields: the UI also
        // converts them back to dBFS for display, where 0.1 percentage points is a 6 dB error
        // on a quiet signal. 1e-6 is far below the -120 dBFS floor and still trims the double.
        const auto roundPercent = [](double v) { return std::round(v * 1.0e6) / 1.0e6; };

        // [peakPercent, rmsPercent, rmsDbu, rmsDbv, rmsVolts, momentaryLufs, shortTermLufs,
        //  integratedLufs, activeChannelCount, stereo, loudnessValid]
        analyzerMsg["l"] = nlohmann::json::array({
            roundPercent(analyzer.peakPercent),
            roundPercent(analyzer.rmsPercent),
            roundDb(analyzer.rmsDbu),
            roundDb(analyzer.rmsDbv),
            std::round(analyzer.rmsVolts * 1000.0) / 1000.0,
            roundDb(analyzer.momentaryLufs),
            roundDb(analyzer.shortTermLufs),
            roundDb(analyzer.integratedLufs),
            analyzer.activeChannelCount,
            analyzer.stereo ? 1 : 0,
            analyzer.loudnessValid ? 1 : 0,
        });
        analyzerMsg["s"] = quantiseBands(analyzer.spectrogramBinsDb);
        analyzerMsg["b"] = quantiseBands(analyzer.barkBandsDb);
        SendMessageToUI(analyzerMsg.dump());
    }
}

void PluginController::RequestPerformanceStatsToUI()
{
    mPendingPerformanceStatsUpdate = true;
    TrySendPendingPerformanceStatsToUI();
}

void PluginController::TrySendPendingPerformanceStatsToUI()
{
    if (!mPendingPerformanceStatsUpdate)
        return;

    constexpr auto kMinPerformanceStatsInterval = std::chrono::milliseconds(1000 / kDspPerformanceStatsRateHz);
    const auto now = std::chrono::steady_clock::now();
    if (mLastPerformanceStatsUpdateSentAt.time_since_epoch().count() != 0
        && (now - mLastPerformanceStatsUpdateSentAt) < kMinPerformanceStatsInterval)
    {
        return;
    }

    mPendingPerformanceStatsUpdate = false;
    mLastPerformanceStatsUpdateSentAt = now;
    SendPerformanceStatsToUI();
}

void PluginController::SendPerformanceStatsToUI()
{
    auto stats = mPresetMixer.GetPerformanceStats();
    const int totalLatencySamples = mPresetMixer.GetTotalLatencySamples();
    nlohmann::json statsJson;
    statsJson["totalProcessingTimeUs"] = stats.totalProcessingTimeUs;
    statsJson["realTimeUs"] = stats.realTimeUs;
    statsJson["dspLoadPercent"] = stats.dspLoadPercent;
    statsJson["totalLatencySamples"] = totalLatencySamples;
    nlohmann::json nodeTimes = nlohmann::json::object();
    for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
        nodeTimes[nodeId] = timeUs;
    statsJson["nodeProcessingTimesUs"] = nodeTimes;
    nlohmann::json scopedNodeTimes = nlohmann::json::object();
    for (const auto& [nodeId, timeUs] : stats.scopedNodeProcessingTimesUs)
        scopedNodeTimes[nodeId] = timeUs;
    statsJson["scopedNodeProcessingTimesUs"] = scopedNodeTimes;
    nlohmann::json nodeLatencies = nlohmann::json::object();
    for (const auto& [nodeId, latencySamples] : stats.nodeLatencySamples)
        nodeLatencies[nodeId] = latencySamples;
    statsJson["nodeLatencySamples"] = nodeLatencies;
    nlohmann::json scopedNodeLatencies = nlohmann::json::object();
    for (const auto& [nodeId, latencySamples] : stats.scopedNodeLatencySamples)
        scopedNodeLatencies[nodeId] = latencySamples;
    statsJson["scopedNodeLatencySamples"] = scopedNodeLatencies;

    nlohmann::json msg;
    msg["type"] = "dspPerformance";
    msg["stats"] = statsJson;
    msg["sampleRate"] = mHost.GetSampleRate();
    msg["blockSize"] = mHost.GetBlockSize();
    SendMessageToUI(msg.dump());
}

void PluginController::SendSpatialPositionsToUI()
{
    static const std::vector<std::string> kSpatialReadoutParams = {
        "currentAzimuth", "currentElevation", "currentDistance",
        "currentItdUs", "currentIldDb", "effectiveRate", "motionMode"
    };

    const auto readouts = mPresetMixer.ReadNodeParamsForType(EffectGuids::kSpatial3D, kSpatialReadoutParams);

    if (readouts.empty())
    {
        // Send one final empty update so a UI that was tracking a node it can no
        // longer see stops animating, then go quiet until a spatialiser reappears.
        if (!mSpatialPositionsWereSent)
            return;
        mSpatialPositionsWereSent = false;
    }
    else
    {
        mSpatialPositionsWereSent = true;
    }

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& readout : readouts)
    {
        nlohmann::json node{
            {"scope", readout.scope},
            {"nodeId", readout.nodeId},
            {"azimuth", readout.values[0]},
            {"elevation", readout.values[1]},
            {"distance", readout.values[2]},
            {"itdUs", readout.values[3]},
            {"ildDb", readout.values[4]},
            {"rateHz", readout.values[5]},
            {"moving", readout.values[6] > 0.5}
        };
        if (!readout.presetId.empty())
            node["presetId"] = readout.presetId;
        nodes.push_back(std::move(node));
    }

    nlohmann::json msg{
        {"type", "spatialPosition"},
        {"nodes", std::move(nodes)}
    };
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
    msg["beatPattern"] = mMetronomeBeatPattern;
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
