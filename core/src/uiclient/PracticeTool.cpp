#include "uiclient/PracticeTool.h"

#include "dsp/FiniteCheck.h"
#include "uiclient/Looper.h"
#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <regex>

namespace guitarfx::uiclient
{
namespace practice
{
namespace
{
struct BandKeys
{
    const char* gain;
    const char* freq;
    const char* q;
    const char* shape; // null for the mid bands
    double freqDefault;
    double qDefault;
};

// The web UI's eqBands.ts: ParametricEQEffect's keys and band defaults.
constexpr BandKeys kBands[] = {{"lowGain", "lowFreq", "lowQ", "lowShape", 100.0, 0.707},
                               {"lowMidGain", "lowMidFreq", "lowMidQ", nullptr, 400.0, 1.0},
                               {"highMidGain", "highMidFreq", "highMidQ", nullptr, 2000.0, 1.0},
                               {"highGain", "highFreq", "highQ", "highShape", 8000.0, 0.707}};

constexpr FaderSpec kSpecs[] = {
    {"Volume", 0.0, 1.5, 1.0}, {"Balance", -1.0, 1.0, 0.0}, {"Speed", 0.25, 2.0, 1.0}, {"Pitch", -12.0, 12.0, 0.0}};

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsFiniteNumber(const nlohmann::json& value)
{
    return value.is_number() && guitarfx::IsFinite(value.get<double>());
}

double ReadNumber(const nlohmann::json& object, const char* key, double fallback)
{
    const auto it = object.find(key);
    return it != object.end() && IsFiniteNumber(*it) ? it->get<double>() : fallback;
}

std::string ReadString(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// A leading signed number ("85%", "-2 st"), as the web UI's parseLeadingNumber.
std::optional<double> LeadingNumber(const std::string& text)
{
    static const std::regex kNumber(R"(^[+-]?\d*\.?\d+)");
    const auto trimmed = Trim(text);
    std::smatch match;

    if (!std::regex_search(trimmed, match, kNumber))
    {
        return std::nullopt;
    }

    const double n = std::strtod(match.str().c_str(), nullptr);
    return guitarfx::IsFinite(n) ? std::optional<double>(n) : std::nullopt;
}

/// The project fields this client reads and writes; everything else is kept as read.
bool IsProjectFieldEditedHere(const std::string& key)
{
    static constexpr const char* kEdited[] = {"id",           "name",         "filePath", "fileTitle", "durationSec",
                                              "loops",        "activeLoopId", "gain",     "balance",   "speed",
                                              "pitchSemitones", "eq",         "presetId", "presetName", "links",
                                              "savedAt"};
    return std::any_of(std::begin(kEdited), std::end(kEdited), [&key](const char* k) { return key == k; });
}

std::optional<Link> ParseLink(const nlohmann::json& value)
{
    if (!value.is_object())
    {
        return std::nullopt;
    }

    Link link;
    link.id = ReadString(value, "id");
    const auto url = NormalizeLinkUrl(ReadString(value, "url"));

    if (link.id.empty() || !url)
    {
        return std::nullopt;
    }

    link.url = *url;
    link.title = Trim(ReadString(value, "title")).substr(0, 200);
    return link;
}
} // namespace

const FaderSpec& SpecFor(Fader fader)
{
    return kSpecs[static_cast<int>(fader)];
}

double FaderToSlider(Fader fader, double value)
{
    const auto& s = SpecFor(fader);

    if (value <= s.def)
    {
        return s.def == s.min ? 0.5 : std::clamp((value - s.min) / (s.def - s.min), 0.0, 1.0) * 0.5;
    }

    if (s.max == s.def)
    {
        return 0.5;
    }

    return 0.5 + std::clamp((value - s.def) / (s.max - s.def), 0.0, 1.0) * 0.5;
}

double SliderToFader(Fader fader, double position)
{
    const auto& s = SpecFor(fader);
    position = std::clamp(position, 0.0, 1.0);

    if (position <= 0.5)
    {
        return s.min + (position / 0.5) * (s.def - s.min);
    }

    return s.def + ((position - 0.5) / 0.5) * (s.max - s.def);
}

std::string FormatFader(Fader fader, double value)
{
    char buffer[32];

    switch (fader)
    {
    case Fader::Volume:
    case Fader::Speed:
        return std::to_string(static_cast<int>(std::lround(value * 100.0))) + "%";
    case Fader::Balance: {
        const long pct = std::lround(value * 100.0);

        if (pct == 0)
        {
            return "C";
        }

        return (pct < 0 ? "L" : "R") + std::to_string(std::labs(pct));
    }
    case Fader::Pitch:
        std::snprintf(buffer, sizeof(buffer), "%s%.1f st", value > 0.0 ? "+" : "", value);
        return buffer;
    }

    return {};
}

std::optional<double> ParseFader(Fader fader, const std::string& text)
{
    std::optional<double> value;

    switch (fader)
    {
    case Fader::Volume:
    case Fader::Speed:

        if (const auto n = LeadingNumber(text))
        {
            value = *n / 100.0;
        }

        break;
    case Fader::Pitch:
        value = LeadingNumber(text);
        break;
    case Fader::Balance: {
        const auto trimmed = Lower(Trim(text));

        if (trimmed == "c" || trimmed == "center" || trimmed == "centre")
        {
            return 0.0;
        }

        static const std::regex kSided(R"(^([lr])\s*(\d+(?:\.\d+)?)$)");
        std::smatch match;

        if (std::regex_match(trimmed, match, kSided))
        {
            const double magnitude = std::strtod(match[2].str().c_str(), nullptr) / 100.0;
            value = match[1].str() == "l" ? -magnitude : magnitude;
            break;
        }

        if (const auto n = LeadingNumber(trimmed))
        {
            value = std::abs(*n) > 1.0 ? *n / 100.0 : *n; // "35" or "0.35"
        }

        break;
    }
    }

    if (!value)
    {
        return std::nullopt;
    }

    const auto& s = SpecFor(fader);
    return std::clamp(*value, s.min, s.max);
}

double FaderValue(const LoopSettings& settings, Fader fader)
{
    switch (fader)
    {
    case Fader::Volume:
        return settings.gain;
    case Fader::Balance:
        return settings.balance;
    case Fader::Speed:
        return settings.speed;
    case Fader::Pitch:
        return settings.pitchSemitones;
    }

    return 0.0;
}

void SetFaderValue(LoopSettings& settings, Fader fader, double value)
{
    switch (fader)
    {
    case Fader::Volume:
        settings.gain = value;
        break;
    case Fader::Balance:
        settings.balance = value;
        break;
    case Fader::Speed:
        settings.speed = value;
        break;
    case Fader::Pitch:
        settings.pitchSemitones = value;
        break;
    }
}

std::string FileFingerprint(const std::string& filePath, double durationSec)
{
    char duration[32];
    std::snprintf(duration, sizeof(duration), "%.2f", durationSec);
    return Lower(Trim(filePath)) + "|" + duration;
}

std::optional<Loop> ParseLoop(const nlohmann::json& value)
{
    if (!value.is_object() || !value.contains("id") || !value["id"].is_string() || !value.contains("name") ||
        !value["name"].is_string() || !value.contains("startSec") || !value["startSec"].is_number() ||
        !value.contains("endSec") || !value["endSec"].is_number())
    {
        return std::nullopt;
    }

    Loop loop;
    loop.id = value["id"].get<std::string>();
    loop.name = value["name"].get<std::string>();
    loop.startSec = value["startSec"].get<double>();
    loop.endSec = value["endSec"].get<double>();

    // Settings only when all four are real numbers: a partial set would mix this loop's values
    // with whatever is on the faders.
    if (const auto it = value.find("settings"); it != value.end() && it->is_object())
    {
        const auto& s = *it;
        const bool valid = s.contains("gain") && IsFiniteNumber(s["gain"]) && s.contains("balance") &&
                           IsFiniteNumber(s["balance"]) && s.contains("speed") && IsFiniteNumber(s["speed"]) &&
                           s.contains("pitchSemitones") && IsFiniteNumber(s["pitchSemitones"]);

        if (valid)
        {
            loop.settings = LoopSettings{s["gain"].get<double>(), s["balance"].get<double>(), s["speed"].get<double>(),
                                         s["pitchSemitones"].get<double>()};
        }
    }

    return loop;
}

nlohmann::json LoopJson(const Loop& loop)
{
    nlohmann::json json = {{"id", loop.id}, {"name", loop.name}, {"startSec", loop.startSec}, {"endSec", loop.endSec}};

    if (loop.settings)
    {
        json["settings"] = {{"gain", loop.settings->gain},
                            {"balance", loop.settings->balance},
                            {"speed", loop.settings->speed},
                            {"pitchSemitones", loop.settings->pitchSemitones}};
    }

    return json;
}

std::vector<Loop> LoadLoops(const nlohmann::json& appSettings, const std::string& fingerprint)
{
    std::vector<Loop> loops;

    if (fingerprint.empty() || !appSettings.is_object() || !appSettings.contains(kLoopsSetting))
    {
        return loops;
    }

    const auto& stored = appSettings[kLoopsSetting];

    if (!stored.is_object() || !stored.contains(fingerprint) || !stored[fingerprint].is_array())
    {
        return loops;
    }

    for (const auto& entry : stored[fingerprint])
    {
        if (auto loop = ParseLoop(entry))
        {
            loops.push_back(std::move(*loop));
        }
    }

    return loops;
}

nlohmann::json LoopsSettingWith(const nlohmann::json& appSettings, const std::string& fingerprint,
                                const std::vector<Loop>& loops)
{
    nlohmann::json map = nlohmann::json::object();

    if (appSettings.is_object() && appSettings.contains(kLoopsSetting) && appSettings[kLoopsSetting].is_object())
    {
        map = appSettings[kLoopsSetting];
    }

    if (loops.empty())
    {
        map.erase(fingerprint);
        return map;
    }

    auto list = nlohmann::json::array();

    for (const auto& loop : loops)
    {
        list.push_back(LoopJson(loop));
    }

    map[fingerprint] = std::move(list);
    return map;
}

std::optional<Project> ParseProject(const nlohmann::json& value)
{
    if (!value.is_object())
    {
        return std::nullopt;
    }

    Project p;
    p.id = ReadString(value, "id");
    p.name = Trim(ReadString(value, "name"));
    p.filePath = ReadString(value, "filePath");

    // parseYouTubeReference: an 11-character video id, else no video.
    if (const auto it = value.find("youtube"); it != value.end() && it->is_object())
    {
        const auto videoId = ReadString(*it, "videoId");
        const bool valid = videoId.size() == 11 && std::all_of(videoId.begin(), videoId.end(), [](unsigned char c) {
                               return std::isalnum(c) || c == '_' || c == '-';
                           });

        if (valid)
        {
            p.youtubeVideoId = videoId;
            p.youtubeTitle = ReadString(*it, "title").substr(0, 300);
        }
    }

    // A project follows a backing track, a YouTube video, or both (parsePracticeToolProject).
    if (p.id.empty() || p.name.empty() || (p.filePath.empty() && p.youtubeVideoId.empty()))
    {
        return std::nullopt;
    }

    for (const auto& [key, field] : value.items())
    {
        if (!IsProjectFieldEditedHere(key))
        {
            p.kept[key] = field;
        }
    }

    if (const auto it = value.find("loops"); it != value.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (auto loop = ParseLoop(entry))
            {
                p.loops.push_back(std::move(*loop));
            }
        }
    }

