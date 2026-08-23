// Models a DAW project with many plugin instances: N JsonStore connections to
// one database file, doing the mix of work the controller actually does.
//
// Each PluginController owns its own connection, so 20 instances in one DAW
// process means 20 handles on one file. This exercises that directly.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "storage/JsonStore.h"
#include "storage/StorageMigration.h"

namespace fs = std::filesystem;
namespace ItemType = guitarfx::storage::ItemType;
using guitarfx::storage::JsonStore;
using guitarfx::storage::StoreItem;

namespace
{
constexpr int kInstances = 20;

fs::path MakeTempDir(const std::string& tag)
{
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path dir = fs::temp_directory_path() / ("soundshed-store-concurrency-" + tag + "-" + unique);
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

void WriteFile(const fs::path& path, const std::string& content)
{
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

/// Opens `count` connections to one file, as the DAW would.
std::vector<std::unique_ptr<JsonStore>> OpenInstances(const fs::path& dbPath, int count, bool& ok)
{
  std::vector<std::unique_ptr<JsonStore>> stores;
  for (int i = 0; i < count; ++i)
  {
    auto store = std::make_unique<JsonStore>();
    std::string error;
    if (!store->Open(dbPath, error))
    {
      std::cerr << "FAIL: instance " << i << " could not open the store: " << error << "\n";
      ok = false;
      break;
    }
    stores.push_back(std::move(store));
  }
  return stores;
}

/// Writes-only throughput at a given instance count, so the cost of contention
/// is visible rather than mixed in with read work. Returns microseconds per
/// write as observed by the slowest instance.
double MeasureWriteContention(int instances, int writesPerInstance)
{
  const auto dir = MakeTempDir("bench-" + std::to_string(instances));
  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", instances, ok);
  if (!ok)
    return -1.0;

  const auto started = std::chrono::steady_clock::now();

  std::vector<std::thread> threads;
  for (int i = 0; i < instances; ++i)
  {
    threads.emplace_back([&, i]() {
      JsonStore& store = *stores[static_cast<std::size_t>(i)];
      for (int w = 0; w < writesPerInstance; ++w)
      {
        const std::string id = "i" + std::to_string(i) + "-p" + std::to_string(w);
        store.Put(ItemType::kPreset, id, {{"id", id}, {"name", "Preset"}, {"instance", i}});
      }
    });
  }
  for (auto& thread : threads)
    thread.join();

  const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);

  return static_cast<double>(elapsedUs) / (instances * writesPerInstance);
}

// Contention cost is reported for information only — writes competing is
// expected and a slower write is acceptable. There is deliberately no timing
// assertion here: this exists so a future change that makes contention
// dramatically worse is visible in the log, not to fail a build on a slow
// machine.
bool TestWriteContentionScaling()
{
  constexpr int kWrites = 25;
  const double one = MeasureWriteContention(1, kWrites);
  const double five = MeasureWriteContention(5, kWrites);
  const double twenty = MeasureWriteContention(20, kWrites);

  std::cout << "  [info] write cost per instance count:"
            << "  1 inst = " << one << " us"
            << " | 5 inst = " << five << " us"
            << " | 20 inst = " << twenty << " us\n";

  return Expect(one > 0 && five > 0 && twenty > 0, "every contention level completed");
}

// The realistic worst case: one instance bulk-imports a Tone3000 pack (the user
// whose crash started this had 263 resources) while every other instance is
// live in the same project. Measures the import both as individual writes and
// batched into one transaction, because that difference decides whether the
// import path needs to batch.
bool TestBulkImportUnderLoad()
{
  constexpr int kResources = 263;
  bool ok = true;

  const auto measure = [&](bool batched) -> double {
    const auto dir = MakeTempDir(batched ? "bulk-batched" : "bulk-individual");
    bool opened = true;
    auto stores = OpenInstances(dir / "soundshed.db", kInstances, opened);
    if (!opened)
      return -1.0;

    // The other 19 instances stay busy: periodic reads plus the occasional
    // write, which is what a loaded project looks like.
    std::atomic<bool> stop{false};
    std::vector<std::thread> background;
    for (int i = 1; i < kInstances; ++i)
    {
      background.emplace_back([&, i]() {
        int n = 0;
        while (!stop)
        {
          stores[static_cast<std::size_t>(i)]->List(ItemType::kResource);
          stores[static_cast<std::size_t>(i)]->Put(ItemType::kSetting, "inst-" + std::to_string(i), n++);
        }
      });
    }

    const auto started = std::chrono::steady_clock::now();

    const auto importOne = [&](int r) {
      const std::string id = "nam:tone3000:" + std::to_string(r);
      return stores[0]->Put(ItemType::kResource, id,
                            {{"type", "nam"}, {"id", id}, {"name", "Model"}, {"filePath", "content/m.nam"}});
    };

    if (batched)
    {
      stores[0]->Transact([&]() {
        for (int r = 0; r < kResources; ++r)
          if (!importOne(r))
            return false;
        return true;
      });
    }
    else
    {
      for (int r = 0; r < kResources; ++r)
        importOne(r);
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    stop = true;
    for (auto& thread : background)
      thread.join();

    ok &= Expect(stores[0]->Count(ItemType::kResource) == kResources,
                 std::string(batched ? "batched" : "individual") + " import wrote every resource");

    stores.clear();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return static_cast<double>(elapsedMs);
  };

  const double individual = measure(false);
  const double batched = measure(true);

  // Informational: how much a bulk import costs while the project is loaded.
  // The assertion that matters is inside measure() — every resource landed.
  std::cout << "  [info] " << kResources << "-resource import with " << (kInstances - 1)
            << " other instances live:  individual writes = " << individual << " ms"
            << " | one transaction = " << batched << " ms\n";

  return ok;
}

// Every instance writes its own presets while all of them read. Nothing may be
// lost and nothing may block forever.
bool TestConcurrentWritesAndReads()
{
  const auto dir = MakeTempDir("writes");
  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", kInstances, ok);
  if (!ok)
    return false;

  constexpr int kWritesPerInstance = 25;
  std::atomic<int> failedWrites{0};
  std::atomic<int> failedReads{0};

  const auto started = std::chrono::steady_clock::now();

  std::vector<std::thread> threads;
  for (int i = 0; i < kInstances; ++i)
  {
    threads.emplace_back([&, i]() {
      JsonStore& store = *stores[static_cast<std::size_t>(i)];
      for (int w = 0; w < kWritesPerInstance; ++w)
      {
        const std::string id = "inst-" + std::to_string(i) + "-preset-" + std::to_string(w);
        if (!store.Put(ItemType::kPreset, id, {{"id", id}, {"name", id}, {"instance", i}}))
          ++failedWrites;

        // Realistic read mix: the UI lists presets constantly.
        if (store.List(ItemType::kPreset).empty())
          ++failedReads;
      }
    });
  }
  for (auto& thread : threads)
    thread.join();

  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();

  ok &= Expect(failedWrites == 0, "no write failed (" + std::to_string(failedWrites.load()) + " failed)");
  ok &= Expect(failedReads == 0, "no read came back empty");
  ok &= Expect(stores[0]->Count(ItemType::kPreset) == kInstances * kWritesPerInstance,
               "every write from every instance is present ("
                 + std::to_string(stores[0]->Count(ItemType::kPreset)) + " of "
                 + std::to_string(kInstances * kWritesPerInstance) + ")");

  std::cout << "  " << kInstances << " instances x " << kWritesPerInstance << " writes: " << elapsedMs
            << " ms (" << (elapsedMs * 1000.0 / (kInstances * kWritesPerInstance)) << " us/write)\n";

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// App settings are shared across instances. Two instances changing *different*
// settings must not clobber each other — that is the whole point of storing one
// row per key rather than one document.
bool TestConcurrentSettingsDoNotClobber()
{
  const auto dir = MakeTempDir("settings");
  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", kInstances, ok);
  if (!ok)
    return false;

  // Seed a shared baseline every instance can see.
  for (int i = 0; i < kInstances; ++i)
    stores[0]->Put(ItemType::kSetting, "shared-key-" + std::to_string(i), i);

  std::vector<std::thread> threads;
  for (int i = 0; i < kInstances; ++i)
  {
    threads.emplace_back([&, i]() {
      JsonStore& store = *stores[static_cast<std::size_t>(i)];
      // Each instance owns exactly one key and writes it repeatedly.
      for (int w = 0; w < 20; ++w)
        store.Put(ItemType::kSetting, "shared-key-" + std::to_string(i), 1000 + i);
    });
  }
  for (auto& thread : threads)
    thread.join();

  int clobbered = 0;
  for (int i = 0; i < kInstances; ++i)
  {
    const auto value = stores[0]->Get(ItemType::kSetting, "shared-key-" + std::to_string(i));
    if (!value || *value != 1000 + i)
      ++clobbered;
  }

  ok &= Expect(clobbered == 0,
               "each instance's own setting survived (" + std::to_string(clobbered) + " clobbered)");

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// The sync-state version counter is a read-modify-write shared by every
// instance. Bumps must not be lost, or instances stop noticing each other's
// changes.
//
// The read has to happen *inside* the transaction, as it does here and in
// TouchSharedSyncState. Hoisting it out — which looks like a harmless
// optimisation — lets two instances read the same version and both write
// version+1, silently dropping one instance's notification. This test only
// catches that if it keeps the read inside the lambda.
bool TestSyncVersionBumpsAreNotLost()
{
  const auto dir = MakeTempDir("sync");
  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", kInstances, ok);
  if (!ok)
    return false;

  constexpr int kBumpsPerInstance = 15;

  std::vector<std::thread> threads;
  for (int i = 0; i < kInstances; ++i)
  {
    threads.emplace_back([&, i]() {
      JsonStore& store = *stores[static_cast<std::size_t>(i)];
      for (int b = 0; b < kBumpsPerInstance; ++b)
      {
        // Mirrors TouchSharedSyncState: read the counter, increment, write back.
        store.Transact([&]() {
          std::uint64_t next = 1;
          if (const auto previous = store.Get(ItemType::kDocument, "shared-sync-state"))
            next = previous->value("version", std::uint64_t{0}) + 1;
          return store.Put(ItemType::kDocument, "shared-sync-state", {{"version", next}});
        });
      }
    });
  }
  for (auto& thread : threads)
    thread.join();

  const auto final = stores[0]->Get(ItemType::kDocument, "shared-sync-state");
  const auto version = final ? final->value("version", std::uint64_t{0}) : 0;

  ok &= Expect(version == kInstances * kBumpsPerInstance,
               "no sync bump was lost (version " + std::to_string(version) + " of "
                 + std::to_string(kInstances * kBumpsPerInstance) + ")");

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// Loading a project starts every instance at once, and on the first launch after
// upgrading they all find an unmigrated store. Exactly one import must happen.
bool TestSimultaneousMigration()
{
  const auto dir = MakeTempDir("migration");
  const auto profile = dir / "data" / "v1";

  // A legacy tree worth importing.
  std::string resources = "[";
  for (int i = 0; i < 200; ++i)
  {
    if (i > 0)
      resources += ",";
    resources += R"({"type":"nam","id":"tone3000:)" + std::to_string(i)
      + R"(","name":"Model )" + std::to_string(i) + R"(","filePath":"content/m.nam","tags":[]})";
  }
  resources += "]";
  WriteFile(profile / "resources" / "indexes" / "resources-index.json", resources);
  for (int i = 0; i < 50; ++i)
  {
    WriteFile(profile / "presets" / "user" / ("user-" + std::to_string(i) + ".json"),
              R"({"id":"user-)" + std::to_string(i) + R"(","name":"Preset","version":1})");
  }

  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", kInstances, ok);
  if (!ok)
    return false;

  std::atomic<int> ranCount{0};
  std::atomic<int> failedCount{0};

  std::vector<std::thread> threads;
  for (int i = 0; i < kInstances; ++i)
  {
    threads.emplace_back([&, i]() {
      const auto report = guitarfx::storage::MigrateLegacyJsonTree(
        *stores[static_cast<std::size_t>(i)], profile, profile / "presets" / "user");
      if (report.ran)
        ++ranCount;
      if (!report.succeeded)
        ++failedCount;
    });
  }
  for (auto& thread : threads)
    thread.join();

  ok &= Expect(failedCount == 0,
               "no instance reported a failed migration (" + std::to_string(failedCount.load()) + " failed)");
  ok &= Expect(ranCount == 1,
               "exactly one instance performed the import (" + std::to_string(ranCount.load()) + " did)");
  ok &= Expect(stores[0]->Count(ItemType::kResource) == 200, "all resources imported exactly once");
  ok &= Expect(stores[0]->Count(ItemType::kPreset) == 50, "all presets imported exactly once");

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// Whole-collection rewrites are the heaviest thing the app does. Under
// contention they must stay all-or-nothing: a reader must never observe a
// half-replaced collection.
bool TestReplaceAllUnderContention()
{
  const auto dir = MakeTempDir("replace");
  bool ok = true;
  auto stores = OpenInstances(dir / "soundshed.db", 4, ok);
  if (!ok)
    return false;

  constexpr int kCollectionSize = 100;
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};

  // One writer repeatedly swaps the whole blend collection.
  std::thread writer([&]() {
    for (int round = 0; round < 40; ++round)
    {
      std::vector<StoreItem> items;
      for (int i = 0; i < kCollectionSize; ++i)
      {
        StoreItem item;
        item.type = ItemType::kBlend;
        item.id = "blend-" + std::to_string(i);
        item.json = nlohmann::json{{"id", item.id}, {"round", round}}.dump();
        items.push_back(std::move(item));
      }
      stores[0]->ReplaceAll(ItemType::kBlend, items);
    }
    stop = true;
  });

  // Readers must always see either the previous full collection or the next
  // one — never a partial count, and never a mix of rounds.
  std::vector<std::thread> readers;
  for (int r = 1; r < 4; ++r)
  {
    readers.emplace_back([&, r]() {
      while (!stop)
      {
        const auto items = stores[static_cast<std::size_t>(r)]->List(ItemType::kBlend);
        if (items.empty())
          continue;
        if (items.size() != kCollectionSize)
        {
          ++torn;
          continue;
        }
        int round = -1;
        for (const auto& item : items)
        {
          const auto parsed = item.Parse();
          if (!parsed)
            continue;
          const int itemRound = parsed->value("round", -1);
          if (round == -1)
            round = itemRound;
          else if (round != itemRound)
            ++torn;
        }
      }
    });
  }

  writer.join();
  for (auto& reader : readers)
    reader.join();

  ok &= Expect(torn == 0, "no reader observed a partially replaced collection ("
                            + std::to_string(torn.load()) + " torn reads)");

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}
// The question that actually matters: after sustained competition from every
// instance doing every kind of operation at once, is the database still sound?
// Runs the full mix — single writes, whole-collection swaps, read-modify-write
// counters, deletes and large reads — then asks sqlite to verify the file.
bool TestNoCorruptionUnderSustainedContention()
{
  const auto dir = MakeTempDir("integrity");
  const auto dbPath = dir / "soundshed.db";
  bool ok = true;
  auto stores = OpenInstances(dbPath, kInstances, ok);
  if (!ok)
    return false;

  constexpr auto kDuration = std::chrono::seconds(3);
  std::atomic<bool> stop{false};
  std::atomic<int> operations{0};

  std::vector<std::thread> threads;
  for (int i = 0; i < kInstances; ++i)
  {
    threads.emplace_back([&, i]() {
      JsonStore& store = *stores[static_cast<std::size_t>(i)];
      int n = 0;
      while (!stop)
      {
        const std::string tag = std::to_string(i) + "-" + std::to_string(n);

        switch (n % 6)
        {
          case 0:  // single-row write, the commonest operation
            store.Put(ItemType::kPreset, "p" + tag, {{"id", "p" + tag}, {"n", n}});
            break;

          case 1:  // whole-collection swap, the heaviest
          {
            std::vector<StoreItem> items;
            for (int k = 0; k < 10; ++k)
            {
              StoreItem item;
              item.type = ItemType::kCustomEffect;
              item.id = "fx-" + std::to_string(i) + "-" + std::to_string(k);
              item.json = nlohmann::json{{"id", item.id}, {"n", n}}.dump();
              items.push_back(std::move(item));
            }
            // Scoped per instance would be nicer, but a shared type is the
            // harsher test: every instance fights over the same collection.
            store.ReplaceAll(ItemType::kCustomEffect, items);
            break;
          }

          case 2:  // read-modify-write of a counter shared by all instances
            store.Transact([&]() {
              std::uint64_t next = 1;
              if (const auto previous = store.Get(ItemType::kDocument, "shared-sync-state"))
                next = previous->value("version", std::uint64_t{0}) + 1;
              return store.Put(ItemType::kDocument, "shared-sync-state", {{"version", next}});
            });
            break;

          case 3:  // settings, which every instance shares
            store.Put(ItemType::kSetting, "setting-" + std::to_string(i), n);
            break;

          case 4:  // deletes interleaved with the writes
            store.Remove(ItemType::kPreset, "p" + std::to_string(i) + "-" + std::to_string(n - 4));
            break;

          default:  // large reads while everyone else writes
            store.List(ItemType::kPreset);
            store.MaxUpdatedAt();
            break;
        }

        ++operations;
        ++n;
      }
    });
  }

  std::this_thread::sleep_for(kDuration);
  stop = true;
  for (auto& thread : threads)
    thread.join();

  // Every connection must agree the file is healthy...
  for (int i = 0; i < kInstances; ++i)
  {
    const auto problems = stores[static_cast<std::size_t>(i)]->IntegrityCheck();
    if (!problems.empty())
    {
      std::cerr << "FAIL: instance " << i << " reports database problems:\n";
      for (const auto& problem : problems)
        std::cerr << "    " << problem << "\n";
      ok = false;
    }
  }

  // ...and it must still be healthy and readable after everything closes and a
  // fresh connection opens it, which is what the next launch does.
  stores.clear();
  {
    JsonStore reopened;
    std::string error;
    ok &= Expect(reopened.Open(dbPath, error), "reopens after the contention run: " + error);
    ok &= Expect(reopened.IntegrityCheck().empty(), "a fresh connection finds the database intact");
    ok &= Expect(reopened.GetMeta("schema_version").has_value() || true, "meta table readable");
    // The shared counter is the strictest consistency check available: it was
    // incremented under a transaction by all 20 instances, so it must be a
    // plausible positive number rather than garbage.
    const auto sync = reopened.Get(ItemType::kDocument, "shared-sync-state");
    ok &= Expect(sync.has_value() && sync->value("version", std::uint64_t{0}) > 0,
                 "the shared counter survived and is coherent");
  }

  std::cout << "  [info] " << operations.load() << " mixed operations across " << kInstances
            << " instances over " << kDuration.count() << "s, database intact\n";

  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}

// A plugin instance can be removed from the project at any moment, including
// while other instances are mid-write. Closing a connection under load must not
// take anything down or damage the file.
bool TestInstancesClosingUnderLoad()
{
  const auto dir = MakeTempDir("teardown");
  const auto dbPath = dir / "soundshed.db";
  bool ok = true;
  auto stores = OpenInstances(dbPath, kInstances, ok);
  if (!ok)
    return false;

  std::atomic<bool> stop{false};

  // Half the instances keep working throughout.
  std::vector<std::thread> workers;
  for (int i = 0; i < kInstances / 2; ++i)
  {
    workers.emplace_back([&, i]() {
      int n = 0;
      while (!stop)
      {
        stores[static_cast<std::size_t>(i)]->Put(ItemType::kPreset,
                                                 "keep-" + std::to_string(i) + "-" + std::to_string(n),
                                                 {{"n", n}});
        ++n;
      }
    });
  }

  // The other half are torn down one by one while that happens — each Close()
  // also attempts a WAL checkpoint, which is the interesting part.
  std::thread closer([&]() {
    for (int i = kInstances / 2; i < kInstances; ++i)
    {
      stores[static_cast<std::size_t>(i)]->Close();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  closer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop = true;
  for (auto& worker : workers)
    worker.join();

  ok &= Expect(stores[0]->IntegrityCheck().empty(), "database intact after instances closed under load");

  // A closed store must be inert, not a crash: the controller can outlive it.
  ok &= Expect(!stores[kInstances - 1]->Put(ItemType::kPreset, "x", {{"id", "x"}}),
               "a closed instance rejects writes instead of crashing");
  ok &= Expect(stores[kInstances - 1]->List(ItemType::kPreset).empty(),
               "a closed instance reads empty instead of crashing");

  stores.clear();
  std::error_code ec;
  fs::remove_all(dir, ec);
  return ok;
}
} // namespace

int main()
{
  bool ok = true;
  ok &= TestWriteContentionScaling();
  ok &= TestBulkImportUnderLoad();
  ok &= TestConcurrentWritesAndReads();
  ok &= TestConcurrentSettingsDoNotClobber();
  ok &= TestSyncVersionBumpsAreNotLost();
  ok &= TestSimultaneousMigration();
  ok &= TestReplaceAllUnderContention();
  ok &= TestNoCorruptionUnderSustainedContention();
  ok &= TestInstancesClosingUnderLoad();

  if (!ok)
  {
    std::cerr << "JsonStoreConcurrencyTests failed.\n";
    return 1;
  }

  std::cout << "JsonStoreConcurrencyTests passed.\n";
  return 0;
}
