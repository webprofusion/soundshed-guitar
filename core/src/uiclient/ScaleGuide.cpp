#include "uiclient/ScaleGuide.h"

#include <algorithm>
#include <regex>
#include <set>

namespace guitarfx::uiclient::scaleguide
{
namespace
{
const std::regex kId("^[\\w-]{1,32}$");
const std::regex kProgression("^[bu]:[\\w-]{1,32}$");
// What the engine's ParseRoman reads; anything else is left out of a saved progression.
const std::regex kNumeralStart("^[b#]?(VII|VI|V|IV|III|II|I|vii|vi|v|iv|iii|ii|i)");
constexpr std::size_t kMaxNumeralLength = 16;

struct Strictness
{
    double share;
    double tone;
};

Strictness RuleFor(const std::string& strictness)
{
    if (strictness == "easy")
    {
        return {0.60, 0.03};
    }

    if (strictness == "strict")
    {
        return {0.80, 0.07};
    }

    return {0.70, 0.05};
}

int Wrap(int value)
{
    return ((value % 12) + 12) % 12;
}

std::string StringField(const nlohmann::json& object, const char* key, const std::string& fallback,
                        const std::regex& pattern)
{
    const auto it = object.find(key);

    if (it == object.end() || !it->is_string())
    {
        return fallback;
    }

    const auto value = it->get<std::string>();
    return std::regex_match(value, pattern) ? value : fallback;
}

std::string OneOf(const nlohmann::json& object, const char* key, const std::string& fallback,
                  std::initializer_list<const char*> allowed)
{
    const auto it = object.find(key);

    if (it == object.end() || !it->is_string())
    {
        return fallback;
    }

    const auto value = it->get<std::string>();
    return std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return value == a; }) ? value : fallback;
}

int IntField(const nlohmann::json& object, const char* key, int fallback, int lowest, int highest)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? std::clamp(it->get<int>(), lowest, highest) : fallback;
}

bool BoolField(const nlohmann::json& object, const char* key, bool fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
} // namespace

/// The most strings a neck has, as ts/scaleGuide/state.ts keeps it.
constexpr int kMaxArpeggioStrings = 8;

Settings ParseSettings(const nlohmann::json& stored)
{
    Settings s;

    if (!stored.is_object())
    {
        return s;
    }

    s.mode = OneOf(stored, "mode", s.mode, {"scale", "chord", "arpeggio"});
    s.key = IntField(stored, "key", s.key, 0, 11);
    s.scale = StringField(stored, "scale", s.scale, kId);
    s.chord = StringField(stored, "chord", s.chord, kId);
    s.progression = StringField(stored, "progression", "", kProgression);
    s.tuning = StringField(stored, "tuning", s.tuning, kId);
    s.labels = OneOf(stored, "labels", s.labels, {"name", "interval", "degree", "none", "order", "finger"});
    s.position = IntField(stored, "position", s.position, 0, 99);
    s.capo = IntField(stored, "capo", s.capo, 0, kMaxCapo);
    s.leftHanded = BoolField(stored, "leftHanded", s.leftHanded);
    s.listen = BoolField(stored, "listen", s.listen);
    s.match = OneOf(stored, "match", s.match, {"easy", "normal", "strict"});
    s.arpeggioStyle = OneOf(stored, "arpeggioStyle", s.arpeggioStyle, {"sweep", "two", "box"});
    s.arpeggioStrings = IntField(stored, "arpeggioStrings", s.arpeggioStrings, 0, kMaxArpeggioStrings);
    return s;
}

nlohmann::json SettingsJson(const Settings& s)
{
    return {{"mode", s.mode},
            {"key", s.key},
            {"scale", s.scale},
            {"chord", s.chord},
            {"progression", s.progression},
            {"tuning", s.tuning},
            {"labels", s.labels},
            {"position", s.position},
            {"capo", s.capo},
            {"leftHanded", s.leftHanded},
            {"listen", s.listen},
            {"match", s.match},
            {"arpeggioStyle", s.arpeggioStyle},
            {"arpeggioStrings", s.arpeggioStrings}};
}