    const auto active = ReadString(value, "activeLoopId");

    if (std::any_of(p.loops.begin(), p.loops.end(), [&active](const Loop& l) { return l.id == active; }))
    {
        p.activeLoopId = active;
    }

    p.fileTitle = ReadString(value, "fileTitle");

    if (p.fileTitle.empty())
    {
        const auto slash = p.filePath.find_last_of("\\/");
        p.fileTitle = slash == std::string::npos ? p.filePath : p.filePath.substr(slash + 1);
    }

    if (p.fileTitle.empty())
    {
        p.fileTitle = p.youtubeTitle.empty() ? "YouTube video" : p.youtubeTitle;
    }

    p.durationSec = std::max(0.0, ReadNumber(value, "durationSec", 0.0));
    p.faders.gain = ReadNumber(value, "gain", 1.0);
    p.faders.balance = ReadNumber(value, "balance", 0.0);
    p.faders.speed = ReadNumber(value, "speed", 1.0);
    p.faders.pitchSemitones = ReadNumber(value, "pitchSemitones", 0.0);

    // Left out for a project saved before the EQ existed, so a recall can tell "none captured"
    // from "deliberately flat".
    if (const auto it = value.find("eq");
        it != value.end() && !it->is_null() && !(it->is_boolean() && !it->get<bool>()))
    {
        p.eq = SanitizeEq(*it);
    }

