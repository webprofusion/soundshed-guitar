// Tests for the one-time import of the legacy JSON tree into the SQLite store.
//
// The rules being pinned down: every legacy source lands in the right place,
// the legacy files are never touched, the import runs exactly once, and a
// damaged source costs only its own contents rather than the whole migration.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "storage/JsonStore.h"
#include "resources/ResourceLibrary.h"
#include "storage/StorageMigration.h"

namespace fs = std::filesystem;
namespace ItemType = guitarfx::storage::ItemType;
using guitarfx::storage::JsonStore;
using guitarfx::storage::MigrateLegacyJsonTree;

namespace
{
bool Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
    }
    return condition;
}

void WriteFile(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Builds a settings tree shaped like a real profile.
struct LegacyProfile
{
    fs::path root;
    fs::path presetDir;

    explicit LegacyProfile(const fs::path& base)
    {
        root = base / "data" / "v1";
        presetDir = root / "presets" / "user";

        WriteFile(root / "resources" / "indexes" / "resources-index.json", R"([
      {"type":"nam","id":"tone3000:52730","name":"Peavey","category":"amp","filePath":"content/a.nam","tags":[]},
      {"type":"ir","id":"tone3000:99","name":"Cab","category":"ir","filePath":"content/b.wav","tags":[]},
      {"type":"nam","id":"","name":"No id — must be skipped"}
    ])");

        WriteFile(root / "custom-effects" / "indexes" / "custom-effects-index.json",
                  R"([{"id":"airy-widen","name":"Airy Widen"}])");

        WriteFile(root / "blends" / "library.json", R"([{"id":"blend-1","models":[]},{"id":"blend-2","models":[]}])");

        WriteFile(presetDir / "user-aaa.json", R"({"id":"user-aaa","name":"Preset A","version":1})");
        WriteFile(presetDir / "user-bbb.json", R"({"id":"user-bbb","name":"Preset B","version":1})");

        WriteFile(root / "composite-presets" / "stack-1.composite.json", R"({"id":"stack-1","name":"Stack"})");

        WriteFile(root / "layouts" / "content" / "my-layout" / "layout.json",
                  R"({"id":"my-layout","name":"My Layout"})");

        WriteFile(root / "settings" / "app.json",
                  R"({"theme":"classic","metronome.bpm":120.0,"audio.processing.multiThreaded":true})");

        WriteFile(root / "settings" / "ui" / "setlists.json",
                  R"({"setlists":[{"id":"s1"}],"activeSetlistId":"s1","bankSize":8,"cursorIndex":2})");
        WriteFile(root / "presets" / "preset-folders.json",
                  R"({"folders":[{"id":"f1","name":"Rock"}],"activeFolderId":"f1"})");
        WriteFile(root / "presets" / "factory-archive-state.json", R"({"archives":{"Preset-Pack":{"hash":"abc"}}})");
    }
};

bool TestImportsEverySource()
{
    const auto base =
        fs::temp_directory_path() /
        ("soundshed-migration-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyProfile profile(base);

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);

    const auto report = MigrateLegacyJsonTree(store, profile.root, profile.presetDir);
    ok &= Expect(report.ran, "migration ran");
    ok &= Expect(report.succeeded, "migration succeeded");

    // Collections become one row per item.
    ok &= Expect(store.Count(ItemType::kResource) == 2, "two resources imported (the id-less one skipped)");
    ok &= Expect(store.Has(ItemType::kResource, "nam:tone3000:52730"), "resource keyed by type:id");
    ok &= Expect(store.Has(ItemType::kResource, "ir:tone3000:99"), "ir resource keyed by type:id");
    ok &= Expect(store.Count(ItemType::kCustomEffect) == 1, "custom effect imported");
    ok &= Expect(store.Count(ItemType::kBlend) == 2, "blends imported");
    ok &= Expect(store.Count(ItemType::kPreset) == 2, "presets imported from the directory");
    ok &= Expect(store.Has(ItemType::kPreset, "user-aaa"), "preset keyed by its own id");
    ok &= Expect(store.Count(ItemType::kCompositePreset) == 1, "composite preset imported");
    ok &= Expect(store.Has(ItemType::kCompositePreset, "stack-1"), "composite keyed without the .composite suffix");
    ok &= Expect(store.Count(ItemType::kLayout) == 1, "layout imported from its subdirectory");

    // Settings become one row per key.
    ok &= Expect(store.Count(ItemType::kSetting) == 3, "each app.json key is its own row");
    const auto theme = store.Get(ItemType::kSetting, "theme");
    ok &= Expect(theme.has_value() && *theme == "classic", "setting value preserved");

    // Envelope documents keep their shape, including non-collection state.
    const auto setlists = store.Get(ItemType::kDocument, "setlists");
    ok &=
        Expect(setlists.has_value() && (*setlists)["cursorIndex"] == 2, "setlist envelope keeps its surrounding state");
    const auto folders = store.Get(ItemType::kDocument, "preset-folders");
    ok &= Expect(folders.has_value() && (*folders)["activeFolderId"] == "f1", "folder envelope preserved");
    ok &= Expect(store.Get(ItemType::kDocument, "factory-archive-state").has_value(), "factory archive state imported");

    // The resource that survived must carry its full document, not a summary.
    const auto resource = store.Get(ItemType::kResource, "nam:tone3000:52730");
    ok &= Expect(resource.has_value() && (*resource)["filePath"] == "content/a.nam",
                 "resource document imported verbatim");

    store.Close();
    std::error_code ec;
    fs::remove_all(base, ec);
    return ok;
}

