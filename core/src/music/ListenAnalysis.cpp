// ListenAnalysis.cpp - LISTEN's chord identification and key estimate (ListenAnalysis.h).

#include "music/ListenAnalysis.h"

#include "music/MusicTheory.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::music
{
namespace
{
/// A chord's tones must hold this much of the chroma (LISTEN's Easy match, listen.ts STRICTNESS).
constexpr double kChordShare = 0.6;
/// Each tone but a plain 5th (in a chord with a 3rd) must read this much of it, and a power chord's
/// 5th, which a single note's leftover overtones can fake, this much. Measured on GuitarSet's strums
/// (docs/plans/scale-guide.md, "Heard").
constexpr double kChordTone = 0.03;
constexpr double kPowerFifth = 0.15;
/// The root must be clearly there: a chord named on a note nobody played reads wrong.
constexpr double kRootTone = 0.08;
/// Each tone past three must earn its place: a 7th chord beats its triad only when the 7th is strong.
constexpr double kExtraTonePenalty = 0.06;
/// Chords of the same notes (C6 and Am7, Csus2 and Gsus4) go to the one whose root is loudest.
constexpr double kRootTieBreak = 0.02;

/// Temperley's key profiles (from the Kostka-Payne corpus): how well each pitch class, from the
/// tonic, fits a key. On GuitarSet's solos they named the key far more often than Krumhansl and
/// Kessler's (docs/plans/scale-guide.md, "Heard"). The minor one is classical, all but its 7th: its
/// leading tone (4) against a ♭7 of 1.5 read minor pentatonic and blues playing as the relative
/// major, so the ♭7 is raised to 2.5 and the 7th lowered to 3.5.
constexpr std::array<double, 12> kMajorProfile{5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0};
constexpr std::array<double, 12> kMinorProfile{5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 2.5, 3.5};
/// A note counts for how long it is held, a reading (~43 ms) at a time, up to a second.
constexpr double kNoteReadingWeight = 1.0 / 23.0;
constexpr double kNoteMaxWeight = 1.0;
/// A chord counts once for being struck and once more for each second it rings, up to eight more
/// (weighting by duration named GuitarSet's keys from their chords a little more often).
constexpr double kChordReadingWeight = 1.0 / 23.0;
constexpr int kChordMaxReadings = 8 * 23;
/// A pitch class counts as heard at this share of the histogram, and the key needs four of them.
constexpr double kHeardShare = 0.04;
constexpr int kMinPitchClasses = 4;
/// A runner-up this close is worth naming.
constexpr double kAlternativeMargin = 0.05;

/// The scales a key's notes are named by, simplest first.
constexpr std::array<const char*, 5> kMajorScales{"majpent", "majblues", "major", "mixolydian", "lydian"};
constexpr std::array<const char*, 6> kMinorScales{"minpent", "blues", "minor", "dorian", "phrygian", "harmin"};

double Correlation(const std::array<double, 12>& weights, const std::array<double, 12>& profile, int tonic)
{
    double meanX = 0.0;
    double meanY = 0.0;

    for (int pc = 0; pc < 12; ++pc)
    {
        meanX += weights[pc];
        meanY += profile[PitchClass(pc - tonic)];
    }

    meanX /= 12.0;
    meanY /= 12.0;
    double sxy = 0.0;
    double sxx = 0.0;
    double syy = 0.0;

    for (int pc = 0; pc < 12; ++pc)
    {
        const double x = weights[pc] - meanX;
        const double y = profile[PitchClass(pc - tonic)] - meanY;
        sxy += x * y;
        sxx += x * x;
        syy += y * y;
    }

    return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
}

std::string KeyTonicName(int tonic, bool minor)
{
    const auto* scale = FindScale(minor ? "minor" : "major");
    const auto spelled = scale != nullptr ? SpellBest(tonic, scale->intervals) : SpelledSet{};
    return spelled.notes.empty() ? PlainNoteName(tonic, false) : spelled.notes.front().name;
}

bool ScaleHolds(const ScaleDef& scale, int tonic, const std::array<bool, 12>& heard)
{
    std::array<bool, 12> in{};

    for (const auto& interval : scale.intervals)
    {
        in[PitchClass(tonic + interval.semitones)] = true;
    }

    for (int pc = 0; pc < 12; ++pc)
    {
        if (heard[pc] && !in[pc])
        {
            return false;
        }
    }

    return true;
}
struct Scored
{
    double share = 0.0;
    double score = 0.0;
};

/// The chroma as shares of its total; nullopt for silence.
std::optional<std::array<double, 12>> Normalised(const std::array<float, 12>& chroma)
{
    double total = 0.0;

    for (const float value : chroma)
    {
        total += std::max(0.0f, value);
    }

    if (!(total > 0.0))
    {
        return std::nullopt;
    }

    std::array<double, 12> at{};

    for (int pc = 0; pc < 12; ++pc)
    {
        at[pc] = std::max(0.0f, chroma[pc]) / total;
    }

    return at;
}

/// How well one chord on one root explains the chroma, or nullopt when it can't be what is heard.
std::optional<Scored> Score(const std::array<double, 12>& at, int root, const ChordDef& def)
{
    std::array<bool, 12> tone{};
    int tones = 0;
    bool hasThird = false;

    for (const auto& interval : def.intervals)
    {
        const int semis = PitchClass(interval.semitones);
        tones += tone[semis] ? 0 : 1;
        tone[semis] = true;
        hasThird = hasThird || interval.degree == 3;
    }

    if (tones < 2 || at[PitchClass(root)] < kRootTone)
    {
        return std::nullopt;
    }

    double share = 0.0;

    for (int semis = 0; semis < 12; ++semis)
    {
        share += tone[semis] ? at[PitchClass(root + semis)] : 0.0;
    }

    if (share < kChordShare)
    {
        return std::nullopt;
    }

    // A chord can lose its plain 5th, but only one with a 3rd: a sus4 without it is a power chord
    // upside down.
    for (const auto& interval : def.intervals)
    {
        const bool optionalFifth = interval.label == "5" && tones > 2 && hasThird;
        const double needed = tones == 2 ? kPowerFifth : kChordTone;

        if (!optionalFifth && at[PitchClass(root + interval.semitones)] < needed)
        {
            return std::nullopt;
        }
    }

    const double extra = static_cast<double>(std::max(0, tones - 3));
    return Scored{share, share - kExtraTonePenalty * extra + kRootTieBreak * at[PitchClass(root)]};
}
} // namespace

std::optional<HeardChord> IdentifyChord(const std::array<float, 12>& chroma)
{
    const auto at = Normalised(chroma);

    if (!at)
    {
        return std::nullopt;
    }

    std::optional<HeardChord> best;

    for (const auto& def : Chords())
    {
        for (int root = 0; root < 12; ++root)
        {
            const auto scored = Score(*at, root, def);

            if (scored && (!best || scored->score > best->score))
            {
                best = HeardChord{&def, root, {}, scored->share, scored->score};
            }
        }
    }

    if (best)
    {
        best->name = SpellChord(best->rootPitchClass, *best->def).name;
    }

    return best;
}

std::optional<HeardKey> EstimateKey(const std::array<double, 12>& weights)
{
    double total = 0.0;

    for (const double weight : weights)
    {
        total += std::max(0.0, weight);
    }

    if (!(total > 0.0))
    {
        return std::nullopt;
    }

    std::array<bool, 12> heard{};
    int distinct = 0;

    for (int pc = 0; pc < 12; ++pc)
    {
        heard[pc] = weights[pc] / total >= kHeardShare;
        distinct += heard[pc] ? 1 : 0;
    }

    if (distinct < kMinPitchClasses)
    {
        return std::nullopt;
    }

    struct Candidate
    {
        int tonic = 0;
        bool minor = false;
        double r = -2.0;
    };

    Candidate best;
    Candidate second;

    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (const bool minor : {false, true})
        {
            const Candidate candidate{tonic, minor, Correlation(weights, minor ? kMinorProfile : kMajorProfile, tonic)};

            if (candidate.r > best.r)
            {
                second = best;
                best = candidate;
            }
            else if (candidate.r > second.r)
            {
                second = candidate;
            }
        }
    }

    HeardKey key;
    key.tonic = best.tonic;
    key.minor = best.minor;
    const auto tonicName = KeyTonicName(best.tonic, best.minor);
    key.name = tonicName + (best.minor ? " minor" : " major");
    key.confidence = std::clamp(best.r, 0.0, 1.0);

    if (second.r >= best.r - kAlternativeMargin)
    {
        key.alternative = KeyTonicName(second.tonic, second.minor) + (second.minor ? " minor" : " major");
    }

    const auto tryScales = [&](const auto& ids) {
        for (const char* id : ids)
        {
            const auto* scale = FindScale(id);

            if (scale != nullptr && ScaleHolds(*scale, best.tonic, heard))
            {
                key.scale = scale;
                key.scaleName = tonicName + " " + scale->name;
                return;
            }
        }
    };

    if (best.minor)
    {
        tryScales(kMinorScales);
    }
    else
    {
        tryScales(kMajorScales);
    }

    return key;
}

