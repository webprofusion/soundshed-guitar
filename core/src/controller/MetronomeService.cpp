#include "controller/MetronomeService.h"

#include "IPluginHost.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// The beat pulse is one atomic word so the message thread reads a beat that
/// actually happened rather than a mix of two. 14 bits of sequence is ~4500
/// bars of 4/4 between polls at 60 Hz — the counter can wrap, the comparison
/// cannot get confused by it.
constexpr std::uint32_t kBeatPulseSeqMask = 0x3FFFu;

constexpr std::uint32_t PackBeatPulse(std::uint32_t seq, int beatIndex, int beatsPerBar, BeatLevel level)
{
    return ((seq & kBeatPulseSeqMask) << 18) | ((static_cast<std::uint32_t>(level) & 0x3u) << 16) |
           ((static_cast<std::uint32_t>(beatsPerBar) & 0xFFu) << 8) | (static_cast<std::uint32_t>(beatIndex) & 0xFFu);
}

const char* BeatLevelName(BeatLevel level)
{
    switch (level)
    {
    case BeatLevel::Accent:
        return "accent";
    case BeatLevel::Medium:
        return "medium";
    case BeatLevel::Silent:
        return "silent";
    case BeatLevel::Normal:
    default:
        return "normal";
    }
}

/// Pitch offset for the synthesised fallback beep, so accents still read as
/// accents on an install whose samples are missing.
double FallbackClickFrequency(ClickVoice voice)
{
    switch (voice)
    {
    case ClickVoice::High:
        return kMetronomeClickFrequencyHz * 1.5;
    case ClickVoice::Sub:
        return kMetronomeClickFrequencyHz * 1.25;
    case ClickVoice::Low:
    default:
        return kMetronomeClickFrequencyHz;
    }
}

/// Older builds wrote their built-in kit into `metronome.clickConfig` as a
/// user override. The kit now comes from the bundled manifest, so that entry
/// would show the same sounds twice under a stale label.
void MigrateLegacyClickConfig(nlohmann::json& appSettings)
{
    const auto it = appSettings.find(kMetronomeClickConfigSettingKey);

    if (it == appSettings.end() || !it->is_array())
    {
        return;
    }

    nlohmann::json kept = nlohmann::json::array();

    for (const auto& entry : *it)
    {
        if (!entry.is_object())
        {
            continue;
        }

        const std::string id = entry.value("id", "");
        const std::string lowPath = entry.value("lowPath", "");
        const bool isLegacyDefault = (id == "drum" || id == "click") && lowPath.rfind("metronome/kit1/", 0) == 0;

        if (!isLegacyDefault)
        {
            kept.push_back(entry);
        }
    }

    if (kept.empty())
    {
        appSettings.erase(kMetronomeClickConfigSettingKey);
    }
    else
    {
        appSettings[kMetronomeClickConfigSettingKey] = std::move(kept);
    }
}
} // namespace

MetronomeService::MetronomeService(IPluginHost& host, nlohmann::json& appSettings,
                                   const std::filesystem::path& resourceRoot, SendMessageFn sendMessage)
    : mHost(host), mAppSettings(appSettings), mClickLibrary(host, resourceRoot), mSendMessage(std::move(sendMessage))
{
}

void MetronomeService::OnIdle(bool uiVisible)
{
    if (!uiVisible || !mSendMessage)
    {
        return;
    }

    BeatPulse pulse;

    if (!ConsumeBeatPulse(pulse))
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "metronomeBeat";
    msg["beatIndex"] = pulse.beatIndex;
    msg["beatsPerBar"] = pulse.beatsPerBar;
    msg["level"] = BeatLevelName(pulse.level);
    mSendMessage(msg.dump());
}

