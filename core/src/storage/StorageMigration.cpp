#include "storage/StorageMigration.h"

#include "storage/JsonStore.h"
#include "util/PathEncoding.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>

namespace guitarfx::storage
{
namespace
{
std::optional<nlohmann::json> ReadJson(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return std::nullopt;
    }

    try
    {
        std::ifstream input(path);
        if (!input.is_open())
        {
            return std::nullopt;
        }
        return nlohmann::json::parse(input);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[StorageMigration] Could not parse " << path.string() << ": " << exception.what() << std::endl;
        return std::nullopt;
    }
}

std::int64_t NowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Best effort — a profile that cannot take this file is not a reason to
/// undo a successful import.
void WriteLegacyTreeMarker(const std::filesystem::path& settingsDirectory, const std::filesystem::path& dbPath,
                           std::int64_t itemsImported)
{
    try
    {
        std::ofstream marker(settingsDirectory / "MIGRATED-TO-soundshed-db.txt");
        if (!marker.is_open())
        {
            return;
        }

        marker << "The JSON files in this folder were imported into a database on " << NowMillis() << " (unix ms).\n\n"
               << "  database: " << util::PathToUtf8(dbPath) << "\n"
               << "  items:    " << itemsImported << "\n\n"
               << "They are kept only so an older build of Soundshed Guitar can still read them.\n"
               << "The running app no longer reads or writes them, so they are frozen at the\n"
               << "state the library was in at the moment of the upgrade. Editing them has no\n"
               << "effect; deleting them loses the ability to downgrade, nothing else.\n";
    }
    catch (const std::exception&)
    {
    }
}

struct Importer
{
    JsonStore& store;
    MigrationReport& report;

    /// Set when a *write* fails, as opposed to a source being missing or
    /// unreadable. A source we cannot read is skipped and noted — the user
    /// loses that one file and the rest of the import still stands. A write
    /// we cannot perform means the database is not accepting data, so
    /// stamping the schema version would freeze a half-imported library in
    /// place forever. That aborts the whole migration instead, leaving the
    /// stamp unset so the next launch retries from the untouched legacy tree.
    bool hardFailure = false;

    void Note(const std::string& source, std::int64_t count)
    {
        report.itemsImported += count;
        report.notes.push_back(source + ": " + std::to_string(count));
    }

    void Fail(const std::string& source, const std::string& why)
    {
        report.failures.push_back(source + ": " + why);
        std::cerr << "[StorageMigration] " << source << " — " << why << std::endl;
    }

    void FailHard(const std::string& source, const std::string& why)
    {
        hardFailure = true;
        Fail(source, why);
    }

    /// Whole file becomes one row. Used for the small envelope documents
    /// (setlists, folders, ratings, sync state) whose shape carries state
    /// beyond the collection itself and which are rewritten wholesale anyway.
    void ImportDocument(const std::filesystem::path& path, const char* documentId)
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        const auto parsed = ReadJson(path);
        if (!parsed)
        {
            return;
        }

        if (store.Put(ItemType::kDocument, documentId, *parsed))
        {
            Note(std::string("document/") + documentId, 1);
        }
        else
        {
            FailHard(std::string("document/") + documentId, "write failed");
        }
    }

    /// A JSON array of objects becomes one row per element, keyed by the value
    /// of `idField`. Elements without a usable id are skipped, not dropped
    /// silently — they are reported.
    void ImportArray(const std::filesystem::path& path, const char* itemType, const char* idField,
                     const std::string& sourceLabel)
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        const auto parsed = ReadJson(path);
        if (!parsed)
        {
            return;
        }

        if (!parsed->is_array())
        {
            Fail(sourceLabel, "expected a JSON array");
            return;
        }

        std::int64_t imported = 0;
        std::int64_t skipped = 0;
        const bool ok = store.Transact([&]() {
            for (const auto& element : *parsed)
            {
                if (!element.is_object())
                {
                    ++skipped;
                    continue;
                }
                const std::string id = element.value(idField, std::string{});
                if (id.empty())
                {
                    ++skipped;
                    continue;
                }
                if (!store.Put(itemType, id, element))
                {
                    return false;
                }
                ++imported;
            }
            return true;
        });