    p.presetId = ReadString(value, "presetId");

    if (!p.presetId.empty())
    {
        p.presetName = ReadString(value, "presetName");
    }

    if (const auto it = value.find("links"); it != value.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (auto link = ParseLink(entry))
            {
                p.links.push_back(std::move(*link));
            }
        }
    }

    p.savedAt = std::max(0.0, ReadNumber(value, "savedAt", 0.0));
    return p;
}

nlohmann::json ProjectJson(const Project& p)
{
    auto loops = nlohmann::json::array();

    for (const auto& loop : p.loops)
    {
        loops.push_back(LoopJson(loop));
    }

    // What this client does not edit first, as it was read; then its own fields over it.
    nlohmann::json json = p.kept.is_object() ? p.kept : nlohmann::json::object();
    json.update(nlohmann::json{
        {"id", p.id},
        {"name", p.name},
        {"filePath", p.filePath},
        {"fileTitle", p.fileTitle},
        {"durationSec", p.durationSec},
        {"loops", std::move(loops)},
        {"activeLoopId", p.activeLoopId.empty() ? nlohmann::json() : nlohmann::json(p.activeLoopId)},
        {"gain", p.faders.gain},
        {"balance", p.faders.balance},
        {"speed", p.faders.speed},
        {"pitchSemitones", p.faders.pitchSemitones},
        {"savedAt", p.savedAt}});

    if (p.eq)
    {
        json["eq"] = EqJson(*p.eq);
    }

    if (!p.presetId.empty())
    {
        json["presetId"] = p.presetId;

        if (!p.presetName.empty())
        {
            json["presetName"] = p.presetName;
        }
    }

    if (!p.links.empty())
    {
        auto links = nlohmann::json::array();

        for (const auto& link : p.links)
        {
            links.push_back({{"id", link.id}, {"title", link.title}, {"url", link.url}});
        }

        json["links"] = std::move(links);
    }

    return json;
}

std::vector<Project> ReadProjects(const nlohmann::json& appSettings)
{
    std::vector<Project> projects;

    if (!appSettings.is_object() || !appSettings.contains(kProjectsSetting) ||
        !appSettings[kProjectsSetting].is_array())
    {
        return projects;
    }

    for (const auto& entry : appSettings[kProjectsSetting])
    {
        if (auto project = ParseProject(entry))
        {
            projects.push_back(std::move(*project));
        }
    }

    std::stable_sort(projects.begin(), projects.end(),
                     [](const Project& a, const Project& b) { return a.savedAt > b.savedAt; });
    return projects;
}

nlohmann::json ProjectsJson(const std::vector<Project>& projects)
{
    auto list = nlohmann::json::array();

    for (const auto& project : projects)
    {
        list.push_back(ProjectJson(project));
    }

    return list;
}

std::string SuggestLoopTemplateName(const std::string& base, const std::vector<std::string>& existing)
{
    const auto trimmed = Trim(base);

    if (trimmed.empty())
    {
        return trimmed;
    }

    std::vector<int> used;
    const auto prefix = trimmed + " ";

    for (const auto& raw : existing)
    {
        const auto name = Trim(raw);

        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }

        const auto digits = name.substr(prefix.size());

        if (std::all_of(digits.begin(), digits.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) &&
            digits.size() < 9)
        {
            used.push_back(std::stoi(digits));
        }
    }

    int next = 1;

    while (std::find(used.begin(), used.end(), next) != used.end())
    {
        ++next;
    }