double MetronomeService::EffectiveTempoBpm() const
{
    if (mHost.IsStandalone())
    {
        return ClampValue(mBpm.load(std::memory_order_relaxed), kMetronomeMinBpm, kMetronomeMaxBpm);
    }

    const double hostTempo = mHost.GetHostTempo();

    if (hostTempo > 0.0)
    {
        return ClampValue(hostTempo, kMetronomeMinBpm, kMetronomeMaxBpm);
    }

    return kMetronomeDefaultBpm;
}

void MetronomeService::ResetTransport()
{
    mSamplesUntilTick = 0.0;
    mClickSamplesRemaining = 0;
    mClickPhase = 0.0;
    mBeatIndex = 0;
    mTickIndex = 0;
    mClickSamplePosition = 0;
    mTickVoice = ClickVoice::Low;
    mTickGain = 1.0f;
    RequestReset();
}

bool MetronomeService::ConsumeBeatPulse(BeatPulse& pulse)
{
    const std::uint32_t packed = mBeatPulse.load(std::memory_order_acquire);

    if (packed == 0)
    {
        return false;
    }

    const std::uint32_t seq = (packed >> 18) & kBeatPulseSeqMask;

    if (seq == mLastBeatPulseSeq)
    {
        return false;
    }

    mLastBeatPulseSeq = seq;
    pulse.beatIndex = static_cast<int>(packed & 0xFFu);
    pulse.beatsPerBar = static_cast<int>((packed >> 8) & 0xFFu);
    pulse.level = static_cast<BeatLevel>((packed >> 16) & 0x3u);
    return true;
}

