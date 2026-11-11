/**
 * JamPracticeToolTests.cpp - The Jam tab's rules the native UIs share: the backing-track search,
 * its cache and favourites (core/src/uiclient/Jam.h), and the Practice Tool's faders, loops,
 * projects and links (core/src/uiclient/PracticeTool.h), each checked against what the web UI's
 * jam/*.ts and practiceTool/*.ts do. The PracticeTool session runs against a UiClient whose
 * transport records what it sends, fed the engine's own replies.
 */

#include "uiclient/Jam.h"
#include "uiclient/PracticeTool.h"
#include "uiclient/UiCommands.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace ui = guitarfx::uiclient;
namespace jam = guitarfx::uiclient::jam;
namespace practice = guitarfx::uiclient::practice;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

bool Near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

void TestQueries()
{
    Expect(jam::NormalizeQuery("  blues   in A ") == "blues in A backing track",
           "spaces collapse, backing track added");
    Expect(jam::NormalizeQuery("Slow Blues Backing Track") == "Slow Blues Backing Track", "already says backing track");
    Expect(jam::NormalizeQuery("backing   track") == "backing track", "collapsed before the check");
    Expect(jam::NormalizeQuery("backing tracks") == "backing tracks backing track", "a whole phrase only");
    Expect(jam::NormalizeQuery("   ").empty(), "an empty query");

    Expect(jam::DecodeYouTubeText("80&#39;s Rock &amp; Roll") == "80's Rock & Roll", "decimal and named entities");
    Expect(jam::DecodeYouTubeText("&#x263A; &QUOT;x&quot;") == "\xE2\x98\xBA \"x\"",
           "hex entity to UTF-8, names in any case");
    Expect(jam::DecodeYouTubeText("&nope; & &#0;") == "&nope; & &#0;", "unknown and invalid entities stay");

    const auto url = jam::SearchUrl("a&b backing track", "k y");
    Expect(url.find("q=a%26b%20backing%20track") != std::string::npos, "the query is encoded");
    Expect(url.find("videoEmbeddable=true") != std::string::npos && url.find("maxResults=12") != std::string::npos,
           "the web UI's search");
    Expect(url.find("key=k%20y") != std::string::npos, "the key is encoded");
}

void TestResults()
{
    const auto payload = nlohmann::json::parse(R"({"items":[
        {"id":{"videoId":"v1"},"snippet":{"title":"Rock &amp; Roll","channelTitle":"Ch","thumbnails":{"default":{"url":"d"},"high":{"url":"h"}}}},
        {"id":{"channelId":"c"},"snippet":{"title":"no video"}},
        {"id":{"videoId":"v2"},"snippet":{"thumbnails":{"medium":{"url":"m"}}}},
        {"id":{"videoId":"v3"}}
    ]})");
    const auto videos = jam::ParseSearchResults(payload);
    Expect(videos.size() == 2, "entries without a video id or snippet are left out");

    if (videos.size() == 2)
    {
        Expect(videos[0].title == "Rock & Roll" && videos[0].channelTitle == "Ch" && videos[0].thumbnailUrl == "h",
               "the best thumbnail");
        Expect(videos[1].title == "v2" && videos[1].channelTitle == "Unknown channel" && videos[1].thumbnailUrl == "m",
               "the fallbacks");
    }

    Expect(jam::DescribeError(nlohmann::json::parse(R"({"error":{"message":"  quota  "}})")) == "quota",
           "the API's message");
    Expect(jam::DescribeError(nullptr) == "Request failed.", "no message");
}

