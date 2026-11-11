#pragma once

/**
 * Arpeggios.h - Chord arpeggios as a guitarist plays them (docs/plans/scale-guide.md): one chord's
 * tones as a run up the strings and back, in each neck position, with the fingers and pick strokes
 * that play it. Like chord shapes these are searched, not tabled, so a drop tuning or an 8-string
 * gets them too.
 *
 * Three styles:
 * - Sweep: the run starts on a chord tone on the lowest string of the set, climbs to the same tone
 *   one or more octaves up with a note or two on each string, and stays under one hand. The second
 *   note on a string is a hammer-on going up and a pull-off coming back; every string change is a
 *   pick stroke in the same direction.
 * - Two per string: every string takes two notes, so the run climbs the neck as it crosses it.
 * - Box: every chord tone in one hand position, lowest to highest. The only style that works on a
 *   re-entrant tuning, where crossing the strings does not climb in pitch.
 *
 * Strings are indexed as Fretboard.h indexes them. UI-side only, like the rest of music/.
 */

#include "music/Fretboard.h"

#include <optional>
#include <string_view>
#include <vector>

namespace guitarfx::music
{
constexpr int kMinArpeggioStrings = 3;
/// What a sweep shape asks of the hand when the player has not chosen: the classic five strings.
constexpr int kDefaultArpeggioStrings = 5;

enum class ArpeggioStyle
{
    Sweep,
    TwoPerString,
    Box
};

/// "sweep", "two" or "box".
[[nodiscard]] std::string_view ArpeggioStyleId(ArpeggioStyle style);
[[nodiscard]] std::optional<ArpeggioStyle> ParseArpeggioStyle(std::string_view id);

enum class PickStroke
{
    Down,
    Up,
    Hammer,
    Pull,
    /// Slid into from the note before, on the same string: only between a progression's shapes.
    Slide
};

/// "down", "up", "hammer", "pull" or "slide".
[[nodiscard]] std::string_view PickStrokeId(PickStroke stroke);

struct ArpeggioNote
{
    int string = 0;
    int fret = 0;
    /// Index into the search's intervals.
    int tone = 0;
    /// 1 (index) … 4 (little), or 0 for an open string.
    int finger = 0;
};

/// One note of the run as played: which note, and how it is sounded.
struct ArpeggioStep
{
    /// Index into ArpeggioShape::notes.
    int note = 0;
    PickStroke stroke = PickStroke::Down;
};

struct ArpeggioShape
{
    /// Lowest in pitch first.
    std::vector<ArpeggioNote> notes;
    /// Up the notes and back down them, the top note once.
    std::vector<ArpeggioStep> steps;
    /// The tone the run starts on: 0 is root position.
    int startTone = 0;
    /// The lowest and highest fretted frets.
    int fretFrom = 0;
    int fretTo = 0;
    /// The lowest and highest strings it uses, in neck order.
    int fromString = 0;
    int toString = 0;
};

struct ArpeggioSearch
{
    int rootPitchClass = 0;
    std::vector<Interval> intervals;
    ArpeggioStyle style = ArpeggioStyle::Sweep;
    /// How many strings to play, counted down from the highest: 0 for the default. A box uses every
    /// string in its hand position and ignores this.
    int strings = 0;
};

/// The string counts a neck can play: three up to all of them.
[[nodiscard]] std::vector<int> ArpeggioStringChoices(const Neck& neck);

/// `requested` if the neck has that choice, else the default for this neck.
[[nodiscard]] int ArpeggioStringsFor(const Neck& neck, int requested);

/// The string counts this chord has runs on in this style, which may be fewer than the neck's: two
/// notes a string would climb 17 frets or more for a triad across five. A box ignores the count,
/// and where no count has a run they are all offered, so the choices stay on the page.
[[nodiscard]] std::vector<int> ArpeggioStringsWithRuns(const Neck& neck, const ArpeggioSearch& search);

/// The choice to play: the most strings, up to `wanted`, else the fewest on offer.
[[nodiscard]] int ClosestArpeggioStrings(const std::vector<int>& choices, int wanted);

/// Every shape of a chord on this neck, sorted up it: one per chord tone the run can start on, in
/// each octave that fits. Empty where a style has no playable shape (a sweep on a re-entrant
/// tuning, two notes a string of a triad across six strings).
[[nodiscard]] std::vector<ArpeggioShape> ArpeggioShapes(const Neck& neck, const ArpeggioSearch& search);
} // namespace guitarfx::music