Settings LoadSettings(const nlohmann::json& appSettings)
{
    const auto it = appSettings.is_object() ? appSettings.find(kViewSetting) : appSettings.end();
    return ParseSettings(appSettings.is_object() && it != appSettings.end() ? *it : nlohmann::json());
}

std::vector<UserProgression> LoadProgressions(const nlohmann::json& appSettings)
{
    std::vector<UserProgression> out;
    const auto it = appSettings.is_object() ? appSettings.find(kProgressionsSetting) : appSettings.end();

    if (!appSettings.is_object() || it == appSettings.end() || !it->is_array())
    {
        return out;
    }

    std::set<std::string> seen;

    for (const auto& entry : *it)
    {
        if (!entry.is_object())
        {
            continue;
        }

        UserProgression p;
        p.id = StringField(entry, "id", "", kId);

        if (const auto chords = entry.find("chords"); chords != entry.end() && chords->is_array())
        {
            for (const auto& chord : *chords)
            {
                const auto token = chord.is_string() ? chord.get<std::string>() : std::string();

                if (!token.empty() && token.size() <= kMaxNumeralLength && std::regex_search(token, kNumeralStart) &&
                    static_cast<int>(p.chords.size()) < kMaxProgressionChords)
                {
                    p.chords.push_back(token);
                }
            }
        }

        if (p.id.empty() || p.chords.empty() || !seen.insert(p.id).second)
        {
            continue;
        }

        p.name = entry.value("name", std::string());
        p.name = p.name.substr(0, 40);
        p.name = p.name.empty() ? "My progression" : p.name;
        out.push_back(std::move(p));
    }

    return out;
}

nlohmann::json ProgressionsJson(const std::vector<UserProgression>& progressions)
{
    nlohmann::json out = nlohmann::json::array();

    for (const auto& p : progressions)
    {
        out.push_back({{"id", p.id}, {"name", p.name}, {"chords", p.chords}});
    }

    return out;
}

nlohmann::json ViewRequest(const Settings& s, const std::vector<UserProgression>& progressions, int step,
                           const std::vector<int>& previousFrets)
{
    nlohmann::json request = {{"mode", s.mode},
                              {"key", s.key},
                              {"scale", s.scale},
                              {"chord", s.chord},
                              {"tuning", s.tuning},
                              {"capo", s.capo},
                              {"position", s.position},
                              {"arpeggioStyle", s.arpeggioStyle},
                              {"arpeggioStrings", s.arpeggioStrings}};

    if (s.mode != "chord")
    {
        return request;
    }

    if (s.progression.rfind("b:", 0) == 0)
    {
        request["progression"] = s.progression.substr(2);
    }
    else if (s.progression.rfind("u:", 0) == 0)
    {
        const auto id = s.progression.substr(2);
        const auto own =
            std::find_if(progressions.begin(), progressions.end(), [&](const auto& p) { return p.id == id; });

        if (own == progressions.end())
        {
            return request;
        }

        request["progressionChords"] = own->chords;
        request["progressionName"] = own->name;
    }
    else
    {
        return request;
    }

    request["step"] = step;

    if (!previousFrets.empty())
    {
        request["previousFrets"] = previousFrets;
    }

    return request;
}