void TestFavoritesAndCache()
{
    nlohmann::json settings = {{"jam.youtubeApiKey", "  key "}};
    Expect(jam::ApiKey(settings) == "key", "the key, trimmed");
    Expect(jam::Favorites(settings).empty(), "no favourites");

    jam::Video a{"a", "A &amp; B", "C", "t"}, b{"b", "B", "C", "t"};
    auto favorites = jam::ToggleFavorite({}, a);
    favorites = jam::ToggleFavorite(favorites, b);
    Expect(favorites.size() == 2 && favorites[0].videoId == "b", "a new favourite goes first");
    favorites = jam::ToggleFavorite(favorites, a);
    Expect(favorites.size() == 1 && !jam::IsFavorite(favorites, "a"), "a favourite toggles off");

    settings[jam::kFavoritesSetting] = jam::VideosJson({a});
    settings[jam::kFavoritesSetting].push_back({{"videoId", "x"}}); // incomplete: dropped
    const auto read = jam::Favorites(settings);
    Expect(read.size() == 1 && read[0].title == "A & B", "stored favourites read back, titles decoded");

    settings[jam::kSearchCacheSetting] = jam::SearchCacheJson(" Blues ", {a, b});
    Expect(settings[jam::kSearchCacheSetting]["query"] == "Blues" &&
               settings[jam::kSearchCacheSetting]["normalizedQuery"] == "Blues backing track",
           "the cache keeps the typed and the sent query");
    Expect(jam::CachedSearch(settings, "blues").has_value() == false,
           "the cache answers only the same normalised query");
    const auto hit = jam::CachedSearch(settings, "Blues  ");
    Expect(hit && hit->results.size() == 2, "a cache hit");
    Expect(jam::SearchCacheJson("  ", {}).is_null(), "nothing kept for an empty query");

    Expect(jam::EmbedUrl("a b") == "https://api.sepia.invalid/v1/embed/youtube?v=a%20b", "the wrapper page");
    Expect(jam::StaysInWebView(jam::EmbedUrl("x")), "the player stays");
    // Scales is drawn natively now (the Scale Guide): the old scale explorer is just another site.
    Expect(!jam::StaysInWebView("https://www.youtube.com/watch?v=x") &&
               !jam::StaysInWebView("https://scalex.soundshed.com/minor") &&
               !jam::StaysInWebView("https://api.sepia.invalid.evil.com/v1/embed/x"),
           "anything else goes to the browser");
}

void TestFaders()
{
    using F = practice::Fader;
    Expect(Near(practice::FaderToSlider(F::Volume, 1.0), 0.5) && Near(practice::FaderToSlider(F::Speed, 1.0), 0.5),
           "the default is the middle");
    Expect(Near(practice::FaderToSlider(F::Volume, 1.5), 1.0) && Near(practice::FaderToSlider(F::Speed, 0.25), 0.0),
           "the ends");
    Expect(Near(practice::SliderToFader(F::Speed, 0.25), 0.625) && Near(practice::SliderToFader(F::Speed, 0.75), 1.5),
           "each half on its own");
    Expect(practice::FormatFader(F::Volume, 0.854) == "85%" && practice::FormatFader(F::Balance, -0.3) == "L30" &&
               practice::FormatFader(F::Balance, 0.001) == "C" && practice::FormatFader(F::Pitch, 2.0) == "+2.0 st" &&
               practice::FormatFader(F::Pitch, -1.25) == "-1.2 st",
           "the web UI's readouts");
    Expect(Near(*practice::ParseFader(F::Speed, "85 %"), 0.85) && Near(*practice::ParseFader(F::Pitch, "-3st"), -3.0),
           "typed values");
    Expect(Near(*practice::ParseFader(F::Balance, "r 25"), 0.25) &&
               Near(*practice::ParseFader(F::Balance, "-35"), -0.35) &&
               Near(*practice::ParseFader(F::Balance, "Center"), 0.0),
           "balance, either side");
    Expect(Near(*practice::ParseFader(F::Volume, "900"), 1.5), "clamped to the range");
    Expect(!practice::ParseFader(F::Pitch, "loud"), "nothing for words");
}

