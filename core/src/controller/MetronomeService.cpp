#include "controller/MetronomeService.h"

#include "IPluginHost.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"
#include "util/FileIO.h"
#include "util/Wav.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iostream>
#include <string_view>
#include <tuple>
#include <utility>

using namespace guitarfx::controller_detail;

namespace guitarfx
{

MetronomeService::MetronomeService(IPluginHost& host,
                                   nlohmann::json& appSettings,
                                   const std::filesystem::path& resourceRoot)
    : mHost(host)
    , mAppSettings(appSettings)
    , mResourceRoot(resourceRoot)
{
}

double MetronomeService::EffectiveTempoBpm() const
{
    if (mHost.IsStandalone())
        return ClampValue(mBpm.load(std::memory_order_relaxed), kMetronomeMinBpm, kMetronomeMaxBpm);

    const double hostTempo = mHost.GetHostTempo();
    if (hostTempo > 0.0)
        return ClampValue(hostTempo, kMetronomeMinBpm, kMetronomeMaxBpm);

    return kMetronomeDefaultBpm;
}

void MetronomeService::ResetTransport()
{
    mSamplesUntilClick = 0.0;
    mClickSamplesRemaining = 0;
    mClickPhase = 0.0;
    mBeatIndex = 0;
    mClickSamplePosition = 0;
    mClickUseHigh = false;
    RequestReset();
}

void MetronomeService::Render(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1])
        return;

    if (!mHost.IsStandalone())
        return;

    const bool guidanceActive = mGuidanceActive;
    if (!guidanceActive && !mEnabled.load(std::memory_order_relaxed))
        return;

    if (mResetPending.exchange(false, std::memory_order_acq_rel))
    {
        mSamplesUntilClick = 0.0;
        mClickSamplesRemaining = 0;
        mClickPhase = 0.0;
        mBeatIndex = 0;
        mClickSamplePosition = 0;
        mClickUseHigh = false;
    }

    const double sampleRate = mHost.GetSampleRate();
    if (sampleRate <= 0.0)
        return;

    const double bpm = guidanceActive
        ? ClampValue(mGuidanceBpm, kMetronomeMinBpm, kMetronomeMaxBpm)
        : EffectiveTempoBpm();
    const int beatsPerBar = std::max(1, guidanceActive ? mGuidanceBeatsPerBar : kMetronomeBeatsPerBar);
    const double beatScale = guidanceActive ? std::max(0.125, mGuidanceBeatScale) : 1.0;
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, bpm)) * beatScale;
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * kMetronomeClickSeconds));
    mClickPhaseIncrement = kTwoPi * kMetronomeClickFrequencyHz / sampleRate;

    const double volumeDb = ClampValue(mVolumeDb.load(std::memory_order_relaxed),
                                       kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    const double volume = ClampValue(LinearFromDb(volumeDb), 0.0, LinearFromDb(kMetronomeMaxVolumeDb));
    const double pan = ClampValue(mPan.load(std::memory_order_relaxed), -1.0, 1.0);
    const double panAngle = (pan + 1.0) * (kTwoPi / 8.0);
    const double panLeft = std::cos(panAngle);
    const double panRight = std::sin(panAngle);

    const auto clickSampleSet = guidanceActive
        ? mGuidanceClickSamples
        : std::atomic_load_explicit(&mClickSamples, std::memory_order_acquire);
    const bool hasSampleClick = clickSampleSet
        && ((!clickSampleSet->low.empty() && !clickSampleSet->low.front().empty())
            || (!clickSampleSet->high.empty() && !clickSampleSet->high.front().empty()));

    const std::string& activeBeatPattern = guidanceActive ? mGuidanceBeatPattern : mBeatPattern;

    for (int frame = 0; frame < numSamples; ++frame)
    {
        if (mSamplesUntilClick <= 0.0)
        {
            const char accent = BeatAccent(activeBeatPattern, mBeatIndex);
            const bool useHigh = (accent == 'H');
            const bool silent  = (accent == 'S');

            if (hasSampleClick)
            {
                if (!silent)
                {
                    const auto& preferred = useHigh ? clickSampleSet->high : clickSampleSet->low;
                    const auto& fallback  = useHigh ? clickSampleSet->low  : clickSampleSet->high;
                    const auto& selected  = (!preferred.empty() && !preferred.front().empty()) ? preferred : fallback;
                    mClickSamplesRemaining = selected.empty() ? 0 : static_cast<int>(selected.front().size());
                    mClickSamplePosition = 0;
                    mClickUseHigh = useHigh;
                }
                else
                {
                    mClickSamplesRemaining = 0;
                }
            }
            else
            {
                mClickSamplesRemaining = silent ? 0 : clickSamples;
            }
            mBeatIndex = (mBeatIndex + 1) % beatsPerBar;
            mSamplesUntilClick += samplesPerBeat;
        }

        float clickSampleL = 0.0f;
        float clickSampleR = 0.0f;
        if (mClickSamplesRemaining > 0)
        {
            if (hasSampleClick)
            {
                const auto& preferred = mClickUseHigh ? clickSampleSet->high : clickSampleSet->low;
                const auto& fallback = mClickUseHigh ? clickSampleSet->low : clickSampleSet->high;
                const auto& selected = (!preferred.empty() && !preferred.front().empty()) ? preferred : fallback;
                if (!selected.empty() && !selected.front().empty())
                {
                    const int index = mClickSamplePosition;
                    if (index >= 0 && static_cast<std::size_t>(index) < selected.front().size())
                    {
                        clickSampleL = selected[0][static_cast<std::size_t>(index)];
                        clickSampleR = selected.size() > 1
                            ? selected[1][static_cast<std::size_t>(index)]
                            : clickSampleL;
                    }
                }
                ++mClickSamplePosition;
                --mClickSamplesRemaining;
            }
            else
            {
                const double envelope = static_cast<double>(mClickSamplesRemaining) / static_cast<double>(clickSamples);
                const auto clickSample = static_cast<float>(std::sin(mClickPhase) * envelope);
                clickSampleL = clickSample;
                clickSampleR = clickSample;
                mClickPhase += mClickPhaseIncrement;
                if (mClickPhase >= kTwoPi)
                    mClickPhase -= kTwoPi;
                --mClickSamplesRemaining;
            }
        }

        outputs[0][frame] += clickSampleL * static_cast<float>(volume * panLeft);
        outputs[1][frame] += clickSampleR * static_cast<float>(volume * panRight);
        mSamplesUntilClick -= 1.0;
    }
}