        if (!ok)
        {
            FailHard(sourceLabel, "write failed; nothing from this source was imported");
            return;
        }

        Note(sourceLabel, imported);
        if (skipped > 0)
        {
            Fail(sourceLabel, std::to_string(skipped) + " entries had no usable id and were skipped");
        }
    }

    /// The resource index keys rows by "<resourceType>:<resourceId>", matching
    /// ResourceLibrary's own in-memory key, so lookups need no translation.
    void ImportResourceIndex(const std::filesystem::path& path)
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        const auto parsed = ReadJson(path);
        if (!parsed)
        {
            return;
        }

        if (!parsed->is_array())
        {
            Fail("resources-index", "expected a JSON array");
            return;
        }

        std::int64_t imported = 0;
        std::int64_t skipped = 0;
        const bool ok = store.Transact([&]() {
            for (const auto& element : *parsed)
            {
                if (!element.is_object())
                {
                    ++skipped;
                    continue;
                }
                const std::string type = element.value("type", std::string{});
                const std::string id = element.value("id", std::string{});
                if (type.empty() || id.empty())
                {
                    ++skipped;
                    continue;
                }
                if (!store.Put(ItemType::kResource, type + ":" + id, element))
                {
                    return false;
                }
                ++imported;
            }
            return true;
        });

        if (!ok)
        {
            FailHard("resources-index", "write failed; nothing from this source was imported");
            return;
        }

        Note("resources-index", imported);
        if (skipped > 0)
        {
            Fail("resources-index", std::to_string(skipped) + " entries had no type/id and were skipped");
        }
    }

    /// Every *.json in a directory becomes one row, keyed by the document's
    /// own `id` field (falling back to the filename stem).
    void ImportDirectory(const std::filesystem::path& directory, const char* itemType, const std::string& sourceLabel,
                         const std::string& requiredStemSuffix = {})
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        std::error_code isDirEc;
        if (!std::filesystem::is_directory(directory, isDirEc))
        {
            return;
        }

        std::int64_t imported = 0;
        std::int64_t unreadable = 0;

        // Its own error_code, not the one is_directory() used: sharing it made a
        // failure to enumerate look identical to an empty directory, so an
        // unreadable presets folder would import zero presets and report success.
        std::error_code iterateEc;
        const bool ok = store.Transact([&]() {
            for (const auto& entry : std::filesystem::directory_iterator(directory, iterateEc))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".json")
                {
                    continue;
                }

                std::string stem = entry.path().stem().string();
                if (!requiredStemSuffix.empty())
                {
                    if (stem.size() <= requiredStemSuffix.size() ||
                        stem.compare(stem.size() - requiredStemSuffix.size(), requiredStemSuffix.size(),
                                     requiredStemSuffix) != 0)
                    {
                        continue;
                    }
                    stem = stem.substr(0, stem.size() - requiredStemSuffix.size());
                }

                const auto parsed = ReadJson(entry.path());
                if (!parsed || !parsed->is_object())
                {
                    ++unreadable;
                    continue;
                }

                const std::string id = parsed->value("id", stem);
                if (id.empty())
                {
                    ++unreadable;
                    continue;
                }

                if (!store.Put(itemType, id, *parsed))
                {
                    return false;
                }
                ++imported;
            }
            return true;
        });

        if (!ok)
        {
            FailHard(sourceLabel, "write failed; nothing from this source was imported");
            return;
        }

        Note(sourceLabel, imported);
        if (unreadable > 0)
        {
            Fail(sourceLabel, std::to_string(unreadable) + " files could not be read and were skipped");
        }
        if (iterateEc)
        {
            Fail(sourceLabel, "the folder could not be fully enumerated: " + iterateEc.message());
        }
    }

    /// Layouts live one-per-subdirectory as layouts/content/<id>/layout.json.
    /// Sidecar images stay on disk; only the document moves.
    void ImportLayouts(const std::filesystem::path& layoutsContentDir)
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        std::error_code isDirEc;
        if (!std::filesystem::is_directory(layoutsContentDir, isDirEc))
        {
            return;
        }

        std::int64_t imported = 0;
        std::error_code iterateEc;
        const bool ok = store.Transact([&]() {
            for (const auto& entry : std::filesystem::directory_iterator(layoutsContentDir, iterateEc))
            {
                if (!entry.is_directory())
                {
                    continue;
                }

                const auto parsed = ReadJson(entry.path() / "layout.json");
                if (!parsed || !parsed->is_object())
                {
                    continue;
                }

                const std::string id = parsed->value("id", entry.path().filename().string());
                if (id.empty())
                {
                    continue;
                }

                if (!store.Put(ItemType::kLayout, id, *parsed))
                {
                    return false;
                }
                ++imported;
            }
            return true;
        });

        if (!ok)
        {
            FailHard("layouts", "write failed; nothing from this source was imported");
            return;
        }
        Note("layouts", imported);
        if (iterateEc)
        {
            Fail("layouts", "the folder could not be fully enumerated: " + iterateEc.message());
        }
    }

    /// app.json becomes one row per top-level key, so two instances writing
    /// different settings no longer contend on a single document.
    void ImportAppSettings(const std::filesystem::path& path)
    {
        // Once a write has failed the database is not accepting data; every
        // later step would just add more rows that the abort throws away.
        if (hardFailure)
        {
            return;
        }

        const auto parsed = ReadJson(path);
        if (!parsed)
        {
            return;
        }

        if (!parsed->is_object())
        {
            Fail("app-settings", "expected a JSON object");
            return;
        }

        std::int64_t imported = 0;
        const bool ok = store.Transact([&]() {
            for (const auto& [key, value] : parsed->items())
            {
                if (key.empty())
                {
                    continue;
                }
                if (!store.Put(ItemType::kSetting, key, value))
                {
                    return false;
                }
                ++imported;
            }
            return true;
        });

        if (!ok)
        {
            FailHard("app-settings", "write failed; nothing from this source was imported");
            return;
        }
        Note("app-settings", imported);
    }
};