void TestLoopsAndProjects()
{
    Expect(practice::SuggestLoopTemplateName("Verse", {"Verse 1", "Verse 3", "Chorus 2"}) == "Verse 2",
           "the lowest free number");
    Expect(practice::SuggestLoopTemplateName("  ", {}).empty(), "no name for no base");
    std::vector<practice::Loop> loops{{"a", "New Loop 2", 0, 1, {}}};
    Expect(practice::SuggestDefaultLoopName(loops) == "New Loop 3", "a default name not taken");

    Expect(practice::FileFingerprint(" C:\\Music\\Song.wav ", 61.234) == "c:\\music\\song.wav|61.23",
           "the per-file key");
    practice::Loop withSettings{"l1", "Verse 1", 1.0, 5.0, practice::LoopSettings{0.8, -0.2, 0.75, 2.0}};
    nlohmann::json settings = nlohmann::json::object();
    settings[practice::kLoopsSetting] = practice::LoopsSettingWith(settings, "f", {withSettings});
    auto partial = practice::LoopJson(withSettings);
    partial["id"] = "l2";
    partial["settings"].erase("speed");
    settings[practice::kLoopsSetting]["f"].push_back(partial);
    const auto read = practice::LoadLoops(settings, "f");
    Expect(read.size() == 2 && read[0].settings && Near(read[0].settings->speed, 0.75),
           "loops read back with their settings");
    Expect(read.size() == 2 && !read[1].settings, "a partial set of settings is dropped");
    Expect(!practice::LoopsSettingWith(settings, "f", {}).contains("f"), "no loops: the entry goes");

    practice::Project p;
    p.id = "p1";
    p.name = " Solo ";
    p.filePath = "C:\\Music\\Song.wav";
    p.loops = {withSettings};
    p.activeLoopId = "l1";
    p.eq = practice::DefaultEq();
    p.eq->params["lowGain"] = 3.0;
    p.links = {{"k1", "", "https://www.songsterr.com/a/b"}};
    p.savedAt = 5;
    auto json = practice::ProjectJson(p);
    const auto back = practice::ParseProject(json);
    Expect(back && back->name == "Solo" && back->fileTitle == "Song.wav" && back->activeLoopId == "l1",
           "a project round-trips");
    Expect(back && back->eq && Near(back->eq->params.at("lowGain"), 3.0) && back->links.size() == 1,
           "its EQ and links");
    json.erase("eq");
    json["activeLoopId"] = "gone";
    const auto old = practice::ParseProject(json);
    Expect(old && !old->eq && old->activeLoopId.empty(), "an old project has no EQ; a missing active loop is none");
    Expect(!practice::ParseProject({{"id", "x"}, {"name", "n"}}), "no track: not a project");

    nlohmann::json projects = nlohmann::json::array({practice::ProjectJson(p)});
    p.id = "p2";
    p.savedAt = 9;
    projects.push_back(practice::ProjectJson(p));
    const auto list = practice::ReadProjects({{practice::kProjectsSetting, projects}});
    Expect(list.size() == 2 && list[0].id == "p2", "newest first");

    Expect(practice::NormalizeLinkUrl("songsterr.com/a/wsa") == "https://songsterr.com/a/wsa",
           "a bare host gets https");
    Expect(practice::NormalizeLinkUrl("HTTP://Example.COM") == "http://example.com/", "as URL.href");
    Expect(!practice::NormalizeLinkUrl("javascript:alert(1)") && !practice::NormalizeLinkUrl("file:///c:/x") &&
               !practice::NormalizeLinkUrl("localhost/x") && !practice::NormalizeLinkUrl("a b.com"),
           "only http(s) with a dotted host");
    Expect(practice::DescribeLink({"x", "", "https://www.songsterr.com/a"}) == "songsterr.com",
           "an untitled link shows its host");

    Expect(practice::FormatClock(65.9) == "1:05", "the clock");
    practice::Loop loop{"x", "x", 10.0, 20.0, {}};
    Expect(Near(practice::JumpBackTarget(15.0, &loop), 15.0) && Near(practice::JumpBackTarget(25.0, &loop), 10.0) &&
               Near(practice::JumpBackTarget(std::nullopt, nullptr), 0.0),
           "jump back: a click inside the loop, else its start");
    Expect(practice::IsReloadablePath("C:/x.wav") && practice::IsReloadablePath("\\\\server\\x") &&
               practice::IsReloadablePath("/x") && !practice::IsReloadablePath("dropped.wav"),
           "only real paths reopen");
    Expect(!practice::IsEqShaping(practice::DefaultEq()), "a flat EQ does nothing");
}

struct Harness
{
    std::vector<nlohmann::json> sent;
    ui::UiClient client{[this](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); }};
    ui::UiCommands commands{client};

    void Receive(const nlohmann::json& message)
    {
        client.Enqueue(message.dump());
        client.DrainPending();
    }

    [[nodiscard]] int Count(const std::string& type) const
    {
        int n = 0;

        for (const auto& m : sent)
        {
            n += m.value("type", std::string()) == type ? 1 : 0;
        }

        return n;
    }

    [[nodiscard]] const nlohmann::json* Last(const std::string& type) const
    {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
        {
            if (it->value("type", std::string()) == type)
            {
                return &*it;
            }
        }

        return nullptr;
    }
};

nlohmann::json FileLoaded(const std::string& path, double duration)
{
    return {{"type", "practiceToolFileLoaded"},
            {"path", path},
            {"title", ""},
            {"durationSec", duration},
            {"waveformPeaksL", {0.1, 0.5}},
            {"waveformPeaksR", {0.2, 0.4}}};
}