void ListenTracker::Reset()
{
    *this = ListenTracker{};
}

void ListenTracker::AddEvent(const std::array<double, 12>& weights, std::size_t window)
{
    mEvents.push_back(weights);

    while (mEvents.size() > window)
    {
        mEvents.pop_front();
    }
}

void ListenTracker::UpdateKey()
{
    // Notes come one pitch class at a time, chords three or four: the key needs fewer chords.
    int notes = 0;
    std::array<double, 12> weights{};

    for (const auto& event : mEvents)
    {
        int classes = 0;

        for (int pc = 0; pc < 12; ++pc)
        {
            weights[pc] += event[pc];
            classes += event[pc] > 0.0 ? 1 : 0;
        }

        notes += classes > 1 ? kMinNotes / kMinChords : 1;
    }

    if (notes < kMinNotes)
    {
        mKey.reset();
        return;
    }

    mKey = EstimateKey(weights);
}

void ListenTracker::Silence()
{
    if (++mSilent >= kForgetReadings && (!mEvents.empty() || mChord))
    {
        mEvents.clear();
        mKey.reset();
        mChord.reset();
        mRinging = false;
        mRecent.clear();
    }
}

void ListenTracker::PushNote(std::optional<int> midi, bool held)
{
    if (held)
    {
        mNote.reset();
        mNoteCandidate.reset();
        mNoteSeen = 0;
        return;
    }

    if (!midi)
    {
        Silence();
        mNoteCandidate.reset();
        mNoteSeen = 0;

        if (mSilent >= kNoteRelease)
        {
            mNote.reset();
        }

        return;
    }

    mSilent = 0;
    const int pitchClass = PitchClass(*midi);

    // A note counts for as long as it is held, up to a second.
    if (mNote == midi)
    {
        if (!mEvents.empty() && mEvents.back()[pitchClass] < kNoteMaxWeight)
        {
            mEvents.back()[pitchClass] = std::min(kNoteMaxWeight, mEvents.back()[pitchClass] + kNoteReadingWeight);
            UpdateKey();
        }

        return;
    }

    // As the guide's NoteFollower: a note counts once it reads the same twice.
    mNoteSeen = mNoteCandidate == midi ? mNoteSeen + 1 : 1;
    mNoteCandidate = midi;

    if (mNoteSeen < 2)
    {
        return;
    }

    mNote = midi;
    mNoteCandidate.reset();
    mNoteSeen = 0;
    mNoteHeld = 0;
    std::array<double, 12> weights{};
    weights[pitchClass] = 2.0 * kNoteReadingWeight; // the two readings that steadied it
    AddEvent(weights, kNoteWindow);
    UpdateKey();
}

