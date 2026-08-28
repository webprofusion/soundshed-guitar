/**
 * PluginControllerMetronome.cpp - The click track: tempo, click synthesis and
 * settings.
 *
 * Standalone owns its own tempo; hosted, the DAW does. GetEffectiveTempoBpm is
 * the single place that decides which, and everything else here follows it.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"
#include "util/FileIO.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

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

} // namespace guitarfx