/// The import sequence. Split out so MigrateLegacyJsonTree can run it inside
/// the single transaction that makes the migration exactly-once.
void RunImport(Importer& importer, const std::filesystem::path& settingsDirectory,
               const std::filesystem::path& userPresetDirectory)
{
    // ── Collections: one row per item ────────────────────────────
    importer.ImportResourceIndex(settingsDirectory / "resources" / "indexes" / "resources-index.json");
    importer.ImportArray(settingsDirectory / "custom-effects" / "indexes" / "custom-effects-index.json",
                         ItemType::kCustomEffect, "id", "custom-effects");
    importer.ImportArray(settingsDirectory / "blends" / "library.json", ItemType::kBlend, "id", "blends");
    importer.ImportDirectory(userPresetDirectory, ItemType::kPreset, "presets");
    importer.ImportDirectory(settingsDirectory / "composite-presets", ItemType::kCompositePreset, "composite-presets",
                             ".composite");
    importer.ImportLayouts(settingsDirectory / "layouts" / "content");

    // ── Settings: one row per key ────────────────────────────────
    const auto legacyAppSettingsPath = settingsDirectory / "settings" / "app.json";
    importer.ImportAppSettings(legacyAppSettingsPath);

    // The riff library folder is user-configurable, and the migration runs
    // before app settings are loaded, so read the location straight out of
    // the legacy settings file rather than assuming the default.
    std::filesystem::path riffLibraryDir = settingsDirectory / "riff-library";
    if (const auto legacySettings = ReadJson(legacyAppSettingsPath); legacySettings && legacySettings->is_object())
    {
        const auto configured = legacySettings->value("riffLibrary.path", std::string{});
        if (!configured.empty())
        {
            riffLibraryDir = util::PathFromUtf8(configured);
        }
    }

    // ── Envelope documents: one row each ─────────────────────────
    // These carry collection state alongside the collection itself
    // (activeFolderId, bankSize, cursorIndex...) and are small enough that
    // splitting them would cost more in handler churn than it buys.
    const auto uiDir = settingsDirectory / "settings" / "ui";
    importer.ImportDocument(uiDir / "automation.json", "automation");
    importer.ImportDocument(uiDir / "setlists.json", "setlists");
    importer.ImportDocument(uiDir / "preset-favorites.json", "preset-favorites");
    importer.ImportDocument(uiDir / "effect-presets.json", "effect-presets");
    importer.ImportDocument(uiDir / "effect-layouts.json", "effect-layouts");
    importer.ImportDocument(settingsDirectory / "presets" / "preset-folders.json", "preset-folders");
    importer.ImportDocument(settingsDirectory / "presets" / "preset-ratings.json", "preset-ratings");
    importer.ImportDocument(settingsDirectory / "settings" / "shared-sync-state.json", "shared-sync-state");
    importer.ImportDocument(settingsDirectory / "presets" / "factory-archive-state.json", "factory-archive-state");
    importer.ImportDocument(riffLibraryDir / "riff-library-index.json", "riff-library");
}
} // namespace

