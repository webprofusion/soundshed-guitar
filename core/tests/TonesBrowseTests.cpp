/**
 * TonesBrowseTests.cpp - The Tones page's Your Presets and Tone 3000 tabs as the native UIs work
 * them out (core/src/uiclient/TonesBrowse.h), held to the web UI's localPresetsModel.ts,
 * tone3000BrowserCache.ts and tone3000Browser.ts.
 */

#include "uiclient/TonesBrowse.h"

#include <iostream>
#include <string>

namespace uic = guitarfx::uiclient;
using namespace std::chrono_literals;

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

uic::PresetSummary Preset(const std::string& id, const std::string& name, std::vector<std::string> tags = {})
{
    uic::PresetSummary p;
    p.id = id;
    p.name = name;
    p.tags = std::move(tags);
    return p;
}

void TestLocalPresets()
{
    uic::ClientState state;
    state.presetRatings = {{"b", 5}, {"c", 3}};
    const std::vector<uic::PresetSummary> presets{Preset("a", "Tone 10"), Preset("b", "Zed"), Preset("c", "Tone 2"),
                                                  Preset("d", "alpha")};

    const auto byName = uic::SortLocalPresets(state, presets, uic::LocalPresetSort::Name, false);
    Expect(byName[0].id == "d" && byName[1].id == "c" && byName[2].id == "a" && byName[3].id == "b",
           "by name, ignoring case, numbers as numbers");

    const auto kept = uic::SortLocalPresets(state, presets, uic::LocalPresetSort::Name, true);
    Expect(kept[0].id == "a" && kept[3].id == "d", "Recents keep their order");

    const auto byRating = uic::SortLocalPresets(state, presets, uic::LocalPresetSort::Rating, false);
    Expect(byRating[0].id == "b" && byRating[1].id == "c" && byRating[2].id == "d" && byRating[3].id == "a",
           "top rated first, the unrated by name after");

    const auto tags = uic::CollectPresetTags({Preset("a", "A", {" metal ", "Clean"}), Preset("b", "B", {"clean", ""}),
                                              Preset("c", "C", {"metal"})});
    Expect(tags == std::vector<std::string>{"Clean", "clean", "metal"}, "tags trimmed, once each, in name order");

    Expect(uic::PresetInitials("01 David Gilmour - PULSE") == "DG", "a leading number is not a word");
    Expect(uic::PresetInitials("clean") == "CL", "one word: its first two letters");
    Expect(uic::PresetInitials("  ") == "?", "nothing to go on");
    Expect(uic::PresetInitials("Big (Muff)") == "BM", "punctuation is not part of a word");
    Expect(uic::PresetInitials("2024") == "20", "a name that is only a number");

    Expect(uic::DescribePresetCount(12, 12) == "12 presets", "all of them");
    Expect(uic::DescribePresetCount(1, 1) == "1 preset", "one");
    Expect(uic::DescribePresetCount(3, 12) == "3 of 12 presets", "narrowed");
}

void TestTone3000Helpers()
{
    Expect(uic::Tone3000Categories().size() == 6 && std::string(uic::Tone3000Categories()[0].id) == "pedal",
           "six categories, Pedals first");
    Expect(std::string(uic::Tone3000CategoryById("cab").platform) == "ir", "Cab IRs are IRs");
    Expect(std::string(uic::Tone3000CategoryById("nope").id) == "pedal", "an unknown category is the first");
    Expect(std::string(uic::Tone3000SortById("recent").api) == "newest", "Most Recent is newest");
    Expect(std::string(uic::Tone3000SortById("name").api) == "best-match", "Name A-Z is best-match");

    uic::tone3000::Tone nam, ir, unknown;
    nam.id = "1";
    nam.platform = "NAM";
    ir.id = "2";
    ir.platform = "ir";
    unknown.id = "3";
    const auto kept = uic::FilterTone3000ByPlatform({nam, ir, unknown}, "nam");
    Expect(kept.size() == 2 && kept[0].id == "1" && kept[1].id == "3", "another platform's tones are left out");

    Expect(uic::FormatCompactCount(999) == "999", "under a thousand");
    Expect(uic::FormatCompactCount(1200) == "1.2K", "thousands");
    Expect(uic::FormatCompactCount(1000) == "1K", "no .0");
    Expect(uic::FormatCompactCount(15600) == "15K", "whole above ten");
    Expect(uic::FormatCompactCount(2'500'000) == "2.5M", "millions");
    Expect(uic::FormatCompactCount(-1) == "0", "not known");

    Expect(uic::Tone3000Initials("Fender Twin") == "FT", "two words");
    Expect(uic::Tone3000Initials("JCM-800") == "JC", "one word, punctuation dropped");
    Expect(uic::Tone3000Initials("") == "T3", "no title");
}