    return trimmed + " " + std::to_string(next);
}

std::string SuggestDefaultLoopName(const std::vector<Loop>& loops)
{
    auto n = loops.size() + 1;
    const auto taken = [&loops](const std::string& name) {
        return std::any_of(loops.begin(), loops.end(), [&name](const Loop& l) { return l.name == name; });
    };
    auto candidate = "New Loop " + std::to_string(n);

    while (taken(candidate))
    {
        candidate = "New Loop " + std::to_string(++n);
    }

    return candidate;
}

Eq DefaultEq()
{
    Eq eq;

    for (const auto& band : kBands)
    {
        eq.params[band.gain] = 0.0;
        eq.params[band.freq] = band.freqDefault;
        eq.params[band.q] = band.qDefault;

        if (band.shape)
        {
            eq.params[band.shape] = 0.0;
        }
    }

    return eq;
}

Eq SanitizeEq(const nlohmann::json& value)
{
    auto eq = DefaultEq();

    if (!value.is_object())
    {
        return eq;
    }

    eq.enabled = value.contains("enabled") && value["enabled"].is_boolean() && value["enabled"].get<bool>();

    if (const auto it = value.find("params"); it != value.end() && it->is_object())
    {
        for (auto& [key, current] : eq.params)
        {
            if (it->contains(key) && IsFiniteNumber((*it)[key]))
            {
                current = (*it)[key].get<double>();
            }
        }
    }

    return eq;
}

nlohmann::json EqParamsJson(const Eq& eq)
{
    nlohmann::json params = nlohmann::json::object();

    for (const auto& [key, value] : eq.params)
    {
        params[key] = value;
    }

    return params;
}

nlohmann::json EqJson(const Eq& eq)
{
    return {{"enabled", eq.enabled}, {"params", EqParamsJson(eq)}};
}

bool IsEqShaping(const Eq& eq)
{
    if (!eq.enabled)
    {
        return false;
    }

    return std::any_of(std::begin(kBands), std::end(kBands), [&eq](const BandKeys& band) {
        const auto it = eq.params.find(band.gain);
        return it != eq.params.end() && std::abs(it->second) > 0.05;
    });
}

std::optional<std::string> NormalizeLinkUrl(const std::string& input)
{
    const auto trimmed = Trim(input);

    if (trimmed.empty() || trimmed.size() > 2048 || std::any_of(trimmed.begin(), trimmed.end(), [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }))
    {
        return std::nullopt;
    }

    static const std::regex kScheme(R"(^[a-zA-Z][a-zA-Z0-9+.\-]*:)");
    const auto withScheme = std::regex_search(trimmed, kScheme) ? trimmed : "https://" + trimmed;
    // scheme://host[:port][/...]: http(s) only, with a dotted host.
    static const std::regex kUrl(R"(^(https?)://([^/?#:@]+(?::[^/?#@]*)?@)?([^/?#:]+)(:\d+)?([/?#].*)?$)",
                                 std::regex::icase);
    std::smatch match;

    if (!std::regex_match(withScheme, match, kUrl))
    {
        return std::nullopt;
    }

    const auto host = Lower(match[3].str());

    if (host.find('.') == std::string::npos)
    {
        return std::nullopt;
    }

    // As URL.href: the scheme and host in lower case, and "/" for an empty path.
    std::string href = Lower(match[1].str()) + "://" + match[2].str() + host + match[4].str();
    const auto rest = match[5].str();
    href += rest.empty() || rest[0] != '/' ? "/" + rest : rest;
    return href;
}

std::string DescribeLink(const Link& link)
{
    if (!link.title.empty())
    {
        return link.title;
    }

    static const std::regex kHost(R"(^[a-zA-Z][a-zA-Z0-9+.\-]*://(?:[^/?#@]*@)?([^/?#:]+))");
    std::smatch match;

    if (!std::regex_search(link.url, match, kHost))
    {
        return link.url;
    }

    auto host = match[1].str();

    if (Lower(host.substr(0, 4)) == "www.")
    {
        host = host.substr(4);
    }

    return host;
}

std::string FormatClock(double seconds)
{
    const long total = std::max(0L, static_cast<long>(std::floor(guitarfx::IsFinite(seconds) ? seconds : 0.0)));
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", total / 60, total % 60);
    return buffer;
}

double JumpBackTarget(std::optional<double> clicked, const Loop* activeLoop)
{
    const double fallback = activeLoop ? activeLoop->startSec : 0.0;

    if (!clicked)
    {
        return fallback;
    }

    if (activeLoop && (*clicked < activeLoop->startSec || *clicked >= activeLoop->endSec))
    {
        return fallback;
    }

    return *clicked;
}

bool IsReloadablePath(const std::string& path)
{
    const auto trimmed = Trim(path);

    if (trimmed.size() >= 3 && std::isalpha(static_cast<unsigned char>(trimmed[0])) && trimmed[1] == ':' &&
        (trimmed[2] == '\\' || trimmed[2] == '/'))
    {
        return true;
    }

    return trimmed.rfind("\\\\", 0) == 0 || trimmed.rfind('/', 0) == 0;
}

bool IsSameTrackPath(const std::string& a, const std::string& b)
{
    return !a.empty() && Lower(Trim(a)) == Lower(Trim(b));
}
} // namespace practice

using namespace practice;

namespace
{
constexpr double kDragSendDelay = 0.08; // the web UI's 80 ms debounce
} // namespace

