#include "music/Arpeggios.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace guitarfx::music
{
namespace
{
constexpr double kNever = std::numeric_limits<double>::max();
constexpr int kOctaves = 8;
/// A run never starts on a tone above the octave: a 9th is a colour at the top, not a bass note.
constexpr int kOctave = 12;
constexpr int kMaxSweepOctaves = 3;
/// How far a two-per-string run may climb the neck before it stops being one shape.
constexpr int kMaxClimb = 16;
constexpr int kNoFinger = 0;
constexpr int kFingers = 4;

/// One rung of the chord stacked up from its root.
struct Rung
{
    int semitones = 0;
    int tone = 0;
};

/// What a style lets the hand do.
struct Reach
{
    int fewestOnString = 1;
    int mostOnString = 3;
    /// Frets the whole run may span.
    int span = 5;
    /// The furthest apart two notes on one string may be.
    int alongString = 5;
    /// The furthest the fret may move changing strings.
    int acrossStrings = 4;
};

constexpr Reach kSweepReach{};

Reach TwoPerStringReach(const Neck& neck)
{
    return {2, 2, neck.frets, 7, 5};
}

/// The chord's tones in pitch order, a few octaves up. An extended chord climbs its stack of thirds,
/// so its 9th takes the place of the octave root: R 3 5 7 9.
std::vector<Rung> ChordLadder(const std::vector<Interval>& intervals)
{
    std::vector<Rung> ladder;
    const bool extended = std::any_of(intervals.begin(), intervals.end(),
                                      [](const Interval& interval) { return interval.semitones >= kOctave; });

    for (int octave = 0; octave < kOctaves; ++octave)
    {
        for (int tone = 0; tone < static_cast<int>(intervals.size()); ++tone)
        {
            const int semitones = intervals[tone].semitones + octave * kOctave;

            if (!extended || semitones != kOctave)
            {
                ladder.push_back({semitones, tone});
            }
        }
    }

    std::stable_sort(ladder.begin(), ladder.end(),
                     [](const Rung& a, const Rung& b) { return a.semitones < b.semitones; });
    ladder.erase(std::unique(ladder.begin(), ladder.end(),
                             [](const Rung& a, const Rung& b) { return a.semitones == b.semitones; }),
                 ladder.end());
    return ladder;
}

/// The strings each note takes, lowest note first, and what that route costs.
struct Route
{
    std::vector<int> strings;
    double cost = kNever;
};

/// The cheapest way to lay `pitches` across strings `first` … `last` inside the frets from `low` to
/// `high`, starting on the first string, ending on the last and missing none. Most of the cost is
/// the fret moved, then the second notes on a string: cheapest on the top string, where the run
/// turns round on a hammer-on and pull-off, dearer on the first and dearest in the middle, where it
/// breaks the sweep. That is what puts the classic shapes' extra notes where they are.
Route CheapestRoute(const Neck& neck, const std::vector<int>& pitches, int first, int last, int low, int high,
                    const Reach& reach)
{
    constexpr double kExtraOnTopString = 1.0;
    constexpr double kExtraOnFirstString = 3.0;
    constexpr double kExtraInside = 4.5;
    constexpr double kFretAlong = 0.1;
    constexpr double kFretAcross = 0.5;

    const int notes = static_cast<int>(pitches.size());
    const int width = last - first + 1;
    const int depth = reach.mostOnString;
    const auto cell = [&](int note, int string, int placed) { return ((note * width + string) * depth) + placed; };
    const auto fretOf = [&](int note, int string) { return pitches[note] - neck.strings[first + string]; };
    const auto fits = [&](int fret) { return fret >= low && fret <= high; };

    std::vector<double> costs(static_cast<std::size_t>(notes * width * depth), kNever);
    std::vector<int> cameFrom(costs.size(), -1);

    if (!fits(fretOf(0, 0)))
    {
        return {};
    }

    costs[cell(0, 0, 0)] = 0.0;

    const auto relax = [&](int to, int from, double cost) {
        if (cost < costs[to])
        {
            costs[to] = cost;
            cameFrom[to] = from;
        }
    };

    for (int note = 0; note + 1 < notes; ++note)
    {
        for (int string = 0; string < width; ++string)
        {
            for (int placed = 0; placed < depth; ++placed)
            {
                const int here = cell(note, string, placed);

                if (costs[here] == kNever)
                {
                    continue;
                }

                const int fret = fretOf(note, string);

                if (placed + 1 < depth)
                {
                    const int next = fretOf(note + 1, string);

                    if (fits(next) && next - fret <= reach.alongString)
                    {
                        const double extra = string == width - 1 ? kExtraOnTopString
                                             : string == 0       ? kExtraOnFirstString
                                                                 : kExtraInside;
                        relax(cell(note + 1, string, placed + 1), here,
                              costs[here] + extra + (next - fret) * kFretAlong);
                    }
                }

                if (string + 1 < width && placed + 1 >= reach.fewestOnString)
                {
                    const int next = fretOf(note + 1, string + 1);

                    if (fits(next) && std::abs(next - fret) <= reach.acrossStrings)
                    {
                        relax(cell(note + 1, string + 1, 0), here, costs[here] + std::abs(next - fret) * kFretAcross);
                    }
                }
            }
        }
    }

    int end = -1;

    for (int placed = reach.fewestOnString - 1; placed < depth; ++placed)
    {
        const int candidate = cell(notes - 1, width - 1, placed);

        if (costs[candidate] != kNever && (end < 0 || costs[candidate] < costs[end]))
        {
            end = candidate;
        }
    }

    if (end < 0)
    {
        return {};
    }

    Route route;
    route.cost = costs[end];
    route.strings.resize(pitches.size());

    for (int note = notes - 1, at = end; note >= 0; --note)
    {
        route.strings[note] = first + ((at / depth) % width);
        at = cameFrom[at];
    }

    return route;
}

/// Fingers for a run: each move wants the fingers as far apart as the frets, neighbouring strings at
/// one fret want one finger rolled across them, and a finger never crosses another. Up the neck the
/// hand may shift, more easily changing strings, so a two-per-string run can go 1-4, 1-4, 1-4.
void AssignRunFingers(std::vector<ArpeggioNote>& notes, int fretFrom, double handPositionWeight)
{
    constexpr double kBrokenRoll = 1.5;
    constexpr double kSlide = 4.0;
    constexpr double kCrossed = 6.0;
    constexpr double kShiftChangingStrings = 0.5;

    std::vector<int> fretted;

    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
    {
        notes[i].finger = kNoFinger;

        if (notes[i].fret > 0)
        {
            fretted.push_back(i);
        }
    }

    if (fretted.empty())
    {
        return;
    }

    using Row = std::array<double, kFingers>;
    const auto seat = [&](int note, int finger) {
        const int natural = std::min(notes[note].fret - fretFrom, kFingers - 1);
        return std::abs(finger - natural) * handPositionWeight;
    };
    const auto move = [&](int from, int fingerFrom, int to, int fingerTo) {
        const int frets = notes[to].fret - notes[from].fret;
        const int fingers = fingerTo - fingerFrom;

        if (frets == 0)
        {
            return fingers == 0 ? 0.0 : kBrokenRoll;
        }

        if (fingers == 0)
        {
            return kSlide + std::abs(frets);
        }

        if ((frets > 0) != (fingers > 0))
        {
            return kCrossed + std::abs(frets);
        }

        const double mismatch = std::abs(std::abs(frets) - std::abs(fingers));
        return notes[to].string == notes[from].string ? mismatch : mismatch * kShiftChangingStrings;
    };

    std::vector<Row> costs(fretted.size());
    std::vector<std::array<int, kFingers>> cameFrom(fretted.size());

    for (int finger = 0; finger < kFingers; ++finger)
    {
        costs[0][finger] = seat(fretted[0], finger);
        cameFrom[0][finger] = -1;
    }

    for (std::size_t i = 1; i < fretted.size(); ++i)
    {
        for (int finger = 0; finger < kFingers; ++finger)
        {
            costs[i][finger] = kNever;

            for (int before = 0; before < kFingers; ++before)
            {
                const double cost = costs[i - 1][before] + move(fretted[i - 1], before, fretted[i], finger);

                if (cost < costs[i][finger])
                {
                    costs[i][finger] = cost;
                    cameFrom[i][finger] = before;
                }
            }

            costs[i][finger] += seat(fretted[i], finger);
        }
    }

    int finger = static_cast<int>(std::min_element(costs.back().begin(), costs.back().end()) - costs.back().begin());

    for (std::size_t i = fretted.size(); i-- > 0;)
    {
        notes[fretted[i]].finger = finger + 1;
        finger = cameFrom[i][finger];
    }
}

/// A stroke follows the pick, not the pitch: towards the floor is down, whichever way the notes go.
PickStroke StrokeBetween(const ArpeggioNote& from, const ArpeggioNote& to)
{
    if (from.string == to.string)
    {
        return to.fret > from.fret ? PickStroke::Hammer : PickStroke::Pull;
    }

    return to.string > from.string ? PickStroke::Down : PickStroke::Up;
}

std::vector<ArpeggioStep> UpAndBack(const std::vector<ArpeggioNote>& notes)
{
    std::vector<ArpeggioStep> steps;
    const int count = static_cast<int>(notes.size());

    for (int i = 0; i < count; ++i)
    {
        steps.push_back({i, i == 0 ? PickStroke::Down : StrokeBetween(notes[i - 1], notes[i])});
    }

    for (int i = count - 2; i >= 0; --i)
    {
        steps.push_back({i, StrokeBetween(notes[i + 1], notes[i])});
    }

    return steps;
}

ArpeggioShape FinishShape(std::vector<ArpeggioNote> notes, int startTone, double handPositionWeight)
{
    ArpeggioShape shape;
    shape.startTone = startTone;
    shape.fretFrom = std::numeric_limits<int>::max();
    shape.fromString = notes.front().string;
    shape.toString = notes.front().string;

    for (const auto& note : notes)
    {
        shape.fromString = std::min(shape.fromString, note.string);
        shape.toString = std::max(shape.toString, note.string);
        shape.fretTo = std::max(shape.fretTo, note.fret);

        if (note.fret > 0)
        {
            shape.fretFrom = std::min(shape.fretFrom, note.fret);
        }
    }

    shape.fretFrom = shape.fretFrom == std::numeric_limits<int>::max() ? 0 : shape.fretFrom;
    AssignRunFingers(notes, shape.fretFrom, handPositionWeight);
    shape.steps = UpAndBack(notes);
    shape.notes = std::move(notes);
    return shape;
}

int FretSpan(const Route& route, const std::vector<int>& pitches, const Neck& neck)
{
    int lowest = std::numeric_limits<int>::max();
    int highest = 0;

    for (std::size_t i = 0; i < pitches.size(); ++i)
    {
        const int fret = pitches[i] - neck.strings[route.strings[i]];
        lowest = std::min(lowest, fret);
        highest = std::max(highest, fret);
    }

    return highest - lowest;
}

/// The best way to play the `count` rungs from `start`, on the strings from `first` to `last`,
/// `fret` being where the run starts.
Route BestRoute(const Neck& neck, const std::vector<Rung>& ladder, std::size_t start, int count, int first, int last,
                int fret, const Reach& reach)
{
    constexpr double kFretOfSpan = 0.3;

    const int rootMidi = neck.strings[first] + fret - ladder[start].semitones;
    std::vector<int> pitches;

    for (int i = 0; i < count; ++i)
    {
        pitches.push_back(rootMidi + ladder[start + i].semitones);
    }

    Route best;
    const int lowest = reach.span >= neck.frets ? 1 : std::max(1, fret - reach.span);
    const int highest = reach.span >= neck.frets ? 1 : fret;

    for (int low = lowest; low <= highest; ++low)
    {
        auto route = CheapestRoute(neck, pitches, first, last, low, std::min(neck.frets, low + reach.span), reach);

        if (route.strings.empty())
        {
            continue;
        }

        route.cost += FretSpan(route, pitches, neck) * kFretOfSpan;

        if (route.cost < best.cost)
        {
            best = std::move(route);
        }
    }

    return best;
}

ArpeggioShape ShapeFromRoute(const Neck& neck, const std::vector<Rung>& ladder, std::size_t start, int fret,
                             const Route& route, double handPositionWeight)
{
    const int rootMidi = neck.strings[route.strings.front()] + fret - ladder[start].semitones;
    std::vector<ArpeggioNote> notes;

    for (std::size_t i = 0; i < route.strings.size(); ++i)
    {
        const auto& rung = ladder[start + i];
        notes.push_back({route.strings[i], rootMidi + rung.semitones - neck.strings[route.strings[i]], rung.tone});
    }

    return FinishShape(std::move(notes), ladder[start].tone, handPositionWeight);
}

/// Where a sweep may finish, in semitones above the root: the tone it started on, however many
/// octaves the strings want, or an extended chord's 9th, the top of its stack of thirds.
std::vector<int> SweepTops(int start, int topInterval)
{
    std::vector<int> tops;

    for (int octaves = 0; octaves < kMaxSweepOctaves; ++octaves)
    {
        tops.push_back(start + (octaves + 1) * kOctave);

        if (topInterval >= kOctave && topInterval > start)
        {
            tops.push_back(topInterval + octaves * kOctave);
        }
    }

    return tops;
}

/// Sweep and two-per-string shapes: from each chord tone on the lowest string of the set.
std::vector<ArpeggioShape> RoutedShapes(const Neck& neck, const ArpeggioSearch& search)
{
    constexpr double kHandStays = 0.5;
    constexpr double kHandShifts = 0.0;

    std::vector<ArpeggioShape> shapes;
    const auto ladder = ChordLadder(search.intervals);
    const bool sweep = search.style == ArpeggioStyle::Sweep;
    const int strings = ArpeggioStringsFor(neck, search.strings);
    const int last = static_cast<int>(neck.strings.size()) - 1;
    const int first = last - strings + 1;
    const Reach reach = sweep ? kSweepReach : TwoPerStringReach(neck);
    int topInterval = 0;

    for (const auto& interval : search.intervals)
    {
        topInterval = std::max(topInterval, interval.semitones);
    }

    for (std::size_t start = 0; start < ladder.size() && ladder[start].semitones < kOctave; ++start)
    {
        for (int fret = 1; fret <= neck.frets; ++fret)
        {
            if (PitchClass(neck.strings[first] + fret) != PitchClass(search.rootPitchClass + ladder[start].semitones))
            {
                continue;
            }

            Route best;

            if (sweep)
            {
                const auto tryCount = [&](int count) {
                    if (count < strings || count > strings * reach.mostOnString)
                    {
                        return;
                    }

                    if (auto route = BestRoute(neck, ladder, start, count, first, last, fret, reach);
                        route.cost < best.cost)
                    {
                        best = std::move(route);
                    }
                };

                for (const int top : SweepTops(ladder[start].semitones, topInterval))
                {
                    const auto end = std::find_if(ladder.begin(), ladder.end(),
                                                  [top](const Rung& rung) { return rung.semitones == top; });

                    if (end != ladder.end())
                    {
                        tryCount(static_cast<int>(end - ladder.begin()) - static_cast<int>(start) + 1);
                    }
                }

                // A tuning the octaves do not fit (DADGAD's top strings are close together) still
                // has shapes: the run stops wherever the last string does, having sounded every tone.
                if (best.strings.empty())
                {
                    const int tones = static_cast<int>(search.intervals.size());

                    for (int count = std::max(strings, tones); count <= strings * reach.mostOnString; ++count)
                    {
                        tryCount(count);
                    }
                }
            }
            else
            {
                best = BestRoute(neck, ladder, start, strings * 2, first, last, fret, reach);
            }

            if (best.strings.empty())
            {
                continue;
            }

            auto shape = ShapeFromRoute(neck, ladder, start, fret, best, sweep ? kHandStays : kHandShifts);

            if (sweep || shape.fretTo - shape.fretFrom <= kMaxClimb)
            {
                shapes.push_back(std::move(shape));
            }
        }
    }

    return shapes;
}

/// Box shapes: a hand position from each chord tone on the lowest-pitched string, every chord tone
/// in it once, lowest to highest. Where one pitch falls twice in the position the lower fret plays
/// it, so the hand stays back.
std::vector<ArpeggioShape> BoxShapes(const Neck& neck, const ArpeggioSearch& search)
{
    constexpr double kHandStays = 1.0;
    constexpr int kFewestNotes = 3;

    struct Cell
    {
        int midi = 0;
        ArpeggioNote note;
    };

    std::array<int, kPitchClasses> toneOf{};
    toneOf.fill(-1);

    for (int tone = static_cast<int>(search.intervals.size()) - 1; tone >= 0; --tone)
    {
        toneOf[PitchClass(search.rootPitchClass + search.intervals[tone].semitones)] = tone;
    }

    std::vector<ArpeggioShape> shapes;
    const int lowestString = neck.LowestString();
    const int lowestOpen = neck.strings[lowestString];

    for (const auto& interval : search.intervals)
    {
        if (interval.semitones >= kOctave)
        {
            continue;
        }

        const int start = PitchClass(search.rootPitchClass + interval.semitones - lowestOpen);

        for (int low = start; low + kHandFrets <= neck.frets; low += kOctave)
        {
            std::vector<Cell> cells;

            for (int string = 0; string < static_cast<int>(neck.strings.size()); ++string)
            {
                for (int fret = low; fret <= low + kHandFrets; ++fret)
                {
                    const int midi = neck.strings[string] + fret;

                    if (const int tone = toneOf[PitchClass(midi)]; tone >= 0)
                    {
                        cells.push_back({midi, {string, fret, tone}});
                    }
                }
            }

            std::stable_sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
                return a.midi != b.midi ? a.midi < b.midi : a.note.fret < b.note.fret;
            });
            cells.erase(
                std::unique(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.midi == b.midi; }),
                cells.end());

            if (static_cast<int>(cells.size()) < kFewestNotes)
            {
                continue;
            }

            std::vector<ArpeggioNote> notes;

            for (const auto& cell : cells)
            {
                notes.push_back(cell.note);
            }

            const int startTone = notes.front().tone;
            shapes.push_back(FinishShape(std::move(notes), startTone, kHandStays));
        }
    }

    return shapes;
}
} // namespace