void TestSession()
{
    Harness h;
    ui::PracticeTool tool(h.client, h.commands);
    int changes = 0;
    tool.onChanged = [&changes] { ++changes; };

    Expect(tool.AddLoop().empty(), "no loop without a track");
    h.Receive(FileLoaded("C:\\Music\\Song.wav", 60.0));
    Expect(tool.HasAudio() && tool.Track().title == "Song.wav" && tool.Track().peaksL.size() == 2,
           "the track as reported");
    Expect(h.Count("setPracticeToolGain") == 1 && h.Count("setPracticeToolSpeed") == 1 &&
               h.Count("setPracticeToolEq") == 1,
           "a new track resets the faders and the EQ");
    Expect(changes > 0, "listeners hear of it");

    // A loop from a marked range: picked, looping, with the current settings, and stored.
    tool.SetFader(practice::Fader::Speed, 0.5, true);
    tool.SetRange(12.0, 10.0, true);
    Expect(tool.Candidate() && Near(tool.Candidate()->first, 10.0), "a range with no loop picked is a candidate");
    const auto id = tool.AddLoop();
    const auto* region = h.Last("setPracticeToolLoopRegion");
    Expect(!id.empty() && tool.ActiveLoop() && tool.ActiveLoop()->name == "New Loop 1", "the new loop is picked");
    Expect(region && region->value("restart", false) && Near(region->value("startSec", 0.0), 10.0) &&
               Near(region->value("endSec", 0.0), 12.0),
           "it loops from its start");
    Expect(tool.ActiveLoop() && tool.ActiveLoop()->settings && Near(tool.ActiveLoop()->settings->speed, 0.5),
           "with the settings it was made under");
    const auto* stored = h.Last("setSetting");
    Expect(stored && stored->value("key", std::string()) == practice::kLoopsSetting, "and stored for the file");

    // Dragging its edges waits for a pause; letting go sends at once.
    const int regionsBefore = h.Count("setPracticeToolLoopRegion");
    tool.Tick(100.0);
    tool.SetRange(9.0, 12.0, false);
    Expect(h.Count("setPracticeToolLoopRegion") == regionsBefore, "a drag tick waits");
    tool.Tick(100.2);
    Expect(h.Count("setPracticeToolLoopRegion") == regionsBefore + 1, "and goes after a pause");

    // A fader moved while the loop is picked becomes the loop's.
    tool.SetFader(practice::Fader::Pitch, 3.0, true);
    Expect(tool.ActiveLoop() && Near(tool.ActiveLoop()->settings->pitchSemitones, 3.0), "the loop follows the faders");

    // A click outside the loop lets it go and marks the next one.
    tool.ClickAt(30.0);
    Expect(!tool.ActiveLoop() && tool.Candidate() && Near(tool.Candidate()->first, 30.0) &&
               Near(tool.Candidate()->second, 34.0),
           "a click outside the loop");
    Expect(h.Last("setPracticeToolLooping") && !h.Last("setPracticeToolLooping")->value("enabled", true),
           "looping off");
    Expect(tool.JumpBackMarker() && Near(*tool.JumpBackMarker(), 30.0), "the click is the jump-back point");

    // Picking it again puts its own settings back.
    tool.SetFader(practice::Fader::Pitch, 0.0, true);
    tool.SelectLoop(id);
    Expect(Near(tool.Faders().pitchSemitones, 3.0), "a loop brings its settings");
    tool.SelectLoop(id);
    Expect(!tool.ActiveLoop(), "picking it again lets it go");

    tool.DeleteLoop(id);
    Expect(tool.Loops().empty() && tool.DeletedLoop(), "deleted, with an undo");
    tool.UndoDelete();
    Expect(tool.Loops().size() == 1 && !tool.DeletedLoop(), "undone");

    // A project, saved and recalled onto another track: the track loads first.
    h.client.MutableState().activePresetId = "pr1";
    Expect(tool.SaveProject("Practice", true), "a project saves");
    auto projects = tool.Projects();
    Expect(projects.size() == 1 && projects[0].presetId == "pr1" && projects[0].loops.size() == 1,
           "with its loops and preset");
    Expect(tool.AddLink(projects[0].id, "Tab", "example.com/tab") == "added" && tool.Projects()[0].links.size() == 1,
           "a link");
    Expect(tool.AddLink(projects[0].id, "", "nope") == "invalid-url", "a bad link");
    Expect(tool.SaveProject("practice", false) && tool.Projects().size() == 1 && tool.Projects()[0].links.size() == 1,
           "saving under the same name overwrites, keeping the links");

    h.Receive(FileLoaded("C:\\Music\\Other.wav", 30.0));
    Expect(tool.Loops().empty() && Near(tool.Faders().pitchSemitones, 0.0), "another track starts afresh");
    Expect(tool.LoadProject(tool.Projects()[0].id), "the project recalls");
    const auto* load = h.Last("loadPracticeToolFile");
    Expect(load && load->value("path", std::string()) == "C:\\Music\\Song.wav", "its track loads first");
    h.Receive(FileLoaded("C:\\Music\\Song.wav", 60.0));
    Expect(tool.Loops().size() == 1 && Near(tool.Faders().speed, 0.5), "then its loops and faders apply");

    tool.DeleteProject(tool.Projects()[0].id);
    Expect(tool.Projects().empty(), "deleted");
}
} // namespace

int main()
{
    TestQueries();
    TestResults();
    TestFavoritesAndCache();
    TestFaders();
    TestLoopsAndProjects();
    TestSession();

    if (gFailures == 0)
    {
        std::cout << "JamPracticeToolTests: all passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
