#pragma once

/**
 * ScaleGuide.h - The Scale Guide as a native UI keeps it (docs/plans/scale-guide.md): the player's
 * choices and their own progressions, read from and written to the same app settings the web UI
 * uses (`scaleGuide.view`, `scaleGuide.progressions`; ts/scaleGuide/state.ts, progressions.ts), the
 * request music/ScaleGuideView.h answers, and LISTEN's logic (ts/scaleGuide/listen.ts): the note
 * being played, steadied, and whether a strummed chord rings clean.
 */

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::uiclient::scaleguide
{
inline constexpr const char* kViewSetting = "scaleGuide.view";
inline constexpr const char* kProgressionsSetting = "scaleGuide.progressions";
inline constexpr int kMaxCapo = 12;
inline constexpr int kMaxProgressionChords = 16;

struct Settings
{
    std::string mode = "scale"; ///< "scale", "chord" or "arpeggio"
    int key = 4;
    std::string scale = "minpent";
    std::string chord = "maj";
    /// "" a single chord, "b:<id>" a built-in progression, "u:<id>" one of the player's own.
    std::string progression;
    std::string tuning = "std";
    /// "name", "interval", "degree", "none", or an arpeggio run's "order" or "finger".
    std::string labels = "name";
    int position = 0;
    int capo = 0;
    bool leftHanded = false;
    bool listen = false;
    std::string match = "normal";        ///< "easy", "normal" or "strict"
    std::string arpeggioStyle = "sweep"; ///< "sweep", "two" or "box"
    int arpeggioStrings = 0;             ///< from the highest string; 0 for the default
    bool arpeggioSlides = false;         ///< a progression's arpeggios joined by slides
    bool listenEverywhere = false;       ///< LISTEN marks a note everywhere on the neck
    int playVolume = 100;                ///< Play's level into the preset, 0 to 100
};

/// The stored choices, anything missing or malformed taking its default.
[[nodiscard]] Settings ParseSettings(const nlohmann::json& stored);
[[nodiscard]] nlohmann::json SettingsJson(const Settings& settings);
[[nodiscard]] Settings LoadSettings(const nlohmann::json& appSettings);

struct UserProgression
{
    std::string id;
    std::string name;
    std::vector<std::string> chords; ///< roman numerals, "I", "bVII7"
};

[[nodiscard]] std::vector<UserProgression> LoadProgressions(const nlohmann::json& appSettings);
[[nodiscard]] nlohmann::json ProgressionsJson(const std::vector<UserProgression>& progressions);

/// The engine request for these choices: the chosen progression (a built-in's id, or one of the
/// player's own as numerals), the step showing, and in Chords the shape to lead from when stepping.
/// In Arpeggios `anchorStep` is the chord whose run the position chooses (ScaleGuideView.h).
[[nodiscard]] nlohmann::json ViewRequest(const Settings& settings, const std::vector<UserProgression>& progressions,
                                         int step, const std::vector<int>& previousFrets = {}, int anchorStep = 0);

// ── Play ────────────────────────────────────────────────────────────────────

/// One note of what Play sounds, as the engine's `scaleGuidePlayback` times it.
struct PlayedNote
{
    int string = 0;
    int fret = 0;
    double startMs = 0.0;
    double lengthMs = 0.0;
    int step = -1;  ///< which step of the tab it is, or -1
    int chord = -1; ///< which chord of a progression it belongs to, or -1
};

/// What Play is sounding: its notes in order, after `delayMs` (the wait for the metronome's bar).
struct Playback
{
    std::vector<PlayedNote> notes;
    double delayMs = 0.0;
};

[[nodiscard]] Playback ParsePlayback(const nlohmann::json& message);

/// What to show at one moment of a playback: the places sounding on the neck, their steps of the
/// tab, and the progression chord to show (-1 for none).
struct PlaybackMoment
{
    std::vector<std::pair<int, int>> places; ///< string, fret
    std::vector<int> steps;
    int chord = -1;

    bool operator==(const PlaybackMoment&) const = default;
};

/// A progression's next chord shows this long before its first note, so the neck is drawn in time.
inline constexpr double kPlaybackStepLeadMs = 80.0;

[[nodiscard]] PlaybackMoment PlaybackAt(const Playback& playback, double elapsedMs);

// ── LISTEN ──────────────────────────────────────────────────────────────────

/// The tuner's note ("C#/Db", octave 3) as a MIDI note.
[[nodiscard]] std::optional<int> ReadingMidi(const std::string& noteName, int octave);

struct ChordTone
{
    int pitchClass = 0;
    std::string interval; ///< "R", "3", "♭3", "5" …
};

struct ChordMatch
{
    double share = 0.0;      ///< the chord's tones' share of the chroma
    std::vector<bool> heard; ///< per tone, in the order given
    std::vector<int> missing;
    bool ok = false;
};

/// Whether `chroma` is the chord, at a strictness of "easy", "normal" or "strict": its tones hold
/// the strictness's share, and each (bar a plain 5th) reads its level.
[[nodiscard]] ChordMatch MatchChord(const std::array<float, 12>& chroma, int rootPitchClass,
                                    const std::vector<ChordTone>& tones, const std::string& strictness);

/// A clean strum: three clean readings in a row (~130 ms); the next needs a jump in level after a
/// short lockout (~340 ms).
class StrumGate
{
  public:
    static constexpr int kHold = 3;
    static constexpr double kJumpDb = 3.0;
    static constexpr int kRearmReadings = 8;
    static constexpr double kQuietDb = -50.0;

    /// True when this reading completes a clean strum.
    bool Push(double levelDb, bool clean);
    void Reset();

  private:
    std::vector<double> mLevels;
    bool mArmed = true;
    int mHeld = 0;
    int mRearmWait = 0;
};

/// The note being played, steadied: it shows once read twice, goes after four silent readings,
/// and the last two stay as a trail until a long silence.
class NoteFollower
{
  public:
    static constexpr int kReleaseReadings = 4;
    static constexpr int kClearReadings = 190;

    /// One reading (nullopt for none); true when what should show has changed.
    bool Push(std::optional<int> midi);
    void Reset();

    [[nodiscard]] std::optional<int> Current() const
    {
        return mCurrent;
    }

    [[nodiscard]] const std::vector<int>& Trail() const
    {
        return mTrail;
    }

  private:
    std::optional<int> mCurrent;
    std::vector<int> mTrail;
    std::optional<int> mCandidate;
    int mSeen = 0;
    int mSilent = 0;
};
} // namespace guitarfx::uiclient::scaleguide