MigrationReport MigrateLegacyJsonTree(JsonStore& store, const std::filesystem::path& settingsDirectory,
                                      const std::filesystem::path& userPresetDirectory)
{
    MigrationReport report;

    if (!store.IsOpen())
    {
        report.failures.push_back("store is not open");
        return report;
    }

    // Fast path, taken on every launch after the first: the stamp is there, so
    // no lock is taken at all.
    if (store.GetMeta(kMetaSchemaVersion).has_value())
    {
        report.succeeded = true;
        return report;
    }

    // Loading a DAW project starts every plugin instance at once, and on the
    // first launch after upgrading they all see an unmigrated store. Do the
    // whole import inside one BEGIN IMMEDIATE and re-check the stamp under that
    // lock, so exactly one instance imports and the rest fall straight through.
    // Raise the busy timeout for the duration: the queued instances must wait
    // for a large library to finish rather than give up and each start again.
    struct BusyTimeoutGuard
    {
        JsonStore& store;

        explicit BusyTimeoutGuard(JsonStore& s) : store(s)
        {
            store.SetBusyTimeoutMs(kMigrationBusyTimeoutMs);
        }

        ~BusyTimeoutGuard()
        {
            store.SetBusyTimeoutMs(JsonStore::kDefaultBusyTimeoutMs);
        }
    } busyTimeoutGuard{store};

    // The BEGIN IMMEDIATE below can block for up to kMigrationBusyTimeoutMs
    // while another instance imports, and it runs on the message thread during
    // Initialize(). Say so before blocking, so a two-minute silent freeze on the
    // first launch after upgrading is at least diagnosable from the log.
    std::cerr << "[StorageMigration] Importing the legacy library into " << util::PathToUtf8(store.Path())
              << "; this runs once and other instances wait for it." << std::endl;

    bool alreadyMigrated = false;
    Importer importer{store, report};

    const bool committed = store.Transact([&]() {
        // Second check, now holding the write lock: whoever got here first has
        // already imported and stamped.
        if (store.GetMeta(kMetaSchemaVersion).has_value())
        {
            alreadyMigrated = true;
            return true;
        }

        report.ran = true;
        RunImport(importer, settingsDirectory, userPresetDirectory);

        // A source that could not be *read* is noted and skipped: the user loses
        // that one file and the rest of the import still stands. A source that
        // could not be *written* aborts everything, because stamping the version
        // over a half-imported library would freeze it there permanently — the
        // stamp is the only thing that makes this run once. Rolling back instead
        // leaves the legacy tree untouched and retries on the next launch.
        if (importer.hardFailure)
        {
            return false;
        }

        // Stamped inside the same transaction, so the import and the record that
        // it happened commit together or not at all.
        return store.SetMeta(kMetaSchemaVersion, std::to_string(kStorageSchemaVersion)) &&
               store.SetMeta(kMetaMigratedAt, std::to_string(NowMillis())) &&
               store.SetMeta(kMetaMigratedFrom, util::PathToUtf8(settingsDirectory));
    });

    if (alreadyMigrated)
    {
        report = MigrationReport{};
        report.succeeded = true;
        return report;
    }

    report.succeeded = committed;
    if (!committed)
    {
        report.failures.push_back("the import was rolled back; it will run again next launch");
        report.itemsImported = 0;
        return report;
    }

    // The legacy tree is deliberately left in place so a downgrade still finds
    // it. Drop a note beside it saying so, or the stale files look like live
    // data to anyone poking at a profile months from now.
    if (report.ran)
    {
        WriteLegacyTreeMarker(settingsDirectory, store.Path(), report.itemsImported);
    }

    return report;
}

} // namespace guitarfx::storage