void MetronomeService::ApplySettingsFromAppSettings()
{
    if (!mHost.IsStandalone())
        return;

    auto readNumber = [this](const char* primary, const char* legacy, double fallback, double minVal, double maxVal) {
        if (mAppSettings.contains(primary) && mAppSettings[primary].is_number())
            return ClampValue(mAppSettings[primary].get<double>(), minVal, maxVal);
        if (mAppSettings.contains(legacy) && mAppSettings[legacy].is_number())
            return ClampValue(mAppSettings[legacy].get<double>(), minVal, maxVal);
        return ClampValue(fallback, minVal, maxVal);
    };

    const double bpm = readNumber(kMetronomeBpmSettingKey, kMetronomeLegacyBpmKey,
                                  kMetronomeDefaultBpm, kMetronomeMinBpm, kMetronomeMaxBpm);
    mBpm.store(bpm, std::memory_order_release);
    mAppSettings[kMetronomeBpmSettingKey] = bpm;

    // The click never starts enabled: a session that reopens clicking is worse
    // than one the user has to switch on.
    mEnabled.store(false, std::memory_order_release);
    if (mAppSettings.contains(kMetronomeEnabledSettingKey))
        mAppSettings.erase(kMetronomeEnabledSettingKey);

    const double volumeDb = readNumber(kMetronomeVolumeDbSettingKey, kMetronomeLegacyVolumeDbKey,
                                       kMetronomeDefaultVolumeDb, kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    mVolumeDb.store(volumeDb, std::memory_order_release);
    mAppSettings[kMetronomeVolumeDbSettingKey] = volumeDb;

    const double pan = readNumber(kMetronomePanSettingKey, kMetronomeLegacyPanKey,
                                  kMetronomeDefaultPan, -1.0, 1.0);
    mPan.store(pan, std::memory_order_release);
    mAppSettings[kMetronomePanSettingKey] = pan;

    std::string clickType = kMetronomeDefaultClickType;
    if (mAppSettings.contains(kMetronomeClickTypeSettingKey) && mAppSettings[kMetronomeClickTypeSettingKey].is_string())
        clickType = mAppSettings[kMetronomeClickTypeSettingKey].get<std::string>();
    else if (mAppSettings.contains(kMetronomeLegacyClickTypeKey) && mAppSettings[kMetronomeLegacyClickTypeKey].is_string())
        clickType = mAppSettings[kMetronomeLegacyClickTypeKey].get<std::string>();

    if (!clickType.empty())
        mClickType = clickType;
    mAppSettings[kMetronomeClickTypeSettingKey] = mClickType;

    mBeatPattern.clear();
    if (mAppSettings.contains(kMetronomeBeatPatternSettingKey) && mAppSettings[kMetronomeBeatPatternSettingKey].is_string())
        mBeatPattern = mAppSettings[kMetronomeBeatPatternSettingKey].get<std::string>();
    mAppSettings[kMetronomeBeatPatternSettingKey] = mBeatPattern;

    UpdateClickConfigFromSettings();
    RefreshClickSamples(mHost.GetSampleRate());
}

void MetronomeService::UpdateClickConfigFromSettings()
{
    mClickConfig.clear();

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

            ClickTypeConfig config;
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

            mClickConfig.push_back(std::move(config));
            hasValidConfig = true;
        }
    }

    if (!hasValidConfig)
    {
        const std::array<std::tuple<std::string, std::string, std::string, std::string>, 1> defaults = {
            std::make_tuple(std::string{"drum"}, std::string{"Drum"},
                            std::string{"metronome/kit1/low.wav"}, std::string{"metronome/kit1/high.wav"}),
        };

        nlohmann::json defaultConfig = nlohmann::json::array();
        for (const auto& entry : defaults)
        {
            const auto& id = std::get<0>(entry);
            const auto& label = std::get<1>(entry);
            const auto& lowPath = std::get<2>(entry);
            const auto& highPath = std::get<3>(entry);
            ClickTypeConfig config;
            config.id = id;
            config.label = label;
            config.lowPath = resolveClickPath(lowPath);
            config.highPath = resolveClickPath(highPath);
            mClickConfig.push_back(config);

            nlohmann::json defaultEntry;
            defaultEntry["id"] = id;
            defaultEntry["label"] = label;
            defaultEntry["lowPath"] = lowPath;
            defaultEntry["highPath"] = highPath;
            defaultConfig.push_back(std::move(defaultEntry));
        }

        mAppSettings[kMetronomeClickConfigSettingKey] = std::move(defaultConfig);
    }

    if (mClickConfig.empty())
        return;

    if (mClickType.empty())
        mClickType = mClickConfig.front().id;
}

