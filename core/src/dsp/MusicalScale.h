#pragma once

#include <array>
#include <cmath>
#include <cstddef>

/**
 * Keys, seven-note scales and the intervals within them, for effects that work in a key.
 *
 * A harmony "a 3rd up" in C major is a major third above C, E and G (E, G, B) but a minor third
 * above D, E, A and B: the interval is counted in scale steps, and how many semitones that is
 * depends on where the note sits in the scale. ScaleStepShift() works that out. Everything here
 * is constexpr and allocation-free, so it can run on the audio thread and be checked at compile
 * time.
 *
 * Only seven-note scales are offered. An interval such as a 3rd or a 6th is defined by counting
 * the notes of a seven-note scale; on a pentatonic one the same count lands on a different
 * interval, which would make the labels lie.
 *
 * A note outside the scale (a chromatic passing tone, or a guitar that is in another key) is
 * moved by the same number of semitones as the scale note nearest it, the lower one on a tie,
 * so a chromatic run is harmonised in parallel rather than stalling on one harmony note.
 */
namespace guitarfx::music
{
constexpr int kPitchClasses = 12;
constexpr int kScaleDegrees = 7;

/// The keys in pitch-class order, C = 0. Sharps or flats as a guitarist usually reads them.
inline constexpr const char* kKeyLabels[] = {"C", "C#/Db", "D", "Eb", "E", "F", "F#/Gb", "G", "Ab", "A", "Bb", "B"};

enum class Scale : int
{
    Major,
    NaturalMinor,
    HarmonicMinor,
    MelodicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    PhrygianDominant,
    Count
};

inline constexpr const char* kScaleLabels[] = {
    "Major",    "Minor",  "Harmonic Minor", "Melodic Minor", "Dorian",
    "Phrygian", "Lydian", "Mixolydian",     "Locrian",       "Phrygian Dominant"};

/// Each scale's degrees, in semitones above its root. Melodic Minor is the ascending (jazz) form.
inline constexpr std::array<std::array<int, kScaleDegrees>, static_cast<std::size_t>(Scale::Count)> kScaleSemitones = {{
    {0, 2, 4, 5, 7, 9, 11}, // Major (Ionian)
    {0, 2, 3, 5, 7, 8, 10}, // Natural Minor (Aeolian)
    {0, 2, 3, 5, 7, 8, 11}, // Harmonic Minor
    {0, 2, 3, 5, 7, 9, 11}, // Melodic Minor
    {0, 2, 3, 5, 7, 9, 10}, // Dorian
    {0, 1, 3, 5, 7, 8, 10}, // Phrygian
    {0, 2, 4, 6, 7, 9, 11}, // Lydian
    {0, 2, 4, 5, 7, 9, 10}, // Mixolydian
    {0, 1, 3, 5, 6, 8, 10}, // Locrian
    {0, 1, 4, 5, 7, 8, 10}, // Phrygian Dominant (the fifth mode of harmonic minor)
}};

static_assert(std::size(kScaleLabels) == kScaleSemitones.size());
static_assert(std::size(kKeyLabels) == kPitchClasses);

/// Intervals as scale steps, from two octaves down to two octaves up.
constexpr int kMinScaleSteps = -14;
constexpr int kMaxScaleSteps = 14;

/// One label per scale-step count from kMinScaleSteps, so an enum parameter over that range can
/// use them directly. A step count of n is the interval numbered |n| + 1: two steps is a 3rd.
inline constexpr const char* kIntervalLabels[] = {
    "2 Octaves Down", "14th Down", "13th Down", "12th Down", "11th Down",   "10th Down", "9th Down", "Octave Down",
    "7th Down",       "6th Down",  "5th Down",  "4th Down",  "3rd Down",    "2nd Down",  "Unison",   "2nd Up",
    "3rd Up",         "4th Up",    "5th Up",    "6th Up",    "7th Up",      "Octave Up", "9th Up",   "10th Up",
    "11th Up",        "12th Up",   "13th Up",   "14th Up",   "2 Octaves Up"};

static_assert(std::size(kIntervalLabels) == kMaxScaleSteps - kMinScaleSteps + 1);

/// `note` folded into 0-11, for negative notes too.
[[nodiscard]] constexpr int PitchClass(int note) noexcept
{
    const int pc = note % kPitchClasses;
    return pc < 0 ? pc + kPitchClasses : pc;
}

[[nodiscard]] constexpr const std::array<int, kScaleDegrees>& ScaleSemitones(Scale scale) noexcept
{
    const auto index = static_cast<std::size_t>(scale);
    return kScaleSemitones[index < kScaleSemitones.size() ? index : 0];
}

/// The scale degree (0-6) nearest a pitch `fromKey` semitones above the key's root (0-11), with a
/// tie going to the lower degree. The root an octave up counts as a candidate, so a note just
/// under the root can still be nearest the root; it comes back as degree 7.
[[nodiscard]] constexpr int NearestScaleDegree(Scale scale, int fromKey) noexcept
{
    const auto& degrees = ScaleSemitones(scale);
    const int pitch = PitchClass(fromKey);
    int best = 0;
    int bestDistance = kPitchClasses;

    for (int degree = 0; degree <= kScaleDegrees; ++degree)
    {
        const int semitones = degree < kScaleDegrees ? degrees[static_cast<std::size_t>(degree)] : kPitchClasses;
        const int distance = pitch > semitones ? pitch - semitones : semitones - pitch;

        // Strictly nearer only: the degrees rise, so the lower of two equal candidates is kept.
        if (distance < bestDistance)
        {
            best = degree;
            bestDistance = distance;
        }
    }

    return best;
}

/// Whether `note` is one of the scale's notes in `key` (0-11).
[[nodiscard]] constexpr bool InScale(int note, int key, Scale scale) noexcept
{
    const int fromKey = PitchClass(note - key);

    for (const int semitones : ScaleSemitones(scale))
    {
        if (semitones == fromKey)
        {
            return true;
        }
    }

    return false;
}

/// The semitones from `note` (a MIDI note number) to the note `steps` scale steps above it (below
/// for negative steps) in `key` (0-11) and `scale`. Steps of a multiple of seven are whole octaves.
/// A note outside the scale moves by what the nearest scale note would.
[[nodiscard]] constexpr int ScaleStepShift(int note, int key, Scale scale, int steps) noexcept
{
    const auto& degrees = ScaleSemitones(scale);
    const int fromKey = PitchClass(note - key);
    // Degree 7, the root an octave up, is degree 0: the shift is relative, so the octave the
    // scale note sits in drops out.
    const int degree = NearestScaleDegree(scale, fromKey) % kScaleDegrees;
    const int target = degree + steps;
    const int octaves = target >= 0 ? target / kScaleDegrees : -((-target + kScaleDegrees - 1) / kScaleDegrees);
    const int targetDegree = target - octaves * kScaleDegrees;
    return degrees[static_cast<std::size_t>(targetDegree)] + octaves * kPitchClasses -
           degrees[static_cast<std::size_t>(degree)];
}

/// The scale note nearest `pitch` (a fractional MIDI note number), the lower on a tie.
///
/// Snapping to the scale rather than to the nearest semitone is what keeps a harmony steady on a
/// real guitar. Notes are often 30-60 cents out, low strings sharp after a hard pick above all,
/// and a pitch that sits near halfway between two semitones flips between them; scale notes lie
/// one or two semitones apart, so the same pitch is nearly always clearly nearest one of them.
[[nodiscard]] inline int NearestScaleNote(double pitch, int key, Scale scale) noexcept
{
    const auto base = static_cast<int>(std::floor(pitch));
    int best = base;
    double bestDistance = 1.0e9;

    // A scale note is never more than two semitones from a pitch.
    for (int note = base - 2; note <= base + 3; ++note)
    {
        const double distance = std::abs(pitch - static_cast<double>(note));

        if (InScale(note, key, scale) && distance < bestDistance - 1.0e-9)
        {
            best = note;
            bestDistance = distance;
        }
    }

    return best;
}

/// The scale note for `pitch`, held at `current` until `pitch` is nearer another scale note by
/// more than `margin` semitones, so vibrato and a bend that stops short do not flicker between
/// two. `current` below 0 means no note yet.
[[nodiscard]] inline int FollowScaleNote(int current, double pitch, int key, Scale scale, double margin) noexcept
{
    const int nearest = NearestScaleNote(pitch, key, scale);

    if (current < 0 || nearest == current)
    {
        return nearest;
    }

    const double fromCurrent = std::abs(pitch - static_cast<double>(current));
    const double fromNearest = std::abs(pitch - static_cast<double>(nearest));
    return fromCurrent - fromNearest > margin ? nearest : current;
}

// Compile-time checks of the arithmetic, which the tests then exercise more widely.
static_assert(ScaleStepShift(60, 0, Scale::Major, 2) == 4);   // C -> E, a major 3rd
static_assert(ScaleStepShift(62, 0, Scale::Major, 2) == 3);   // D -> F, a minor 3rd
static_assert(ScaleStepShift(71, 0, Scale::Major, 2) == 3);   // B -> D, across the octave
static_assert(ScaleStepShift(60, 0, Scale::Major, -2) == -3); // C -> A below
static_assert(ScaleStepShift(64, 0, Scale::Major, 7) == 12);  // an octave is an octave
static_assert(ScaleStepShift(64, 0, Scale::Major, -14) == -24);
static_assert(ScaleStepShift(61, 0, Scale::Major, 2) == 4);        // C# moves as C does
static_assert(ScaleStepShift(70, 0, Scale::Major, 2) == 3);        // Bb moves as A does (tie, lower)
static_assert(ScaleStepShift(59, 2, Scale::Major, 2) == 3);        // B in D major -> D
static_assert(ScaleStepShift(64, 4, Scale::NaturalMinor, 2) == 3); // E in E minor -> G
static_assert(NearestScaleDegree(Scale::Major, 11) == 6);
static_assert(NearestScaleDegree(Scale::Mixolydian, 11) == 6); // b7 and the root are both 1 away
} // namespace guitarfx::music
