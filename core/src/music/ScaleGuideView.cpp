#include "music/ScaleGuideView.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace guitarfx::music
{
namespace
{
using nlohmann::json;

/// The neck the request plays on: its tuning (or the player's own), cut off at the capo.
struct ResolvedNeck
{
    Neck neck;
    std::string tuningId;
    std::string tuningName;
    bool flats = false;
};

ResolvedNeck Resolve(const ScaleGuideRequest& request)
{
    ResolvedNeck resolved;
    const auto count = static_cast<int>(request.strings.size());

    if (request.tuning == "custom" && count >= kMinStrings && count <= kMaxStrings)
    {
        resolved.neck.strings = request.strings;
        resolved.neck.instrument = request.instrument;
        resolved.tuningId = "custom";
        resolved.tuningName = "Custom";
    }
    else
    {
        const auto* tuning = FindTuning(request.tuning);

        if (tuning == nullptr)
        {
            tuning = &Tunings().front();
        }

        resolved.neck.strings = tuning->strings;
        resolved.neck.instrument = tuning->instrument;
        resolved.tuningId = tuning->id;
        resolved.tuningName = tuning->name;
        resolved.flats = tuning->flats;
    }

    resolved.neck.frets = request.frets;
    return resolved;
}

/// The neck above a capo: its strings sound `capo` higher and it has that many fewer frets.
Neck AboveCapo(const Neck& neck, int capo)
{
    Neck shifted = neck;

    for (auto& note : shifted.strings)
    {
        note += capo;
    }

    shifted.frets = neck.frets - capo;
    return shifted;
}

json IntervalLabels(const SpelledSet& set)
{
    std::string formula;

    for (const auto& note : set.notes)
    {
        formula += (formula.empty() ? "" : " ") + note.interval.label;
    }

    return formula;
}

json TonesJson(const SpelledSet& set)
{
    json tones = json::array();

    for (const auto& note : set.notes)
    {
        tones.push_back({{"name", note.name},
                         {"interval", note.interval.label},
                         {"degree", note.interval.degree},
                         {"pitchClass", note.pitchClass},
                         {"root", note.interval.degree == 1 && note.interval.semitones == 0}});
    }

    return tones;
}

json ShapeJson(const ChordShape& shape, int capo)
{
    json frets = json::array();

    for (const int fret : shape.frets)
    {
        frets.push_back(fret < 0 ? -1 : fret + capo);
    }

    json barres = json::array();

    for (const auto& barre : shape.barres)
    {
        barres.push_back({{"fret", barre.fret + capo},
                          {"fromString", barre.fromString},
                          {"toString", barre.toString},
                          {"finger", barre.finger}});
    }

    return {{"frets", frets},
            {"fingers", shape.fingers},
            {"barres", barres},
            {"position", shape.position + capo},
            {"caged", shape.caged}};
}

/// How a run is named: by where it starts, as a player thinks of an inversion.
std::string RunName(const ArpeggioShape& shape, const SpelledSet& tones)
{
    return shape.startTone == 0 ? std::string("Root position")
                                : "From the " + tones.notes[shape.startTone].interval.label;
}

json RunJson(const ArpeggioShape& shape, const SpelledSet& tones, const std::string& name)
{
    json notes = json::array();

    for (const auto& note : shape.notes)
    {
        const auto& tone = tones.notes[note.tone];
        notes.push_back({{"string", note.string},
                         {"fret", note.fret},
                         {"tone", note.tone},
                         {"name", tone.name},
                         {"interval", tone.interval.label},
                         {"root", note.tone == 0},
                         {"finger", note.finger}});
    }

    json steps = json::array();

    for (const auto& step : shape.steps)
    {
        steps.push_back({{"note", step.note}, {"stroke", std::string(PickStrokeId(step.stroke))}});
    }

    return {{"name", name},
            {"start", tones.notes[shape.startTone].interval.label},
            {"fretFrom", shape.fretFrom},
            {"fretTo", shape.fretTo},
            {"fromString", shape.fromString},
            {"toString", shape.toString},
            {"notes", notes},
            {"steps", steps}};
}

/// A scale position as a run, so it gets the same tab and playback as an arpeggio: its notes rising
/// and then falling, alternate-picked. A pitch two strings share in the box is played once.
ArpeggioShape ScalePositionRun(const Neck& neck, const std::vector<FretPos>& cells,
                               const std::array<int, kPitchClasses>& toneIndex)
{
    const auto pitch = [&neck](const FretPos& cell) { return neck.strings[cell.string] + cell.fret; };
    auto rising = cells;
    std::sort(rising.begin(), rising.end(), [&pitch](const FretPos& a, const FretPos& b) {
        return pitch(a) != pitch(b) ? pitch(a) < pitch(b) : a.string < b.string;
    });
    rising.erase(std::unique(rising.begin(), rising.end(),
                             [&pitch](const FretPos& a, const FretPos& b) { return pitch(a) == pitch(b); }),
                 rising.end());

    ArpeggioShape run;

    if (rising.empty())
    {
        return run;
    }

    run.fretFrom = rising.front().fret;
    run.fretTo = rising.front().fret;
    run.fromString = rising.front().string;
    run.toString = rising.front().string;

    for (const auto& cell : rising)
    {
        run.notes.push_back({cell.string, cell.fret, std::max(0, toneIndex[PitchClass(pitch(cell))]), 0});
        run.fretFrom = std::min(run.fretFrom, cell.fret);
        run.fretTo = std::max(run.fretTo, cell.fret);
        run.fromString = std::min(run.fromString, cell.string);
        run.toString = std::max(run.toString, cell.string);
    }

    const int count = static_cast<int>(run.notes.size());

    for (int step = 0; step < 2 * count - 1; ++step)
    {
        run.steps.push_back(
            {step < count ? step : 2 * count - 2 - step, step % 2 == 0 ? PickStroke::Down : PickStroke::Up});
    }

    run.startTone = run.notes.front().tone;
    return run;
}

/// How a style is played, for the page's quick reference.
std::string ArpeggioAdvice(ArpeggioStyle style)
{
    switch (style)
    {
    case ArpeggioStyle::TwoPerString:
        return "Two notes on every string, the hand shifting up the neck as it crosses: pick the first and hammer "
               "the second, then pick and pull off coming back.";
    case ArpeggioStyle::Box:
        return "Every chord tone under one hand position, a good way to learn where they sit. The tab shows one way "
               "through; try it picked as well as swept.";
    case ArpeggioStyle::Sweep:
        break;
    }

    return "One pick stroke across the strings, down going up and up coming back, with a hammer-on and pull-off "
           "where a string has two notes. Lift each finger as the next note sounds, so the notes never ring together.";
}

/// Why a style has nothing to show here.
std::string ArpeggioEmptyNote(ArpeggioStyle style, const Neck& neck)
{
    if (style != ArpeggioStyle::Box && !neck.Ascending())
    {
        return "This tuning's strings do not rise in pitch, so a run cannot cross them in one sweep: try Box. "
               "The neck still marks all the chord's notes.";
    }

    return "Nothing playable for this chord in this style: try Box. The neck still marks all its notes.";
}

/// A run over a capo: its frets as the neck numbers them.
ArpeggioShape OnNeck(ArpeggioShape shape, int capo)
{
    for (auto& note : shape.notes)
    {
        note.fret += capo;
    }

    shape.fretFrom += capo;
    shape.fretTo += capo;
    return shape;
}

/// Every chord of a progression with a shape, voice-led from the one showing: walking on from
/// the current step, each chord takes the shape nearest the last, as stepping through it would.
json ProgressionStrip(const Neck& neck, const std::vector<SpelledChord>& chords, int step,
                      const std::vector<ChordShape>& currentShapes, int anchor, bool allowInversions, int capo)
{
    const int count = static_cast<int>(chords.size());
    std::vector<json> strip(count);
    std::vector<std::pair<std::string, std::vector<ChordShape>>> searched;
    std::optional<ChordShape> previous;

    for (int k = 0; k < count; ++k)
    {
        const int index = (step + k) % count;
        const auto& chord = chords[index];
        const std::vector<ChordShape>* shapes = &currentShapes;

        if (k > 0)
        {
            const std::string key = std::to_string(chord.rootPitchClass) + chord.def->id;
            auto found = std::find_if(searched.begin(), searched.end(), [&](const auto& s) { return s.first == key; });

            if (found == searched.end())
            {
                ChordSearch search;
                search.rootPitchClass = chord.rootPitchClass;
                search.intervals = chord.def->intervals;
                search.allowInversions = allowInversions;
                searched.emplace_back(key, ChordShapes(neck, search));
                found = std::prev(searched.end());
            }

            shapes = &found->second;
        }

        json shape = nullptr;

        if (!shapes->empty())
        {
            const int pick = k == 0 ? anchor : previous ? std::max(0, NearestShape(*shapes, *previous)) : 0;
            previous = (*shapes)[pick];
            shape = ShapeJson((*shapes)[pick], capo);
        }

        strip[index] = {{"name", chord.name},
                        {"numeral", RomanNumeral(*chord.roman)},
                        {"rootPitchClass", chord.rootPitchClass},
                        {"shape", shape}};
    }

    return strip;
}

/// The request's progression in its key, or no chords for a single chord.
struct ResolvedProgression
{
    std::vector<SpelledChord> chords;
    int step = 0;
    /// Its id, name, step and the next chord's name: null without a progression.
    json header;
};

ResolvedProgression ResolveProgression(const ScaleGuideRequest& request)
{
    ResolvedProgression resolved;
    std::vector<RomanChord> romans = request.progressionChords;
    std::string id = romans.empty() ? std::string() : "custom";
    std::string name = request.progressionName;

    if (romans.empty())
    {
        if (const auto* progression = FindProgression(request.progression))
        {
            romans = progression->chords;
            id = progression->id;
            name = progression->name;
        }
    }

    if (!romans.empty())
    {
        resolved.chords = ChordsInKey(request.key, romans);
        resolved.step = request.step % static_cast<int>(resolved.chords.size());
        resolved.header = {{"id", id},
                           {"name", name},
                           {"step", resolved.step},
                           {"next", resolved.chords[(resolved.step + 1) % resolved.chords.size()].name}};
    }

    return resolved;
}

/// A progression's arpeggios as one piece of tab: every chord's chosen run end to end, a bar each.
/// Its notes are numbered through the whole piece, and a bar says which of the steps are its own.
json ProgressionRunJson(const std::vector<SpelledChord>& chords, const std::vector<ArpeggioProgressionChord>& walked,
                        const std::string& name, bool slides, int capo)
{
    json notes = json::array();
    json steps = json::array();
    json bars = json::array();
    std::vector<int> firstNote(chords.size(), 0);
    int fretFrom = 0;
    int fretTo = 0;
    bool any = false;

    for (std::size_t i = 0; i < chords.size(); ++i)
    {
        firstNote[i] = static_cast<int>(notes.size());
        json bar = {{"chord", static_cast<int>(i)},
                    {"name", chords[i].name},
                    {"numeral", RomanNumeral(*chords[i].roman)},
                    {"from", 0},
                    {"count", 0}};

        if (walked[i].chosen >= 0)
        {
            const auto run = OnNeck(walked[i].runs[walked[i].chosen], capo);
            const auto runJson = RunJson(run, chords[i].notes, RunName(run, chords[i].notes));
            notes.insert(notes.end(), runJson["notes"].begin(), runJson["notes"].end());
            bar["run"] = runJson["name"];
            bar["fretFrom"] = run.fretFrom;
            bar["fretTo"] = run.fretTo;
            fretFrom = any ? std::min(fretFrom, run.fretFrom) : run.fretFrom;
            fretTo = any ? std::max(fretTo, run.fretTo) : run.fretTo;
            any = true;
        }

        bars.push_back(std::move(bar));
    }

    for (const auto& step : ArpeggioProgressionSteps(walked, slides))
    {
        auto& bar = bars[static_cast<std::size_t>(step.chord)];

        if (bar["count"] == 0)
        {
            bar["from"] = static_cast<int>(steps.size());
        }

        bar["count"] = bar["count"].get<int>() + 1;
        steps.push_back(
            {{"note", firstNote[step.chord] + step.note}, {"stroke", std::string(PickStrokeId(step.stroke))}});
    }

    return {{"name", name},   {"slides", slides}, {"fretFrom", fretFrom}, {"fretTo", fretTo},
            {"notes", notes}, {"steps", steps},   {"bars", bars}};
}

std::vector<int> ShiftedFrets(const std::vector<int>& frets, int capo)
{
    std::vector<int> out;

    for (const int fret : frets)
    {
        out.push_back(fret < 0 ? -1 : std::max(0, fret - capo));
    }

    return out;
}
} // namespace

json BuildScaleGuideView(const ScaleGuideRequest& request)
{
    const auto resolved = Resolve(request);
    const auto& neck = resolved.neck;
    const int capo = std::min(request.capo, neck.frets - kMinFrets / 2);
    const Neck playing = AboveCapo(neck, capo);
    const bool chordMode = request.mode == "chord";
    const bool arpeggioMode = request.mode == "arpeggio";

    json view;
    view["mode"] = request.mode;
    view["key"] = request.key;
    view["instrument"] = std::string(InstrumentId(neck.instrument));
    view["tuning"] = {{"id", resolved.tuningId}, {"name", resolved.tuningName}};
    view["frets"] = neck.frets;
    view["capo"] = capo;

    json strings = json::array();

    for (const int note : neck.strings)
    {
        strings.push_back({{"openMidi", note}, {"name", PlainNoteName(note, resolved.flats)}});
    }

    view["strings"] = strings;

    SpelledSet tones;
    std::vector<std::vector<FretPos>> positionCells;
    std::vector<ArpeggioShape> runs;
    int position = request.position;

    if (arpeggioMode)
    {
        const auto* def = FindChord(request.chord);
        const auto progression = ResolveProgression(request);
        const auto chord = progression.chords.empty()
                               ? SpellChord(request.key, def != nullptr ? *def : Chords().front())
                               : progression.chords[progression.step];
        tones = chord.notes;

        ArpeggioSearch search;
        search.rootPitchClass = chord.rootPitchClass;
        search.intervals = chord.def->intervals;
        search.style = request.arpeggioStyle;
        // The saved count stands for the next style even where this one plays fewer.
        const auto stringChoices = ArpeggioStringsWithRuns(playing, search);
        search.strings = ClosestArpeggioStrings(stringChoices, ArpeggioStringsFor(playing, request.arpeggioStrings));
        json runsJson = json::array();

        for (auto& shape : ArpeggioShapes(playing, search))
        {
            runs.push_back(OnNeck(std::move(shape), capo));
            std::vector<FretPos> cells;

            for (const auto& note : runs.back().notes)
            {
                cells.push_back({note.string, note.fret});
            }

            positionCells.push_back(std::move(cells));
            runsJson.push_back(RunJson(runs.back(), tones, RunName(runs.back(), tones)));
        }

        view["title"] = chord.name + " arpeggio";
        view["chord"] = {{"id", chord.def->id},
                         {"name", chord.name},
                         {"quality", chord.def->name},
                         {"formula", IntervalLabels(tones)}};
        view["arpeggio"] = {{"style", std::string(ArpeggioStyleId(request.arpeggioStyle))},
                            {"strings", search.strings},
                            {"stringChoices", stringChoices},
                            {"advice", ArpeggioAdvice(request.arpeggioStyle)},
                            {"empty", ArpeggioEmptyNote(request.arpeggioStyle, playing)},
                            {"runs", runsJson}};

        if (!progression.chords.empty())
        {
            // One chord's run is the player's; the others follow it round the progression.
            ArpeggioProgressionSearch walk;
            walk.style = request.arpeggioStyle;
            walk.strings = request.arpeggioStrings;
            walk.slides = request.arpeggioSlides;
            walk.anchorChord = request.anchorStep >= 0 ? request.anchorStep : progression.step;
            walk.anchorChord %= static_cast<int>(progression.chords.size());
            walk.anchorRun = std::max(0, request.position - 1);
            const auto walked = ArpeggioProgression(playing, progression.chords, walk);
            json chords = json::array();

            for (std::size_t i = 0; i < progression.chords.size(); ++i)
            {
                chords.push_back({{"name", progression.chords[i].name},
                                  {"numeral", RomanNumeral(*progression.chords[i].roman)},
                                  {"rootPitchClass", progression.chords[i].rootPitchClass},
                                  {"shape", nullptr},
                                  {"position", walked[i].chosen + 1}});
            }

            // The whole neck stays the whole neck; a chosen run becomes this chord's place in the walk.
            const int anchorPosition = request.position > 0 ? walked[walk.anchorChord].chosen + 1 : 0;
            position = request.position > 0 ? walked[progression.step].chosen + 1 : 0;
            view["progression"] = progression.header;
            view["progression"]["chords"] = chords;
            view["progression"]["anchorStep"] = walk.anchorChord;
            view["progression"]["anchorPosition"] = anchorPosition;
            view["progressionRun"] =
                ProgressionRunJson(progression.chords, walked, progression.header.value("name", std::string()),
                                   request.arpeggioSlides, capo);
        }
    }
    else if (!chordMode)
    {
        const auto* scale = FindScale(request.scale);

        if (scale == nullptr)
        {
            scale = &Scales().front();
        }

        tones = SpellBest(request.key, scale->intervals);
        const auto positions = ScalePositions(playing, *scale, request.key, request.notesPerString);
        json positionsJson = json::array();

        for (const auto& p : positions)
        {
            std::vector<FretPos> cells;

            for (const auto& cell : p.cells)
            {
                cells.push_back({cell.string, cell.fret + capo});
            }

            positionCells.push_back(std::move(cells));
            positionsJson.push_back({{"kind", p.kind}, {"fretFrom", p.fretFrom + capo}, {"fretTo", p.fretTo + capo}});
        }

        json diatonic = json::array();

        for (const auto& chord : DiatonicTriads(tones))
        {
            diatonic.push_back({{"name", chord.name},
                                {"numeral", chord.numeral},
                                {"quality", chord.quality},
                                {"rootPitchClass", chord.rootPitchClass}});
        }

        view["title"] = tones.notes.front().name + " " + scale->name;
        view["positions"] = positionsJson;
        view["scale"] = {{"id", scale->id},
                         {"name", scale->name},
                         {"steps", StepPattern(scale->intervals)},
                         {"formula", IntervalLabels(tones)},
                         {"diatonic", diatonic}};
    }
    else
    {
        SpelledChord chord;
        const auto progression = ResolveProgression(request);
        json progressionJson = progression.header;
        const auto& progressionChords = progression.chords;
        const int step = progression.step;

        if (!progressionChords.empty())
        {
            chord = progressionChords[step];
        }
        else
        {
            const auto* def = FindChord(request.chord);
            chord = SpellChord(request.key, def != nullptr ? *def : Chords().front());
        }

        tones = chord.notes;

        ChordSearch search;
        search.rootPitchClass = chord.rootPitchClass;
        search.intervals = chord.def->intervals;
        search.allowInversions = request.allowInversions;
        const auto shapes = ChordShapes(playing, search);

        if (!request.previousFrets.empty() && !shapes.empty())
        {
            ChordShape previous;
            previous.frets = ShiftedFrets(request.previousFrets, capo);
            position = NearestShape(shapes, previous) + 1;
        }

        json shapesJson = json::array();

        for (const auto& shape : shapes)
        {
            std::vector<FretPos> cells;

            for (int s = 0; s < static_cast<int>(shape.frets.size()); ++s)
            {
                if (shape.frets[s] >= 0)
                {
                    cells.push_back({s, shape.frets[s] + capo});
                }
            }

            positionCells.push_back(std::move(cells));
            shapesJson.push_back(ShapeJson(shape, capo));
        }

        view["title"] = chord.name;
        view["shapes"] = shapesJson;
        view["chord"] = {{"id", chord.def->id},
                         {"name", chord.name},
                         {"quality", chord.def->name},
                         {"formula", IntervalLabels(tones)}};

        if (!progressionJson.is_null())
        {
            const int anchor = position >= 1 && position <= static_cast<int>(shapes.size()) ? position - 1 : 0;
            progressionJson["chords"] =
                ProgressionStrip(playing, progressionChords, step, shapes, anchor, request.allowInversions, capo);
            view["progression"] = progressionJson;
        }
    }

    if (position < 0 || position > static_cast<int>(positionCells.size()))
    {
        position = 0;
    }

    view["position"] = position;
    view["flats"] = tones.UsesFlats();
    view["tones"] = TonesJson(tones);

    std::array<int, kPitchClasses> toneIndex{};
    toneIndex.fill(-1);

    for (int i = 0; i < static_cast<int>(tones.notes.size()); ++i)
    {
        if (toneIndex[tones.notes[i].pitchClass] < 0)
        {
            toneIndex[tones.notes[i].pitchClass] = i;
        }
    }

    const std::vector<FretPos>* selected = position > 0 ? &positionCells[position - 1] : nullptr;
    const ArpeggioShape* run = arpeggioMode && position > 0 ? &runs[position - 1] : nullptr;

    if (run != nullptr)
    {
        view["run"] = RunJson(*run, tones, RunName(*run, tones));
    }
    else if (!chordMode && selected != nullptr)
    {
        view["run"] =
            RunJson(ScalePositionRun(neck, *selected, toneIndex), tones, "Position " + std::to_string(position));
    }

    json dots = json::array();

    for (int s = 0; s < static_cast<int>(neck.strings.size()); ++s)
    {
        for (int fret = capo; fret <= neck.frets; ++fret)
        {
            const int index = toneIndex[PitchClass(neck.strings[s] + fret)];

            if (index < 0)
            {
                continue;
            }

            const auto& note = tones.notes[index];
            const bool inPosition = selected == nullptr ||
                                    std::find(selected->begin(), selected->end(), FretPos{s, fret}) != selected->end();
            json dot = {{"string", s},
                        {"fret", fret},
                        {"tone", index},
                        {"name", note.name},
                        {"interval", note.interval.label},
                        {"degree", note.interval.degree},
                        {"root", index == 0},
                        {"inPosition", inPosition}};

            if (run != nullptr && inPosition)
            {
                // Its place in the run, so the neck can number the notes or show their fingers.
                const auto played = std::find_if(run->notes.begin(), run->notes.end(), [&](const ArpeggioNote& n) {
                    return n.string == s && n.fret == fret;
                });

                if (played != run->notes.end())
                {
                    dot["order"] = static_cast<int>(played - run->notes.begin()) + 1;
                    dot["finger"] = played->finger;
                }
            }

            dots.push_back(std::move(dot));
        }
    }

    view["dots"] = dots;
    return view;
}
} // namespace guitarfx::music
