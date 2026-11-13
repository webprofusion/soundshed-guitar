#include "music/MusicTheory.h"

#include <array>
#include <cstdlib>

namespace guitarfx::music
{
namespace
{
constexpr std::array<int, kLetters> kLetterPitchClasses = {0, 2, 4, 5, 7, 9, 11};
constexpr const char* kLetterNames = "CDEFGAB";
constexpr std::array<const char*, kLetters> kNumerals = {"I", "II", "III", "IV", "V", "VI", "VII"};

/// A semitone difference folded into -6 … 5, the accidental that bridges it.
int Wrap(int semitones)
{
    return ((semitones % kPitchClasses) + kPitchClasses + 6) % kPitchClasses - 6;
}

int AccidentalCost(int accidental)
{
    const int size = std::abs(accidental);
    return size <= 1 ? size : 3;
}

std::string Signs(int accidental)
{
    std::string text;

    for (int i = 0; i < std::abs(accidental); ++i)
    {
        text += accidental < 0 ? kFlatSign : kSharpSign;
    }

    return text;
}

/// A leading accidental: 'b', '#', or the UTF-8 ♭ or ♯. Returns its value, or 0 when there is none.
int TakeAccidental(std::string_view& text)
{
    if (text.starts_with('b'))
    {
        text.remove_prefix(1);
        return -1;
    }

    if (text.starts_with('#'))
    {
        text.remove_prefix(1);
        return 1;
    }

    if (text.starts_with(kFlatSign))
    {
        text.remove_prefix(std::string_view(kFlatSign).size());
        return -1;
    }

    if (text.starts_with(kSharpSign))
    {
        text.remove_prefix(std::string_view(kSharpSign).size());
        return 1;
    }

    return 0;
}

std::string Upper(std::string_view text)
{
    std::string out(text);

    for (char& c : out)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    return out;
}

std::string Lower(std::string_view text)
{
    std::string out(text);

    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return out;
}

/// The quality a numeral's suffix names; `upper` is the numeral's case. Empty for an unknown one.
std::string QualityFromSuffix(std::string_view suffix, bool upper)
{
    const std::string degree = kDegreeSign;
    const std::string halfDim = kHalfDimSign;

    if (suffix.empty())
    {
        return upper ? "maj" : "m";
    }

    if (suffix == "5" || suffix == "add9" || suffix == "sus2" || suffix == "sus4" || suffix == "7sus4" ||
        suffix == "maj9")
    {
        return std::string(suffix);
    }

    if (suffix == "7")
    {
        return upper ? "7" : "m7";
    }

    if (suffix == "maj7")
    {
        return upper ? "maj7" : "mMaj7";
    }

    if (suffix == "6")
    {
        return upper ? "6" : "m6";
    }

    if (suffix == "9")
    {
        return upper ? "9" : "m9";
    }

    if (suffix == degree || suffix == "o" || suffix == "dim")
    {
        return "dim";
    }

    if (suffix == degree + "7" || suffix == "o7" || suffix == "dim7")
    {
        return "dim7";
    }

    if (suffix == halfDim || suffix == halfDim + "7" || suffix == "m7b5")
    {
        return "m7b5";
    }

    if (suffix == "+" || suffix == "aug")
    {
        return "aug";
    }

    if (suffix == "+7" || suffix == "aug7")
    {
        return "aug7";
    }

    return {};
}

std::string NumeralSuffix(std::string_view quality)
{
    if (quality == "maj" || quality == "m")
    {
        return {};
    }

    if (quality == "m7" || quality == "m6" || quality == "m9")
    {
        return std::string(quality.substr(1));
    }

    if (quality == "mMaj7")
    {
        return "maj7";
    }

    if (quality == "dim")
    {
        return kDegreeSign;
    }

    if (quality == "dim7")
    {
        return std::string(kDegreeSign) + "7";
    }

    if (quality == "m7b5")
    {
        return std::string(kHalfDimSign) + "7";
    }

    if (quality == "aug")
    {
        return "+";
    }

    if (quality == "aug7")
    {
        return "+7";
    }

    return std::string(quality);
}
} // namespace

std::optional<Interval> ParseInterval(std::string_view token)
{
    int accidental = 0;

    for (int taken = TakeAccidental(token); taken != 0; taken = TakeAccidental(token))
    {
        accidental += taken;
    }

    if (token.empty() || token.size() > 2 || std::abs(accidental) > 2)
    {
        return std::nullopt;
    }

    int degree = 0;

    for (const char c : token)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }

