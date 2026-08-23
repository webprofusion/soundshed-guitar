#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace guitarfx::storage
{
  class JsonStore;

  /// Meta keys written by the migration. `kMetaSchemaVersion` is the guard that
  /// makes the import run exactly once.
  inline constexpr const char* kMetaSchemaVersion = "schema_version";
  inline constexpr const char* kMetaMigratedAt = "migrated_at";
  inline constexpr const char* kMetaMigratedFrom = "migrated_from";
  inline constexpr const char* kMetaAppVersion = "app_version";

  /**
   * The store's schema version. This exists to gate the one-time import, not to
   * describe the shape of the data: the `items` table is generic, so adding item
   * kinds or changing a document's fields must not bump it. Only a change to the
   * tables themselves would, and the design intent is that never happens.
   */
  inline constexpr int kStorageSchemaVersion = 1;

  /// How long an instance waits for another instance's migration to finish
  /// before giving up. Generous on purpose: a large library takes a while, and
  /// the instances queued behind it should wait rather than each start over.
  inline constexpr int kMigrationBusyTimeoutMs = 120000;

  struct MigrationReport
  {
    bool ran = false;              ///< false when the store was already migrated
    bool succeeded = false;
    std::int64_t itemsImported = 0;
    std::vector<std::string> notes;   ///< one line per source, for the session log
    std::vector<std::string> failures;///< sources that existed but could not be read
  };

  /**
   * Imports the legacy JSON tree under `settingsDirectory` into `store`, once.
   *
   * The legacy files are read only — nothing is deleted, renamed, or rewritten,
   * so a user can downgrade (losing anything done since the upgrade, but never
   * their pre-upgrade library). Re-running after a successful migration is a
   * no-op; a failed migration leaves the version stamp unset so the next launch
   * retries.
   */
  MigrationReport MigrateLegacyJsonTree(JsonStore& store,
                                        const std::filesystem::path& settingsDirectory,
                                        const std::filesystem::path& userPresetDirectory);

} // namespace guitarfx::storage