std::string_view ArpeggioStyleId(ArpeggioStyle style)
{
    switch (style)
    {
    case ArpeggioStyle::TwoPerString:
        return "two";
    case ArpeggioStyle::Box:
        return "box";
    case ArpeggioStyle::Sweep:
        break;
    }

    return "sweep";
}

std::optional<ArpeggioStyle> ParseArpeggioStyle(std::string_view id)
{
    for (const auto style : {ArpeggioStyle::Sweep, ArpeggioStyle::TwoPerString, ArpeggioStyle::Box})
    {
        if (ArpeggioStyleId(style) == id)
        {
            return style;
        }
    }

    return std::nullopt;
}

std::string_view PickStrokeId(PickStroke stroke)
{
    switch (stroke)
    {
    case PickStroke::Up:
        return "up";
    case PickStroke::Hammer:
        return "hammer";
    case PickStroke::Pull:
        return "pull";
    case PickStroke::Slide:
        return "slide";
    case PickStroke::Down:
        break;
    }

    return "down";
}

std::vector<int> ArpeggioStringChoices(const Neck& neck)
{
    std::vector<int> choices;

    for (int strings = kMinArpeggioStrings; strings <= static_cast<int>(neck.strings.size()); ++strings)
    {
        choices.push_back(strings);
    }

    return choices;
}

