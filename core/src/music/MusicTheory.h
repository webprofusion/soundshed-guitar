#pragma once

/**
 * MusicTheory.h - Intervals, note spelling and roman-numeral chords for the Scale Guide
 * (docs/plans/scale-guide.md). UI-side only: it allocates, so never call it from the audio thread
 * (dsp/MusicalScale.h is the audio thread's small table).
 *
 * Notes are spelled by letter, the way a guitarist reads them: each degree of a scale or chord
 * takes the letter `root + degree - 1` and whatever accidental makes its pitch, so E-flat Dorian
 * reads E♭ F G♭ A♭ B♭ C D♭ rather than D♯ F F♯ … Where a root has two spellings (C♯/D♭) the one
 * needing fewer accidentals wins.
 *
 * Text comes back as UTF-8 (♭ ♯ ° ø); the escapes below keep the sources ASCII, since MSVC reads
 * them in the system code page.
 */

#include "dsp/MusicalScale.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::music
{
inline constexpr const char* kFlatSign = "\xE2\x99\xAD";  // ♭
inline constexpr const char* kSharpSign = "\xE2\x99\xAF"; // ♯
inline constexpr const char* kDegreeSign = "\xC2\xB0";    // °
inline constexpr const char* kHalfDimSign = "\xC3\xB8";   // ø

constexpr int kLetters = 7;

/// An interval above a root: its number (1-13) and its size in semitones (0-23).
struct Interval
{
    int degree = 1;
    int semitones = 0;
    /// "R", "♭3", "♯4", "9", "♭♭7".
    std::string label;
};

/// "b3", "#4", "bb7", "9", "1" (♭ and ♯ accepted too); nullopt for anything else.
[[nodiscard]] std::optional<Interval> ParseInterval(std::string_view token);

/// A space-separated formula, "1 b3 5 b7"; empty when any token is not an interval.
[[nodiscard]] std::vector<Interval> ParseFormula(std::string_view formula);

/// A note as written: its letter (0 = C … 6 = B) and accidental (-2 … 2).
struct Spelling
{
    int letter = 0;
    int accidental = 0;

    bool operator==(const Spelling&) const = default;
};

/// The pitch class of a letter with no accidental.
[[nodiscard]] int LetterPitchClass(int letter);

/// The pitch class a spelling sounds.
[[nodiscard]] int SpellingPitchClass(Spelling spelling);

/// "C", "F♯", "B♭". A double accidental is written as its plain enharmonic (B♭♭ reads A), since
/// that is how a fretboard player names the note.
[[nodiscard]] std::string NoteName(Spelling spelling);

/// A pitch class's name with sharps or with flats, for a note no scale spells.
[[nodiscard]] std::string PlainNoteName(int pitchClass, bool flats);

struct SpelledNote
{
    int pitchClass = 0;
    Spelling spelling;
    std::string name;
    Interval interval;
};

struct SpelledSet
{
    std::vector<SpelledNote> notes;
    /// One per accidental, three for a double accidental: lower reads better.
    int cost = 0;

    [[nodiscard]] bool UsesFlats() const;
};

/// The intervals spelled up from a root as written.
[[nodiscard]] SpelledSet SpellFrom(Spelling root, const std::vector<Interval>& intervals);

/// How a key on this pitch class can be written: one spelling, or two (C♯ and D♭).
[[nodiscard]] std::vector<Spelling> KeySpellings(int pitchClass);

/// The intervals spelled from whichever spelling of the root reads better (sharps on a tie).
[[nodiscard]] SpelledSet SpellBest(int rootPitchClass, const std::vector<Interval>& intervals);

/// The letter a degree of a key falls on, and the accidental that makes `pitchClass` from it.
[[nodiscard]] Spelling SpellDegree(Spelling key, int degree, int pitchClass);

/// A chord of a progression, relative to its key.
struct RomanChord
{
    /// Semitones above the key's root, 0-11.
    int offset = 0;
    /// A chord quality id (MusicCatalog.h), "maj", "m7", "dim" …
    std::string quality;
    /// The numeral's degree, 1-7: ♭VII is 7, so its root is spelled on the key's 7th letter.
    int degree = 1;
};

/// "I", "bVII", "ii7", "vii°", "iiø7", "I5", "IVmaj7", "V7sus4"; nullopt for anything else.
[[nodiscard]] std::optional<RomanChord> ParseRoman(std::string_view token);

/// A space-separated sequence; empty when any token fails.
[[nodiscard]] std::vector<RomanChord> ParseRomanSequence(std::string_view sequence);

/// The numeral as a chart writes it: "♭VII", "ii7", "vii°", "iiø7".
[[nodiscard]] std::string RomanNumeral(const RomanChord& chord);

/// Whether a quality reads as minor (a lower-case numeral).
[[nodiscard]] bool IsMinorQuality(std::string_view quality);

/// The steps between a scale's notes in semitones, "2 1 2 2 1 2 2" (the last one back to the octave).
[[nodiscard]] std::string StepPattern(const std::vector<Interval>& intervals);

/// A chord built on one degree of a seven-note scale.
struct DiatonicChord
{
    std::string name;
    std::string numeral;
    /// "maj", "m", "dim", "aug", or "" for a degree whose stacked thirds name no triad.
    std::string quality;
    int rootPitchClass = 0;
};

/// The triad on each degree of a seven-note scale; empty for any other size.
[[nodiscard]] std::vector<DiatonicChord> DiatonicTriads(const SpelledSet& scale);
} // namespace guitarfx::music
