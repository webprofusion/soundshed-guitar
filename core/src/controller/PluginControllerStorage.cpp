/**
 * PluginControllerStorage.cpp - The document store, and the small JSON
 * documents the UI keeps in it.
 *
 * Store() opens the database lazily on first touch, under a call_once because
 * the message thread and a folder-scan worker can both get there first. Opening
 * is where the legacy JSON tree is imported and where a damaged database is
 * quarantined rather than left in place — the migration stamp lives inside the
 * file, so a store that refuses to open means an empty library on every launch
 * from then on.
 *
 * LoadUiStorageJson/SaveUiStorageJson are the envelope documents (automation,
 * setlists, preset folders and friends): small, always rewritten whole, so one
 * row each rather than the per-item rows presets and resources get.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"

#include <chrono>
#include <fstream>
#include <iostream>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
std::filesystem::path PluginController::ResolveDocumentStorePath() const
{
    // Deliberately the real profile, not GetEffectiveSettingsDirectory(): while
    // a preset-archive session is active that points at a throwaway directory
    // which is deleted when the session ends. The session shadows presets and
    // folders on purpose, but the database itself must never move.
    return mFileSystem.ResolveSettingsDirectory() / "soundshed.db";
}

storage::JsonStore& PluginController::Store() const
{
    // call_once, not a bool: concurrent first-touches from the message thread
    // and a folder-scan worker would otherwise race, and two Open() calls on one
    // handle would leave the loser thinking the store is unavailable.
    std::call_once(mStoreOpenOnce, [this]() { OpenDocumentStore(); });

    return mStore;
}

void PluginController::OpenDocumentStore() const
{
    // Resolve the host's user-data path first, and discard it.
    //
    // On macOS this is what triggers migrateDataOutOfSandboxContainerOnce(),
    // which copies a pre-sandbox-removal profile out of
    // ~/Library/Containers/... into ~/Library/Soundshed Guitar. That copy is
    // skipped if the destination already contains anything, and opening the
    // store creates <destination>/data/v1/ — so opening first would strand the
    // user's entire library in the container, silently and permanently.
    //
    // Initialize() happens to call this before anything else today; this makes
    // it a guarantee rather than an ordering coincidence, which matters now that
    // the store opens lazily on first touch.
    (void)mHost.GetUserDataPath();

    const auto dbPath = ResolveDocumentStorePath();

    std::string error;
    auto status = mStore.OpenChecked(dbPath, error);

    if (status == storage::JsonStore::OpenStatus::Damaged)
    {
        // A damaged file is the one failure worth recovering from here, and
        // leaving it in place is not a recovery: the migration stamp lives
        // *inside* the database, so a store that refuses to open means an empty
        // library on every subsequent launch, forever, with nothing but a log
        // line to explain it.
        //
        // Moving it aside makes the next Open() create a fresh database with no
        // stamp, which re-runs the legacy import and puts the user back to their
        // pre-upgrade library. The damaged file is kept, not deleted, so anything
        // salvageable can still be recovered from it by hand.
        const auto quarantinePath =
            dbPath.parent_path() / (dbPath.filename().string() + ".damaged-" +
                                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count()));

        std::error_code renameEc;
        std::filesystem::rename(dbPath, quarantinePath, renameEc);

        if (renameEc)
        {
            AppendSessionLog("The document store at " + dbPath.string() +
                             " is damaged and could not be moved aside: " + renameEc.message());
            std::cerr << "[Plugin] Damaged document store could not be quarantined: " << renameEc.message()
                      << std::endl;
            return;
        }

        // The -wal and -shm sidecars belong to the quarantined file; leaving
        // them would let sqlite try to replay them into the new database.
        for (const char* suffix : {"-wal", "-shm"})
        {
            std::error_code sidecarEc;
            std::filesystem::remove(dbPath.parent_path() / (dbPath.filename().string() + suffix), sidecarEc);
        }

        AppendSessionLog("The document store at " + dbPath.string() + " was damaged (" + error +
                         "). It has been moved to " + quarantinePath.string() +
                         " and the library will be rebuilt from the legacy files.");
        std::cerr << "[Plugin] Damaged document store quarantined as " << quarantinePath.string() << std::endl;

        error.clear();
        status = mStore.OpenChecked(dbPath, error);
    }

    if (status != storage::JsonStore::OpenStatus::Ok)
    {
        // Everything downstream degrades to empty-and-read-only rather than
        // crashing, and the legacy tree is still on disk untouched, so the user
        // loses this session's changes but never their library.
        AppendSessionLog("Could not open the document store at " + dbPath.string() + ": " + error);
        std::cerr << "[Plugin] Document store unavailable: " << error << std::endl;
        return;
    }

    // Both paths are recomputed rather than read from members: the store can be
    // opened lazily before Initialize() has filled those in, and importing with
    // an empty preset directory would stamp the schema version having silently
    // skipped every preset.
    const auto report = storage::MigrateLegacyJsonTree(mStore, mFileSystem.ResolveSettingsDirectory(),
                                                       mFileSystem.ResolvePresetDirectory() / "user");

    if (report.ran)
    {
        std::string summary = "Imported the legacy JSON tree into " + dbPath.string() + ": " +
                              std::to_string(report.itemsImported) + " items";

        for (const auto& note : report.notes)
        {
            summary += "\n  " + note;
        }

        for (const auto& failure : report.failures)
        {
            summary += "\n  ! " + failure;
        }

        AppendSessionLog(summary);
        std::cout << "[Plugin] " << summary << std::endl;
    }
}

std::string PluginController::UiStorageDocumentId(const std::string& filename)
{
    // The legacy filename is the document id, minus the extension. Keeping the
    // mapping mechanical means callers keep passing the names they always did
    // and the migration lands documents under exactly these ids.
    const auto dot = filename.rfind(".json");
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

nlohmann::json PluginController::LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const
{
    // These are envelope documents — a collection plus the state that surrounds
    // it (activeFolderId, bankSize, cursorIndex). They are small and always
    // rewritten whole, so they live as one row each rather than being split into
    // per-item rows the way presets and resources are.
    //
    // A stored JSON `null` is treated as absent: Get() returns it as a value
    // rather than nullopt, and handing a null to callers that expect the shape
    // of `fallback` pushes the failure somewhere harder to read.
    if (auto stored = Store().Get(storage::ItemType::kDocument, UiStorageDocumentId(filename));
        stored && !stored->is_null())
    {
        return *stored;
    }

    return fallback;
}

void PluginController::SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const
{
    const bool wrote = Store().Put(storage::ItemType::kDocument, UiStorageDocumentId(filename), payload);

    if (!wrote)
    {
        AppendSessionLog("Failed to save UI storage document: " + UiStorageDocumentId(filename));
        return;
    }

    std::vector<std::string> domains;

    if (filename == "automation.json")
    {
        domains.push_back("automation");
    }
    else if (filename == "setlists.json")
    {
        domains.push_back("setlists");
    }
    else if (filename == "preset-folders.json" || filename == "preset-favorites.json" ||
             filename == "preset-ratings.json")
    {
        domains.push_back("presetMetadata");
    }
    else
    {
        domains.push_back("uiStorage");
    }

    TouchSharedSyncState(domains);
}

bool PluginController::WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const
{
    try
    {
        std::ofstream ofs(target, std::ios::binary);

        if (!ofs.is_open())
        {
            return false;
        }

        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace guitarfx
