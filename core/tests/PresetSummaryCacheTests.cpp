/**
 * @file PresetSummaryCacheTests.cpp
 * @brief The preset list's summaries match a full load, and only changed presets are read again.
 *
 * The preset list is sent on every editor open, preset delete and shared-sync catch-up. It used to
 * deserialise every preset in full for five fields. PresetSummaryCache reads just those fields,
 * and on later calls only the presets written since, as each row's updated_at tells it. Checked
 * here against PresetStorage::LoadAllFromStore, the old way:
 *  - the same presets, in the same order, with the same fields;
 *  - a preset added, renamed (twice in one millisecond), deleted, or broken by another
 *    connection to the same store is seen at once;
 *  - and on a 650-preset library a call with nothing changed costs a small part of a full load.
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "presets/PresetStorage.h"
#include "storage/JsonStore.h"

namespace fs = std::filesystem;
using namespace guitarfx;
namespace ItemType = storage::ItemType;
using storage::JsonStore;

namespace
{
int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

/// A preset about the size of a real one: a chain of `nodes` effects with a dozen parameters each.
Preset MakePreset(const std::string& id, int nodes)
{
    Preset preset;
    preset.id = id;
    preset.name = "Preset " + id;
    preset.category = id.size() % 2 ? "Clean" : "Lead";
    preset.description = "A preset called " + id;
    preset.tags = {"tag-" + id, "rock"};
    GraphNode in;
    in.id = "in";
    in.type = kNodeTypeInput;
    preset.graph.nodes.push_back(in);
    std::string previous = in.id;

    for (int n = 0; n < nodes; ++n)
    {
        GraphNode node;
        node.id = "fx" + std::to_string(n);
        node.type = "passthrough";

        for (int p = 0; p < 12; ++p)
        {
            node.params["param" + std::to_string(p)] = 0.1 * p + n;
        }

        preset.graph.nodes.push_back(node);
        preset.graph.edges.push_back({previous, node.id, 0, 0, 1.0});
        previous = node.id;
    }

    GraphNode out;
    out.id = "out";
    out.type = kNodeTypeOutput;
    preset.graph.nodes.push_back(out);
    preset.graph.edges.push_back({previous, out.id, 0, 0, 1.0});
    return preset;
}

/// The summaries LoadAllFromStore gives, the way the preset list used to build them.
std::vector<PresetListEntry> FullLoad(const JsonStore& store)
{
    std::vector<PresetListEntry> summaries;

    for (const auto& preset : PresetStorage::LoadAllFromStore(store))
    {
        summaries.push_back({preset.id, preset.name, preset.category, preset.description, preset.tags});
    }

    return summaries;
}

bool Same(const std::vector<PresetListEntry>& a, const std::vector<PresetListEntry>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].category != b[i].category ||
            a[i].description != b[i].description || a[i].tags != b[i].tags)
        {
            return false;
        }
    }

    return true;
}

void TestMatchesFullLoad(const fs::path& db)
{
    JsonStore store;
    JsonStore other; // another instance's connection to the same store
    std::string error;

    if (!store.Open(db, error) || !other.Open(db, error))
    {
        Check(false, "the test can open the store: " + error);
        return;
    }

    for (int i = 0; i < 20; ++i)
    {
        (void)PresetStorage::SaveToStore(store, MakePreset("p" + std::to_string(i), 6));
    }

    PresetSummaryCache cache;
    Check(Same(cache.Load(store), FullLoad(store)), "a first look matches a full load");
    Check(Same(cache.Load(store), FullLoad(store)), "and so does a look with nothing changed");

    (void)PresetStorage::SaveToStore(other, MakePreset("p5-new", 6));
    other.Remove(ItemType::kPreset, "p7");
    Check(Same(cache.Load(store), FullLoad(store)), "a preset added and one deleted elsewhere are seen");

    // Renamed twice within a millisecond, with a look in between: the second is still seen.
    auto renamed = MakePreset("p3", 6);
    renamed.name = "First rename";
    (void)PresetStorage::SaveToStore(other, renamed);
    (void)cache.Load(store);
    renamed.name = "Second rename";
    renamed.tags = {"changed"};
    (void)PresetStorage::SaveToStore(other, renamed);
    const auto afterRenames = cache.Load(store);
    Check(Same(afterRenames, FullLoad(store)), "a preset renamed twice in a row is seen with its latest name");

    // Rows that do not read as presets are left out, as a full load leaves them out.
    other.PutRaw(ItemType::kPreset, "broken-json", "{ not json");
    other.PutRaw(ItemType::kPreset, "bad-name", R"({"id":"bad-name","name":42,"graph":{}})");
    other.PutRaw(ItemType::kPreset, "bad-tag", R"({"id":"bad-tag","name":"Bad tag","tags":["ok",7]})");
    const auto withBroken = cache.Load(store);
    Check(Same(withBroken, FullLoad(store)), "rows that do not read as presets are left out alike");

    other.PutRaw(ItemType::kPreset, "bad-name", R"({"id":"bad-name","name":"Fixed","graph":{}})");
    Check(Same(cache.Load(store), FullLoad(store)), "and one put right is listed");
}

void TestCost(const fs::path& db)
{
    JsonStore store;
    std::string error;

    if (!store.Open(db, error))
    {
        Check(false, "the test can open the store: " + error);
        return;
    }

    for (int i = 0; i < 650; ++i)
    {
        (void)PresetStorage::SaveToStore(store, MakePreset("lib" + std::to_string(i), 24));
    }

    const auto time = [](auto&& fn) {
        double best = 1.0e9;

        for (int round = 0; round < 3; ++round)
        {
            const auto start = std::chrono::steady_clock::now();
            fn();
            best = std::min(
                best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        return best;
    };

    const double full = time([&] { (void)FullLoad(store); });
    const double first = time([&] {
        PresetSummaryCache fresh;
        (void)fresh.Load(store);
    });
    PresetSummaryCache cache;
    (void)cache.Load(store);
    const double again = time([&] { (void)cache.Load(store); });
    (void)PresetStorage::SaveToStore(store, MakePreset("lib7", 24));
    const double oneChanged = time([&] { (void)cache.Load(store); });
    std::cout << "650 presets: full load " << full << " ms, first look " << first << " ms, nothing changed " << again
              << " ms, one changed " << oneChanged << " ms\n";
#ifdef NDEBUG
    // A Debug build spends nearly all of either in tokenising, which both do in full.
    Check(first < full * 0.7, "a first look costs clearly less than a full load");
#endif
    Check(again < full * 0.1, "and a look with nothing changed a small part of it");
}
} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "guitarfx-preset-summary-cache-tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    TestMatchesFullLoad(root / "match.db");
    TestCost(root / "cost.db");

    fs::remove_all(root, ec);
    std::cout << (gFailures == 0 ? "PresetSummaryCacheTests PASSED\n" : "PresetSummaryCacheTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
