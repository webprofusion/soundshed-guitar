#pragma once

// MetronomeService — the click track, and the count-in guidance click that
// riff capture plays over it.
//
// Both are the same engine. Guidance is not a second metronome: while it is
// active it overrides the click's tempo, bar plan and sample set, so the two
// can never sound at once and there is only one beat cursor to keep in step.
// Deactivating guidance hands the engine back to the user's own settings.
//
// Standalone only. Hosted, the DAW owns the tempo and provides the click, so
// Render() returns immediately and nothing here is loaded.
//
// Threading. Render() is the audio thread. Everything it needs that the user
// can change — the sample set and the bar plan (meter, accents, subdivision)
// — is an immutable object published through an atomic shared_ptr, so the
// message thread swaps in a replacement without tearing and the audio thread
// keeps the one it is already using alive for the block. Simple scalars are
// atomics. The transport cursor is touched only by Render(); the message
// thread asks for a restart with RequestReset() rather than writing it.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "controller/MetronomeClickLibrary.h"
#include "controller/internal/MetronomeSupport.h"

namespace guitarfx
{
class IPluginHost;

class MetronomeService
{
  public:
    using ClickSamples = MetronomeClickLibrary::ClickSamples;
    using BarPlan = controller_detail::MetronomeBarPlan;
    using SendMessageFn = std::function<void(const std::string&)>;

    /// What riff capture wants the click to do for the duration of a take.
    struct GuidanceConfig
    {
        double tempoBpm = 0.0; ///< <= 0 means "follow the current tempo"
        int timeSigNum = 4;
        int timeSigDen = 4;
        std::string beatPattern;
        std::string clickType; ///< empty means the default click
    };

    /// What a Set-metronome request changed, so the caller knows whether to
    /// broadcast state, persist settings, or both.
    struct RequestOutcome
    {
        bool stateChanged = false;
        bool settingsChanged = false;
    };

    /// A beat the engine has just played, for the UI's beat display.
    struct BeatPulse
    {
        int beatIndex = 0;
        int beatsPerBar = 4;
        controller_detail::BeatLevel level = controller_detail::BeatLevel::Normal;
    };

    MetronomeService(IPluginHost& host, nlohmann::json& appSettings, const std::filesystem::path& resourceRoot,
                     SendMessageFn sendMessage = {});

    /// Sends the beat the click last played to the UI, at most one message per
    /// beat. Called from the idle tick. `uiVisible` false sends nothing: this
    /// drives the on-screen beat display and nothing else.
    void OnIdle(bool uiVisible);

    // ── Tempo ───────────────────────────────────────────────────────

    /// Standalone uses the user's own tempo; hosted follows the DAW, falling
    /// back to the default when the host reports none. The single place that
    /// decision is made.
    [[nodiscard]] double EffectiveTempoBpm() const;

    // ── Audio thread ────────────────────────────────────────────────

    /// Mixes the click on top of already-processed audio. No-op when hosted,
    /// or when neither the metronome nor guidance is running.
    void Render(float** outputs, int numSamples);

    // ── Transport ───────────────────────────────────────────────────

    /// Restarts the beat cursor on the next block. Safe from any thread.
    void RequestReset()
    {
        mResetPending.store(true, std::memory_order_release);
    }

    /// Clears the cursor outright. Message thread only, and only while the
    /// audio thread is not running (Prepare holds the DSP lock).
    void ResetTransport();

    /// The beat the engine last played, once per beat. False when nothing has
    /// sounded since the previous call. Message thread only.
    [[nodiscard]] bool ConsumeBeatPulse(BeatPulse& pulse);

    // ── Settings ────────────────────────────────────────────────────

    /// Loads bpm/volume/pan/click type/meter/pattern out of app settings,
    /// writing back the normalised values, then reloads the click samples.
    void ApplySettingsFromAppSettings();

    /// Rebuilds the click sample set for a new sample rate or click type.
    void RefreshClickSamples(double sampleRate);

