// Tests for the SQLite-backed document store that replaces the JSON file tree.
//
// The properties that matter here are the ones the file-based store could not
// offer: an interrupted write never damages what was already there, a whole
// collection swaps atomically, and several handles on one database file (the
// standalone app plus plugin instances) can read and write concurrently.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "storage/JsonStore.h"

namespace fs = std::filesystem;
namespace ItemType = guitarfx::storage::ItemType;
using guitarfx::storage::JsonStore;
using guitarfx::storage::StoreItem;

namespace
{
fs::path MakeTempDir()
{
  const auto uniqueId = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path dir = fs::temp_directory_path() / ("soundshed-jsonstore-test-" + uniqueId);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

bool Expect(bool condition, const std::string& what)
{
  if (!condition)
    std::cerr << "FAIL: " << what << "\n";
  return condition;
}

nlohmann::json MakeResource(const std::string& id, const std::string& name)
{
  return {{"type", "nam"}, {"id", id}, {"name", name}, {"category", "amp"}, {"tags", nlohmann::json::array()}};
}

bool TestRoundTripAndListing()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  ok &= Expect(store.Put(ItemType::kResource, "nam:tone3000:1", MakeResource("tone3000:1", "First")), "put one");
  ok &= Expect(store.Put(ItemType::kResource, "nam:tone3000:2", MakeResource("tone3000:2", "Second")), "put two");
  ok &= Expect(store.Put(ItemType::kPreset, "user-a", {{"id", "user-a"}, {"name", "A"}}), "put preset");

  const auto fetched = store.Get(ItemType::kResource, "nam:tone3000:1");
  ok &= Expect(fetched.has_value() && (*fetched)["name"] == "First", "round-trips the document");
  ok &= Expect(store.Has(ItemType::kResource, "nam:tone3000:2"), "Has finds an existing item");
  ok &= Expect(!store.Has(ItemType::kResource, "nam:missing"), "Has rejects a missing item");

  // Types are independent namespaces.
  ok &= Expect(store.Count(ItemType::kResource) == 2, "counts only its own type");
  ok &= Expect(store.Count(ItemType::kPreset) == 1, "preset type counted separately");
  ok &= Expect(store.List(ItemType::kResource).size() == 2, "lists only its own type");

  // Upsert replaces rather than duplicating.
  ok &= Expect(store.Put(ItemType::kResource, "nam:tone3000:1", MakeResource("tone3000:1", "Renamed")), "upsert");
  ok &= Expect(store.Count(ItemType::kResource) == 2, "upsert does not add a row");
  const auto renamed = store.Get(ItemType::kResource, "nam:tone3000:1");
  ok &= Expect(renamed.has_value() && (*renamed)["name"] == "Renamed", "upsert replaces the value");

  ok &= Expect(store.Remove(ItemType::kResource, "nam:tone3000:1"), "remove");
  ok &= Expect(!store.Has(ItemType::kResource, "nam:tone3000:1"), "removed item is gone");
  ok &= Expect(store.Count(ItemType::kResource) == 1, "remove drops exactly one row");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// Data written by one handle must be visible to the next one that opens the
// same file — the basic durability guarantee the JSON tree failed to give.
bool TestPersistsAcrossReopen()
{
  const auto dir = MakeTempDir();
  const auto dbPath = dir / "soundshed.db";
  std::string error;

  {
    JsonStore store;
    bool opened = store.Open(dbPath, error);
    if (!opened)
    {
      std::cerr << "FAIL: open for write: " << error << "\n";
      return false;
    }
    store.Put(ItemType::kSetting, "theme", "classic");
    store.SetMeta("schema_version", "1");
    store.Close();
  }

  JsonStore reopened;
  bool ok = Expect(reopened.Open(dbPath, error), "reopen: " + error);
  const auto theme = reopened.Get(ItemType::kSetting, "theme");
  ok &= Expect(theme.has_value() && *theme == "classic", "setting survived the reopen");
  ok &= Expect(reopened.GetMeta("schema_version").value_or("") == "1", "meta survived the reopen");

  reopened.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// A failed bulk operation must leave the previous contents exactly as they
// were, rather than half-replaced.
bool TestFailedTransactionRollsBack()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  store.Put(ItemType::kBlend, "keeper", {{"id", "keeper"}});

  const bool committed = store.Transact([&]() {
    store.Put(ItemType::kBlend, "doomed-1", {{"id", "doomed-1"}});
    store.Put(ItemType::kBlend, "doomed-2", {{"id", "doomed-2"}});
    return false;  // the work decided to abort
  });

  ok &= Expect(!committed, "aborted transaction reports failure");
  ok &= Expect(store.Count(ItemType::kBlend) == 1, "aborted writes were rolled back");
  ok &= Expect(store.Has(ItemType::kBlend, "keeper"), "pre-existing row untouched");

  // The same, but via an exception escaping the transaction body.
  const bool threw = store.Transact([&]() -> bool {
    store.Put(ItemType::kBlend, "doomed-3", {{"id", "doomed-3"}});
    throw std::runtime_error("boom");
  });
  ok &= Expect(!threw, "throwing transaction reports failure");
  ok &= Expect(store.Count(ItemType::kBlend) == 1, "throwing transaction rolled back");

  // And a successful one commits.
  const bool good = store.Transact([&]() {
    store.Put(ItemType::kBlend, "added", {{"id", "added"}});
    return true;
  });
  ok &= Expect(good && store.Count(ItemType::kBlend) == 2, "successful transaction commits");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// A nested transaction runs against a SAVEPOINT, so a step that reports failure
// leaves nothing of itself behind while the enclosing transaction stays usable.
//
// The migration is the caller that depends on this: it wraps the whole import in
// one transaction and runs each source in a nested one, then tells the user
// "nothing from this source was imported". Without the savepoint that sentence is
// a lie — the outer commit carries whatever the failed source managed to write —
// and the user gets a silently partial library stamped as fully migrated.
bool TestNestedTransactionRollsBackOnlyItself()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  bool innerReportedFailure = false;
  bool outerSawInnerResult = false;

  const bool committed = store.Transact([&]() {
    store.Put(ItemType::kBlend, "outer-before", {{"id", "outer-before"}});

    // A nested step that writes two rows and then fails.
    const bool inner = store.Transact([&]() {
      store.Put(ItemType::kBlend, "inner-1", {{"id", "inner-1"}});
      store.Put(ItemType::kBlend, "inner-2", {{"id", "inner-2"}});
      return false;
    });
    innerReportedFailure = !inner;
    outerSawInnerResult = true;

    // The enclosing transaction must still be usable after the inner rollback.
    store.Put(ItemType::kBlend, "outer-after", {{"id", "outer-after"}});
    return true;
  });

  ok &= Expect(committed, "outer transaction still commits after a nested failure");
  ok &= Expect(innerReportedFailure, "nested transaction reports its own failure");
  ok &= Expect(outerSawInnerResult, "control returned to the outer body");
  ok &= Expect(store.Has(ItemType::kBlend, "outer-before"), "work before the nested step survives");
  ok &= Expect(store.Has(ItemType::kBlend, "outer-after"), "work after the nested step survives");
  ok &= Expect(!store.Has(ItemType::kBlend, "inner-1"), "first row of the failed step was undone");
  ok &= Expect(!store.Has(ItemType::kBlend, "inner-2"), "second row of the failed step was undone");
  ok &= Expect(store.Count(ItemType::kBlend) == 2, "exactly the outer rows remain");

  // A nested step that succeeds commits with the outer one, and a nested step
  // that throws is rolled back the same way an explicit failure is.
  const bool second = store.Transact([&]() {
    const bool goodInner = store.Transact([&]() {
      store.Put(ItemType::kBlend, "inner-ok", {{"id", "inner-ok"}});
      return true;
    });

    const bool threwInner = store.Transact([&]() -> bool {
      store.Put(ItemType::kBlend, "inner-threw", {{"id", "inner-threw"}});
      throw std::runtime_error("nested boom");
    });

    return goodInner && !threwInner;
  });

  ok &= Expect(second, "outer transaction survives a throwing nested step");
  ok &= Expect(store.Has(ItemType::kBlend, "inner-ok"), "successful nested step committed");
  ok &= Expect(!store.Has(ItemType::kBlend, "inner-threw"), "throwing nested step was undone");

  // And an outer abort still discards everything, nested successes included.
  const bool aborted = store.Transact([&]() {
    (void)store.Transact([&]() {
      store.Put(ItemType::kBlend, "doomed-by-outer", {{"id", "doomed-by-outer"}});
      return true;
    });
    return false;
  });

  ok &= Expect(!aborted, "outer abort reports failure");
  ok &= Expect(!store.Has(ItemType::kBlend, "doomed-by-outer"),
               "a committed nested step is still discarded when the outer aborts");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// ReplaceAll is what a whole-collection rewrite becomes. It must be all-or-nothing
// and must not disturb other types.
bool TestReplaceAllIsScopedAndAtomic()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  store.Put(ItemType::kResource, "nam:old-1", MakeResource("old-1", "Old One"));
  store.Put(ItemType::kResource, "nam:old-2", MakeResource("old-2", "Old Two"));
  store.Put(ItemType::kPreset, "user-a", {{"id", "user-a"}});

  std::vector<StoreItem> replacement;
  replacement.push_back({std::string(ItemType::kResource), "nam:new-1", MakeResource("new-1", "New One").dump(), 0});
  replacement.push_back({std::string(ItemType::kResource), "nam:new-2", MakeResource("new-2", "New Two").dump(), 0});
  replacement.push_back({std::string(ItemType::kResource), "nam:new-3", MakeResource("new-3", "New Three").dump(), 0});

  ok &= Expect(store.ReplaceAll(ItemType::kResource, replacement), "replace-all succeeds");
  ok &= Expect(store.Count(ItemType::kResource) == 3, "collection fully replaced");
  ok &= Expect(!store.Has(ItemType::kResource, "nam:old-1"), "old entries gone");
  ok &= Expect(store.Has(ItemType::kResource, "nam:new-3"), "new entries present");
  ok &= Expect(store.Count(ItemType::kPreset) == 1, "other types untouched");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// updated_at is the cross-instance change detector that replaces polling a
// shared-sync-state file.
bool TestMaxUpdatedAtTracksWrites()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  ok &= Expect(store.MaxUpdatedAt() == 0, "empty store has no watermark");

  store.Put(ItemType::kPreset, "user-a", {{"id", "user-a"}});
  const auto afterFirst = store.MaxUpdatedAt();
  ok &= Expect(afterFirst > 0, "watermark advances on write");

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  store.Put(ItemType::kResource, "nam:x", MakeResource("x", "X"));

  ok &= Expect(store.MaxUpdatedAt() >= afterFirst, "global watermark covers all types");
  ok &= Expect(store.MaxUpdatedAt(ItemType::kPreset) == afterFirst, "per-type watermark is scoped");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// The standalone app and plugin instances share one database file. Two handles
// writing different types concurrently must both succeed.
bool TestConcurrentHandles()
{
  const auto dir = MakeTempDir();
  const auto dbPath = dir / "soundshed.db";
  std::string error;

  JsonStore first;
  JsonStore second;
  bool ok = Expect(first.Open(dbPath, error), "open first: " + error);
  ok &= Expect(second.Open(dbPath, error), "open second: " + error);

  constexpr int kWrites = 50;
  std::atomic<int> firstOk{0};
  std::atomic<int> secondOk{0};

  std::thread a([&]() {
    for (int i = 0; i < kWrites; ++i)
      if (first.Put(ItemType::kPreset, "a-" + std::to_string(i), {{"id", "a-" + std::to_string(i)}}))
        ++firstOk;
  });
  std::thread b([&]() {
    for (int i = 0; i < kWrites; ++i)
      if (second.Put(ItemType::kResource, "b-" + std::to_string(i), MakeResource("b", "B")))
        ++secondOk;
  });
  a.join();
  b.join();

  ok &= Expect(firstOk == kWrites, "every write on handle one succeeded");
  ok &= Expect(secondOk == kWrites, "every write on handle two succeeded");
  ok &= Expect(first.Count(ItemType::kResource) == kWrites, "handle one sees handle two's writes");
  ok &= Expect(second.Count(ItemType::kPreset) == kWrites, "handle two sees handle one's writes");

  first.Close();
  second.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// A store that failed to open must degrade to no-ops rather than crashing, so a
// permissions problem does not take the app down.
bool TestClosedStoreIsSafe()
{
  JsonStore store;
  bool ok = Expect(!store.IsOpen(), "starts closed");
  ok &= Expect(!store.Put(ItemType::kPreset, "x", {{"id", "x"}}), "put on a closed store fails cleanly");
  ok &= Expect(!store.Get(ItemType::kPreset, "x").has_value(), "get on a closed store returns nothing");
  ok &= Expect(!store.Has(ItemType::kPreset, "x"), "has on a closed store is false");
  ok &= Expect(store.List(ItemType::kPreset).empty(), "list on a closed store is empty");
  ok &= Expect(store.Count(ItemType::kPreset) == 0, "count on a closed store is zero");
  ok &= Expect(!store.Transact([]() { return true; }), "transact on a closed store fails cleanly");
  store.Close();  // must not crash
  return ok;
}

// Garbage in the json column must not throw into a caller.
bool TestUnparseableDocumentIsSurvivable()
{
  const auto dir = MakeTempDir();
  JsonStore store;
  std::string error;
  bool ok = Expect(store.Open(dir / "soundshed.db", error), "open: " + error);

  ok &= Expect(store.PutRaw(ItemType::kDocument, "broken", "{not json"), "raw put accepts arbitrary text");
  ok &= Expect(!store.Get(ItemType::kDocument, "broken").has_value(), "get reports failure instead of throwing");
  ok &= Expect(store.GetRaw(ItemType::kDocument, "broken").value_or("") == "{not json", "raw text is still retrievable");

  store.Close();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// A database can arrive already damaged — copied mid-write by an external
// migration (the macOS sandbox-container copy is a bulk directory copy), or off
// bad hardware. Opening it and writing anyway would compound the damage, so the
// open must fail and leave the file alone for recovery.
bool TestDamagedDatabaseIsRefused()
{
  const auto dir = MakeTempDir();
  const auto dbPath = dir / "soundshed.db";
  std::string error;

  {
    JsonStore store;
    if (!store.Open(dbPath, error))
    {
      std::cerr << "FAIL: could not create the database to damage: " << error << "\n";
      return false;
    }
    for (int i = 0; i < 200; ++i)
      store.Put(ItemType::kPreset, "p" + std::to_string(i), {{"id", i}, {"pad", std::string(200, 'x')}});
    store.Close();
  }

  // Corrupt the middle of the file, past the header, the way a torn copy would.
  const auto sizeBefore = fs::file_size(dbPath);
  {
    std::fstream file(dbPath, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(sizeBefore / 2));
    const std::string garbage(1024, '\xEE');
    file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }

  JsonStore damaged;
  bool ok = Expect(!damaged.Open(dbPath, error), "a damaged database is refused");
  ok &= Expect(error.find("integrity") != std::string::npos,
               "the refusal says why: \"" + error + "\"");
  ok &= Expect(!damaged.IsOpen(), "the store stays closed");
  ok &= Expect(!damaged.Put(ItemType::kPreset, "new", {{"id", "new"}}),
               "nothing can be written into the damaged file");
  ok &= Expect(fs::exists(dbPath) && fs::file_size(dbPath) == sizeBefore,
               "the damaged file is left in place for recovery");

  // Damage has to be distinguishable from every other open failure. The caller
  // recovers from it by moving the file aside and reopening — which would
  // destroy a perfectly good database if a permissions error or a full disk
  // reported the same way.
  std::string damagedError;
  ok &= Expect(damaged.OpenChecked(dbPath, damagedError) == JsonStore::OpenStatus::Damaged,
               "a damaged file reports Damaged, not a generic failure");

  // A path whose "directory" is really a regular file cannot be opened, but is
  // not damaged — nothing there should be moved aside.
  {
    const auto blocker = dir / "blocker";
    std::ofstream(blocker) << "not a directory";

    JsonStore unopenable;
    std::string unopenableError;
    ok &= Expect(unopenable.OpenChecked(blocker / "child.db", unopenableError) == JsonStore::OpenStatus::Failed,
                 "an unopenable path reports Failed, not Damaged");
  }

  // And the recovery the caller performs actually works: with the damaged file
  // moved aside, a fresh store opens at the same path.
  const auto quarantined = dbPath.parent_path() / "soundshed.db.damaged-test";
  std::error_code renameEc;
  fs::rename(dbPath, quarantined, renameEc);
  ok &= Expect(!renameEc, "the damaged file can be moved aside");

  JsonStore rebuilt;
  std::string rebuiltError;
  ok &= Expect(rebuilt.Open(dbPath, rebuiltError), "a fresh store opens in its place: " + rebuiltError);
  ok &= Expect(rebuilt.Count(ItemType::kPreset) == 0, "the rebuilt store starts empty");
  ok &= Expect(!rebuilt.GetMeta("schema_version").has_value(),
               "and unstamped, so the legacy import runs again");
  rebuilt.Close();

  ok &= Expect(fs::exists(quarantined), "the damaged file is kept, not deleted");

  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}
} // namespace

int main()
{
  bool ok = true;
  ok &= TestRoundTripAndListing();
  ok &= TestPersistsAcrossReopen();
  ok &= TestFailedTransactionRollsBack();
  ok &= TestNestedTransactionRollsBackOnlyItself();
  ok &= TestReplaceAllIsScopedAndAtomic();
  ok &= TestMaxUpdatedAtTracksWrites();
  ok &= TestConcurrentHandles();
  ok &= TestClosedStoreIsSafe();
  ok &= TestUnparseableDocumentIsSurvivable();
  ok &= TestDamagedDatabaseIsRefused();

  if (!ok)
  {
    std::cerr << "JsonStoreTests failed.\n";
    return 1;
  }

  std::cout << "JsonStoreTests passed.\n";
  return 0;
}
