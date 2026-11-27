#pragma once

/**
 * PracticeTool.h - The Jam tab's Practice Tool as a native UI drives it: a backing track the
 * engine plays (PracticeToolService), with loops, four faders (Volume, Balance, Speed, Pitch), a
 * backing-track EQ and saved projects kept on the UI side, exactly as the web UI's
 * practiceTool.ts and practiceTool/*.ts keep them. The loops and projects live in the same app
 * settings (practiceTool.loops, practiceTool.projects), so either product picks up a session
 * the other saved.
 *
 * The pure helpers below are the web UI's rules one for one; PracticeTool (the class) is the
 * live session: it follows the engine's reports (Topic::PracticeTool) and sends what each user
 * action sends. UI thread only.
 */

#include "uiclient/ClientState.h"
#include "uiclient/UiClient.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class UiCommands;

namespace practice
{
inline constexpr const char* kLoopsSetting = "practiceTool.loops";
inline constexpr const char* kProjectsSetting = "practiceTool.projects";
inline constexpr double kMinLoopSpanSec = 0.25;
inline constexpr double kDefaultNewLoopSec = 4.0;
inline constexpr int kMaxProjects = 200;
inline constexpr int kMaxLinks = 30;
/// Song sections offered for a loop's name.
inline const std::vector<std::string> kLoopNameTemplates = {"Intro", "Verse", "Pre-Chorus", "Chorus",   "Bridge",
                                                            "Solo",  "Outro", "Turnaround", "Breakdown"};

struct LoopSettings
{
    double gain = 1.0;
    double balance = 0.0;
    double speed = 1.0;
    double pitchSemitones = 0.0;
};

struct Loop
{
    std::string id;
    std::string name;
    double startSec = 0.0;
    double endSec = 0.0;
    /// Applied whenever the loop is picked; absent on loops saved before loops had settings.
    std::optional<LoopSettings> settings;
};

/// The backing-track EQ: ParametricEQEffect's own parameter names.
struct Eq
{
    bool enabled = false;
    std::map<std::string, double> params;
};

struct Link
{
    std::string id;
    std::string title; // may be empty: the row shows the host
    std::string url;
};

struct Project
{
    std::string id;
    std::string name;
    std::string filePath;
    std::string fileTitle;
    double durationSec = 0.0;
    std::vector<Loop> loops;
    std::string activeLoopId;
    LoopSettings faders;
    std::optional<Eq> eq; // absent in projects saved before the EQ existed
    std::string presetId;
    std::string presetName;
    std::vector<Link> links;
    double savedAt = 0.0; // ms since the epoch, as Date.now()
    /// The YouTube video the project follows (the web UI's `youtube`), when it has one. A
    /// project follows a backing track, a video, or both; with only a video, filePath is empty.
    std::string youtubeVideoId;
    std::string youtubeTitle;
    /// Every field this client does not edit (the tab binding, the YouTube reference, anything a
    /// newer app adds), as it was read: written back unchanged, so saving here never drops what
    /// the web UI saved. The projects list is one shared setting, rewritten whole on every edit.
    nlohmann::json kept = nlohmann::json::object();
};

// ── The faders ──────────────────────────────────────────────────────────────────
enum class Fader
{
    Volume,
    Balance,
    Speed,
    Pitch
};

struct FaderSpec
{
    const char* label;
    double min, max, def;
};

[[nodiscard]] const FaderSpec& SpecFor(Fader fader);
/// A slider's place (0..1) for a value: each half scaled on its own, so the default sits in
/// the middle whatever the range.
[[nodiscard]] double FaderToSlider(Fader fader, double value);
[[nodiscard]] double SliderToFader(Fader fader, double position);
[[nodiscard]] std::string FormatFader(Fader fader, double value);
/// What a typed value means ("85%", "L 30", "-2 st"), or nothing.
[[nodiscard]] std::optional<double> ParseFader(Fader fader, const std::string& text);
[[nodiscard]] double FaderValue(const LoopSettings& settings, Fader fader);
void SetFaderValue(LoopSettings& settings, Fader fader, double value);

// ── Loops, projects, links ──────────────────────────────────────────────────────
/// What loops are stored under: the path and the duration.
[[nodiscard]] std::string FileFingerprint(const std::string& filePath, double durationSec);
[[nodiscard]] std::vector<Loop> LoadLoops(const nlohmann::json& appSettings, const std::string& fingerprint);
/// The whole practiceTool.loops value with this file's loops replaced (removed when empty).
[[nodiscard]] nlohmann::json LoopsSettingWith(const nlohmann::json& appSettings, const std::string& fingerprint,
                                              const std::vector<Loop>& loops);
[[nodiscard]] std::optional<Loop> ParseLoop(const nlohmann::json& value);
[[nodiscard]] nlohmann::json LoopJson(const Loop& loop);

[[nodiscard]] std::optional<Project> ParseProject(const nlohmann::json& value);
[[nodiscard]] nlohmann::json ProjectJson(const Project& project);
/// Newest first.
[[nodiscard]] std::vector<Project> ReadProjects(const nlohmann::json& appSettings);
[[nodiscard]] nlohmann::json ProjectsJson(const std::vector<Project>& projects);

/// "Verse" -> "Verse 1", then "Verse 2", ... whatever names exist.
[[nodiscard]] std::string SuggestLoopTemplateName(const std::string& base, const std::vector<std::string>& existing);
[[nodiscard]] std::string SuggestDefaultLoopName(const std::vector<Loop>& loops);

[[nodiscard]] Eq DefaultEq();
[[nodiscard]] Eq SanitizeEq(const nlohmann::json& value);
[[nodiscard]] nlohmann::json EqJson(const Eq& eq);
[[nodiscard]] nlohmann::json EqParamsJson(const Eq& eq);
/// On and doing something (a band off 0 dB).
[[nodiscard]] bool IsEqShaping(const Eq& eq);

/// http(s) only; a bare host gets https://. Nothing for anything else.
[[nodiscard]] std::optional<std::string> NormalizeLinkUrl(const std::string& input);
/// The title, or the host without "www.".
[[nodiscard]] std::string DescribeLink(const Link& link);

/// "1:05".
[[nodiscard]] std::string FormatClock(double seconds);
/// Where Jump Back goes: the point last clicked while it is inside the active loop (or with
/// no loop), else the loop's start or the top of the track.
[[nodiscard]] double JumpBackTarget(std::optional<double> clicked, const Loop* activeLoop);
/// Whether a project's path can be opened again (a dropped-in file left only its name).
[[nodiscard]] bool IsReloadablePath(const std::string& path);
[[nodiscard]] bool IsSameTrackPath(const std::string& a, const std::string& b);
} // namespace practice