        degree = degree * 10 + (c - '0');
    }

    if (degree < 1 || degree > 13)
    {
        return std::nullopt;
    }

    Interval interval;
    interval.degree = degree;
    interval.semitones = kLetterPitchClasses[(degree - 1) % kLetters] + 12 * ((degree - 1) / kLetters) + accidental;
    interval.label = degree == 1 && accidental == 0 ? "R" : Signs(accidental) + std::to_string(degree);
    return interval;
}

std::vector<Interval> ParseFormula(std::string_view formula)
{
    std::vector<Interval> intervals;

    while (!formula.empty())
    {
        const auto space = formula.find(' ');
        const auto token = formula.substr(0, space);
        formula = space == std::string_view::npos ? std::string_view{} : formula.substr(space + 1);

        if (token.empty())
        {
            continue;
        }

        const auto interval = ParseInterval(token);

        if (!interval)
        {
            return {};
        }

        intervals.push_back(*interval);
    }

    return intervals;
}

int LetterPitchClass(int letter)
{
    return kLetterPitchClasses[((letter % kLetters) + kLetters) % kLetters];
}

int SpellingPitchClass(Spelling spelling)
{
    return PitchClass(LetterPitchClass(spelling.letter) + spelling.accidental);
}

std::string NoteName(Spelling spelling)
{
    if (std::abs(spelling.accidental) > 1)
    {
        return PlainNoteName(SpellingPitchClass(spelling), spelling.accidental < 0);
    }

    return std::string(1, kLetterNames[((spelling.letter % kLetters) + kLetters) % kLetters]) +
           Signs(spelling.accidental);
}

std::string PlainNoteName(int pitchClass, bool flats)
{
    const auto spellings = KeySpellings(PitchClass(pitchClass));
    const auto& spelling = flats ? spellings.back() : spellings.front();
    return std::string(1, kLetterNames[spelling.letter]) + Signs(spelling.accidental);
}

bool SpelledSet::UsesFlats() const
{
    for (const auto& note : notes)
    {
        if (note.spelling.accidental < 0)
        {
            return true;
        }
    }

    return false;
}

SpelledSet SpellFrom(Spelling root, const std::vector<Interval>& intervals)
{
    SpelledSet set;
    const int rootPitchClass = SpellingPitchClass(root);

    for (const auto& interval : intervals)
    {
        SpelledNote note;
        note.pitchClass = PitchClass(rootPitchClass + interval.semitones);
        note.spelling.letter = (root.letter + interval.degree - 1) % kLetters;
        note.spelling.accidental = Wrap(note.pitchClass - kLetterPitchClasses[note.spelling.letter]);
        note.name = NoteName(note.spelling);
        note.interval = interval;
        set.cost += AccidentalCost(note.spelling.accidental);
        set.notes.push_back(std::move(note));
    }

    return set;
}

std::vector<Spelling> KeySpellings(int pitchClass)
{
    switch (PitchClass(pitchClass))
    {
    case 0:
        return {{0, 0}};
    case 1:
        return {{0, 1}, {1, -1}};
    case 2:
        return {{1, 0}};
    case 3:
        return {{1, 1}, {2, -1}};
    case 4:
        return {{2, 0}};
    case 5:
        return {{3, 0}};
    case 6:
        return {{3, 1}, {4, -1}};
    case 7:
        return {{4, 0}};
    case 8:
        return {{4, 1}, {5, -1}};
    case 9:
        return {{5, 0}};
    case 10:
        return {{5, 1}, {6, -1}};
    default:
        return {{6, 0}};
    }
}

SpelledSet SpellBest(int rootPitchClass, const std::vector<Interval>& intervals)
{
    SpelledSet best;
    bool found = false;

    for (const auto spelling : KeySpellings(rootPitchClass))
    {
        auto candidate = SpellFrom(spelling, intervals);

        if (!found || candidate.cost < best.cost)
        {
            best = std::move(candidate);
            found = true;
        }
    }

    return best;
}