// Nothing in the legacy tree may be modified: a user who downgrades keeps their
// pre-upgrade library.
bool TestLeavesLegacyFilesUntouched()
{
    const auto base =
        fs::temp_directory_path() / ("soundshed-migration-untouched-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyProfile profile(base);

    const auto indexPath = profile.root / "resources" / "indexes" / "resources-index.json";
    const auto presetPath = profile.presetDir / "user-aaa.json";
    const auto indexBefore = ReadFile(indexPath);
    const auto presetBefore = ReadFile(presetPath);

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);
    MigrateLegacyJsonTree(store, profile.root, profile.presetDir);

    ok &= Expect(fs::exists(indexPath), "legacy index still exists");
    ok &= Expect(fs::exists(presetPath), "legacy preset still exists");
    ok &= Expect(ReadFile(indexPath) == indexBefore, "legacy index byte-for-byte unchanged");
    ok &= Expect(ReadFile(presetPath) == presetBefore, "legacy preset byte-for-byte unchanged");

    store.Close();
    std::error_code ec;
    fs::remove_all(base, ec);
    return ok;
}

// The import is guarded by the version stamp, so later launches must not
// re-import and clobber changes made since.
bool TestRunsOnlyOnce()
{
    const auto base =
        fs::temp_directory_path() /
        ("soundshed-migration-once-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyProfile profile(base);

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);

    const auto first = MigrateLegacyJsonTree(store, profile.root, profile.presetDir);
    ok &= Expect(first.ran && first.succeeded, "first pass runs");

    // Simulate the user renaming a preset after upgrading.
    store.Put(ItemType::kPreset, "user-aaa", {{"id", "user-aaa"}, {"name", "Renamed After Upgrade"}});
    store.Remove(ItemType::kBlend, "blend-1");

    const auto second = MigrateLegacyJsonTree(store, profile.root, profile.presetDir);
    ok &= Expect(!second.ran, "second pass is a no-op");
    ok &= Expect(second.succeeded, "second pass reports success");

    const auto preset = store.Get(ItemType::kPreset, "user-aaa");
    ok &= Expect(preset.has_value() && (*preset)["name"] == "Renamed After Upgrade",
                 "post-migration edit not overwritten by a re-import");
    ok &= Expect(store.Count(ItemType::kBlend) == 1, "post-migration deletion not resurrected");

    store.Close();
    std::error_code ec;
    fs::remove_all(base, ec);
    return ok;
}

// A fresh install has no legacy tree at all. That must be a clean, successful
// no-content migration rather than an error.
bool TestFreshInstall()
{
    const auto base =
        fs::temp_directory_path() /
        ("soundshed-migration-fresh-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(base, ec);

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);

    const auto report = MigrateLegacyJsonTree(store, base / "data" / "v1", base / "data" / "v1" / "presets" / "user");
    ok &= Expect(report.ran, "migration attempts once");
    ok &= Expect(report.succeeded, "no legacy tree is not a failure");
    ok &= Expect(report.itemsImported == 0, "nothing imported");
    ok &= Expect(store.GetMeta(guitarfx::storage::kMetaSchemaVersion).has_value(), "version stamped anyway");

    store.Close();
    fs::remove_all(base, ec);
    return ok;
}