void TestImportStatus()
{
    uic::ClientState state;
    uic::tone3000::Tone tone;
    tone.id = "42";
    tone.modelsCount = 2;
    Expect(uic::Tone3000ToneImportStatus(state, tone).kind == uic::Tone3000ImportStatus::Kind::None, "nothing");

    uic::LibraryResource a;
    a.id = "tone3000:7";
    a.metadata = {{"toneId", 42}, {"modelId", "7"}};
    state.resources["nam"].push_back(a);
    auto status = uic::Tone3000ToneImportStatus(state, tone);
    Expect(status.kind == uic::Tone3000ImportStatus::Kind::Partial && status.importedCount == 1,
           "one of two models, its tone id a number");

    uic::LibraryResource b;
    b.id = "tone3000:8";
    b.metadata = {{"groupId", "42"}, {"modelId", "8"}};
    state.resources["nam"].push_back(b);
    Expect(uic::Tone3000ToneImportStatus(state, tone).kind == uic::Tone3000ImportStatus::Kind::Imported,
           "every model, one by its group id");

    state.resources["nam"][1].fileMissing = true;
    Expect(uic::Tone3000ToneImportStatus(state, tone).kind == uic::Tone3000ImportStatus::Kind::Partial,
           "a missing file does not count");
}

void TestCache()
{
    uic::Tone3000ResultsCache cache;
    const auto t0 = std::chrono::system_clock::now();
    uic::Tone3000ResultsRequest request{false, "amp", "  Twin ", "popular", 1};
    uic::Tone3000CachedResults results;
    results.request = request;
    results.tones.resize(3);
    results.fetchedAt = t0;
    cache.Store(results);

    uic::Tone3000ResultsRequest same{false, "amp", "twin", "popular", 1};
    Expect(cache.Find(same, t0 + 59min).has_value(), "the same search, ignoring case and spaces, within the hour");
    Expect(!cache.Find({false, "amp", "twin", "recent", 1}, t0).has_value(), "another sort is another request");
    Expect(!cache.Find({false, "amp", "twin", "popular", 2}, t0).has_value(), "another page is another request");
    Expect(!cache.Find(same, t0 - 1min).has_value(), "a clock set back is stale");
    Expect(!cache.Find(same, t0 + 60min).has_value(), "an hour old is stale");
    Expect(!cache.Find(same, t0).has_value(), "a stale entry is dropped");

    uic::Tone3000CachedResults favourites;
    favourites.request = {true, "amp", "x", "popular", 1};
    favourites.fetchedAt = t0;
    cache.Store(favourites);
    Expect(cache.Find({true, "cab", "y", "recent", 1}, t0).has_value(), "favourites ignore all but the page");

    for (int page = 1; page <= 45; ++page)
    {
        uic::Tone3000CachedResults r;
        r.request = {false, "amp", "", "popular", page};
        r.fetchedAt = t0;
        cache.Store(r);
    }

    Expect(!cache.Find({false, "amp", "", "popular", 1}, t0).has_value(), "the oldest go past forty");
    Expect(cache.Find({false, "amp", "", "popular", 45}, t0).has_value(), "the newest stay");

    Expect(uic::DescribeTone3000ResultsAge(t0, t0 + 30s) == "just now", "under a minute");
    Expect(uic::DescribeTone3000ResultsAge(t0, t0 + 12min) == "12 min ago", "minutes");
    Expect(uic::DescribeTone3000ResultsAge(t0, t0 + 61min) == "over an hour ago", "an hour");
    Expect(uic::DescribeTone3000ResultsAge(t0, t0 + 3h) == "3 hours ago", "hours");
}
} // namespace

int main()
{
    TestLocalPresets();
    TestTone3000Helpers();
    TestImportStatus();
    TestCache();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "TonesBrowseTests passed" << std::endl;
    return 0;
}