std::optional<int> ReadingMidi(const std::string& noteName, int octave)
{
    static const int kLetters[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G

    if (noteName.empty() || noteName[0] < 'A' || noteName[0] > 'G')
    {
        return std::nullopt;
    }

    const int pitchClass = Wrap(kLetters[noteName[0] - 'A'] + (noteName.size() > 1 && noteName[1] == '#' ? 1 : 0));
    return (octave + 1) * 12 + pitchClass;
}

ChordMatch MatchChord(const std::array<float, 12>& chroma, int rootPitchClass, const std::vector<ChordTone>& tones,
                      const std::string& strictness)
{
    const auto rule = RuleFor(strictness);
    double total = 0.0;

    for (const float value : chroma)
    {
        total += value;
    }

    const auto at = [&](int pitchClass) {
        return total > 0.0 ? chroma[static_cast<std::size_t>(Wrap(pitchClass))] / total : 0.0;
    };

    std::vector<int> intervals;

    for (const auto& tone : tones)
    {
        const int interval = Wrap(tone.pitchClass - rootPitchClass);

        if (std::find(intervals.begin(), intervals.end(), interval) == intervals.end())
        {
            intervals.push_back(interval);
        }
    }

    const auto shareOf = [&](const std::vector<int>& set) {
        double share = 0.0;

        for (const int interval : set)
        {
            share += at(rootPitchClass + interval);
        }

        return share;
    };

    ChordMatch match;
    match.share = shareOf(intervals);

    for (const auto& tone : tones)
    {
        const bool heard = at(tone.pitchClass) >= rule.tone;
        match.heard.push_back(heard);

        if (!heard && !(tone.interval == "5" && intervals.size() > 2))
        {
            match.missing.push_back(tone.pitchClass);
        }
    }

    match.ok = total > 0.0 && match.share >= rule.share && match.missing.empty();
    return match;
}

bool StrumGate::Push(double levelDb, bool clean)
{
    mLevels.push_back(levelDb);

    if (mLevels.size() > 10)
    {
        mLevels.erase(mLevels.begin());
    }

    mRearmWait = std::max(0, mRearmWait - 1);

    if (mRearmWait == 0 && levelDb > kQuietDb &&
        levelDb - *std::min_element(mLevels.begin(), mLevels.end()) >= kJumpDb)
    {
        mArmed = true;
    }

    mHeld = clean && mArmed ? mHeld + 1 : 0;

    if (mHeld >= kHold)
    {
        mHeld = 0;
        mArmed = false;
        // The quiet before this strum must not read as a jump while it still rings.
        mLevels.assign(1, levelDb);
        mRearmWait = kRearmReadings;
        return true;
    }

    return false;
}

void StrumGate::Reset()
{
    mLevels.clear();
    mArmed = true;
    mHeld = 0;
    mRearmWait = 0;
}

bool NoteFollower::Push(std::optional<int> midi)
{
    const auto remove = [this](int note) {
        mTrail.erase(std::remove(mTrail.begin(), mTrail.end(), note), mTrail.end());
    };

    if (!midi)
    {
        ++mSilent;
        mCandidate.reset();

        if (mCurrent && mSilent >= kReleaseReadings)
        {
            remove(*mCurrent);
            mTrail.insert(mTrail.begin(), *mCurrent);
            mTrail.resize(std::min<std::size_t>(mTrail.size(), 2));
            mCurrent.reset();
            return true;
        }

        if (!mCurrent && !mTrail.empty() && mSilent >= kClearReadings)
        {
            mTrail.clear();
            return true;
        }

        return false;
    }

    mSilent = 0;

    if (midi == mCurrent)
    {
        return false;
    }

    mSeen = midi == mCandidate ? mSeen + 1 : 1;
    mCandidate = midi;

    if (mSeen < 2)
    {
        return false;
    }

    remove(*midi);

    if (mCurrent)
    {
        remove(*mCurrent);
        mTrail.insert(mTrail.begin(), *mCurrent);
        mTrail.resize(std::min<std::size_t>(mTrail.size(), 2));
    }

    mCurrent = midi;
    mCandidate.reset();
    return true;
}

void NoteFollower::Reset()
{
    mCurrent.reset();
    mTrail.clear();
    mCandidate.reset();
    mSeen = 0;
    mSilent = 0;
}
} // namespace guitarfx::uiclient::scaleguide