PracticeTool::PracticeTool(UiClient& client, UiCommands& commands) : mClient(client), mCommands(commands)
{
    mSeenLoads = mClient.State().practiceTool.loads;
    mSubscription = mClient.Subscribe(Topic::PracticeTool, [this] { OnTrack(); });
}

const PracticeToolTrack& PracticeTool::Track() const
{
    return mClient.State().practiceTool;
}

bool PracticeTool::HasAudio() const
{
    const auto& t = Track();
    return t.durationSec > 0.0 && !t.filePath.empty();
}

double PracticeTool::PositionNow(double now) const
{
    const auto& t = Track();

    if (!t.playing)
    {
        return t.positionSec;
    }

    double projected = t.positionSec + std::max(0.0, now - t.positionAt) * mFaders.speed;

    // Between reports the engine has gone round the loop: so does the playhead.
    if (const auto* loop = ActiveLoop();
        loop && loop->endSec > loop->startSec && t.positionSec <= loop->endSec && projected > loop->endSec)
    {
        projected = loop->startSec + std::fmod(projected - loop->startSec, loop->endSec - loop->startSec);
    }

    return std::clamp(projected, 0.0, t.durationSec);
}

void PracticeTool::Changed()
{
    if (onChanged)
    {
        onChanged();
    }
}

void PracticeTool::OnTrack()
{
    const auto& t = Track();

    if (t.loads == mSeenLoads)
    {
        Changed();
        return;
    }

    mSeenLoads = t.loads;

    // A new track resets the session: its own loops come back from the per-file store, and the
    // faders and EQ go back to their defaults - unless this load is a project being recalled.
    mLoops = LoadLoops(mClient.State().appSettings, FileFingerprint(t.filePath, t.durationSec));
    mActiveLoopId.clear();
    mCandidate.reset();
    mJumpBackPoint.reset();
    mDeleted.reset();
    auto recalled = std::move(mPendingRecall);
    mPendingRecall.reset();

    if (recalled && IsSameTrackPath(recalled->filePath, t.filePath))
    {
        ApplyProject(*recalled);
    }
    else
    {
        ApplyFaders(LoopSettings{});
        mEq = DefaultEq();
        PushEq();
    }

    Changed();
}

void PracticeTool::Tick(double now)
{
    mLastTick = now;

    if (mPendingSpeed.set && now >= mPendingSpeed.due)
    {
        mPendingSpeed.set = false;
        mCommands.SetPracticeToolSpeed(mPendingSpeed.value);
    }

    if (mPendingPitch.set && now >= mPendingPitch.due)
    {
        mPendingPitch.set = false;
        mCommands.SetPracticeToolPitch(mPendingPitch.value);
    }

    if (mPendingRegion && now >= mPendingRegionDue)
    {
        mPendingRegion = false;

        if (const auto* loop = ActiveLoop())
        {
            mCommands.SetPracticeToolLoopRegion(loop->startSec, loop->endSec);
        }
    }
}

// ── Transport ───────────────────────────────────────────────────────────────────

void PracticeTool::Browse()
{
    mCommands.BrowsePracticeToolFile();
}

void PracticeTool::PlayPause()
{
    if (!HasAudio())
    {
        return;
    }

    mCommands.SetPracticeToolTransport(Track().playing ? "pause" : "play");
}

void PracticeTool::Stop()
{
    mCommands.SetPracticeToolTransport("stop");
}

void PracticeTool::ClickAt(double seconds)
{
    const auto& t = Track();

    if (!HasAudio())
    {
        return;
    }

    seconds = std::clamp(seconds, 0.0, t.durationSec);

    if (const auto* loop = ActiveLoop(); loop && (seconds < loop->startSec || seconds > loop->endSec))
    {
        // A click outside the loop starts the next one there.
        Deactivate();
        const double end = std::min(t.durationSec, seconds + kDefaultNewLoopSec);
        const double start = std::max(0.0, std::min(seconds, end - kMinLoopSpanSec));
        mCandidate = std::make_pair(start, end);
    }

    mJumpBackPoint = seconds;
    mCommands.SeekPracticeToolFile(seconds);
    // The engine reports no position while paused: move the drawn playhead now.
    auto& state = mClient.MutableState().practiceTool;
    state.positionSec = seconds;
    state.positionAt = LooperClockNow();
    Changed();
}

void PracticeTool::JumpBack()
{
    if (!HasAudio())
    {
        return;
    }

    const double target = JumpBackTarget(mJumpBackPoint, ActiveLoop());
    mCommands.SeekPracticeToolFile(target);
    auto& state = mClient.MutableState().practiceTool;
    state.positionSec = target;
    state.positionAt = LooperClockNow();
    Changed();
}

std::optional<double> PracticeTool::JumpBackMarker() const
{
    const auto* loop = ActiveLoop();
    const double target = JumpBackTarget(mJumpBackPoint, loop);

    if (target == (loop ? loop->startSec : 0.0))
    {
        return std::nullopt;
    }

    return target;
}

// ── Faders ──────────────────────────────────────────────────────────────────────

void PracticeTool::SendFader(Fader fader, double value, bool immediate)
{
    switch (fader)
    {
    case Fader::Volume:
        mCommands.SetPracticeToolGain(value);
        break;
    case Fader::Balance:
        mCommands.SetPracticeToolBalance(value);
        break;
    case Fader::Speed:
    case Fader::Pitch: {
        auto& pending = fader == Fader::Speed ? mPendingSpeed : mPendingPitch;

        if (immediate)
        {
            pending.set = false;

            if (fader == Fader::Speed)
            {
                mCommands.SetPracticeToolSpeed(value);
            }
            else
            {
                mCommands.SetPracticeToolPitch(value);
            }
        }
        else
        {
            pending = {value, mLastTick + kDragSendDelay, true};
        }

        break;
    }
    }
}

