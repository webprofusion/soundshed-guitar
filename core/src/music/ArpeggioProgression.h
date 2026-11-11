#pragma once

/**
 * ArpeggioProgression.h - A chord progression played as arpeggios (docs/plans/scale-guide.md): one
 * run per chord, each the shape nearest the one before it, written end to end as one piece.
 *
 * One chord is the anchor: the player chose its run, and the rest follow from it round the
 * progression, so stepping from chord to chord never moves the shapes. With slides on, each chord
 * takes the nearest shape that starts on the same string at another fret, and the hand slides into
 * it from the last note of the chord before.
 */

#include "music/Arpeggios.h"
#include "music/MusicCatalog.h"

#include <vector>

namespace guitarfx::music
{
struct ArpeggioProgressionSearch
{
    ArpeggioStyle style = ArpeggioStyle::Sweep;
    /// How many strings to play, as ArpeggioSearch has it: 0 for the default.
    int strings = 0;
    bool slides = false;
    /// The chord whose run the player chose, and which of its runs (0 the first).
    int anchorChord = 0;
    int anchorRun = 0;
};

struct ArpeggioProgressionChord
{
    /// This chord's runs in the style, sorted up the neck.
    std::vector<ArpeggioShape> runs;
    /// The one the progression plays, or -1 where the chord has none.
    int chosen = -1;
};

/// One note of the whole piece: which chord's run it belongs to, and which of that run's steps.
struct ArpeggioProgressionStep
{
    int chord = 0;
    /// Index into the chosen run's notes.
    int note = 0;
    PickStroke stroke = PickStroke::Down;
};

/// Every chord's runs and the one chosen for it. A chord repeated takes the same string count as
/// its first appearance; each chord plays the most strings it has runs on, up to the count asked.
[[nodiscard]] std::vector<ArpeggioProgressionChord> ArpeggioProgression(const Neck& neck,
                                                                        const std::vector<SpelledChord>& chords,
                                                                        const ArpeggioProgressionSearch& search);

/// The chosen runs end to end, each up and back. With `slides`, a chord whose first note is on the
/// string the last chord ended on, at another fret, is slid into instead of picked.
[[nodiscard]] std::vector<ArpeggioProgressionStep> ArpeggioProgressionSteps(
    const std::vector<ArpeggioProgressionChord>& chords, bool slides);
} // namespace guitarfx::music
