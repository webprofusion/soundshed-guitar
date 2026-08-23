#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace guitarfx::storage
{
  /**
   * Every app-owned document lives in one SQLite table:
   *
   *     items(type TEXT, id TEXT, json TEXT, updated_at INTEGER, PRIMARY KEY (type, id))
   *
   * The schema is deliberately closed. A new kind of thing is a new `type`
   * string, and a change to any thing's shape is a change to the JSON it stores
   * — neither requires DDL, so the schema is expected never to version again.
   *
   * When a query needs to reach inside a document, use SQLite's json_extract()
   * against the `json` column rather than promoting a field to a column; an
   * expression index can be added later without touching the table.
   *
   * Concurrency: the standalone app and any number of plugin instances share one
   * database file. WAL plus a busy timeout makes that safe across processes, and
   * an internal mutex serializes the handle across threads in one process.
   */

  /// Well-known values for the `type` column. Free-form by design — this list is
  /// a convention, not a constraint, and adding to it needs no schema change.
  namespace ItemType
  {
    inline constexpr const char* kSetting = "setting";              // id = setting key
    inline constexpr const char* kPreset = "preset";                // id = preset id
    inline constexpr const char* kCompositePreset = "composite";    // id = composite id
    inline constexpr const char* kResource = "resource";            // id = "<resourceType>:<resourceId>"
    inline constexpr const char* kBlend = "blend";                  // id = blend id
    inline constexpr const char* kCustomEffect = "custom-effect";   // id = effect id
    inline constexpr const char* kRiff = "riff";                    // id = riff id
    inline constexpr const char* kLayout = "layout";                // id = layout id
    inline constexpr const char* kSetlist = "setlist";              // id = setlist id
    inline constexpr const char* kPresetFolder = "preset-folder";   // id = folder id
    inline constexpr const char* kPresetFavorite = "preset-favorite"; // id = preset id
    inline constexpr const char* kPresetRating = "preset-rating";     // id = preset id
    inline constexpr const char* kEffectPreset = "effect-preset";   // id = effect preset id
    inline constexpr const char* kEffectLayout = "effect-layout";   // id = effect type
    inline constexpr const char* kAutomationSlot = "automation-slot";
    /// Singleton documents that have no natural per-item decomposition
    /// (factory-archive bookkeeping, shared-sync state, UI view state).
    inline constexpr const char* kDocument = "document";            // id = logical name
  } // namespace ItemType

  struct StoreItem
  {
    std::string type;
    std::string id;
    std::string json;
    std::int64_t updatedAt = 0;

    /// Parses `json`. Returns nullopt when the stored text is not valid JSON,
    /// which should not happen but is never allowed to throw into a caller.
    [[nodiscard]] std::optional<nlohmann::json> Parse() const;
  };

  class JsonStore
  {
  public:
    JsonStore();
    ~JsonStore();

    JsonStore(const JsonStore&) = delete;
    JsonStore& operator=(const JsonStore&) = delete;

    /// Why an Open() failed. `Damaged` specifically means the file exists but
    /// failed its integrity check, which is the one failure a caller can recover
    /// from automatically (move it aside and rebuild). Everything else —
    /// permissions, a full disk, a path that is not writable — must not be
    /// "recovered" by destroying the file.
    enum class OpenStatus
    {
      Ok,
      Damaged,
      Failed
    };

    /**
     * Opens (creating if needed) the database at `dbPath`, applies the pragmas
     * and creates the schema. Returns false and fills `error` on failure; the
     * store then stays closed and every operation is a safe no-op.
     */
    bool Open(const std::filesystem::path& dbPath, std::string& error);
    /// As Open(), but distinguishes a damaged file from every other failure.
    OpenStatus OpenChecked(const std::filesystem::path& dbPath, std::string& error);
    void Close();
    [[nodiscard]] bool IsOpen() const;
    [[nodiscard]] const std::filesystem::path& Path() const { return mPath; }

    // ── Documents ──────────────────────────────────────────────────
    bool Put(std::string_view type, std::string_view id, const nlohmann::json& value);
    bool PutRaw(std::string_view type, std::string_view id, std::string_view json);
    /// Replaces every item of `type` with `items` in one transaction. Used by
    /// stores that own a whole collection and rewrite it wholesale.
    bool ReplaceAll(std::string_view type, const std::vector<StoreItem>& items);

    [[nodiscard]] std::optional<nlohmann::json> Get(std::string_view type, std::string_view id) const;
    [[nodiscard]] std::optional<std::string> GetRaw(std::string_view type, std::string_view id) const;
    [[nodiscard]] bool Has(std::string_view type, std::string_view id) const;

    bool Remove(std::string_view type, std::string_view id);
    std::int64_t RemoveAllOfType(std::string_view type);

    [[nodiscard]] std::vector<StoreItem> List(std::string_view type) const;
    [[nodiscard]] std::vector<std::string> ListIds(std::string_view type) const;
    [[nodiscard]] std::int64_t Count(std::string_view type) const;

    /// Newest updated_at across the given type, or the whole table when `type`
    /// is empty. 0 when nothing matches. Cheap change-detector for the
    /// cross-instance sync poll.
    [[nodiscard]] std::int64_t MaxUpdatedAt(std::string_view type = {}) const;

    // ── Meta (schema version, migration bookkeeping) ───────────────
    [[nodiscard]] std::optional<std::string> GetMeta(std::string_view key) const;
    bool SetMeta(std::string_view key, std::string_view value);

    /**
     * Runs `work` inside a transaction, committing if it returns true and
     * rolling back otherwise (or if it throws).
     *
     * Nested calls run against a SAVEPOINT, so an inner failure undoes only the
     * inner work and leaves the enclosing transaction usable. That matters
     * wherever a caller reports "nothing from this step was written" — without
     * the savepoint that claim is false, because the outer commit still carries
     * whatever the failed step managed to write.
     *
     * Note that an inner failure does *not* abort the outer transaction; the
     * outermost caller still owns the commit decision and must propagate the
     * failure itself if the whole unit should be abandoned.
     */
    bool Transact(const std::function<bool()>& work);

    /// Best-effort `PRAGMA wal_checkpoint(TRUNCATE)` plus `optimize`. Called on
    /// shutdown so the -wal file does not grow without bound.
    void Checkpoint();

    /// How long a write will wait for another connection to release the lock.
    /// The default suits short interactive writes; raise it around a long
    /// exclusive operation (the one-time migration) so the instances queued
    /// behind it wait rather than failing.
    void SetBusyTimeoutMs(int milliseconds);
    static constexpr int kDefaultBusyTimeoutMs = 5000;

    /// Runs `PRAGMA integrity_check`. Returns empty on a healthy database, or
    /// the problems sqlite reported. Used by the tests to prove that heavy
    /// concurrent use leaves the file intact; also useful for a support command.
    [[nodiscard]] std::vector<std::string> IntegrityCheck() const;

  private:
    // Callers must already hold mMutex for the Locked helpers.
    [[nodiscard]] std::vector<std::string> RunCheckLocked(const char* pragma) const;
    /// Cheaper than integrity_check — skips the index cross-check — so it can
    /// run on every open.
    [[nodiscard]] std::vector<std::string> QuickCheckLocked() const;
    [[nodiscard]] bool ApplyPragmasLocked(std::string& error) const;
    [[nodiscard]] bool CreateSchemaLocked(std::string& error) const;
    [[nodiscard]] bool ExecLocked(const char* sql, std::string& error) const;
    bool PutRawLocked(std::string_view type, std::string_view id, std::string_view json);

    mutable std::recursive_mutex mMutex;
    sqlite3* mDb = nullptr;
    std::filesystem::path mPath;
    int mTransactionDepth = 0;
  };

} // namespace guitarfx::storage