void ListenTracker::PushChroma(const std::array<float, 12>& chroma, double levelDb, bool held)
{
    std::optional<HeardChord> now;

    if (held)
    {
        mChordCandidate.reset();
        mChordSeen = 0;
    }
    else if (levelDb > kQuietDb)
    {
        mSilent = 0;
        now = IdentifyChord(chroma);

        // A strum's readings waver between near-equal names (C and Cadd9): the one being held on
        // to keeps its place unless another clearly beats it, so one of them gets to show.
        if (now && mChordCandidate && !now->Same(*mChordCandidate))
        {
            const auto at = Normalised(chroma);
            const auto kept = at ? Score(*at, mChordCandidate->rootPitchClass, *mChordCandidate->def) : std::nullopt;

            if (kept && kept->score >= now->score - kChordStickiness)
            {
                now = mChordCandidate;
                now->share = kept->share;
                now->score = kept->score;
            }
        }
    }
    else
    {
        Silence();
    }

    if (now && mChord && now->Same(*mChord))
    {
        mChordMiss = 0;

        // A chord counts for as long as it rings, up to eight seconds: the one a piece keeps
        // coming back to and resting on is usually its key's.
        if (mRinging && mChordHeld < kChordMaxReadings && !mEvents.empty())
        {
            ++mChordHeld;

            for (int pc = 0; pc < 12; ++pc)
            {
                mEvents.back()[pc] += mChordWeights[pc] * kChordReadingWeight;
            }

            UpdateKey();
        }
    }
    else if (++mChordMiss >= kChordRelease)
    {
        mRinging = false;
    }

    if (!now)
    {
        mChordCandidate.reset();
        mChordSeen = 0;
        return;
    }

    mChordSeen = mChordCandidate && now->Same(*mChordCandidate) ? mChordSeen + 1 : 1;
    mChordCandidate = now;

    if (mChordSeen < kChordHold || (mChord && mChord->Same(*now) && mRinging))
    {
        return;
    }

    if (mChord && !mChord->Same(*now))
    {
        mRecent.insert(mRecent.begin(), mChord->name);
    }

    std::vector<std::string> recent;

    for (const auto& name : mRecent)
    {
        if (name != now->name && std::find(recent.begin(), recent.end(), name) == recent.end() &&
            static_cast<int>(recent.size()) < kRecentChords)
        {
            recent.push_back(name);
        }
    }

    mRecent = std::move(recent);
    mChord = now;
    mRinging = true;
    mChordMiss = 0;
    mChordHeld = 0;

    std::array<double, 12> weights{};

    for (const auto& interval : now->def->intervals)
    {
        weights[PitchClass(now->rootPitchClass + interval.semitones)] = interval.semitones == 0 ? 1.5 : 1.0;
    }

    mChordWeights = weights;
    AddEvent(weights, kChordWindow);
    UpdateKey();
}

nlohmann::json ListenTracker::Json(bool chords) const
{
    nlohmann::json heard = nlohmann::json::object();

    if (mKey)
    {
        nlohmann::json key{{"name", mKey->name},
                           {"tonic", mKey->tonic},
                           {"minor", mKey->minor},
                           {"confidence", std::round(mKey->confidence * 100.0) / 100.0}};

        if (mKey->scale != nullptr)
        {
            key["scale"] = mKey->scale->id;
            key["scaleName"] = mKey->scaleName;
        }

        if (!mKey->alternative.empty())
        {
            key["alternative"] = mKey->alternative;
        }

        heard["key"] = std::move(key);
    }
    else
    {
        heard["key"] = nullptr;
    }

    if (chords)
    {
        heard["chord"] = mChord ? nlohmann::json{{"name", mChord->name},
                                                 {"root", mChord->rootPitchClass},
                                                 {"id", mChord->def->id},
                                                 {"ringing", mRinging}}
                                : nlohmann::json(nullptr);
        heard["recent"] = mRecent;
    }

    return heard;
}
} // namespace guitarfx::music
