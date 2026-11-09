#include "music/MusicCatalog.h"

#include <cstdlib>

namespace guitarfx::music
{
namespace
{
struct ScaleRow
{
    const char* id;
    const char* name;
    const char* formula;
    const char* boxParent;
};

struct ScaleGroup
{
    const char* name;
    std::vector<ScaleRow> rows;
};

// Seven-letter formulas wherever a scale has seven notes, so each degree spells on its own letter
// (the altered scale is ♭4, not 3, for that reason).
const std::vector<ScaleGroup>& ScaleGroups()
{
    static const std::vector<ScaleGroup> groups = {
        {"Essentials",
         {{"minpent", "Minor Pentatonic", "1 b3 4 5 b7", ""},
          {"blues", "Blues", "1 b3 4 b5 5 b7", "minpent"},
          {"majpent", "Major Pentatonic", "1 2 3 5 6", ""},
          {"majblues", "Major Blues", "1 2 b3 3 5 6", "majpent"},
          {"minor", "Natural Minor (Aeolian)", "1 2 b3 4 5 b6 b7", ""},
          {"major", "Major (Ionian)", "1 2 3 4 5 6 7", ""},
          {"harmin", "Harmonic Minor", "1 2 b3 4 5 b6 7", ""},
          {"melmin", "Melodic Minor", "1 2 b3 4 5 6 7", ""}}},
        {"Modes",
         {{"dorian", "Dorian", "1 2 b3 4 5 6 b7", ""},
          {"phrygian", "Phrygian", "1 b2 b3 4 5 b6 b7", ""},
          {"lydian", "Lydian", "1 2 3 #4 5 6 7", ""},
          {"mixolydian", "Mixolydian", "1 2 3 4 5 6 b7", ""},
          {"locrian", "Locrian", "1 b2 b3 4 b5 b6 b7", ""}}},
        {"Melodic minor modes",
         {{"dorianb2", "Dorian b2", "1 b2 b3 4 5 6 b7", ""},
          {"lydaug", "Lydian Augmented", "1 2 3 #4 #5 6 7", ""},
          {"lyddom", "Lydian Dominant", "1 2 3 #4 5 6 b7", ""},
          {"mixob6", "Mixolydian b6", "1 2 3 4 5 b6 b7", ""},
          {"locnat2", "Locrian natural 2", "1 2 b3 4 b5 b6 b7", ""},
          {"altered", "Altered (Super Locrian)", "1 b2 b3 b4 b5 b6 b7", ""}}},
        {"Harmonic minor modes",
         {{"locnat6", "Locrian natural 6", "1 b2 b3 4 b5 6 b7", ""},
          {"ionaug", "Ionian Augmented", "1 2 3 4 #5 6 7", ""},
          {"dorsharp4", "Dorian #4", "1 2 b3 #4 5 6 b7", ""},
          {"phrdom", "Phrygian Dominant", "1 b2 3 4 5 b6 b7", ""},
          {"lydsharp2", "Lydian #2", "1 #2 3 #4 5 6 7", ""},
          {"ultraloc", "Ultralocrian", "1 b2 b3 b4 b5 b6 bb7", ""}}},
        {"Exotic",
         {{"hunmin", "Hungarian Minor", "1 2 b3 #4 5 b6 7", ""},
          {"dblharm", "Double Harmonic", "1 b2 3 4 5 b6 7", ""},
          {"neamin", "Neapolitan Minor", "1 b2 b3 4 5 b6 7", ""},
          {"neamaj", "Neapolitan Major", "1 b2 b3 4 5 6 7", ""},
          {"persian", "Persian", "1 b2 3 4 b5 b6 7", ""},
          {"enigmatic", "Enigmatic", "1 b2 3 #4 #5 #6 7", ""},
          {"hirajoshi", "Hirajoshi", "1 2 b3 5 b6", ""},
          {"kumoi", "Kumoi", "1 2 b3 5 6", ""},
          {"iwato", "Iwato", "1 b2 4 b5 b7", ""},
          {"egyptian", "Egyptian", "1 2 4 5 b7", ""}}},
        {"Symmetric and bebop",
         {{"wholetone", "Whole Tone", "1 2 3 #4 #5 b7", ""},
          {"dimwh", "Diminished (whole-half)", "1 2 b3 4 b5 b6 6 7", ""},
          {"dimhw", "Diminished (half-whole)", "1 b2 b3 3 #4 5 6 b7", ""},
          {"bebopdom", "Bebop Dominant", "1 2 3 4 5 6 b7 7", ""},
          {"bebopmaj", "Bebop Major", "1 2 3 4 5 #5 6 7", ""},
          {"chromatic", "Chromatic", "1 b2 2 b3 3 4 b5 5 b6 6 b7 7", ""}}}};
    return groups;
}

struct ChordRow
{
    const char* id;
    const char* name;
    std::string symbol;
    const char* formula;
};

const std::vector<ChordRow>& ChordRows()
{
    static const std::vector<ChordRow> rows = {
        {"maj", "Major", "", "1 3 5"},
        {"m", "Minor", "m", "1 b3 5"},
        {"5", "Power chord", "5", "1 5"},
        {"sus2", "Sus 2", "sus2", "1 2 5"},
        {"sus4", "Sus 4", "sus4", "1 4 5"},
        {"dim", "Diminished", "dim", "1 b3 b5"},
        {"aug", "Augmented", "aug", "1 3 #5"},
        {"6", "Major 6", "6", "1 3 5 6"},
        {"m6", "Minor 6", "m6", "1 b3 5 6"},
        {"7", "Dominant 7", "7", "1 3 5 b7"},
        {"maj7", "Major 7", "maj7", "1 3 5 7"},
        {"m7", "Minor 7", "m7", "1 b3 5 b7"},
        {"mMaj7", "Minor-major 7", "m(maj7)", "1 b3 5 7"},
        {"m7b5", "Half-diminished", std::string("m7") + kFlatSign + "5", "1 b3 b5 b7"},
        {"dim7", "Diminished 7", "dim7", "1 b3 b5 bb7"},
        {"aug7", "Augmented 7", std::string("7") + kSharpSign + "5", "1 3 #5 b7"},
        {"7sus4", "Dominant 7 sus 4", "7sus4", "1 4 5 b7"},
        {"add9", "Add 9", "add9", "1 3 5 9"},
        {"9", "Dominant 9", "9", "1 3 5 b7 9"},
        {"maj9", "Major 9", "maj9", "1 3 5 7 9"},
        {"m9", "Minor 9", "m9", "1 b3 5 b7 9"}};
    return rows;
}

struct ProgressionRow
{
    const char* id;
    const char* name;
    const char* sequence;
};

const std::vector<ProgressionRow>& ProgressionRows()
{
    static const std::vector<ProgressionRow> rows = {
        {"pop", "Axis", "I V vi IV"},
        {"sadpop", "Axis from the vi", "vi IV I V"},
        {"epicminor", "Axis in minor", "i bVI bIII bVII"},
        {"fifties", "Doo-wop", "I vi IV V"},
        {"canon", "Pachelbel's Canon", "I V vi iii IV I IV V"},
        {"threechord", "Garage rock", "I IV V IV"},
        {"classicrock", "Mixolydian loop", "I bVII IV"},
        {"minorrock", "Modal rock", "i bIII bVII IV"},
        {"dorianvamp", "Dorian vamp", "i7 IV7"},
        {"andalusian", "Andalusian cadence", "i bVII bVI V"},
        {"minor", "Minor cadence", "i iv V i"},
        {"metal", "Tritone riff", "I5 bV5 I5 bIII5"},
        {"phrygianmetal", "Phrygian climb", "I5 bII5 bIII5 bII5"},
        {"blues12", "12-bar blues", "I7 I7 I7 I7 IV7 IV7 I7 I7 V7 IV7 I7 V7"},
        {"quickchange", "12-bar blues, quick change", "I7 IV7 I7 I7 IV7 IV7 I7 I7 V7 IV7 I7 V7"},
        {"minblues", "Minor blues", "i7 i7 i7 i7 iv7 iv7 i7 i7 bVI7 V7 i7 V7"},
        {"twofiveone", "ii V I", "ii7 V7 Imaj7 Imaj7"},
        {"minortwofiveone", "Minor ii V i",
         "ii\xC3\xB8"
         "7 V7 i7 i7"},
        {"turnaround", "Turnaround", "Imaj7 vi7 ii7 V7"}};
    return rows;
}
} // namespace

const std::vector<ScaleDef>& Scales()
{
    static const std::vector<ScaleDef> scales = [] {
        std::vector<ScaleDef> out;

        for (const auto& group : ScaleGroups())
        {
            for (const auto& row : group.rows)
            {
                ScaleDef scale;
                scale.id = row.id;
                scale.name = row.name;
                scale.group = group.name;
                scale.intervals = ParseFormula(row.formula);
                scale.boxParent = row.boxParent;
                out.push_back(std::move(scale));
            }
        }

        for (const auto& chord : Chords())
        {
            ScaleDef scale;
            scale.id = "arp-" + chord.id;
            scale.name = chord.name + " arpeggio";
            scale.group = "Arpeggios";
            scale.intervals = chord.intervals;
            scale.arpeggio = true;
            out.push_back(std::move(scale));
        }

        return out;
    }();
    return scales;
}

const ScaleDef* FindScale(std::string_view id)
{
    for (const auto& scale : Scales())
    {
        if (scale.id == id)
        {
            return &scale;
        }
    }

    return nullptr;
}

const std::vector<ChordDef>& Chords()
{
    static const std::vector<ChordDef> chords = [] {
        std::vector<ChordDef> out;

        for (const auto& row : ChordRows())
        {
            ChordDef chord;
            chord.id = row.id;
            chord.name = row.name;
            chord.symbol = row.symbol;
            chord.intervals = ParseFormula(row.formula);
            out.push_back(std::move(chord));
        }

        return out;
    }();
    return chords;
}

const ChordDef* FindChord(std::string_view id)
{
    for (const auto& chord : Chords())
    {
        if (chord.id == id)
        {
            return &chord;
        }
    }

    return nullptr;
}

const std::vector<ProgressionDef>& Progressions()
{
    static const std::vector<ProgressionDef> progressions = [] {
        std::vector<ProgressionDef> out;

        for (const auto& row : ProgressionRows())
        {
            ProgressionDef progression;
            progression.id = row.id;
            progression.name = row.name;
            progression.chords = ParseRomanSequence(row.sequence);
            out.push_back(std::move(progression));
        }

        return out;
    }();
    return progressions;
}

const ProgressionDef* FindProgression(std::string_view id)
{
    for (const auto& progression : Progressions())
    {
        if (progression.id == id)
        {
            return &progression;
        }
    }

    return nullptr;
}

const std::vector<TuningDef>& Tunings()
{
    static const std::vector<TuningDef> tunings = {
        {"std", "Standard E", Instrument::Guitar, {40, 45, 50, 55, 59, 64}, false},
        {"dropd", "Drop D", Instrument::Guitar, {38, 45, 50, 55, 59, 64}, false},
        {"eb", "E\xE2\x99\xAD standard", Instrument::Guitar, {39, 44, 49, 54, 58, 63}, true},
        {"d", "D standard", Instrument::Guitar, {38, 43, 48, 53, 57, 62}, false},
        {"dropc", "Drop C", Instrument::Guitar, {36, 43, 48, 53, 57, 62}, false},
        {"c", "C standard", Instrument::Guitar, {36, 41, 46, 51, 55, 60}, true},
        {"dropb", "Drop B", Instrument::Guitar, {35, 42, 47, 52, 56, 61}, false},
        {"dadgad", "DADGAD", Instrument::Guitar, {38, 45, 50, 55, 57, 62}, false},
        {"openg", "Open G", Instrument::Guitar, {38, 43, 50, 55, 59, 62}, false},
        {"opend", "Open D", Instrument::Guitar, {38, 45, 50, 54, 57, 62}, false},
        {"opene", "Open E", Instrument::Guitar, {40, 47, 52, 56, 59, 64}, false},
        {"openc", "Open C", Instrument::Guitar, {36, 43, 48, 55, 60, 64}, false},
        {"b7", "7-string B", Instrument::Guitar, {35, 40, 45, 50, 55, 59, 64}, false},
        {"dropa7", "7-string drop A", Instrument::Guitar, {33, 40, 45, 50, 55, 59, 64}, false},
        {"fsharp8", "8-string F\xE2\x99\xAF", Instrument::Guitar, {30, 35, 40, 45, 50, 55, 59, 64}, false},
        {"drope8", "8-string drop E", Instrument::Guitar, {28, 35, 40, 45, 50, 55, 59, 64}, false},
        {"bass4", "Bass 4 (E A D G)", Instrument::Bass, {28, 33, 38, 43}, false},
        {"bass4dropd", "Bass 4 drop D", Instrument::Bass, {26, 33, 38, 43}, false},
        {"bass5", "Bass 5 (B E A D G)", Instrument::Bass, {23, 28, 33, 38, 43}, false},
        {"bass6", "Bass 6 (B E A D G C)", Instrument::Bass, {23, 28, 33, 38, 43, 48}, false},
        {"uke", "Ukulele (G C E A, high G)", Instrument::Ukulele, {67, 60, 64, 69}, false},
        {"ukelowg", "Ukulele low G", Instrument::Ukulele, {55, 60, 64, 69}, false},
        {"ukebaritone", "Baritone ukulele (D G B E)", Instrument::Ukulele, {50, 55, 59, 64}, false}};
    return tunings;
}

const TuningDef* FindTuning(std::string_view id)
{
    for (const auto& tuning : Tunings())
    {
        if (tuning.id == id)
        {
            return &tuning;
        }
    }

    return nullptr;
}

std::string_view InstrumentId(Instrument instrument)
{
    switch (instrument)
    {
    case Instrument::Bass:
        return "bass";
    case Instrument::Ukulele:
        return "ukulele";
    default:
        return "guitar";
    }
}

std::optional<Instrument> ParseInstrument(std::string_view id)
{
    if (id == "guitar")
    {
        return Instrument::Guitar;
    }

    if (id == "bass")
    {
        return Instrument::Bass;
    }

    if (id == "ukulele")
    {
        return Instrument::Ukulele;
    }

    return std::nullopt;
}

SpelledChord SpellChord(int rootPitchClass, const ChordDef& def)
{
    SpelledChord chord;
    chord.def = &def;
    chord.rootPitchClass = PitchClass(rootPitchClass);
    chord.notes = SpellBest(chord.rootPitchClass, def.intervals);
    chord.name = chord.notes.notes.empty() ? std::string() : chord.notes.notes.front().name + def.symbol;
    return chord;
}

Spelling ProgressionKeySpelling(int keyPitchClass, const std::vector<RomanChord>& chords)
{
    const auto spellings = KeySpellings(keyPitchClass);
    Spelling best = spellings.front();
    int bestCost = -1;

    for (const auto key : spellings)
    {
        int cost = 0;

        for (const auto& chord : chords)
        {
            const auto* def = FindChord(chord.quality);
            const auto root = SpellDegree(key, chord.degree, keyPitchClass + chord.offset);

            if (std::abs(root.accidental) > 1 || def == nullptr)
            {
                cost += 9;
                continue;
            }

            cost += SpellFrom(root, def->intervals).cost;
        }

        const bool tieToFlat = cost == bestCost && key.accidental < 0 && PitchClass(keyPitchClass) != 6;

        if (bestCost < 0 || cost < bestCost || tieToFlat)
        {
            best = key;
            bestCost = cost;
        }
    }

    return best;
}

std::vector<SpelledChord> ChordsInKey(int keyPitchClass, const std::vector<RomanChord>& chords)
{
    std::vector<SpelledChord> out;
    const auto key = ProgressionKeySpelling(keyPitchClass, chords);

    for (const auto& roman : chords)
    {
        const auto* def = FindChord(roman.quality);

        if (def == nullptr)
        {
            def = FindChord("maj");
        }

        const int rootPitchClass = PitchClass(keyPitchClass + roman.offset);
        const auto root = SpellDegree(key, roman.degree, rootPitchClass);
        SpelledChord chord = SpellChord(rootPitchClass, *def);

        if (std::abs(root.accidental) <= 1)
        {
            chord.notes = SpellFrom(root, def->intervals);
            chord.name = chord.notes.notes.front().name + def->symbol;
        }

        chord.roman = roman;
        out.push_back(std::move(chord));
    }

    return out;
}
} // namespace guitarfx::music