class PracticeTool
{
  public:
    PracticeTool(UiClient& client, UiCommands& commands);

    PracticeTool(const PracticeTool&) = delete;
    PracticeTool& operator=(const PracticeTool&) = delete;

    /// Told after anything here changes (not the playhead: views read PositionNow).
    std::function<void()> onChanged;
    /// A message for the user ("Load an audio file first").
    std::function<void(const std::string& title, const std::string& detail)> onNotice;

    /// Sends what waits on a pause in a drag (speed, pitch, a loop edge). Call each UI tick.
    void Tick(double now);

    [[nodiscard]] const PracticeToolTrack& Track() const;
    [[nodiscard]] bool HasAudio() const;
    /// The playhead now: the last report moved on by the time since, at the current speed.
    [[nodiscard]] double PositionNow(double now) const;

    // ── Transport ──
    void Browse();
    void PlayPause();
    void Stop();
    /// Seeks, playing or not. A click outside the active loop lets it go and marks a default
    /// length from there to make the next loop of; it is also the point Jump Back returns to.
    void ClickAt(double seconds);
    void JumpBack();
    [[nodiscard]] std::optional<double> JumpBackMarker() const;

    // ── Faders ──
    [[nodiscard]] const practice::LoopSettings& Faders() const
    {
        return mFaders;
    }

    /// `immediate` on release, reset or a typed value; a drag's ticks wait for a pause before
    /// speed and pitch go (they flush the engine's render-ahead).
    void SetFader(practice::Fader fader, double value, bool immediate);

    // ── EQ ──
    [[nodiscard]] const practice::Eq& GetEq() const
    {
        return mEq;
    }