void MetronomeService::Render(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1])
    {
        return;
    }

    if (!mHost.IsStandalone())
    {
        return;
    }

    const bool guidanceActive = mGuidanceActive;

    if (!guidanceActive && !mEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto plan =
        guidanceActive ? mGuidanceBarPlan : std::atomic_load_explicit(&mBarPlan, std::memory_order_acquire);

    if (!plan || plan->beats.empty())
    {
        return;
    }

    if (mResetPending.exchange(false, std::memory_order_acq_rel))
    {
        mSamplesUntilTick = 0.0;
        mClickSamplesRemaining = 0;
        mClickPhase = 0.0;
        mBeatIndex = 0;
        mTickIndex = 0;
        mClickSamplePosition = 0;
        mTickVoice = ClickVoice::Low;
        mTickGain = 1.0f;
    }

    const double sampleRate = mHost.GetSampleRate();

    if (sampleRate <= 0.0)
    {
        return;
    }

    const double bpm =
        guidanceActive ? ClampValue(mGuidanceBpm, kMetronomeMinBpm, kMetronomeMaxBpm) : EffectiveTempoBpm();
    const auto beatsPerBar = static_cast<int>(plan->beats.size());
    const int ticksPerBeat = std::max(1, plan->ticksPerBeat);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, bpm)) * std::max(0.125, plan->beatScale);
    const double samplesPerTick = samplesPerBeat / static_cast<double>(ticksPerBeat);
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * kMetronomeClickSeconds));

    const double volumeDb =
        ClampValue(mVolumeDb.load(std::memory_order_relaxed), kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    const double volume = ClampValue(LinearFromDb(volumeDb), 0.0, LinearFromDb(kMetronomeMaxVolumeDb));
    const double pan = ClampValue(mPan.load(std::memory_order_relaxed), -1.0, 1.0);
    const double panAngle = (pan + 1.0) * (kTwoPi / 8.0);
    const double panLeft = std::cos(panAngle);
    const double panRight = std::sin(panAngle);

    const auto clickSampleSet =
        guidanceActive ? mGuidanceClickSamples : std::atomic_load_explicit(&mClickSamples, std::memory_order_acquire);
    const bool hasSampleClick = clickSampleSet && !clickSampleSet->Empty();

    for (int frame = 0; frame < numSamples; ++frame)
    {
        if (mSamplesUntilTick <= 0.0)
        {
            const bool onBeat = (mTickIndex == 0);
            const int beatIndex = std::min(mBeatIndex, beatsPerBar - 1);
            const auto& beat = plan->beats[static_cast<std::size_t>(beatIndex)];
            const bool silent = onBeat && beat.silent;

            mTickVoice = onBeat ? beat.voice : ClickVoice::Sub;
            mTickGain = onBeat ? beat.gain : plan->subGain;

            if (onBeat)
            {
                mBeatPulse.store(PackBeatPulse(++mBeatPulseSeq, beatIndex, beatsPerBar, beat.level),
                                 std::memory_order_release);
            }

            if (silent)
            {
                mClickSamplesRemaining = 0;
            }
            else if (hasSampleClick)
            {
                const auto* selected = clickSampleSet->Voice(mTickVoice);
                mClickSamplesRemaining = selected ? static_cast<int>(selected->front().size()) : 0;
                mClickSamplePosition = 0;
            }
            else
            {
                mClickSamplesRemaining = clickSamples;
                mClickPhase = 0.0;
                mClickPhaseIncrement = kTwoPi * FallbackClickFrequency(mTickVoice) / sampleRate;
            }

            ++mTickIndex;

            if (mTickIndex >= ticksPerBeat)
            {
                mTickIndex = 0;
                mBeatIndex = (beatIndex + 1) % beatsPerBar;
            }

            mSamplesUntilTick += samplesPerTick;
        }

        float clickSampleL = 0.0f;
        float clickSampleR = 0.0f;

        if (mClickSamplesRemaining > 0)
        {
            if (hasSampleClick)
            {
                const auto* selected = clickSampleSet->Voice(mTickVoice);

                if (selected != nullptr)
                {
                    const auto index = static_cast<std::size_t>(mClickSamplePosition);

                    if (mClickSamplePosition >= 0 && index < selected->front().size())
                    {
                        clickSampleL = (*selected)[0][index];
                        clickSampleR = selected->size() > 1 ? (*selected)[1][index] : clickSampleL;
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
                {
                    mClickPhase -= kTwoPi;
                }

                --mClickSamplesRemaining;
            }

            clickSampleL *= mTickGain;
            clickSampleR *= mTickGain;
        }

        outputs[0][frame] += clickSampleL * static_cast<float>(volume * panLeft);
        outputs[1][frame] += clickSampleR * static_cast<float>(volume * panRight);
        mSamplesUntilTick -= 1.0;
    }
}

void MetronomeService::RefreshBarPlan()
{
    auto plan =
        std::make_shared<const BarPlan>(BuildBarPlan(mTimeSigNum, mTimeSigDen, mGrouping, mBeatPattern, mSubdivision));
    std::atomic_store_explicit(&mBarPlan, std::move(plan), std::memory_order_release);
}

void MetronomeService::LoadMeterFromAppSettings()
{
    const auto readInt = [this](const char* key, int fallback) {
        if (mAppSettings.contains(key) && mAppSettings[key].is_number_integer())
        {
            return mAppSettings[key].get<int>();
        }

        return fallback;
    };

    const auto readString = [this](const char* key) {
        if (mAppSettings.contains(key) && mAppSettings[key].is_string())
        {
            return mAppSettings[key].get<std::string>();
        }

        return std::string{};
    };

    mTimeSigNum = ClampBeatsPerBar(readInt(kMetronomeTimeSigNumSettingKey, kMetronomeDefaultTimeSigNum));
    mTimeSigDen = ClampTimeSigDen(readInt(kMetronomeTimeSigDenSettingKey, kMetronomeDefaultTimeSigDen));
    mGrouping = NormaliseGrouping(readString(kMetronomeGroupingSettingKey), mTimeSigNum);
    mSubdivision = NormaliseSubdivisionId(readString(kMetronomeSubdivisionSettingKey));
    mBeatPattern =
        NormaliseBeatPattern(readString(kMetronomeBeatPatternSettingKey), mTimeSigNum, mTimeSigDen, mGrouping);
}

void MetronomeService::StoreMeterToAppSettings()
{
    mAppSettings[kMetronomeTimeSigNumSettingKey] = mTimeSigNum;
    mAppSettings[kMetronomeTimeSigDenSettingKey] = mTimeSigDen;
    mAppSettings[kMetronomeGroupingSettingKey] = mGrouping;
    mAppSettings[kMetronomeSubdivisionSettingKey] = mSubdivision;
    mAppSettings[kMetronomeBeatPatternSettingKey] = mBeatPattern;
}

void MetronomeService::ApplySettingsFromAppSettings()
{
    if (!mHost.IsStandalone())
    {
        return;
    }

    auto readNumber = [this](const char* primary, const char* legacy, double fallback, double minVal, double maxVal) {
        if (mAppSettings.contains(primary) && mAppSettings[primary].is_number())
        {
            return ClampValue(mAppSettings[primary].get<double>(), minVal, maxVal);
        }

        if (mAppSettings.contains(legacy) && mAppSettings[legacy].is_number())
        {
            return ClampValue(mAppSettings[legacy].get<double>(), minVal, maxVal);
        }

        return ClampValue(fallback, minVal, maxVal);
    };

    const double bpm = readNumber(kMetronomeBpmSettingKey, kMetronomeLegacyBpmKey, kMetronomeDefaultBpm,
                                  kMetronomeMinBpm, kMetronomeMaxBpm);
    mBpm.store(bpm, std::memory_order_release);
    mAppSettings[kMetronomeBpmSettingKey] = bpm;

    // The click never starts enabled: a session that reopens clicking is worse
    // than one the user has to switch on.
    mEnabled.store(false, std::memory_order_release);

    if (mAppSettings.contains(kMetronomeEnabledSettingKey))
    {
        mAppSettings.erase(kMetronomeEnabledSettingKey);
    }

    const double volumeDb = readNumber(kMetronomeVolumeDbSettingKey, kMetronomeLegacyVolumeDbKey,
                                       kMetronomeDefaultVolumeDb, kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    mVolumeDb.store(volumeDb, std::memory_order_release);
    mAppSettings[kMetronomeVolumeDbSettingKey] = volumeDb;

    const double pan = readNumber(kMetronomePanSettingKey, kMetronomeLegacyPanKey, kMetronomeDefaultPan, -1.0, 1.0);
    mPan.store(pan, std::memory_order_release);
    mAppSettings[kMetronomePanSettingKey] = pan;

    std::string clickType = kMetronomeDefaultClickType;

    if (mAppSettings.contains(kMetronomeClickTypeSettingKey) && mAppSettings[kMetronomeClickTypeSettingKey].is_string())
    {
        clickType = mAppSettings[kMetronomeClickTypeSettingKey].get<std::string>();
    }
    else if (mAppSettings.contains(kMetronomeLegacyClickTypeKey) &&
             mAppSettings[kMetronomeLegacyClickTypeKey].is_string())
    {
        clickType = mAppSettings[kMetronomeLegacyClickTypeKey].get<std::string>();
    }

    // The bundled kit used to be called "drum"; it is now listed by folder.
    if (clickType == "drum" || clickType == "click")
    {
        clickType = kMetronomeDefaultClickType;
    }

    if (!clickType.empty())
    {
        mClickType = clickType;
    }

    mAppSettings[kMetronomeClickTypeSettingKey] = mClickType;

    MigrateLegacyClickConfig(mAppSettings);
    LoadMeterFromAppSettings();
    StoreMeterToAppSettings();
    RefreshBarPlan();

    const auto overrides = mAppSettings.find(kMetronomeClickConfigSettingKey);
    mClickLibrary.Rebuild(overrides == mAppSettings.end() ? nullptr : &(*overrides));
    RefreshClickSamples(mHost.GetSampleRate());
}

void MetronomeService::RefreshClickSamples(double sampleRate)
{
    if (!mHost.IsStandalone())
    {
        return;
    }

    if (mClickLibrary.Empty())
    {
        const auto overrides = mAppSettings.find(kMetronomeClickConfigSettingKey);
        mClickLibrary.Rebuild(overrides == mAppSettings.end() ? nullptr : &(*overrides));
    }

    if (!mBarPlan)
    {
        RefreshBarPlan();
    }

    if (sampleRate <= 0.0)
    {
        return;
    }

    const auto* config = mClickLibrary.Find(mClickType);

    if (!config)
    {
        std::atomic_store_explicit(&mClickSamples, std::shared_ptr<ClickSamples>{}, std::memory_order_release);
        return;
    }

    // Find() falls back to the first entry when the stored id is gone; write
    // the substitute back so the UI and settings agree with what plays.
    if (config->id != mClickType)
    {
        mClickType = config->id;
        mAppSettings[kMetronomeClickTypeSettingKey] = mClickType;
    }

    auto samples = mClickLibrary.Load(*config, sampleRate);
    std::atomic_store_explicit(&mClickSamples, std::move(samples), std::memory_order_release);
}

MetronomeService::RequestOutcome MetronomeService::ApplyRequest(const nlohmann::json& payload)
{
    RequestOutcome outcome;

    if (!mHost.IsStandalone())
    {
        return outcome;
    }

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
        {
            mAppSettings.erase(kMetronomeEnabledSettingKey);
        }

        outcome.stateChanged = true;
        resetRequired = enabled && !wasEnabled;
    }

    if (payload.contains("volumeDb") && payload["volumeDb"].is_number())
    {
        const double volumeDb = ClampValue(payload.value("volumeDb", kMetronomeDefaultVolumeDb), kMetronomeMinVolumeDb,
                                           kMetronomeMaxVolumeDb);
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
        mClickLibrary.Rebuild(&mAppSettings[kMetronomeClickConfigSettingKey]);
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

    // The meter, its grouping, the accent pattern and the subdivision only
    // mean anything together, so they are resolved as one and republished as a
    // single bar plan.
    const bool hasPattern = payload.contains("beatPattern") && payload["beatPattern"].is_string();
    int nextNum = mTimeSigNum;
    int nextDen = mTimeSigDen;
    std::string nextGrouping = mGrouping;

    if (payload.contains("timeSigNum") && payload["timeSigNum"].is_number_integer())
    {
        nextNum = ClampBeatsPerBar(payload.value("timeSigNum", mTimeSigNum));
    }

    if (payload.contains("timeSigDen") && payload["timeSigDen"].is_number_integer())
    {
        nextDen = ClampTimeSigDen(payload.value("timeSigDen", mTimeSigDen));
    }

    if (payload.contains("grouping") && payload["grouping"].is_string())
    {
        nextGrouping = payload.value("grouping", std::string{});
    }

    nextGrouping = NormaliseGrouping(nextGrouping, nextNum);

    const bool meterChanged = (nextNum != mTimeSigNum) || (nextDen != mTimeSigDen) || (nextGrouping != mGrouping);
    bool planChanged = false;

    if (meterChanged)
    {
        mTimeSigNum = nextNum;
        mTimeSigDen = nextDen;
        mGrouping = nextGrouping;

        // A pattern is one character per beat, so it cannot survive a meter
        // change. Re-seed it unless this same request supplies a new one.
        if (!hasPattern)
        {
            mBeatPattern = DefaultBeatPattern(mTimeSigNum, mTimeSigDen, mGrouping);
        }

        planChanged = true;
    }

    if (hasPattern)
    {
        mBeatPattern =
            NormaliseBeatPattern(payload.value("beatPattern", std::string{}), mTimeSigNum, mTimeSigDen, mGrouping);
        planChanged = true;
    }

    if (payload.contains("subdivision") && payload["subdivision"].is_string())
    {
        const std::string subdivision = NormaliseSubdivisionId(payload.value("subdivision", std::string{}));

        if (subdivision != mSubdivision)
        {
            mSubdivision = subdivision;
            planChanged = true;
        }
    }

    if (planChanged)
    {
        RefreshBarPlan();
        StoreMeterToAppSettings();
        outcome.stateChanged = true;
        outcome.settingsChanged = true;

        // Restart the bar so beat one lands where the new meter says it does.
        // Guidance owns the cursor while a take is running, so leave it alone.
        if (meterChanged && !mGuidanceActive)
        {
            resetRequired = true;
        }
    }

    if (outcome.stateChanged && resetRequired)
    {
        RequestReset();
    }

    return outcome;
}

void MetronomeService::AppendStateTo(nlohmann::json& target) const
{
    target["enabled"] = mEnabled.load();
    target["volumeDb"] = mVolumeDb.load();
    target["pan"] = mPan.load();
    target["clickType"] = mClickType;
    target["beatPattern"] = mBeatPattern;
    target["timeSigNum"] = mTimeSigNum;
    target["timeSigDen"] = mTimeSigDen;
    target["grouping"] = mGrouping;
    target["subdivision"] = mSubdivision;

    nlohmann::json clickTypes = nlohmann::json::array();

    for (const auto& config : mClickLibrary.Types())
    {
        clickTypes.push_back({{"id", config.id}, {"label", config.label}});
    }

    target["clickTypes"] = std::move(clickTypes);

    nlohmann::json subdivisions = nlohmann::json::array();

    for (const auto& option : kMetronomeSubdivisions)
    {
        subdivisions.push_back({{"id", option.id}, {"ticksPerBeat", option.ticksPerBeat}});
    }

    target["subdivisions"] = std::move(subdivisions);
}

void MetronomeService::ActivateGuidance(const GuidanceConfig& config, bool enabled, bool forPreview)
{
    if (!mHost.IsStandalone())
    {
        return;
    }

    if (!enabled)
    {
        mGuidanceActive = false;
        mGuidanceForPreview = false;
        mGuidancePreviewWasActive = false;
        mGuidanceBarPlan.reset();
        mGuidanceClickSamples.reset();
        RequestReset();
        return;
    }

    mGuidanceActive = true;
    mGuidanceForPreview = forPreview;
    mGuidancePreviewWasActive = false;
    mGuidanceBpm =
        ClampValue(config.tempoBpm > 0.0 ? config.tempoBpm : EffectiveTempoBpm(), kMetronomeMinBpm, kMetronomeMaxBpm);

    // A take counts plain beats: the accents come from the capture's own
    // pattern, and subdivisions would only fight the player's timing.
    mGuidanceBarPlan = std::make_shared<const BarPlan>(BuildBarPlan(config.timeSigNum, config.timeSigDen, std::string{},
                                                                    config.beatPattern, kMetronomeDefaultSubdivision));

    const std::string clickType = config.clickType.empty() ? mClickType : config.clickType;
    const auto* clickConfig = mClickLibrary.Find(clickType);
    const double sampleRate = mHost.GetSampleRate();

    if (clickConfig && sampleRate > 0.0)
    {
        mGuidanceClickSamples = mClickLibrary.Load(*clickConfig, sampleRate);
    }
    else
    {
        mGuidanceClickSamples.reset();
    }

    // Fall back to the metronome's own samples rather than the synthesised
    // beep: a capture click that sounds different from the metronome the user
    // just set up reads as a bug.
    if (!mGuidanceClickSamples)
    {
        mGuidanceClickSamples = std::atomic_load_explicit(&mClickSamples, std::memory_order_acquire);
    }

    RequestReset();
}

void MetronomeService::DeactivateGuidance(bool previewOnly)
{
    if (previewOnly && !mGuidanceForPreview)
    {
        return;
    }

    mGuidanceActive = false;
    mGuidanceForPreview = false;
    mGuidanceBarPlan.reset();
    mGuidanceClickSamples.reset();
    RequestReset();
}
} // namespace guitarfx