Spelling SpellDegree(Spelling key, int degree, int pitchClass)
{
    Spelling spelling;
    spelling.letter = (key.letter + degree - 1) % kLetters;
    spelling.accidental = Wrap(pitchClass - kLetterPitchClasses[spelling.letter]);
    return spelling;
}

std::optional<RomanChord> ParseRoman(std::string_view token)
{
    const int accidental = TakeAccidental(token);

    for (int degree = kLetters; degree >= 1; --degree)
    {
        const std::string_view numeral = kNumerals[degree - 1];

        if (token.size() < numeral.size())
        {
            continue;
        }

        const auto head = token.substr(0, numeral.size());
        const bool upper = head == numeral;

        if (!upper && head != Lower(numeral))
        {
            continue;
        }

        auto quality = QualityFromSuffix(token.substr(numeral.size()), upper);

        if (quality.empty())
        {
            return std::nullopt;
        }

        RomanChord chord;
        chord.degree = degree;
        chord.offset = PitchClass(kLetterPitchClasses[degree - 1] + accidental);
        chord.quality = std::move(quality);
        return chord;
    }

    return std::nullopt;
}

std::vector<RomanChord> ParseRomanSequence(std::string_view sequence)
{
    std::vector<RomanChord> chords;

    while (!sequence.empty())
    {
        const auto space = sequence.find(' ');
        const auto token = sequence.substr(0, space);
        sequence = space == std::string_view::npos ? std::string_view{} : sequence.substr(space + 1);

        if (token.empty())
        {
            continue;
        }

        const auto chord = ParseRoman(token);

        if (!chord)
        {
            return {};
        }

        chords.push_back(*chord);
    }

    return chords;
}

std::string RomanNumeral(const RomanChord& chord)
{
    const int degree = chord.degree >= 1 && chord.degree <= kLetters ? chord.degree : 1;
    const std::string numeral = kNumerals[degree - 1];
    const int accidental = Wrap(chord.offset - kLetterPitchClasses[degree - 1]);
    return Signs(accidental) + (IsMinorQuality(chord.quality) ? Lower(numeral) : Upper(numeral)) +
           NumeralSuffix(chord.quality);
}

bool IsMinorQuality(std::string_view quality)
{
    return quality == "m" || quality == "m7" || quality == "m6" || quality == "m9" || quality == "mMaj7" ||
           quality == "dim" || quality == "dim7" || quality == "m7b5";
}

std::string StepPattern(const std::vector<Interval>& intervals)
{
    std::string pattern;

    for (std::size_t i = 0; i < intervals.size(); ++i)
    {
        const int from = intervals[i].semitones % kPitchClasses;
        const int to = i + 1 < intervals.size() ? intervals[i + 1].semitones % kPitchClasses : kPitchClasses;
        const int step = to - from;

        if (!pattern.empty())
        {
            pattern += ' ';
        }

        pattern += std::to_string(step);
    }

    return pattern;
}

std::vector<DiatonicChord> DiatonicTriads(const SpelledSet& scale)
{
    std::vector<DiatonicChord> chords;

    if (scale.notes.size() != static_cast<std::size_t>(kLetters))
    {
        return chords;
    }

    for (int d = 0; d < kLetters; ++d)
    {
        const auto& root = scale.notes[d];
        const int third = PitchClass(scale.notes[(d + 2) % kLetters].pitchClass - root.pitchClass);
        const int fifth = PitchClass(scale.notes[(d + 4) % kLetters].pitchClass - root.pitchClass);

        DiatonicChord chord;
        chord.rootPitchClass = root.pitchClass;
        chord.quality = third == 4 && fifth == 7   ? "maj"
                        : third == 3 && fifth == 7 ? "m"
                        : third == 3 && fifth == 6 ? "dim"
                        : third == 4 && fifth == 8 ? "aug"
                                                   : "";
        chord.name = root.name + (chord.quality == "maj" ? "" : chord.quality);

        RomanChord numeral;
        numeral.degree = d + 1;
        numeral.offset = PitchClass(root.interval.semitones);
        numeral.quality = chord.quality.empty() ? (third == 3 ? "m" : "maj") : chord.quality;
        chord.numeral = RomanNumeral(numeral);
        chords.push_back(std::move(chord));
    }

    return chords;
}
} // namespace guitarfx::music