void PracticeTool::SetFader(Fader fader, double value, bool immediate)
{
    const auto& spec = SpecFor(fader);
    value = std::clamp(value, spec.min, spec.max);

    if (fader == Fader::Speed)
    {
        // The playhead runs on at the old rate up to now, then at the new one.
        auto& state = mClient.MutableState().practiceTool;
        const double now = LooperClockNow();
        state.positionSec = PositionNow(now);
        state.positionAt = now;
    }

    SetFaderValue(mFaders, fader, value);
    SendFader(fader, value, immediate);

    // The active loop takes the new values as its own, saved on release.
    if (auto* loop = FindLoop(mActiveLoopId))
    {
        loop->settings = mFaders;

        if (immediate)
        {
            PersistLoops();
        }
    }

    Changed();
}

void PracticeTool::ApplyFaders(const LoopSettings& settings)
{
    for (const auto fader : {Fader::Volume, Fader::Balance, Fader::Speed, Fader::Pitch})
    {
        const auto& spec = SpecFor(fader);
        const double value = std::clamp(FaderValue(settings, fader), spec.min, spec.max);
        SetFaderValue(mFaders, fader, value);
        SendFader(fader, value, true);
    }
}

// ── EQ ──────────────────────────────────────────────────────────────────────────

void PracticeTool::PushEq()
{
    mCommands.SetPracticeToolEq(mEq.enabled, EqParamsJson(mEq));
}

void PracticeTool::SetEqEnabled(bool enabled)
{
    mEq.enabled = enabled;
    mCommands.SetPracticeToolEq(enabled);
    Changed();
}

void PracticeTool::SetEqParam(const std::string& key, double value)
{
    if (mEq.params.count(key) == 0 || !guitarfx::IsFinite(value))
    {
        return;
    }

    mEq.params[key] = value;
    mCommands.SetPracticeToolEq(std::nullopt, {{key, value}});
    Changed();
}

void PracticeTool::ResetEq()
{
    const bool enabled = mEq.enabled;
    mEq = DefaultEq();
    mEq.enabled = enabled;
    PushEq();
    Changed();
}

// ── Loops ───────────────────────────────────────────────────────────────────────

Loop* PracticeTool::FindLoop(const std::string& loopId)
{
    if (loopId.empty())
    {
        return nullptr;
    }

    const auto it = std::find_if(mLoops.begin(), mLoops.end(), [&loopId](const Loop& l) { return l.id == loopId; });
    return it != mLoops.end() ? &*it : nullptr;
}

const Loop* PracticeTool::ActiveLoop() const
{
    return const_cast<PracticeTool*>(this)->FindLoop(mActiveLoopId);
}

std::string PracticeTool::NewId(const char* prefix)
{
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s-%llx-%x", prefix, static_cast<unsigned long long>(ms), ++mIdCounter);
    return buffer;
}

void PracticeTool::PersistLoops()
{
    const auto& t = Track();

    if (t.filePath.empty())
    {
        return;
    }

    const auto fingerprint = FileFingerprint(t.filePath, t.durationSec);
    mCommands.SetSetting(kLoopsSetting, LoopsSettingWith(mClient.State().appSettings, fingerprint, mLoops));
}

void PracticeTool::ActivateLoop(Loop& loop)
{
    // Cleared first: applying this loop's settings must not record them onto the one being left.
    mActiveLoopId.clear();

    if (loop.settings)
    {
        ApplyFaders(*loop.settings);
    }

    mActiveLoopId = loop.id;

    if (!loop.settings)
    {
        // A loop from before loops had settings adopts the current ones.
        loop.settings = mFaders;
        PersistLoops();
    }

    mCandidate.reset();
    mJumpBackPoint.reset();
    mPendingRegion = false;
    mCommands.SetPracticeToolLoopRegion(loop.startSec, loop.endSec, true);
    auto& state = mClient.MutableState().practiceTool;
    state.positionSec = loop.startSec;
    state.positionAt = LooperClockNow();
}

void PracticeTool::Deactivate()
{
    mActiveLoopId.clear();
    mPendingRegion = false;
    mCommands.ClearPracticeToolLoopRegion();
    mCommands.SetPracticeToolLooping(false);
}

void PracticeTool::ReleaseActiveLoop()
{
    if (mActiveLoopId.empty())
    {
        return;
    }

    Deactivate();
    Changed();
}

void PracticeTool::SetRange(double startSec, double endSec, bool done)
{
    const auto& t = Track();

    if (!HasAudio())
    {
        return;
    }

    if (startSec > endSec)
    {
        std::swap(startSec, endSec);
    }

    startSec = std::clamp(startSec, 0.0, t.durationSec);
    endSec = std::clamp(endSec, 0.0, t.durationSec);
    const double minSpan = std::min(kMinLoopSpanSec, t.durationSec);

    if (endSec - startSec < minSpan)
    {
        endSec = std::min(t.durationSec, startSec + minSpan);
        startSec = std::max(0.0, endSec - minSpan);
    }

    if (auto* loop = FindLoop(mActiveLoopId))
    {
        loop->startSec = startSec;
        loop->endSec = endSec;

        if (done)
        {
            mPendingRegion = false;
            mCommands.SetPracticeToolLoopRegion(startSec, endSec);
            PersistLoops();
        }
        else
        {
            mPendingRegion = true;
            mPendingRegionDue = mLastTick + kDragSendDelay;
        }
    }
    else
    {
        mCandidate = std::make_pair(startSec, endSec);
    }

    Changed();
}

