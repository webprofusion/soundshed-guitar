#include "music/ArpeggioProgression.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace guitarfx::music
{
namespace
{
/// How far the hand moves from one run to the next: mostly where they start, since one ends there
/// and the other begins, then how far up the neck each reaches.
int Shift(const ArpeggioShape& from, const ArpeggioShape& to)
{
    const auto& a = from.notes.front();
    const auto& b = to.notes.front();
    return 2 * std::abs(b.fret - a.fret) + 4 * std::abs(b.string - a.string) + std::abs(to.fretTo - from.fretTo);
}

bool CanSlide(const ArpeggioNote& from, const ArpeggioNote& to)
{
    return from.string == to.string && from.fret != to.fret && from.fret > 0 && to.fret > 0;
}

/// The run nearest `previous`; with `slides`, the nearest it can be slid into, if any can.
int NearestRun(const std::vector<ArpeggioShape>& runs, const ArpeggioShape& previous, bool slides)
{
    int best = -1;
    int bestShift = std::numeric_limits<int>::max();
    bool bestSlides = false;

    for (int i = 0; i < static_cast<int>(runs.size()); ++i)
    {
        const bool slid = slides && CanSlide(previous.notes.front(), runs[i].notes.front());
        const int shift = Shift(previous, runs[i]);

        if (best < 0 || (slid && !bestSlides) || (slid == bestSlides && shift < bestShift))
        {
            best = i;
            bestShift = shift;
            bestSlides = slid;
        }
    }

    return best;
}
} // namespace

std::vector<ArpeggioProgressionChord> ArpeggioProgression(const Neck& neck, const std::vector<SpelledChord>& chords,
                                                          const ArpeggioProgressionSearch& search)
{
    const int count = static_cast<int>(chords.size());
    std::vector<ArpeggioProgressionChord> out(chords.size());
    std::vector<std::pair<std::string, int>> searched;

    for (int i = 0; i < count; ++i)
    {
        const auto& chord = chords[i];

        if (chord.def == nullptr)
        {
            continue;
        }

        // A 12-bar has three chords in its twelve bars: each is searched once.
        const std::string key = std::to_string(chord.rootPitchClass) + chord.def->id;
        const auto found =
            std::find_if(searched.begin(), searched.end(), [&](const auto& s) { return s.first == key; });

        if (found != searched.end())
        {
            out[i].runs = out[found->second].runs;
            continue;
        }

        ArpeggioSearch one;
        one.rootPitchClass = chord.rootPitchClass;
        one.intervals = chord.def->intervals;
        one.style = search.style;
        one.strings =
            ClosestArpeggioStrings(ArpeggioStringsWithRuns(neck, one), ArpeggioStringsFor(neck, search.strings));
        out[i].runs = ArpeggioShapes(neck, one);
        searched.emplace_back(key, i);
    }

    if (count == 0)
    {
        return out;
    }

    const int anchor = ((search.anchorChord % count) + count) % count;
    const ArpeggioShape* previous = nullptr;

    for (int k = 0; k < count; ++k)
    {
        auto& chord = out[(anchor + k) % count];

        if (chord.runs.empty())
        {
            continue;
        }

        if (previous == nullptr)
        {
            // The anchor, or the first chord after it that has a run at all.
            const bool chosen =
                k == 0 && search.anchorRun >= 0 && search.anchorRun < static_cast<int>(chord.runs.size());
            chord.chosen = chosen ? search.anchorRun : 0;
        }
        else
        {
            chord.chosen = NearestRun(chord.runs, *previous, search.slides);
        }

        previous = &chord.runs[chord.chosen];
    }

    return out;
}

std::vector<ArpeggioProgressionStep> ArpeggioProgressionSteps(const std::vector<ArpeggioProgressionChord>& chords,
                                                              bool slides)
{
    std::vector<ArpeggioProgressionStep> steps;
    const ArpeggioNote* last = nullptr;

    for (int i = 0; i < static_cast<int>(chords.size()); ++i)
    {
        const auto& chord = chords[i];

        if (chord.chosen < 0)
        {
            continue;
        }

        const auto& run = chord.runs[chord.chosen];
        bool first = true;

        for (const auto& step : run.steps)
        {
            const auto& note = run.notes[step.note];
            const bool slid = first && slides && last != nullptr && CanSlide(*last, note);
            steps.push_back({i, step.note, slid ? PickStroke::Slide : step.stroke});
            last = &note;
            first = false;
        }
    }

    return steps;
}
} // namespace guitarfx::music
