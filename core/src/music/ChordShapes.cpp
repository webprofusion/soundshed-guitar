// ChordShapes.cpp - The chord-shape half of Fretboard.h: the search and its rules per instrument,
// fingering and voice leading. Scale positions are in Fretboard.cpp.

#include "music/Fretboard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>

namespace guitarfx::music
{
namespace
{
constexpr int kOpenWindowLimit = 3;
constexpr double kScoreSpread = 8.0;
constexpr double kExtendedRangeSpread = 14.0;
constexpr int kStandardStrings[] = {40, 45, 50, 55, 59, 64};

using PitchSet = std::array<bool, kPitchClasses>;

PitchSet PitchesOf(const std::vector<Interval>& intervals, int rootPitchClass)
{
    PitchSet set{};

    for (const auto& interval : intervals)
    {
        set[PitchClass(rootPitchClass + interval.semitones)] = true;
    }

    return set;
}

struct Rules
{
    PitchSet allowed{};
    std::vector<int> required;
    int minStrings = 3;
    int maxStrings = 6;
    int fullStrings = 6; ///< sounding strings past this add nothing to a shape's score
    bool allStrings = false;
    bool rootLowest = true;
};

int FindDegree(const std::vector<Interval>& intervals, int degree)
{
    for (const auto& interval : intervals)
    {
        if (interval.degree == degree)
        {
            return interval.semitones;
        }
    }

    return -1;
}

Rules RulesFor(const Neck& neck, const ChordSearch& search)
{
    Rules rules;
    const int strings = static_cast<int>(neck.strings.size());
    const int root = PitchClass(search.rootPitchClass);
    rules.allowed = PitchesOf(search.intervals, root);
    rules.rootLowest = !search.allowInversions;

    const bool dropFifth = search.intervals.size() >= 4;

    for (const auto& interval : search.intervals)
    {
        const int pc = PitchClass(root + interval.semitones);
        const bool plainFifth = interval.degree == 5 && interval.semitones == 7;
        const bool present = std::find(rules.required.begin(), rules.required.end(), pc) != rules.required.end();

        if (!present && !(dropFifth && plainFifth))
        {
            rules.required.push_back(pc);
        }
    }

    const bool power = search.intervals.size() == 2;
    // A chord of five notes or more is voiced on four or five strings (x 7 6 7 8 x for E7#9): a
    // sixth string, usually a full barre, is no fuller and would crowd those grips out.
    rules.fullStrings = search.intervals.size() >= 5 ? 5 : 6;

    switch (neck.instrument)
    {
    case Instrument::Bass: {
        // Root and fifth (or third), two or three strings: what a bass player fingers as a chord.
        const int fifth = FindDegree(search.intervals, 5);
        const int other = fifth >= 0 ? fifth : FindDegree(search.intervals, 3);
        rules.required = {root};

        if (other >= 0)
        {
            rules.required.push_back(PitchClass(root + other));
        }

        rules.minStrings = 2;
        rules.maxStrings = std::min(3, strings);
        break;
    }
    case Instrument::Ukulele:
        rules.allStrings = true;
        rules.rootLowest = false;
        rules.minStrings = strings;
        rules.maxStrings = strings;
        break;
    default:
        rules.minStrings = power ? 2 : std::max(std::min(3, strings), static_cast<int>(rules.required.size()));
        rules.maxStrings = power ? std::min(3, strings) : strings;
        break;
    }

    return rules;
}

int FingerCount(const ChordShape& shape)
{
    int highest = 0;

    for (const int finger : shape.fingers)
    {
        highest = std::max(highest, finger);
    }

    return highest;
}

std::string Caged(const Neck& neck, const ChordShape& shape, int rootPitchClass)
{
    if (neck.instrument != Instrument::Guitar ||
        !std::equal(neck.strings.begin(), neck.strings.end(), std::begin(kStandardStrings), std::end(kStandardStrings)))
    {
        return {};
    }

    const int bass = shape.FirstSoundingString();

    if (bass < 0 || PitchClass(neck.strings[bass] + shape.frets[bass]) != PitchClass(rootPitchClass))
    {
        return {};
    }

    const bool atTop = shape.frets[bass] == shape.highestFret && shape.frets[bass] > shape.position;

    switch (bass)
    {
    case 0:
        return atTop ? "G" : "E";
    case 1:
        return atTop ? "C" : "A";
    case 2:
        return "D";
    default:
        return {};
    }
}
} // namespace

int ChordShape::FirstSoundingString() const
{
    for (std::size_t s = 0; s < frets.size(); ++s)
    {
        if (frets[s] >= 0)
        {
            return static_cast<int>(s);
        }
    }

    return -1;
}

double ChordShape::AverageFret() const
{
    double sum = 0.0;
    int count = 0;

    for (const int fret : frets)
    {
        if (fret > 0)
        {
            sum += fret;
            ++count;
        }
    }

    return count > 0 ? sum / count : 0.0;
}

bool AssignFingers(ChordShape& shape)
{
    const int strings = static_cast<int>(shape.frets.size());
    shape.fingers.assign(strings, 0);
    shape.barres.clear();

    std::vector<int> fretted;

    for (int s = 0; s < strings; ++s)
    {
        if (shape.frets[s] > 0)
        {
            fretted.push_back(s);
        }
    }

    if (fretted.empty())
    {
        return true;
    }

    std::stable_sort(fretted.begin(), fretted.end(), [&](int a, int b) { return shape.frets[a] < shape.frets[b]; });
    const int lowest = shape.frets[fretted.front()];

    struct Group
    {
        int fret;
        int from;
        int to;
    };

    // Separate fingers; a barre at the lowest fret; that plus small barres; small barres alone.
    constexpr std::array<std::array<bool, 2>, 4> kTries = {
        {{false, false}, {true, false}, {true, true}, {false, true}}};

    for (const auto& [barreLowest, mergeRuns] : kTries)
    {
        std::vector<Group> groups;
        std::vector<bool> used(strings, false);

        if (barreLowest)
        {
            std::vector<int> atLowest;

            for (const int s : fretted)
            {
                if (shape.frets[s] == lowest)
                {
                    atLowest.push_back(s);
                }
            }

            std::sort(atLowest.begin(), atLowest.end());

            if (atLowest.size() < 2)
            {
                continue;
            }

            const int from = atLowest.front();
            const int to = atLowest.back();
            bool covered = true;

            for (int s = from; s <= to; ++s)
            {
                covered = covered && shape.frets[s] >= lowest;
            }

            if (!covered)
            {
                continue;
            }

            groups.push_back({lowest, from, to});

            for (const int s : atLowest)
            {
                used[s] = true;
            }
        }

        for (const int s : fretted)
        {
            if (used[s])
            {
                continue;
            }

            int to = s;

            while (mergeRuns && to + 1 < strings && !used[to + 1] && shape.frets[to + 1] == shape.frets[s])
            {
                ++to;
            }

            for (int k = s; k <= to; ++k)
            {
                used[k] = true;
            }

            groups.push_back({shape.frets[s], s, to});
        }

        std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.fret < b.fret; });