// One corrupt source must cost only its own contents. This is the exact shape of
// the failure that started all this: a truncated resources-index.json.
bool TestCorruptSourceIsIsolated()
{
    const auto base =
        fs::temp_directory_path() /
        ("soundshed-migration-corrupt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyProfile profile(base);

    // Truncate the resource index the way an unclean shutdown would.
    WriteFile(profile.root / "resources" / "indexes" / "resources-index.json",
              R"([{"type":"nam","id":"tone3000:52730","na)");

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);

    const auto report = MigrateLegacyJsonTree(store, profile.root, profile.presetDir);
    ok &= Expect(report.succeeded, "migration still completes");
    ok &= Expect(store.Count(ItemType::kResource) == 0, "nothing salvaged from the damaged index");
    ok &= Expect(store.Count(ItemType::kPreset) == 2, "presets imported despite the damaged index");
    ok &= Expect(store.Count(ItemType::kBlend) == 2, "blends imported despite the damaged index");
    ok &= Expect(store.Count(ItemType::kSetting) == 3, "settings imported despite the damaged index");

    store.Close();
    std::error_code ec;
    fs::remove_all(base, ec);
    return ok;
}

// Migrated resource rows keep whatever relative-path convention the legacy index
// used, and there were two: older builds wrote paths relative to the index's own
// folder (resources/indexes/), newer ones relative to resources/. The file loader
// tried both bases. The store loader has to as well — resolving only against
// resources/ silently unresolves every path an old profile stored, and the user
// upgrades into a library where the amp models have gone missing.
bool TestMigratedResourcePathsResolveForBothLegacyConventions()
{
    const auto base =
        fs::temp_directory_path() /
        ("soundshed-migration-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    const auto root = base / "data" / "v1";
    const auto resourcesRoot = root / "resources";
    const auto indexDir = resourcesRoot / "indexes";

    // The same relative path, "content/x.nam", under each of the two bases.
    WriteFile(indexDir / "content" / "old-style.nam", "old");
    WriteFile(resourcesRoot / "content" / "new-style.nam", "new");

    WriteFile(indexDir / "resources-index.json", R"([
    {"type":"nam","id":"old-convention","name":"Old","category":"amp",
     "filePath":"content/old-style.nam","tags":[]},
    {"type":"nam","id":"new-convention","name":"New","category":"amp",
     "filePath":"content/new-style.nam","tags":[]}
  ])");

    JsonStore store;
    std::string error;
    bool ok = Expect(store.Open(base / "soundshed.db", error), "open: " + error);

    const auto report = MigrateLegacyJsonTree(store, root, root / "presets" / "user");
    ok &= Expect(report.succeeded, "migration completes");
    ok &= Expect(store.Count(ItemType::kResource) == 2, "both resources imported");

    guitarfx::ResourceLibrary library;
    library.LoadFromStore(store, resourcesRoot);

    const auto oldStyle = library.LookupResource("nam", "old-convention");
    const auto newStyle = library.LookupResource("nam", "new-convention");

    ok &= Expect(oldStyle.has_value(), "index-relative resource loaded");
    ok &= Expect(newStyle.has_value(), "resources-relative resource loaded");

    if (oldStyle)
    {
        std::error_code existsEc;
        ok &= Expect(fs::exists(oldStyle->filePath, existsEc),
                     "index-relative path resolves to a real file, got: " + oldStyle->filePath.string());
    }
    if (newStyle)
    {
        std::error_code existsEc;
        ok &= Expect(fs::exists(newStyle->filePath, existsEc),
                     "resources-relative path resolves to a real file, got: " + newStyle->filePath.string());
    }

    store.Close();
    std::error_code ec;
    fs::remove_all(base, ec);
    return ok;
}
} // namespace

int main()
{
    bool ok = true;
    ok &= TestImportsEverySource();
    ok &= TestLeavesLegacyFilesUntouched();
    ok &= TestRunsOnlyOnce();
    ok &= TestFreshInstall();
    ok &= TestCorruptSourceIsIsolated();
    ok &= TestMigratedResourcePathsResolveForBothLegacyConventions();

    if (!ok)
    {
        std::cerr << "StorageMigrationTests failed.\n";
        return 1;
    }

    std::cout << "StorageMigrationTests passed.\n";
    return 0;
}
