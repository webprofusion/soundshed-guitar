#pragma once

/**
 * ListenAnalysis.h - What LISTEN hears, whatever the Scale Guide is showing (docs/plans/scale-guide.md,
 * "Heard"): the chord a strum's chroma most likely is, and the key the last few notes or chords most
 * likely belong to. TunerService runs a ListenTracker over the readings while the guide listens and
 * sends its answer with each `tunerUpdate` as `heard`, so the web UI and Nano Q only draw it.
 */

#include "music/MusicCatalog.h"

#include <nlohmann/json.hpp>

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::music
{
struct HeardChord
{
    const ChordDef* def = nullptr;
    int rootPitchClass = 0;
    /// "Am7", "F♯m".
    std::string name;
    /// Its tones' share of the chroma, 0-1, and how well it explains it (the share, less a little for
    /// each tone past three): what chords are ranked by.
    double share = 0.0;
    double score = 0.0;

    [[nodiscard]] bool Same(const HeardChord& other) const
    {
        return def == other.def && rootPitchClass == other.rootPitchClass;
    }
};

/// The catalogue chord a chroma (energy per pitch class, C first, overtones removed) most likely is:
/// the one whose tones hold the most of it, less a little for each tone past three, with every tone
/// but a plain 5th clearly present. nullopt when none holds enough of it, as for a single note.
[[nodiscard]] std::optional<HeardChord> IdentifyChord(const std::array<float, 12>& chroma);

struct HeardKey
{
    int tonic = 0;
    bool minor = false;
    /// "A minor", "E♭ major".
    std::string name;
    /// The simplest common scale on the tonic that holds every note heard (a pentatonic before the
    /// full scale before a mode), or null when none does.
    const ScaleDef* scale = nullptr;
    /// "A Minor Pentatonic"; empty without a scale.
    std::string scaleName;
    /// How well the notes fit the key's profile, 0-1.
    double confidence = 0.0;
    /// The runner-up key when it fits nearly as well (the relative major or minor, usually), else empty.
    std::string alternative;
};

/// The key a weighted pitch-class histogram most likely is in, by correlation with Temperley's key
/// profiles. nullopt with fewer than four pitch classes heard.
[[nodiscard]] std::optional<HeardKey> EstimateKey(const std::array<double, 12>& weights);

/// LISTEN's readings turned into what was heard: a chord once it reads the same for a moment (it
/// stays shown after it stops ringing, until another replaces it) and the key of the recent notes,
/// in Scales and Arpeggios, or of the recent chords, in Chords.
class ListenTracker
{
  public:
    static constexpr int kChordHold = 3;       ///< readings (~130 ms) a chord must read the same to show
    static constexpr int kChordRelease = 6;    ///< readings without it before it stops ringing
    static constexpr int kNoteRelease = 4;     ///< silent readings before the same note counts again
    static constexpr int kForgetReadings = 470; ///< ~20 s of silence forgets the key
    static constexpr int kNoteWindow = 32;
    static constexpr int kChordWindow = 8;
    static constexpr int kMinNotes = 6;
    static constexpr int kMinChords = 2;
    static constexpr int kRecentChords = 3;
    static constexpr double kQuietDb = -52.0;
    /// The chord being held on to keeps its place against one that beats it by less than this.
    static constexpr double kChordStickiness = 0.03;

    void Reset();

    /// A reading in Scales or Arpeggios: the tuner's note as MIDI, or nullopt. `held` while the guide's
    /// own sound may be what is heard: nothing counts.
    void PushNote(std::optional<int> midi, bool held);
    /// A reading in Chords.
    void PushChroma(const std::array<float, 12>& chroma, double levelDb, bool held);

    [[nodiscard]] const std::optional<HeardChord>& Chord() const
    {
        return mChord;
    }

    /// Whether the chord shown is still being heard, rather than the last one heard.
    [[nodiscard]] bool ChordRinging() const
    {
        return mChord.has_value() && mRinging;
    }

    /// The chords heard before the one shown, newest first.
    [[nodiscard]] const std::vector<std::string>& RecentChords() const
    {
        return mRecent;
    }

    [[nodiscard]] const std::optional<HeardKey>& Key() const
    {
        return mKey;
    }

    /// `heard` for a tunerUpdate: {key, and with `chords` chord and recent}.
    [[nodiscard]] nlohmann::json Json(bool chords) const;

  private:
    void AddEvent(const std::array<double, 12>& weights, std::size_t window);
    void UpdateKey();
    void Silence();

    std::deque<std::array<double, 12>> mEvents;
    std::optional<HeardKey> mKey;
    int mSilent = 0;

    std::optional<int> mNote;
    std::optional<int> mNoteCandidate;
    int mNoteSeen = 0;
    int mNoteHeld = 0;

    std::optional<HeardChord> mChord;
    std::optional<HeardChord> mChordCandidate;
    int mChordSeen = 0;
    int mChordMiss = 0;
    int mChordHeld = 0; ///< readings the chord showing has rung since it was struck
    std::array<double, 12> mChordWeights{}; ///< what striking it added, which each second ringing adds again
    bool mRinging = false;
    std::vector<std::string> mRecent;
};
} // namespace guitarfx::music