        std::vector<int> fingers(strings, 0);
        std::vector<Barre> barres;
        int next = 1;
        bool fits = true;

        for (const auto& group : groups)
        {
            const int finger = std::max(next, 1 + group.fret - lowest);

            if (finger > kMaxFingers)
            {
                fits = false;
                break;
            }

            for (int s = group.from; s <= group.to; ++s)
            {
                if (shape.frets[s] == group.fret)
                {
                    fingers[s] = finger;
                }
            }

            if (group.to > group.from)
            {
                barres.push_back({group.fret, group.from, group.to, finger});
            }

            next = finger + 1;
        }

        if (fits)
        {
            shape.fingers = std::move(fingers);
            shape.barres = std::move(barres);
            return true;
        }
    }

    return false;
}

std::vector<ChordShape> ChordShapes(const Neck& neck, const ChordSearch& search)
{
    const int strings = static_cast<int>(neck.strings.size());
    const auto rules = RulesFor(neck, search);
    const int root = PitchClass(search.rootPitchClass);
    const bool ascending = neck.Ascending();

    if (strings == 0 || static_cast<int>(rules.required.size()) > rules.maxStrings)
    {
        return {};
    }

    std::vector<ChordShape> best;
    std::vector<int> current(strings, -1);

    for (int window = 1; window + kHandFrets - 1 <= neck.frets; ++window)
    {
        std::vector<std::vector<int>> options(strings);

        for (int s = 0; s < strings; ++s)
        {
            if (!rules.allStrings)
            {
                options[s].push_back(-1);
            }

            if (window <= kOpenWindowLimit && rules.allowed[PitchClass(neck.strings[s])])
            {
                options[s].push_back(0);
            }

            for (int fret = window; fret < window + kHandFrets; ++fret)
            {
                if (rules.allowed[PitchClass(neck.strings[s] + fret)])
                {
                    options[s].push_back(fret);
                }
            }
        }

        std::optional<ChordShape> windowBest;

        const auto leaf = [&] {
            int count = 0;
            int lowestFret = 99;
            int highestFret = 0;
            int bassNote = 1000;
            PitchSet have{};

            for (int s = 0; s < strings; ++s)
            {
                const int fret = current[s];

                if (fret < 0)
                {
                    continue;
                }

                ++count;
                have[PitchClass(neck.strings[s] + fret)] = true;
                bassNote = std::min(bassNote, neck.strings[s] + fret);

                if (fret > 0)
                {
                    lowestFret = std::min(lowestFret, fret);
                    highestFret = std::max(highestFret, fret);
                }
            }

            const bool allOpen = highestFret == 0;

            if (count < rules.minStrings || count > rules.maxStrings || (allOpen ? window != 1 : lowestFret != window))
            {
                return;
            }

            for (const int pc : rules.required)
            {
                if (!have[pc])
                {
                    return;
                }
            }

            if (rules.rootLowest && PitchClass(bassNote) != root)
            {
                return;
            }

            ChordShape shape;
            shape.frets = current;
            shape.position = allOpen ? 0 : lowestFret;
            shape.highestFret = highestFret;

            if (!AssignFingers(shape))
            {
                return;
            }

            int fretted = 0;
            int opens = 0;
            int first = -1;
            int last = -1;

            for (int s = 0; s < strings; ++s)
            {
                fretted += current[s] > 0 ? 1 : 0;
                opens += current[s] == 0 ? 1 : 0;

                if (!allOpen && current[s] == lowestFret)
                {
                    first = first < 0 ? s : first;
                    last = s;
                }
            }

            // Too many notes for one finger each, with an open string inside what would be the
            // barre (F as 1 0 3 2 1 1): the hand has to arch over it.
            int brokenBarre = 0;

            for (int s = first + 1; fretted > kMaxFingers && s < last; ++s)
            {
                brokenBarre += current[s] == 0 ? 1 : 0;
            }

            // An open string under a shape up the neck rings against it (C7 as x 3 5 3 5 0).
            const int openUpNeck = highestFret >= 5 ? opens : 0;

            const int span = allOpen ? 0 : highestFret - lowestFret;
            // More strings ring fuller (six is plenty); a stretch hurts more the wider it gets.
            shape.score = 3.0 * std::min(count, rules.fullStrings) - 0.6 * span * span - 0.3 * FingerCount(shape) -
                          2.0 * brokenBarre - 1.5 * openUpNeck;

            if (!windowBest || shape.score > windowBest->score)
            {
                windowBest = std::move(shape);
            }
        };

        // Strings in order; once a sounding string is followed by a muted one, the rest stay muted.
        std::function<void(int, bool, bool)> walk = [&](int s, bool started, bool ended) {
            if (s == strings)
            {
                leaf();
                return;
            }

            for (const int fret : options[s])
            {
                if (fret < 0)
                {
                    current[s] = -1;
                    walk(s + 1, started, started || ended);
                    continue;
                }

                if (ended)
                {
                    continue;
                }

                // On an ascending neck the first sounding string is the bass, so it must be the root.
                if (!started && ascending && rules.rootLowest && PitchClass(neck.strings[s] + fret) != root)
                {
                    continue;
                }

                current[s] = fret;
                walk(s + 1, true, false);
            }

            current[s] = -1;
        };

        walk(0, false, false);

        if (windowBest)
        {
            windowBest->caged = Caged(neck, *windowBest, root);
            best.push_back(std::move(*windowBest));
        }
    }

    if (best.empty())
    {
        return {};
    }

    std::stable_sort(best.begin(), best.end(),
                     [](const ChordShape& a, const ChordShape& b) { return a.score > b.score; });
    const double top = best.front().score;
    std::vector<ChordShape> picks;

    for (const auto& shape : best)
    {
        if (static_cast<int>(picks.size()) >= search.maxShapes)
        {
            break;
        }

        const bool near = std::any_of(picks.begin(), picks.end(),
                                      [&](const ChordShape& p) { return std::abs(p.position - shape.position) < 2; });

        if (shape.score >= top - kScoreSpread && !near)
        {
            picks.push_back(shape);
        }
    }

    // An extended-range guitar is played for its low strings: keep one shape that uses them.
    const int extraStrings = strings - 6;

    if (neck.instrument == Instrument::Guitar && extraStrings > 0)
    {
        const auto usesLow = [extraStrings](const ChordShape& s) {
            const int first = s.FirstSoundingString();
            return first >= 0 && first < extraStrings;
        };

        if (std::none_of(picks.begin(), picks.end(), usesLow))
        {
            const auto low = std::find_if(best.begin(), best.end(), [&](const ChordShape& s) {
                return usesLow(s) && s.score >= top - kExtendedRangeSpread;
            });

            if (low != best.end())
            {
                if (static_cast<int>(picks.size()) >= search.maxShapes)
                {
                    picks.pop_back();
                }

                picks.push_back(*low);
            }
        }
    }

    std::stable_sort(picks.begin(), picks.end(),
                     [](const ChordShape& a, const ChordShape& b) { return a.position < b.position; });
    return picks;
}

int NearestShape(const std::vector<ChordShape>& shapes, const ChordShape& previous)
{
    int nearest = -1;
    double nearestDistance = 0.0;

    for (int i = 0; i < static_cast<int>(shapes.size()); ++i)
    {
        const double distance = std::abs(shapes[i].AverageFret() - previous.AverageFret()) +
                                0.5 * std::abs(shapes[i].FirstSoundingString() - previous.FirstSoundingString());

        if (nearest < 0 || distance < nearestDistance)
        {
            nearest = i;
            nearestDistance = distance;
        }
    }

    return nearest;
}
} // namespace guitarfx::music
