#pragma once

/**
 * ScaleGuideView.h - The Scale Guide's one view model (docs/plans/scale-guide.md): what a renderer
 * draws for a key, a scale or chord and a tuning. The web UI receives it as JSON, Nano Q builds it
 * directly; neither does any theory of its own.
 *
 * The model holds every label a dot can show (note name, interval, degree), so switching labels,
 * mirroring for a left-handed player or turning the neck over are the renderer's business alone.
 */

#include "music/Arpeggios.h"
#include "music/Fretboard.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace guitarfx::music
{
constexpr int kMinFrets = 12;
constexpr int kMaxFrets = 27;
constexpr int kMaxCapo = 12;
constexpr int kMinStrings = 4;
constexpr int kMaxStrings = 8;
constexpr int kMaxProgressionChords = 16;

struct ScaleGuideRequest
{
    /// "scale", "chord" or "arpeggio".
    std::string mode = "scale";
    /// The key (or the chord's root), as a pitch class.
    int key = 4;
    std::string scale = "minpent";
    std::string chord = "maj";
    /// A built-in progression's id; ignored when `progressionChords` is given.
    std::string progression;
    /// The player's own progression, already parsed.
    std::vector<RomanChord> progressionChords;
    std::string progressionName;
    int step = 0;
    /// A TuningDef id, or "custom" with `strings` and `instrument`.
    std::string tuning = "std";
    std::vector<int> strings;
    Instrument instrument = Instrument::Guitar;
    int frets = kDefaultFrets;
    int capo = 0;
    /// 0 is the whole neck (all tones); 1 … n a scale position, chord shape or arpeggio run.
    int position = 0;
    int notesPerString = 0;
    bool allowInversions = false;
    ArpeggioStyle arpeggioStyle = ArpeggioStyle::Sweep;
    /// How many strings an arpeggio plays, from the highest: 0 for the default.
    int arpeggioStrings = 0;
    /// The shape last shown, as frets per string: when set, a chord takes the shape nearest it.
    std::vector<int> previousFrets;
};

/// A request from its JSON form (the `scaleGuideModel` message); missing or bad fields keep their
/// defaults, numbers are clamped.
[[nodiscard]] ScaleGuideRequest ParseScaleGuideRequest(const nlohmann::json& json);

/// The view model for a request.
[[nodiscard]] nlohmann::json BuildScaleGuideView(const ScaleGuideRequest& request);

/// Every scale, chord quality, progression and tuning the guide offers, for its pickers.
[[nodiscard]] nlohmann::json ScaleGuideCatalogJson();
} // namespace guitarfx::music
