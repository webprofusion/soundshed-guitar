#pragma once

/**
 * Intonation.h - The tuner's intonation check, as the web UI has it (core/ui/ts/tunerIntonation.ts;
 * keep the two in step).
 *
 * The player tunes an open string and sets it as the reference. Every note they then play on that
 * string is read against it: its distance from the open string in semitones names the fret (the
 * nearest whole semitone) and what is left over is how far that fret is out. Only the relative
 * pitch matters, so the reference pitch and how well the open string was tuned make no difference.
 *
 * A plucked note starts sharp and keeps falling a little as it decays, so a fret is measured over
 * its sustain (NoteSettler): the first few readings of each pluck are skipped, the rest must agree,
 * and the measurement follows the note while it rings. Scored on real DI single-note takes, that
 * measured 241 of 246 notes with 2 wrong frets (the tracker's octave errors) and ~3 cents of
 * scatter about each string's curve, whenever in the sustain it was read.
 */

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient::intonation
{
/// The highest fret read; a note further above the open string is taken as another string.
constexpr int kMaxFret = 24;
/// Readings skipped at the start of each pluck (a reading is ~43 ms): the attack reads sharp.
constexpr int kAttackReadings = 4;
/// Readings of sustain, after the attack, before a measurement counts (~260 ms).
constexpr std::size_t kSettleReadings = 6;
/// The most recent readings of the sustain a measurement is taken over (~690 ms).
constexpr std::size_t kSettleWindow = 16;
/// A reading this many cents or more from the window's median is not steady.
constexpr double kSettleSpreadCents = 4.0;
/// At least this share of the window must be steady for the measurement to count.
constexpr double kSettleSteadyShare = 0.75;
/// A rise in level this large between readings is a new pluck of the same note.
constexpr double kNewPluckDb = 6.0;
/// Frets up to this one are near the nut: their pitch is set more by the nut than the saddle.
constexpr int kNutFrets = 3;
/// From this fret up the saddle's position dominates; the 12th is the usual check.
constexpr int kSaddleFrets = 7;

enum class Quality
{
    Excellent,
    Good,
    Fair,
    Poor
};

/// How good a fret's intonation is, from how many cents out it is either way.
[[nodiscard]] Quality QualityOf(double cents);
[[nodiscard]] const char* QualityLabel(Quality quality);

struct FretReading
{
    int fret = 0;
    /// How far the note is from where that fret should be, sharp positive.
    double cents = 0.0;
    /// Where the fret should be: the open string's pitch raised by `fret` semitones.
    double targetFrequency = 0.0;
};

/// The fret a note is nearest on a string tuned to `openFrequency`, and how far out it is. None
/// for a note below the open string or above the top fret (another string, most likely).
[[nodiscard]] std::optional<FretReading> ReadFret(double openFrequency, double frequency);

struct NearestNote
{
    std::string name; // "E2"
    double cents = 0.0;
};

[[nodiscard]] NearestNote Nearest(double frequency, double referenceFrequency = 440.0);

/// What to do about one fret that is out, in a sentence; empty when it is fine.
[[nodiscard]] std::string Advice(int fret, double cents);

struct Fret
{
    int fret = 0;
    double cents = 0.0;
    Quality quality = Quality::Excellent;
};

/// What a string's frets say together, a sentence each: the saddle from the fret at the 7th or
/// above nearest the 12th; the nut when the frets near it are sharper than the frets higher up.
[[nodiscard]] std::vector<std::string> StringAdvice(const std::vector<Fret>& frets);

/// "+3.2¢", "−1.0¢", "±0.0¢".
[[nodiscard]] std::string FormatCents(double cents);

struct Reading
{
    bool detected = false;
    double frequency = 0.0;
    std::optional<double> levelDb;
};

/// A held note's pitch measured over its sustain.
struct SteadyNote
{
    double frequency = 0.0;
    std::size_t readings = 0;
};

/// Turns the tuner's readings into steady measurements of each held note. A note runs from the
/// first reading that finds it until the signal goes, the pitch jumps to another note or the
/// level jumps (a new pluck); its attack is skipped and its sustain measured.
class NoteSettler
{
  public:
    void Reset();
    std::optional<SteadyNote> Push(const Reading& reading);

  private:
    int mSeen = 0;
    std::optional<double> mLastSemitones;
    std::optional<double> mLastLevelDb;
    std::deque<double> mWindow;
};

/// One string checked: its open pitch and the frets measured on it, lowest fret first.
struct String
{
    std::string name; // the open string's nearest note, "E2"; a string is known by it
    double openFrequency = 0.0;
    std::vector<Fret> frets;
};

struct Measurement : Fret
{
    double frequency = 0.0;
    double targetFrequency = 0.0;
};

struct View
{
    /// False until an open string is set.
    bool hasString = false;
    /// The string being checked (valid while hasString).
    String string;
    /// The last steady note heard, which "Set open string" would take.
    std::optional<NearestNote> candidate;
    /// The current note's steady measurement against the open string, while it lasts.
    std::optional<Measurement> measurement;
    /// A steady note that is no fret of this string: below it or above the top fret.
    bool offString = false;
    /// Every string checked, lowest first.
    std::vector<String> strings;
};

/// The intonation check's state: the open string set, what has been measured on each string and
/// the latest note heard.
class IntonationTest
{
  public:
    void SetReferenceFrequency(double hz);

    /// One tuner reading in (unsmoothed).
    void Push(const Reading& reading);

    /// Takes the last steady note as the open string. False when no note has been heard yet.
    bool SetOpenString();

    /// Back to setting an open string, keeping what has been measured.
    void NewString();

    /// Forgets every string measured.
    void Clear();

    /// A new run of readings (the tuner reopened): the note in progress is forgotten.
    void ResetReadings();

    [[nodiscard]] View GetView() const;

    /// The big readout while a string is set: the fret nearest `frequency`; none to show the note.
    [[nodiscard]] std::optional<FretReading> Readout(double frequency) const;

  private:
    NoteSettler mSettler;
    std::vector<String> mChecked;
    std::optional<std::string> mCurrent; // the name of the string being checked
    std::optional<SteadyNote> mLastSteady;
    std::optional<Measurement> mMeasurement;
    bool mOffString = false;
    double mReferenceFrequency = 440.0;
};
} // namespace guitarfx::uiclient::intonation