    void SetEqEnabled(bool enabled);
    void SetEqParam(const std::string& key, double value);
    void ResetEq();

    // ── Loops ──
    [[nodiscard]] const std::vector<practice::Loop>& Loops() const
    {
        return mLoops;
    }

    [[nodiscard]] const practice::Loop* ActiveLoop() const;

    /// A range marked on the waveform not yet made a loop, in seconds.
    [[nodiscard]] const std::optional<std::pair<double, double>>& Candidate() const
    {
        return mCandidate;
    }

    /// A drag on the waveform: moves the active loop's edges, or marks a candidate.
    void SetRange(double startSec, double endSec, bool done);
    /// A sweep begun outside the active loop lets the loop go.
    void ReleaseActiveLoop();
    /// Makes a loop of the candidate, or of the default length from the playhead; it starts
    /// out picked and looping, with the current fader settings. Returns its id.
    std::string AddLoop(const std::string& name = {});
    /// Picks a loop (its settings, looping from its start), or lets it go when it is the one.
    void SelectLoop(const std::string& loopId);
    /// The row's play button: picks the loop and plays it from the top, or stops it.
    void ToggleLoopPlayback(const std::string& loopId);
    void RenameLoop(const std::string& loopId, const std::string& name);
    void SetLoopTimes(const std::string& loopId, double startSec, double endSec);
    void DeleteLoop(const std::string& loopId);

    /// The loop deleted last, which can still be put back.
    [[nodiscard]] const std::optional<practice::Loop>& DeletedLoop() const
    {
        return mDeleted;
    }

    void UndoDelete();
    void ForgetDeleted();

    // ── Projects ──
    [[nodiscard]] std::vector<practice::Project> Projects() const;

    [[nodiscard]] const std::string& SelectedProjectId() const
    {
        return mSelectedProject;
    }

    void SelectProject(const std::string& projectId)
    {
        mSelectedProject = projectId;
        Changed();
    }

    /// A name for a new project: the track's title.
    [[nodiscard]] std::string SuggestedProjectName() const;
    /// Saves the session under `name` (over a project of that name, keeping its id and links),
    /// with the active preset when asked. False when there is nothing to save or no room.
    bool SaveProject(const std::string& name, bool includePreset);
    /// Applies a project: at once when its track is loaded, else after loading it. False when
    /// its track cannot be reopened (it was dropped in).
    bool LoadProject(const std::string& projectId);
    void DeleteProject(const std::string& projectId);
    /// "added", "invalid-url", "too-many" or "no-project".
    std::string AddLink(const std::string& projectId, const std::string& title, const std::string& url);
    void RemoveLink(const std::string& projectId, const std::string& linkId);

  private:
    void OnTrack();
    void Changed();
    void SendFader(practice::Fader fader, double value, bool immediate);
    void ApplyFaders(const practice::LoopSettings& settings);
    void PushEq();
    void ActivateLoop(practice::Loop& loop);
    void Deactivate();
    void PersistLoops();
    void ApplyProject(const practice::Project& project);
    void WriteProjects(const std::vector<practice::Project>& projects);
    [[nodiscard]] practice::Loop* FindLoop(const std::string& loopId);
    [[nodiscard]] std::string NewId(const char* prefix);

    UiClient& mClient;
    UiCommands& mCommands;
    Subscription mSubscription;
    int mSeenLoads = 0;

    std::vector<practice::Loop> mLoops;
    std::string mActiveLoopId;
    practice::LoopSettings mFaders;
    practice::Eq mEq = practice::DefaultEq();
    std::optional<std::pair<double, double>> mCandidate;
    std::optional<double> mJumpBackPoint;
    std::optional<practice::Loop> mDeleted;
    std::size_t mDeletedIndex = 0;
    std::optional<practice::Project> mPendingRecall;
    std::string mSelectedProject;

    // Sends that wait on a pause: speed and pitch while dragged, a loop edge being dragged.
    struct Pending
    {
        double value = 0.0;
        double due = 0.0;
        bool set = false;
    };

    Pending mPendingSpeed, mPendingPitch;
    bool mPendingRegion = false;
    double mPendingRegionDue = 0.0;
    double mLastTick = 0.0;
    unsigned mIdCounter = 0;
};
} // namespace guitarfx::uiclient
