// ScaleGuideRequest.cpp - What the UI may ask the Scale Guide for: the catalogue it chooses from
// (scaleGuideCatalog: the keys, scales with their formulas, chords, progressions as numerals and
// tunings with their strings' names), and the request it sends, read and held to safe values.
// ScaleGuideView.cpp builds the view that answers it.

#include "music/ScaleGuideView.h"

#include "music/MusicCatalog.h"
#include "music/MusicTheory.h"

#include <algorithm>
#include <string>

namespace guitarfx::music
{
using nlohmann::json;

namespace
{
int IntField(const json& object, const char* key, int fallback, int lowest, int highest)
{
    const auto it = object.find(key);

    if (it == object.end() || !it->is_number())
    {
        return fallback;
    }

    return std::clamp(it->get<int>(), lowest, highest);
}

std::string StringField(const json& object, const char* key, std::string fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}
} // namespace

ScaleGuideRequest ParseScaleGuideRequest(const json& object)
{
    ScaleGuideRequest request;

    if (!object.is_object())
    {
        return request;
    }

    const auto mode = StringField(object, "mode", request.mode);
    request.mode = mode == "chord" || mode == "arpeggio" ? mode : "scale";
    request.key = PitchClass(IntField(object, "key", request.key, -1000, 1000));
    request.scale = StringField(object, "scale", request.scale);
    request.chord = StringField(object, "chord", request.chord);
    request.progression = StringField(object, "progression", "");
    request.progressionName = StringField(object, "progressionName", "");
    request.step = IntField(object, "step", 0, 0, 1000);
    request.tuning = StringField(object, "tuning", request.tuning);
    request.frets = IntField(object, "frets", kDefaultFrets, kMinFrets, kMaxFrets);
    request.capo = IntField(object, "capo", 0, 0, kMaxCapo);
    request.position = IntField(object, "position", 0, 0, 100);
    request.notesPerString = IntField(object, "notesPerString", 0, 0, 4);
    request.allowInversions = object.value("allowInversions", false);
    request.arpeggioStyle = ParseArpeggioStyle(StringField(object, "arpeggioStyle", "")).value_or(ArpeggioStyle::Sweep);
    request.arpeggioStrings = IntField(object, "arpeggioStrings", 0, 0, kMaxStrings);
    request.arpeggioSlides = object.value("arpeggioSlides", false);
    request.anchorStep = IntField(object, "anchorStep", -1, -1, 1000);
    request.instrument = ParseInstrument(StringField(object, "instrument", "guitar")).value_or(Instrument::Guitar);

    if (const auto it = object.find("progressionChords"); it != object.end() && it->is_array())
    {
        for (const auto& token : *it)
        {
            if (!token.is_string() || static_cast<int>(request.progressionChords.size()) >= kMaxProgressionChords)
            {
                continue;
            }

            if (const auto chord = ParseRoman(token.get<std::string>()))
            {
                request.progressionChords.push_back(*chord);
            }
        }
    }

    if (const auto it = object.find("strings"); it != object.end() && it->is_array())
    {
        for (const auto& note : *it)
        {
            if (note.is_number_integer() && note.get<int>() >= 0 && note.get<int>() <= 127)
            {
                request.strings.push_back(note.get<int>());
            }
        }
    }

    if (const auto it = object.find("previousFrets"); it != object.end() && it->is_array())
    {
        for (const auto& fret : *it)
        {
            request.previousFrets.push_back(fret.is_number_integer() ? fret.get<int>() : -1);
        }
    }

    return request;
}

json ScaleGuideCatalogJson()
{
    json scales = json::array();

    for (const auto& scale : Scales())
    {
        std::string formula;

        for (const auto& interval : scale.intervals)
        {
            formula += (formula.empty() ? "" : " ") + interval.label;
        }

        scales.push_back({{"id", scale.id}, {"name", scale.name}, {"group", scale.group}, {"formula", formula}});
    }

    json chords = json::array();

    for (const auto& chord : Chords())
    {
        chords.push_back({{"id", chord.id}, {"name", chord.name}, {"symbol", chord.symbol}});
    }

    json progressions = json::array();

    for (const auto& progression : Progressions())
    {
        json numerals = json::array();

        for (const auto& chord : progression.chords)
        {
            numerals.push_back(RomanNumeral(chord));
        }

        progressions.push_back({{"id", progression.id}, {"name", progression.name}, {"numerals", numerals}});
    }

    json tunings = json::array();

    for (const auto& tuning : Tunings())
    {
        json names = json::array();

        for (const int note : tuning.strings)
        {
            names.push_back(PlainNoteName(note, tuning.flats));
        }

        tunings.push_back({{"id", tuning.id},
                           {"name", tuning.name},
                           {"instrument", std::string(InstrumentId(tuning.instrument))},
                           {"strings", tuning.strings},
                           {"stringNames", names}});
    }

    json keys = json::array();

    for (int pc = 0; pc < kPitchClasses; ++pc)
    {
        const auto spellings = KeySpellings(pc);
        keys.push_back(spellings.size() > 1 ? PlainNoteName(pc, false) + "/" + PlainNoteName(pc, true)
                                            : PlainNoteName(pc, false));
    }

    return {
        {"keys", keys}, {"scales", scales}, {"chords", chords}, {"progressions", progressions}, {"tunings", tunings}};
}
} // namespace guitarfx::music
