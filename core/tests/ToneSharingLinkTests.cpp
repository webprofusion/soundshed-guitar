/**
 * ToneSharingLinkTests.cpp - The tone sharing links and installed-item lookups the native UIs
 * share (core/src/uiclient/ToneSharing.h): reading a deep link or share link, finding which
 * shared item an installed preset came from, and the paths a favourite and rating are sent to.
 */

#include "uiclient/ToneSharing.h"

#include <iostream>
#include <string>

namespace tones = guitarfx::uiclient::tones;

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

bool Is(const std::optional<tones::LinkTarget>& target, tones::Tone::Kind kind, const std::string& id)
{
    return target && target->kind == kind && target->id == id;
}

void TestDeepLinks()
{
    using Kind = tones::Tone::Kind;
    Expect(Is(tones::ParseLink("soundshed://tone-sharing?itemId=itm_abc123"), Kind::Preset, "itm_abc123"),
           "an item deep link");
    Expect(Is(tones::ParseLink("soundshed://tone-sharing?packId=pck_9&utm=x"), Kind::Pack, "pck_9"),
           "a pack deep link, other parameters ignored");
    Expect(Is(tones::ParseLink("  \"soundshed://open?itemId=itm_1\"  "), Kind::Preset, "itm_1"),
           "the command line's quotes and spaces");
    Expect(Is(tones::ParseLink("itemId=itm_2"), Kind::Preset, "itm_2"),
           "the query alone, as the JUCE editor passes it");
    Expect(Is(tones::ParseLink("soundshed%3A%2F%2Fx%3FitemId%3Ditm_3"), Kind::Preset, "itm_3"), "percent-encoded");
    Expect(Is(tones::ParseLink("https://api.sepia.invalid/share/item/itm_4"), Kind::Preset, "itm_4"),
           "an item share link");
    Expect(Is(tones::ParseLink("https://api.sepia.invalid/share/pack/pck_5?ref=a"), Kind::Pack, "pck_5"),
           "a pack share link");
    Expect(!tones::ParseLink("").has_value(), "nothing");
    Expect(!tones::ParseLink("C:\\Users\\me\\preset.zip").has_value(), "a file path");
    Expect(!tones::ParseLink("soundshed://x?xitemId=itm_6").has_value(), "only a whole parameter name counts");
    Expect(!tones::ParseLink("soundshed://x?itemId=../../etc").has_value(), "an id with characters no id has");
    Expect(tones::ShareLink({tones::Tone::Kind::Pack, "pck_7"}).find("/share/pack/pck_7") != std::string::npos &&
               Is(tones::ParseLink(tones::ShareLink({tones::Tone::Kind::Pack, "pck_7"})), Kind::Pack, "pck_7"),
           "a share link reads back as what it shares");
}

void TestItemForPreset()
{
    const nlohmann::json settings = {
        {tones::kInstalledSettingKey,
         {{{"id", tones::ItemEntryId("itm_single")}, {"presetIds", {"user-1"}}},
          {{"id", tones::PackEntryId("pck_1")},
           {"packId", "pck_1"},
           {"presetIds", {"user-2", "user-3"}},
           {"presetItems", {{"user-2", "itm_a"}, {"user-3", "itm_b"}}}},
          {{"id", "zip-import:x"}, {"presetIds", {"user-4"}}}}},
    };
    Expect(tones::ItemIdForPreset(settings, "user-1") == "itm_single", "a single item's preset, by its entry id");
    Expect(tones::ItemIdForPreset(settings, "user-3") == "itm_b", "a pack's preset, by the item recorded for it");
    Expect(tones::ItemIdForPreset(settings, "user-4").empty(), "a preset imported from a file has none");
    Expect(tones::ItemIdForPreset(settings, "user-9").empty(), "nor does one never installed");
    Expect(tones::ItemIdForPreset(nlohmann::json::object(), "user-1").empty(), "nor with nothing installed");
}

void TestPaths()
{
    Expect(tones::ItemFavoritePath("itm_1") == "/items/itm_1/favorite", "the favourite's path");
    Expect(tones::ItemRatingPath("itm_1") == "/items/itm_1/rating", "the rating's path");
}

void TestPackDetailEntries()
{
    const nlohmann::json data = {
        {"pack", {{"id", "pck_1"}, {"title", "Pack"}}},
        {"items",
         {{{"itemId", "itm_b"},
           {"title", "B"},
           {"sortOrder", 2},
           {"description", " Second "},
           {"tags", {"lead", "preset"}}},
          {{"itemId", "itm_a"}, {"title", "A"}, {"sortOrder", 1}}}},
    };
    const auto detail = tones::ParsePackDetail(data);
    Expect(detail && detail->items.size() == 2 && detail->items[0].itemId == "itm_a", "a pack's presets in its order");

    if (detail && detail->items.size() == 2)
    {
        Expect(detail->items[1].description == "Second", "with their descriptions");
        Expect(detail->items[1].tags.size() == 1 && detail->items[1].tags[0] == "lead",
               "and their tags, without \"preset\"");
    }
}
} // namespace

int main()
{
    TestDeepLinks();
    TestItemForPreset();
    TestPaths();
    TestPackDetailEntries();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "ToneSharingLinkTests passed" << std::endl;
    return 0;
}