std::string PracticeTool::AddLoop(const std::string& name)
{
    const auto& t = Track();

    if (!HasAudio())
    {
        if (onNotice)
        {
            onNotice("Load an audio file first", {});
        }

        return {};
    }

    double start = 0.0, end = 0.0;

    if (mCandidate)
    {
        start = mCandidate->first;
        end = mCandidate->second;
    }
    else
    {
        start = PositionNow(LooperClockNow());
        end = std::min(t.durationSec, start + kDefaultNewLoopSec);

        if (end - start < kMinLoopSpanSec)
        {
            start = std::max(0.0, end - kMinLoopSpanSec);
        }
    }

    Loop loop;
    loop.id = NewId("loop");
    const auto trimmed = name.find_first_not_of(" \t") == std::string::npos ? std::string() : name;
    loop.name = trimmed.empty() ? SuggestDefaultLoopName(mLoops) : trimmed;
    loop.startSec = start;
    loop.endSec = end;
    loop.settings = mFaders; // made under the current track settings
    mLoops.push_back(loop);
    ActivateLoop(mLoops.back());
    PersistLoops();
    Changed();
    return loop.id;
}

void PracticeTool::SelectLoop(const std::string& loopId)
{
    auto* loop = FindLoop(loopId);

    if (!loop)
    {
        return;
    }

    if (mActiveLoopId == loopId)
    {
        Deactivate(); // the only way to stop looping: no loop picked
    }
    else
    {
        ActivateLoop(*loop);
    }

    Changed();
}

void PracticeTool::ToggleLoopPlayback(const std::string& loopId)
{
    auto* loop = FindLoop(loopId);

    if (!loop || !HasAudio())
    {
        return;
    }

    if (loop->id == mActiveLoopId && Track().playing)
    {
        Stop();
        return;
    }

    ActivateLoop(*loop);
    mCommands.SetPracticeToolTransport("play");
    Changed();
}

void PracticeTool::RenameLoop(const std::string& loopId, const std::string& name)
{
    auto* loop = FindLoop(loopId);
    const auto trimmed = name.find_first_not_of(" \t") == std::string::npos ? std::string() : name;

    if (!loop || trimmed.empty() || loop->name == trimmed)
    {
        return;
    }

    loop->name = trimmed;
    PersistLoops();
    Changed();
}

void PracticeTool::SetLoopTimes(const std::string& loopId, double startSec, double endSec)
{
    auto* loop = FindLoop(loopId);
    const auto& t = Track();

    if (!loop || !guitarfx::IsFinite(startSec) || !guitarfx::IsFinite(endSec))
    {
        return;
    }

    startSec = std::clamp(startSec, 0.0, t.durationSec);
    endSec = std::clamp(endSec, 0.0, t.durationSec);

    if (endSec - startSec < std::min(kMinLoopSpanSec, t.durationSec))
    {
        return;
    }

    loop->startSec = startSec;
    loop->endSec = endSec;

    if (loop->id == mActiveLoopId)
    {
        mCommands.SetPracticeToolLoopRegion(startSec, endSec);
    }

    PersistLoops();
    Changed();
}

void PracticeTool::DeleteLoop(const std::string& loopId)
{
    const auto it = std::find_if(mLoops.begin(), mLoops.end(), [&loopId](const Loop& l) { return l.id == loopId; });

    if (it == mLoops.end())
    {
        return;
    }

    // A second delete makes the first permanent: one undo at a time.
    mDeleted = *it;
    mDeletedIndex = static_cast<std::size_t>(it - mLoops.begin());
    const bool wasActive = loopId == mActiveLoopId;
    mLoops.erase(it);

    if (wasActive)
    {
        Deactivate();
    }

    PersistLoops();
    Changed();
}

void PracticeTool::UndoDelete()
{
    if (!mDeleted)
    {
        return;
    }

    mLoops.insert(mLoops.begin() + static_cast<std::ptrdiff_t>(std::min(mDeletedIndex, mLoops.size())), *mDeleted);
    mDeleted.reset();
    PersistLoops();
    Changed();
}

void PracticeTool::ForgetDeleted()
{
    if (!mDeleted)
    {
        return;
    }

    mDeleted.reset();
    Changed();
}

// ── Projects ────────────────────────────────────────────────────────────────────

std::vector<Project> PracticeTool::Projects() const
{
    return ReadProjects(mClient.State().appSettings);
}

void PracticeTool::WriteProjects(const std::vector<Project>& projects)
{
    mCommands.SetSetting(kProjectsSetting, ProjectsJson(projects));
}

std::string PracticeTool::SuggestedProjectName() const
{
    auto title = Track().title;
    const auto dot = title.find_last_of('.');

    if (dot != std::string::npos && dot > 0 && title.size() - dot <= 5)
    {
        title = title.substr(0, dot);
    }

    return title;
}