int ArpeggioStringsFor(const Neck& neck, int requested)
{
    const int count = static_cast<int>(neck.strings.size());

    if (requested >= kMinArpeggioStrings && requested <= count)
    {
        return requested;
    }

    return std::min(kDefaultArpeggioStrings, count);
}

std::vector<int> ArpeggioStringsWithRuns(const Neck& neck, const ArpeggioSearch& search)
{
    const auto all = ArpeggioStringChoices(neck);

    if (search.style == ArpeggioStyle::Box)
    {
        return all;
    }

    std::vector<int> choices;

    for (const int strings : all)
    {
        auto oneCount = search;
        oneCount.strings = strings;

        if (!ArpeggioShapes(neck, oneCount).empty())
        {
            choices.push_back(strings);
        }
    }

    return choices.empty() ? all : choices;
}

int ClosestArpeggioStrings(const std::vector<int>& choices, int wanted)
{
    int closest = choices.empty() ? wanted : choices.front();

    for (const int strings : choices)
    {
        if (strings <= wanted)
        {
            closest = strings;
        }
    }

    return closest;
}

std::vector<ArpeggioShape> ArpeggioShapes(const Neck& neck, const ArpeggioSearch& search)
{
    if (neck.strings.size() < static_cast<std::size_t>(kMinArpeggioStrings) || search.intervals.empty())
    {
        return {};
    }

    if (search.style != ArpeggioStyle::Box && !neck.Ascending())
    {
        return {};
    }

    auto shapes = search.style == ArpeggioStyle::Box ? BoxShapes(neck, search) : RoutedShapes(neck, search);
    std::stable_sort(shapes.begin(), shapes.end(),
                     [](const ArpeggioShape& a, const ArpeggioShape& b) { return a.fretFrom < b.fretFrom; });
    return shapes;
}
} // namespace guitarfx::music