    /// Applies a `setMetronome` payload.
    RequestOutcome ApplyRequest(const nlohmann::json& payload);

    /// Adds the UI-facing metronome state to `target`.
    void AppendStateTo(nlohmann::json& target) const;

    [[nodiscard]] const std::string& BeatPattern() const
    {
        return mBeatPattern;
    }

    // ── Riff capture guidance ───────────────────────────────────────

    /// Takes the click over for a capture. `enabled` false stops guidance
    /// instead, which is what a take with the click switched off wants.
    void ActivateGuidance(const GuidanceConfig& config, bool enabled, bool forPreview);

    /// `previewOnly` deactivates only if guidance was started for a preview,
    /// leaving a live capture's guidance running.
    void DeactivateGuidance(bool previewOnly);

    [[nodiscard]] bool IsGuidanceForPreview() const
    {
        return mGuidanceForPreview;
    }

    [[nodiscard]] bool GuidancePreviewWasActive() const
    {
        return mGuidancePreviewWasActive;
    }

    void SetGuidancePreviewWasActive(bool value)
    {
        mGuidancePreviewWasActive = value;
    }

  private:
    /// Rebuilds and publishes the bar plan from the current meter settings.
    void RefreshBarPlan();

    /// Reads meter/pattern/subdivision out of app settings, normalising and
    /// writing back what it read. Returns false when nothing was stored yet.
    void LoadMeterFromAppSettings();

    /// Stores the meter settings back so a reload starts where we left off.
    void StoreMeterToAppSettings();

    IPluginHost& mHost;
    nlohmann::json& mAppSettings;
    MetronomeClickLibrary mClickLibrary;
    SendMessageFn mSendMessage;

    // User-facing settings.
    std::atomic<double> mBpm{controller_detail::kMetronomeDefaultBpm};
    std::atomic<bool> mEnabled{false};
    std::atomic<double> mVolumeDb{controller_detail::kMetronomeDefaultVolumeDb};
    std::atomic<double> mPan{controller_detail::kMetronomeDefaultPan};
    std::string mClickType{controller_detail::kMetronomeDefaultClickType};
    std::string mBeatPattern; // one character per beat, e.g. "HLML"
    std::string mGrouping;    // e.g. "2+2+3", empty when the meter is even
    std::string mSubdivision{controller_detail::kMetronomeDefaultSubdivision};
    int mTimeSigNum = controller_detail::kMetronomeDefaultTimeSigNum;
    int mTimeSigDen = controller_detail::kMetronomeDefaultTimeSigDen;

    std::shared_ptr<const BarPlan> mBarPlan;
    std::shared_ptr<ClickSamples> mClickSamples;

    // Beat cursor. Audio thread only, apart from mResetPending and mBeatPulse.
    std::atomic<bool> mResetPending{false};
    std::atomic<std::uint32_t> mBeatPulse{0};
    std::uint32_t mBeatPulseSeq = 0;     // audio thread
    std::uint32_t mLastBeatPulseSeq = 0; // message thread
    double mSamplesUntilTick = 0.0;
    int mClickSamplesRemaining = 0;
    double mClickPhase = 0.0;
    double mClickPhaseIncrement = 0.0;
    int mBeatIndex = 0;
    int mTickIndex = 0;
    int mClickSamplePosition = 0;
    float mTickGain = 1.0f;
    controller_detail::ClickVoice mTickVoice = controller_detail::ClickVoice::Low;

    // Guidance overrides, set from the message thread while guidance starts.
    bool mGuidanceActive = false;
    bool mGuidanceForPreview = false;
    bool mGuidancePreviewWasActive = false;
    double mGuidanceBpm = controller_detail::kMetronomeDefaultBpm;
    std::shared_ptr<const BarPlan> mGuidanceBarPlan;
    std::shared_ptr<ClickSamples> mGuidanceClickSamples;
};
} // namespace guitarfx