bool PracticeTool::SaveProject(const std::string& name, bool includePreset)
{
    const auto& t = Track();
    const auto trimmed =
        name.find_first_not_of(" \t") == std::string::npos ? std::string() : name.substr(name.find_first_not_of(" \t"));

    if (t.filePath.empty() || trimmed.empty())
    {
        return false;
    }

    auto projects = Projects();
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const auto existing = std::find_if(projects.begin(), projects.end(),
                                       [&](const Project& p) { return lower(p.name) == lower(trimmed); });

    Project p;
    p.id = existing != projects.end() ? existing->id : NewId("ptproj");
    p.name = trimmed;
    p.filePath = t.filePath;
    p.fileTitle = t.title.empty() ? t.filePath.substr(t.filePath.find_last_of("\\/") + 1) : t.title;
    p.durationSec = t.durationSec;
    p.loops = mLoops;
    p.activeLoopId = mActiveLoopId;
    p.faders = mFaders;
    p.eq = mEq;
    const auto& state = mClient.State();

    if (includePreset && !state.activePresetId.empty())
    {
        p.presetId = state.activePresetId;

        if (const auto* summary = state.FindPresetSummary(state.activePresetId))
        {
            p.presetName = summary->name;
        }
    }

    // Links are edited on the saved project: saving over it keeps them. So does what only the web
    // UI edits (the tab binding, the YouTube video), while the project still follows this track.
    if (existing != projects.end())
    {
        p.links = existing->links;

        if (existing->filePath.empty() || IsSameTrackPath(existing->filePath, t.filePath))
        {
            p.kept = existing->kept;
            p.youtubeVideoId = existing->youtubeVideoId;
            p.youtubeTitle = existing->youtubeTitle;
        }
    }

    p.savedAt = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    if (existing != projects.end())
    {
        *existing = p;
    }
    else if (static_cast<int>(projects.size()) >= kMaxProjects)
    {
        return false;
    }
    else
    {
        projects.push_back(p);
    }

    WriteProjects(projects);
    mSelectedProject = p.id;
    Changed();
    return true;
}

bool PracticeTool::LoadProject(const std::string& projectId)
{
    const auto projects = Projects();
    const auto it =
        std::find_if(projects.begin(), projects.end(), [&projectId](const Project& p) { return p.id == projectId; });

    if (it == projects.end())
    {
        return false;
    }

    mSelectedProject = projectId;

    // A project that follows only a YouTube video plays in the web UI's tab view.
    if (it->filePath.empty())
    {
        Changed();
        return false;
    }

    if (!it->presetId.empty() && mClient.State().FindPresetSummary(it->presetId) != nullptr &&
        mClient.State().activePresetId != it->presetId)
    {
        mCommands.LoadPreset(it->presetId);
    }

    if (IsSameTrackPath(Track().filePath, it->filePath))
    {
        ApplyProject(*it);
        Changed();
        return true;
    }

    if (!IsReloadablePath(it->filePath))
    {
        Changed();
        return false;
    }

    // Applied once the engine reports the track loaded (OnTrack).
    mPendingRecall = *it;
    mCommands.LoadPracticeToolFile(it->filePath);
    Changed();
    return true;
}

void PracticeTool::ApplyProject(const Project& project)
{
    mLoops = project.loops;
    mActiveLoopId.clear();
    // The recalled set becomes the track's working set.
    PersistLoops();
    ApplyFaders(project.faders);
    mEq = project.eq ? *project.eq : DefaultEq();
    PushEq();

    if (auto* loop = FindLoop(project.activeLoopId))
    {
        ActivateLoop(*loop);
    }
    else
    {
        Deactivate();
    }

    mCandidate.reset();
    mDeleted.reset();
}

void PracticeTool::DeleteProject(const std::string& projectId)
{
    auto projects = Projects();
    const auto before = projects.size();
    projects.erase(
        std::remove_if(projects.begin(), projects.end(), [&projectId](const Project& p) { return p.id == projectId; }),
        projects.end());

    if (projects.size() == before)
    {
        return;
    }

    WriteProjects(projects);

    if (mSelectedProject == projectId)
    {
        mSelectedProject.clear();
    }

    Changed();
}

std::string PracticeTool::AddLink(const std::string& projectId, const std::string& title, const std::string& url)
{
    auto projects = Projects();
    const auto it =
        std::find_if(projects.begin(), projects.end(), [&projectId](const Project& p) { return p.id == projectId; });

    if (it == projects.end())
    {
        return "no-project";
    }

    const auto normalized = NormalizeLinkUrl(url);

    if (!normalized)
    {
        return "invalid-url";
    }

    if (static_cast<int>(it->links.size()) >= kMaxLinks)
    {
        return "too-many";
    }

    auto trimmedTitle = title;
    const auto first = trimmedTitle.find_first_not_of(" \t");
    trimmedTitle = first == std::string::npos
                       ? std::string()
                       : trimmedTitle.substr(first, trimmedTitle.find_last_not_of(" \t") - first + 1);
    it->links.push_back({NewId("ptlink"), trimmedTitle.substr(0, 200), *normalized});
    WriteProjects(projects); // savedAt untouched: a link is not a re-save
    Changed();
    return "added";
}

void PracticeTool::RemoveLink(const std::string& projectId, const std::string& linkId)
{
    auto projects = Projects();
    const auto it =
        std::find_if(projects.begin(), projects.end(), [&projectId](const Project& p) { return p.id == projectId; });

    if (it == projects.end())
    {
        return;
    }

    const auto before = it->links.size();
    it->links.erase(
        std::remove_if(it->links.begin(), it->links.end(), [&linkId](const Link& l) { return l.id == linkId; }),
        it->links.end());

    if (it->links.size() == before)
    {
        return;
    }

    WriteProjects(projects);
    Changed();
}
} // namespace guitarfx::uiclient