const MetronomeService::ClickTypeConfig* MetronomeService::FindClickType(const std::string& id) const
{
    for (const auto& config : mClickConfig)
    {
        if (config.id == id)
            return &config;
    }
    return mClickConfig.empty() ? nullptr : &mClickConfig.front();
}

std::shared_ptr<MetronomeService::ClickSamples>
MetronomeService::BuildClickSamples(const ClickTypeConfig& config, double targetSampleRate) const
{
    if (targetSampleRate <= 0.0)
        return nullptr;

    auto samples = std::make_shared<ClickSamples>();

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

        // Channels must be the same length: Render() indexes them together.
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

void MetronomeService::RefreshClickSamples(double sampleRate)
{
    if (!mHost.IsStandalone())
        return;

    if (mClickConfig.empty())
        UpdateClickConfigFromSettings();

    if (sampleRate <= 0.0)
        return;

    const auto* config = FindClickType(mClickType);
    if (!config)
    {
        std::atomic_store_explicit(&mClickSamples, std::shared_ptr<ClickSamples>{}, std::memory_order_release);
        return;
    }

    // FindClickType falls back to the first entry when the stored id is gone;
    // write the substitute back so the UI and settings agree with what plays.
    if (config->id != mClickType)
    {
        mClickType = config->id;
        mAppSettings[kMetronomeClickTypeSettingKey] = mClickType;
    }

    auto samples = BuildClickSamples(*config, sampleRate);
    std::atomic_store_explicit(&mClickSamples, std::move(samples), std::memory_order_release);
}

MetronomeService::RequestOutcome MetronomeService::ApplyRequest(const nlohmann::json& payload)
{
    RequestOutcome outcome;
    if (!mHost.IsStandalone())
        return outcome;

    bool resetRequired = false;
    const bool wasEnabled = mEnabled.load(std::memory_order_relaxed);

    if (payload.contains("bpm") && payload["bpm"].is_number())
    {
        const double bpm = ClampValue(payload.value("bpm", kMetronomeDefaultBpm), kMetronomeMinBpm, kMetronomeMaxBpm);
        mBpm.store(bpm, std::memory_order_release);
        mAppSettings[kMetronomeBpmSettingKey] = bpm;
        outcome.stateChanged = true;
        outcome.settingsChanged = true;
    }

    if (payload.contains("enabled") && payload["enabled"].is_boolean())
    {
        const bool enabled = payload.value("enabled", false);
        mEnabled.store(enabled, std::memory_order_release);
        // Deliberately not persisted — see ApplySettingsFromAppSettings.
        if (mAppSettings.contains(kMetronomeEnabledSettingKey))
            mAppSettings.erase(kMetronomeEnabledSettingKey);
        outcome.stateChanged = true;
        resetRequired = enabled && !wasEnabled;
    }

    if (payload.contains("volumeDb") && payload["volumeDb"].is_number())
    {
        const double volumeDb = ClampValue(payload.value("volumeDb", kMetronomeDefaultVolumeDb),
                                           kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
        mVolumeDb.store(volumeDb, std::memory_order_release);
        mAppSettings[kMetronomeVolumeDbSettingKey] = volumeDb;
        outcome.stateChanged = true;
        outcome.settingsChanged = true;
    }

    if (payload.contains("pan") && payload["pan"].is_number())
    {
        const double pan = ClampValue(payload.value("pan", kMetronomeDefaultPan), -1.0, 1.0);
        mPan.store(pan, std::memory_order_release);
        mAppSettings[kMetronomePanSettingKey] = pan;
        outcome.stateChanged = true;
        outcome.settingsChanged = true;
    }

    if (payload.contains("clickConfig") && payload["clickConfig"].is_array())
    {
        mAppSettings[kMetronomeClickConfigSettingKey] = payload["clickConfig"];
        UpdateClickConfigFromSettings();
        RefreshClickSamples(mHost.GetSampleRate());
        outcome.stateChanged = true;
        outcome.settingsChanged = true;
    }

    if (payload.contains("clickType") && payload["clickType"].is_string())
    {
        const std::string clickType = payload.value("clickType", std::string{kMetronomeDefaultClickType});
        if (!clickType.empty())
        {
            mClickType = clickType;
            mAppSettings[kMetronomeClickTypeSettingKey] = clickType;
            RefreshClickSamples(mHost.GetSampleRate());
            outcome.stateChanged = true;
            outcome.settingsChanged = true;
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
        mBeatPattern = validated;
        mAppSettings[kMetronomeBeatPatternSettingKey] = validated;
        outcome.stateChanged = true;
        outcome.settingsChanged = true;
    }

    if (outcome.stateChanged && resetRequired)
        RequestReset();

    return outcome;
}

void MetronomeService::AppendStateTo(nlohmann::json& target) const
{
    target["enabled"] = mEnabled.load();
    target["volumeDb"] = mVolumeDb.load();
    target["pan"] = mPan.load();
    target["clickType"] = mClickType;

    nlohmann::json clickTypes = nlohmann::json::array();
    for (const auto& config : mClickConfig)
        clickTypes.push_back({ {"id", config.id}, {"label", config.label} });
    target["clickTypes"] = std::move(clickTypes);
}

void MetronomeService::ActivateGuidance(const GuidanceConfig& config, bool enabled, bool forPreview)
{
    if (!mHost.IsStandalone())
        return;

    if (!enabled)
    {
        mGuidanceActive = false;
        mGuidanceForPreview = false;
        mGuidancePreviewWasActive = false;
        mGuidanceBeatScale = 1.0;
        mGuidanceClickSamples.reset();
        RequestReset();
        return;
    }

    mGuidanceActive = true;
    mGuidanceForPreview = forPreview;
    mGuidancePreviewWasActive = false;
    mGuidanceBeatPattern = config.beatPattern;
    mGuidanceBpm = ClampValue(config.tempoBpm > 0.0 ? config.tempoBpm : EffectiveTempoBpm(),
                              kMetronomeMinBpm,
                              kMetronomeMaxBpm);
    mGuidanceBeatsPerBar = std::max(1, config.timeSigNum);
    mGuidanceBeatScale = 4.0 / static_cast<double>(std::max(1, config.timeSigDen));

    const std::string clickType = config.clickType.empty()
        ? std::string{kMetronomeDefaultClickType}
        : config.clickType;
    const auto* clickConfig = FindClickType(clickType);
    const double sampleRate = mHost.GetSampleRate();
    if (clickConfig && sampleRate > 0.0)
        mGuidanceClickSamples = BuildClickSamples(*clickConfig, sampleRate);
    else
        mGuidanceClickSamples.reset();

    // Fall back to the metronome's own samples rather than the synthesised
    // beep: a capture click that sounds different from the metronome the user
    // just set up reads as a bug.
    if (!mGuidanceClickSamples)
        mGuidanceClickSamples = std::atomic_load_explicit(&mClickSamples, std::memory_order_acquire);

    RequestReset();
}

void MetronomeService::DeactivateGuidance(bool previewOnly)
{
    if (previewOnly && !mGuidanceForPreview)
        return;

    mGuidanceActive = false;
    mGuidanceForPreview = false;
    mGuidanceBeatScale = 1.0;
    mGuidanceClickSamples.reset();
    RequestReset();
}

} // namespace guitarfx
