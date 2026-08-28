#pragma once

// MetronomeService — the click track, and the count-in guidance click that
// riff capture plays over it.
//
// Both are the same engine. Guidance is not a second metronome: while it is
// active it overrides the click's tempo, bar length and pattern, and supplies
// its own sample set, so the two can never sound at once and there is only one
// beat cursor to keep in step. Deactivating guidance hands the engine back to
// the user's own metronome settings.
//
// Standalone only. Hosted, the DAW owns the tempo and provides the click, so
// Render() returns immediately and nothing here is loaded.
//
// Threading. Render() is the audio thread. It reads the click sample set
// through an atomic shared_ptr load, so RefreshClickSamples() on the message
// thread can swap in a new set without tearing — the audio thread keeps the
// old one alive for the duration of the block it is already using. The
// transport cursor (mSamplesUntilClick and friends) is touched only by
// Render(); the message thread asks for a restart with RequestReset() rather
// than writing it, which is why that one flag is atomic and the rest are not.

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{

class IPluginHost;

class MetronomeService
{
  public:
    struct ClickTypeConfig
    {
        std::string id;
        std::string label;
        std::filesystem::path lowPath;
        std::filesystem::path highPath;
    };

    struct ClickSamples
    {
        std::vector<std::vector<float>> low;
        std::vector<std::vector<float>> high;
    };

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

    MetronomeService(IPluginHost& host, nlohmann::json& appSettings, const std::filesystem::path& resourceRoot);

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

    // ── Settings ────────────────────────────────────────────────────

    /// Loads bpm/volume/pan/click type/pattern out of app settings, writing
    /// back the normalised values, then reloads the click samples.
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
    [[nodiscard]] const ClickTypeConfig* FindClickType(const std::string& id) const;
    [[nodiscard]] std::shared_ptr<ClickSamples> BuildClickSamples(const ClickTypeConfig& config,
                                                                  double targetSampleRate) const;
    void UpdateClickConfigFromSettings();

    IPluginHost& mHost;
    nlohmann::json& mAppSettings;
    const std::filesystem::path& mResourceRoot;

    // User-facing settings.
    std::atomic<double> mBpm{120.0};
    std::atomic<bool> mEnabled{false};
    std::atomic<double> mVolumeDb{-12.0};
    std::atomic<double> mPan{0.0};
    std::string mClickType{"click"};
    std::string mBeatPattern; // e.g. "HLLL"

    std::vector<ClickTypeConfig> mClickConfig;
    std::shared_ptr<ClickSamples> mClickSamples;

    // Beat cursor. Audio thread only, apart from mResetPending.
    std::atomic<bool> mResetPending{false};
    double mSamplesUntilClick = 0.0;
    int mClickSamplesRemaining = 0;
    double mClickPhase = 0.0;
    double mClickPhaseIncrement = 0.0;
    int mBeatIndex = 0;
    int mClickSamplePosition = 0;
    bool mClickUseHigh = false;

    // Guidance overrides, set from the message thread while guidance starts.
    bool mGuidanceActive = false;
    bool mGuidanceForPreview = false;
    bool mGuidancePreviewWasActive = false;
    double mGuidanceBpm = 120.0;
    int mGuidanceBeatsPerBar = 4;
    double mGuidanceBeatScale = 1.0;
    std::string mGuidanceBeatPattern;
    std::shared_ptr<ClickSamples> mGuidanceClickSamples;
};

} // namespace guitarfx
