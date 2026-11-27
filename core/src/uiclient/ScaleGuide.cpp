#include "uiclient/ScaleGuide.h"

#include <algorithm>
#include <cctype>
#include <map>
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
    s.arpeggioStyle = OneOf(stored, "arpeggioStyle", s.arpeggioStyle, {"sweep", "two", "box"});
    s.arpeggioStrings = IntField(stored, "arpeggioStrings", s.arpeggioStrings, 0, kMaxArpeggioStrings);
    s.arpeggioSlides = BoolField(stored, "arpeggioSlides", s.arpeggioSlides);
    s.listenEverywhere = BoolField(stored, "listenEverywhere", s.listenEverywhere);
    s.playVolume = IntField(stored, "playVolume", s.playVolume, 0, 100);
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
            {"arpeggioStyle", s.arpeggioStyle},
            {"arpeggioStrings", s.arpeggioStrings},
            {"arpeggioSlides", s.arpeggioSlides},
            {"listenEverywhere", s.listenEverywhere},
            {"playVolume", s.playVolume}};
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

std::string RomanToken(int offset, const std::string& quality)
{
    // How each quality is written after its numeral, and whether the numeral is lower case.
    static const std::map<std::string, std::pair<const char*, bool>> kSuffixes = {
        {"maj", {"", false}},
        {"m", {"", true}},
        {"5", {"5", false}},
        {"sus2", {"sus2", false}},
        {"sus4", {"sus4", false}},
        {"dim", {"Â°", true}},
        {"aug", {"+", false}},
        {"6", {"6", false}},
        {"m6", {"6", true}},
        {"7", {"7", false}},
        {"maj7", {"maj7", false}},
        {"m7", {"7", true}},
        {"mMaj7", {"maj7", true}},
        {"m7b5",
         {"Ã¸"
          "7",
          true}},
        {"dim7",
         {"Â°"
          "7",
          true}},
        {"aug7", {"+7", false}},
        {"7sus4", {"7sus4", false}},
        {"add9", {"add9", false}},
        {"9", {"9", false}},
        {"7#9", {"7#9", false}},
        {"maj9", {"maj9", false}},
        {"m9", {"9", true}},
    };
    const std::string degree = kDegreeNumerals[static_cast<std::size_t>(Wrap(offset))];
    const auto found = kSuffixes.find(quality);
    const auto& [suffix, minor] = found != kSuffixes.end() ? found->second : kSuffixes.at("maj");
    const bool flat = degree[0] == 'b';
    std::string numeral = degree.substr(flat ? 1 : 0);

    if (minor)
    {
        std::transform(numeral.begin(), numeral.end(), numeral.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    }

    return (flat ? "b" : "") + numeral + suffix;
}

std::string DisplayNumeral(const std::string& token)
{
    if (!token.empty() && token[0] == 'b')
    {
        return "â­" + token.substr(1);
    }

    if (!token.empty() && token[0] == '#')
    {
        return "â¯" + token.substr(1);
    }

    return token;
}

std::vector<std::string> NumeralTokens(const std::vector<std::string>& numerals)
{
    static const std::string kFlat = "â­", kSharp = "â¯";
    std::vector<std::string> out;

    for (const auto& n : numerals)
    {
        out.push_back(n.rfind(kFlat, 0) == 0    ? "b" + n.substr(kFlat.size())
                      : n.rfind(kSharp, 0) == 0 ? "#" + n.substr(kSharp.size())
                                                : n);
    }

    return out;
}

std::string NewProgressionId(const std::vector<UserProgression>& existing)
{
    auto n = existing.size() + 1;

    while (std::any_of(existing.begin(), existing.end(),
                       [&](const UserProgression& p) { return p.id == "p" + std::to_string(n); }))
    {
        ++n;
    }

    return "p" + std::to_string(n);
}

nlohmann::json ViewRequest(const Settings& s, const std::vector<UserProgression>& progressions, int step,
                           const std::vector<int>& previousFrets, int anchorStep, const UserProgression* draft)
{
    nlohmann::json request = {{"mode", s.mode},
                              {"key", s.key},
                              {"scale", s.scale},
                              {"chord", s.chord},
                              {"tuning", s.tuning},
                              {"capo", s.capo},
                              {"position", s.position},
                              {"arpeggioStyle", s.arpeggioStyle},
                              {"arpeggioStrings", s.arpeggioStrings},
                              {"arpeggioSlides", s.arpeggioSlides}};

    // Chords and Arpeggios take a progression; a scale has none.
    if (s.mode == "scale")
    {
        return request;
    }

    if (draft != nullptr)
    {
        // The one being written, as the web's draft: nothing until it has a chord.
        if (draft->chords.empty())
        {
            return request;
        }

        request["progressionChords"] = draft->chords;
        request["progressionName"] = draft->name;
        step = std::min(step, static_cast<int>(draft->chords.size()) - 1);
    }
    else if (s.progression.rfind("b:", 0) == 0)
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

    if (s.mode == "arpeggio")
    {
        request["anchorStep"] = anchorStep;
    }
    else if (!previousFrets.empty())
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

EarRule EarRuleAt(double tightness)
{
    const double t = std::clamp(tightness, 0.0, 1.0);
    return {0.60 + 0.20 * t, 0.03 + 0.04 * t};
}

void ChordEar::Landed()
{
    mTightness = std::min(1.0, mTightness + kTighten);
    mStruggle = 0;
}

bool ChordEar::Push(bool loud, bool ok)
{
    if (!loud || ok)
    {
        return false;
    }

    if (++mStruggle < kStruggleReadings)
    {
        return false;
    }

    mStruggle = 0;
    const double before = mTightness;
    mTightness = std::max(0.0, mTightness - kEase);
    return mTightness != before;
}

void ChordEar::Reset()
{
    mTightness = kStart;
    mStruggle = 0;
}

ChordMatch MatchChord(const std::array<float, 12>& chroma, int rootPitchClass, const std::vector<ChordTone>& tones,
                      const EarRule& rule)
{
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

    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
    {
        match.spread[static_cast<std::size_t>(pitchClass)] = at(pitchClass);
    }

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

    if (mRearmWait == 0 && levelDb > kQuietDb && levelDb - *std::min_element(mLevels.begin(), mLevels.end()) >= kJumpDb)
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

void StrumGate::Disarm(double levelDb)
{
    mLevels.assign(1, levelDb);
    mArmed = false;
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

Playback ParsePlayback(const nlohmann::json& message)
{
    Playback out;

    if (!message.is_object())
    {
        return out;
    }

    const auto number = [](const nlohmann::json& object, const char* key, double fallback) {
        const auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    };
    out.delayMs = std::max(0.0, number(message, "delayMs", 0.0));
    const auto notes = message.find("notes");

    if (notes == message.end() || !notes->is_array())
    {
        return out;
    }

    for (const auto& n : *notes)
    {
        if (n.is_object())
        {
            out.notes.push_back({static_cast<int>(number(n, "string", 0.0)), static_cast<int>(number(n, "fret", 0.0)),
                                 number(n, "startMs", 0.0), number(n, "lengthMs", 0.0),
                                 static_cast<int>(number(n, "step", -1.0)),
                                 static_cast<int>(number(n, "chord", -1.0))});
        }
    }

    return out;
}

PlaybackMoment PlaybackAt(const Playback& playback, double elapsedMs)
{
    PlaybackMoment moment;
    double chordFrom = -1.0;

    for (const auto& note : playback.notes)
    {
        const double start = playback.delayMs + note.startMs;

        // The chord of the latest note due, a little ahead of it.
        if (note.chord >= 0 && elapsedMs >= start - kPlaybackStepLeadMs && start >= chordFrom)
        {
            moment.chord = note.chord;
            chordFrom = start;
        }

        if (elapsedMs >= start && elapsedMs < start + note.lengthMs)
        {
            moment.places.emplace_back(note.string, note.fret);

            if (note.step >= 0)
            {
                moment.steps.push_back(note.step);
            }
        }
    }

    return moment;
}

Heard ParseHeard(const nlohmann::json& heard)
{
    Heard out;

    if (!heard.is_object())
    {
        return out;
    }

    const auto text = [](const nlohmann::json& object, const char* key) {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto number = [](const nlohmann::json& object, const char* key) {
        const auto it = object.find(key);
        return it != object.end() && it->is_number_integer() ? ((it->get<int>() % 12) + 12) % 12 : 0;
    };

    if (const auto key = heard.find("key"); key != heard.end() && key->is_object() && !text(*key, "name").empty())
    {
        out.hasKey = true;
        out.key = text(*key, "name");
        out.keyTonic = number(*key, "tonic");
        out.keyMinor = key->value("minor", false);
        out.scale = text(*key, "scale");
        out.scaleName = text(*key, "scaleName");
        out.alternative = text(*key, "alternative");
    }

    if (const auto chord = heard.find("chord");
        chord != heard.end() && chord->is_object() && !text(*chord, "name").empty() && !text(*chord, "id").empty())
    {
        out.hasChord = true;
        out.chord = text(*chord, "name");
        out.chordRoot = number(*chord, "root");
        out.chordId = text(*chord, "id");
        out.ringing = chord->value("ringing", false);
    }

    if (const auto recent = heard.find("recent"); recent != heard.end() && recent->is_array())
    {
        for (const auto& name : *recent)
        {
            if (name.is_string())
            {
                out.recent.push_back(name.get<std::string>());
            }
        }
    }

    return out;
}

std::string HeardKeyText(const Heard& heard)
{
    if (!heard.hasKey)
    {
        return {};
    }

    std::string text = "Key " + heard.key;

    if (!heard.scaleName.empty())
    {
        text += " \xC2\xB7 " + heard.scaleName + " fits";
    }

    if (!heard.alternative.empty())
    {
        text += " \xC2\xB7 or " + heard.alternative;
    }

    return text;
}

bool ApplyHeard(const Heard& heard, bool chord, Settings& settings)
{
    if (chord && heard.hasChord)
    {
        settings.mode = "chord";
        settings.key = heard.chordRoot;
        settings.chord = heard.chordId;
        settings.progression.clear();
        return true;
    }

    if (!chord && heard.hasKey)
    {
        settings.mode = "scale";
        settings.key = heard.keyTonic;
        settings.scale = !heard.scale.empty() ? heard.scale : heard.keyMinor ? "minor" : "major";
        return true;
    }

    return false;
}
} // namespace guitarfx::uiclient::scaleguide
