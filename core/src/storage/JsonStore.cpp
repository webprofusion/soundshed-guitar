#include "storage/JsonStore.h"

#include "util/PathEncoding.h"

#include <sqlite3.h>

#include <chrono>
#include <iostream>

namespace guitarfx::storage
{
namespace
{
std::int64_t NowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// RAII for a prepared statement so every early return finalizes it.
class Stmt
{
  public:
    Stmt(sqlite3* db, const char* sql)
    {
        mRc = sqlite3_prepare_v2(db, sql, -1, &mStmt, nullptr);
    }

    ~Stmt()
    {
        sqlite3_finalize(mStmt);
    }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    [[nodiscard]] bool Ok() const
    {
        return mRc == SQLITE_OK && mStmt != nullptr;
    }

    [[nodiscard]] sqlite3_stmt* Get() const
    {
        return mStmt;
    }

    void BindText(int index, std::string_view value) const
    {
        // SQLITE_TRANSIENT: sqlite copies, so the view may die immediately after.
        sqlite3_bind_text(mStmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }

    void BindInt64(int index, std::int64_t value) const
    {
        sqlite3_bind_int64(mStmt, index, value);
    }

    [[nodiscard]] int Step() const
    {
        return sqlite3_step(mStmt);
    }

    [[nodiscard]] std::string ColumnText(int index) const
    {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(mStmt, index));
        const int bytes = sqlite3_column_bytes(mStmt, index);
        return text ? std::string(text, static_cast<std::size_t>(bytes)) : std::string{};
    }

    [[nodiscard]] std::int64_t ColumnInt64(int index) const
    {
        return sqlite3_column_int64(mStmt, index);
    }

  private:
    sqlite3_stmt* mStmt = nullptr;
    int mRc = SQLITE_ERROR;
};

void LogFailure(const char* what, sqlite3* db)
{
    std::cerr << "[JsonStore] " << what << ": " << (db ? sqlite3_errmsg(db) : "no database") << std::endl;
}
} // namespace

std::optional<nlohmann::json> StoreItem::Parse() const
{
    try
    {
        return nlohmann::json::parse(json);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[JsonStore] Unparseable document " << type << ":" << id << " — " << exception.what() << std::endl;
        return std::nullopt;
    }
}

JsonStore::JsonStore() = default;

JsonStore::~JsonStore()
{
    Close();
}

bool JsonStore::Open(const std::filesystem::path& dbPath, std::string& error)
{
    return OpenChecked(dbPath, error) == OpenStatus::Ok;
}

JsonStore::OpenStatus JsonStore::OpenChecked(const std::filesystem::path& dbPath, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb != nullptr)
    {
        error = "Store is already open";
        return OpenStatus::Failed;
    }

    // SQLITE_OMIT_AUTOINIT is set in the build, so initialize explicitly.
    if (const int rc = sqlite3_initialize(); rc != SQLITE_OK)
    {
        error = "sqlite3_initialize failed (" + std::to_string(rc) + ")";
        return OpenStatus::Failed;
    }

    std::error_code dirEc;

    if (!dbPath.parent_path().empty())
    {
        std::filesystem::create_directories(dbPath.parent_path(), dirEc);
    }

    // FULLMUTEX: one handle is shared by the message thread and by background
    // workers (folder scans, downloads), so let sqlite serialize it for us.
    //
    // This is belt-and-braces on top of mMutex, which is the load-bearing one:
    // sqlite's own serialization protects a single call, but Transact() has to
    // hold the handle across many, so the recursive_mutex cannot be removed.
    // FULLMUTEX can be, if the extra lock ever shows up in a profile.
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    if (const int rc = sqlite3_open_v2(util::PathToUtf8(dbPath).c_str(), &mDb, flags, nullptr); rc != SQLITE_OK)
    {
        error = mDb ? sqlite3_errmsg(mDb) : "sqlite3_open_v2 failed";
        sqlite3_close_v2(mDb);
        mDb = nullptr;
        return OpenStatus::Failed;
    }

    if (!ApplyPragmasLocked(error))
    {
        sqlite3_close_v2(mDb);
        mDb = nullptr;
        return OpenStatus::Failed;
    }

    // Verify before writing anything. A database can arrive here already
    // damaged — most plausibly copied mid-write by an external migration (the
    // macOS sandbox-container copy does exactly that kind of bulk copy), or off
    // failing hardware. Writing into it would compound the damage, so refuse to
    // open and report Damaged: the caller moves the file aside and reopens,
    // which rebuilds an empty store and re-runs the legacy import, rather than
    // leaving the user with a permanently empty library.
    //
    // quick_check rather than integrity_check: it skips the (expensive)
    // index-vs-table cross-check but still catches structural damage, so it is
    // cheap enough to run on every launch.
    if (const auto problems = QuickCheckLocked(); !problems.empty())
    {
        error = "database failed its integrity check: " + problems.front();
        std::cerr << "[JsonStore] " << dbPath.string() << " is damaged and will not be opened:" << std::endl;

        for (const auto& problem : problems)
        {
            std::cerr << "[JsonStore]   " << problem << std::endl;
        }

        sqlite3_close_v2(mDb);
        mDb = nullptr;
        return OpenStatus::Damaged;
    }

    if (!CreateSchemaLocked(error))
    {
        sqlite3_close_v2(mDb);
        mDb = nullptr;
        return OpenStatus::Failed;
    }

    mPath = dbPath;
    return OpenStatus::Ok;
}

void JsonStore::Close()
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return;
    }

    Checkpoint();

    // close_v2, not close: every statement is finalized by Stmt's destructor, so
    // a BUSY return should not happen — but if one ever did, close() would leave
    // the handle open while the line below drops our only pointer to it.
    // close_v2 hands sqlite the responsibility to free it once it can.
    if (const int rc = sqlite3_close_v2(mDb); rc != SQLITE_OK)
    {
        LogFailure("Close", mDb);
    }

    mDb = nullptr;
    mPath.clear();
    mTransactionDepth = 0;
}

bool JsonStore::IsOpen() const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mDb != nullptr;
}

bool JsonStore::ApplyPragmasLocked(std::string& error) const
{
    // WAL lets readers run while a writer commits, and survives a process kill
    // without corrupting the database — the failure mode that motivated moving
    // off plain JSON files in the first place.
    if (!ExecLocked("PRAGMA journal_mode=WAL;", error))
    {
        return false;
    }

    // NORMAL is the recommended pairing with WAL: durable across a process
    // crash (which is what we care about), only at risk on OS/power loss.
    if (!ExecLocked("PRAGMA synchronous=NORMAL;", error))
    {
        return false;
    }

    if (!ExecLocked("PRAGMA foreign_keys=ON;", error))
    {
        return false;
    }

    // Other instances of the app may hold the write lock briefly.
    sqlite3_busy_timeout(mDb, kDefaultBusyTimeoutMs);
    return true;
}

void JsonStore::SetBusyTimeoutMs(int milliseconds)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb != nullptr)
    {
        sqlite3_busy_timeout(mDb, milliseconds);
    }
}

bool JsonStore::CreateSchemaLocked(std::string& error) const
{
    // WITHOUT ROWID: (type, id) is the only access path, so storing rows
    // directly in the primary-key btree saves a level of indirection.
    static constexpr const char* kSchema =
        "CREATE TABLE IF NOT EXISTS items ("
        "  type       TEXT NOT NULL,"
        "  id         TEXT NOT NULL,"
        "  json       TEXT NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  PRIMARY KEY (type, id)"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_items_type_updated ON items(type, updated_at);"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");";

    return ExecLocked(kSchema, error);
}

bool JsonStore::ExecLocked(const char* sql, std::string& error) const
{
    char* message = nullptr;

    if (sqlite3_exec(mDb, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message ? message : "SQL execution failed";
        sqlite3_free(message);
        return false;
    }

    sqlite3_free(message);
    return true;
}

bool JsonStore::Put(std::string_view type, std::string_view id, const nlohmann::json& value)
{
    std::string serialized;
    try
    {
        serialized = value.dump();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[JsonStore] Could not serialize " << type << ":" << id << " — " << exception.what() << std::endl;
        return false;
    }
    return PutRaw(type, id, serialized);
}

bool JsonStore::PutRaw(std::string_view type, std::string_view id, std::string_view json)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return PutRawLocked(type, id, json);
}

bool JsonStore::PutRawLocked(std::string_view type, std::string_view id, std::string_view json)
{
    if (mDb == nullptr || type.empty() || id.empty())
    {
        return false;
    }

    Stmt stmt(mDb, "INSERT INTO items(type, id, json, updated_at) VALUES(?1, ?2, ?3, ?4) "
                   "ON CONFLICT(type, id) DO UPDATE SET json=excluded.json, updated_at=excluded.updated_at;");

    if (!stmt.Ok())
    {
        LogFailure("Prepare put", mDb);
        return false;
    }

    stmt.BindText(1, type);
    stmt.BindText(2, id);
    stmt.BindText(3, json);
    stmt.BindInt64(4, NowMillis());

    if (stmt.Step() != SQLITE_DONE)
    {
        LogFailure("Put", mDb);
        return false;
    }

    return true;
}

bool JsonStore::ReplaceAll(std::string_view type, const std::vector<StoreItem>& items)
{
    if (type.empty())
    {
        return false;
    }

    // One transaction, so a crash mid-rewrite leaves the previous collection
    // intact rather than a half-written one.
    return Transact([&]() {
        std::lock_guard<std::recursive_mutex> lock(mMutex);

        if (mDb == nullptr)
        {
            return false;
        }

        {
            Stmt del(mDb, "DELETE FROM items WHERE type = ?1;");

            if (!del.Ok())
            {
                LogFailure("Prepare replace-all delete", mDb);
                return false;
            }

            del.BindText(1, type);

            if (del.Step() != SQLITE_DONE)
            {
                LogFailure("Replace-all delete", mDb);
                return false;
            }
        }

        for (const auto& item : items)
        {
            if (!PutRawLocked(type, item.id, item.json))
            {
                return false;
            }
        }

        return true;
    });
}

std::optional<std::string> JsonStore::GetRaw(std::string_view type, std::string_view id) const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return std::nullopt;
    }

    Stmt stmt(mDb, "SELECT json FROM items WHERE type = ?1 AND id = ?2;");

    if (!stmt.Ok())
    {
        LogFailure("Prepare get", mDb);
        return std::nullopt;
    }

    stmt.BindText(1, type);
    stmt.BindText(2, id);

    if (stmt.Step() != SQLITE_ROW)
    {
        return std::nullopt;
    }

    return stmt.ColumnText(0);
}

std::optional<nlohmann::json> JsonStore::Get(std::string_view type, std::string_view id) const
{
    const auto raw = GetRaw(type, id);

    if (!raw)
    {
        return std::nullopt;
    }

    try
    {
        return nlohmann::json::parse(*raw);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[JsonStore] Unparseable document " << type << ":" << id << " — " << exception.what() << std::endl;
        return std::nullopt;
    }
}

bool JsonStore::Has(std::string_view type, std::string_view id) const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return false;
    }

    Stmt stmt(mDb, "SELECT 1 FROM items WHERE type = ?1 AND id = ?2;");

    if (!stmt.Ok())
    {
        return false;
    }

    stmt.BindText(1, type);
    stmt.BindText(2, id);
    return stmt.Step() == SQLITE_ROW;
}

bool JsonStore::Remove(std::string_view type, std::string_view id)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return false;
    }

    Stmt stmt(mDb, "DELETE FROM items WHERE type = ?1 AND id = ?2;");

    if (!stmt.Ok())
    {
        LogFailure("Prepare remove", mDb);
        return false;
    }

    stmt.BindText(1, type);
    stmt.BindText(2, id);

    if (stmt.Step() != SQLITE_DONE)
    {
        LogFailure("Remove", mDb);
        return false;
    }

    return true;
}

std::int64_t JsonStore::RemoveAllOfType(std::string_view type)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return 0;
    }

    Stmt stmt(mDb, "DELETE FROM items WHERE type = ?1;");

    if (!stmt.Ok())
    {
        return 0;
    }

    stmt.BindText(1, type);

    if (stmt.Step() != SQLITE_DONE)
    {
        LogFailure("RemoveAllOfType", mDb);
        return 0;
    }

    return sqlite3_changes(mDb);
}

std::vector<StoreItem> JsonStore::List(std::string_view type) const
{
    std::vector<StoreItem> result;

    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return result;
    }

    Stmt stmt(mDb, "SELECT id, json, updated_at FROM items WHERE type = ?1 ORDER BY id;");

    if (!stmt.Ok())
    {
        LogFailure("Prepare list", mDb);
        return result;
    }

    stmt.BindText(1, type);

    while (stmt.Step() == SQLITE_ROW)
    {
        StoreItem item;
        item.type = std::string(type);
        item.id = stmt.ColumnText(0);
        item.json = stmt.ColumnText(1);
        item.updatedAt = stmt.ColumnInt64(2);
        result.push_back(std::move(item));
    }

    return result;
}

std::vector<std::string> JsonStore::ListIds(std::string_view type) const
{
    std::vector<std::string> result;

    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return result;
    }

    Stmt stmt(mDb, "SELECT id FROM items WHERE type = ?1 ORDER BY id;");

    if (!stmt.Ok())
    {
        return result;
    }

    stmt.BindText(1, type);

    while (stmt.Step() == SQLITE_ROW)
    {
        result.push_back(stmt.ColumnText(0));
    }

    return result;
}

std::int64_t JsonStore::Count(std::string_view type) const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return 0;
    }

    Stmt stmt(mDb, "SELECT COUNT(*) FROM items WHERE type = ?1;");

    if (!stmt.Ok())
    {
        return 0;
    }

    stmt.BindText(1, type);
    return stmt.Step() == SQLITE_ROW ? stmt.ColumnInt64(0) : 0;
}

std::int64_t JsonStore::MaxUpdatedAt(std::string_view type) const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return 0;
    }

    if (type.empty())
    {
        Stmt stmt(mDb, "SELECT IFNULL(MAX(updated_at), 0) FROM items;");

        if (!stmt.Ok())
        {
            return 0;
        }

        return stmt.Step() == SQLITE_ROW ? stmt.ColumnInt64(0) : 0;
    }

    Stmt stmt(mDb, "SELECT IFNULL(MAX(updated_at), 0) FROM items WHERE type = ?1;");

    if (!stmt.Ok())
    {
        return 0;
    }

    stmt.BindText(1, type);
    return stmt.Step() == SQLITE_ROW ? stmt.ColumnInt64(0) : 0;
}

std::optional<std::string> JsonStore::GetMeta(std::string_view key) const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return std::nullopt;
    }

    Stmt stmt(mDb, "SELECT value FROM meta WHERE key = ?1;");

    if (!stmt.Ok())
    {
        return std::nullopt;
    }

    stmt.BindText(1, key);

    if (stmt.Step() != SQLITE_ROW)
    {
        return std::nullopt;
    }

    return stmt.ColumnText(0);
}

bool JsonStore::SetMeta(std::string_view key, std::string_view value)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return false;
    }

    Stmt stmt(mDb, "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");

    if (!stmt.Ok())
    {
        LogFailure("Prepare set-meta", mDb);
        return false;
    }

    stmt.BindText(1, key);
    stmt.BindText(2, value);

    if (stmt.Step() != SQLITE_DONE)
    {
        LogFailure("SetMeta", mDb);
        return false;
    }

    return true;
}

bool JsonStore::Transact(const std::function<bool()>& work)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return false;
    }

    // Nested call: run against a SAVEPOINT so a failure here undoes only this
    // step. The outermost caller still owns the final commit/rollback, but it no
    // longer inherits the partial writes of a step that reported failure.
    if (mTransactionDepth > 0)
    {
        const std::string savepoint = "jsonstore_sp_" + std::to_string(mTransactionDepth);
        std::string savepointError;

        if (!ExecLocked(("SAVEPOINT " + savepoint + ";").c_str(), savepointError))
        {
            std::cerr << "[JsonStore] Could not create savepoint: " << savepointError << std::endl;
            return false;
        }

        ++mTransactionDepth;
        bool inner = false;
        try
        {
            inner = work();
        }
        catch (const std::exception& exception)
        {
            std::cerr << "[JsonStore] Nested transaction body threw: " << exception.what() << std::endl;
            inner = false;
        }
        catch (...)
        {
            std::cerr << "[JsonStore] Nested transaction body threw an unknown exception" << std::endl;
            inner = false;
        }
        --mTransactionDepth;

        if (!inner)
        {
            // ROLLBACK TO leaves the savepoint on the stack, so it still has to be
            // released afterwards or the transaction keeps growing.
            std::string rollbackError;

            if (!ExecLocked(("ROLLBACK TO " + savepoint + ";").c_str(), rollbackError))
            {
                std::cerr << "[JsonStore] Savepoint rollback failed: " << rollbackError << std::endl;
            }
        }

        std::string releaseError;

        if (!ExecLocked(("RELEASE " + savepoint + ";").c_str(), releaseError))
        {
            std::cerr << "[JsonStore] Savepoint release failed: " << releaseError << std::endl;
        }

        return inner;
    }

    std::string error;

    if (!ExecLocked("BEGIN IMMEDIATE;", error))
    {
        std::cerr << "[JsonStore] Could not begin transaction: " << error << std::endl;
        return false;
    }

    ++mTransactionDepth;
    bool succeeded = false;
    try
    {
        succeeded = work();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[JsonStore] Transaction body threw: " << exception.what() << std::endl;
        succeeded = false;
    }
    catch (...)
    {
        std::cerr << "[JsonStore] Transaction body threw an unknown exception" << std::endl;
        succeeded = false;
    }
    --mTransactionDepth;

    if (!succeeded)
    {
        std::string rollbackError;

        if (!ExecLocked("ROLLBACK;", rollbackError))
        {
            std::cerr << "[JsonStore] Rollback failed: " << rollbackError << std::endl;
        }

        return false;
    }

    if (!ExecLocked("COMMIT;", error))
    {
        std::cerr << "[JsonStore] Commit failed: " << error << std::endl;
        // Best effort: the commit already failed, so there is nothing useful to
        // do if unwinding it fails too.
        std::string rollbackError;
        (void)ExecLocked("ROLLBACK;", rollbackError);
        return false;
    }

    return true;
}

std::vector<std::string> JsonStore::RunCheckLocked(const char* pragma) const
{
    Stmt stmt(mDb, pragma);

    if (!stmt.Ok())
    {
        return {std::string{"could not run "} + pragma};
    }

    std::vector<std::string> problems;

    while (stmt.Step() == SQLITE_ROW)
    {
        // A healthy database reports the single row "ok".
        if (auto row = stmt.ColumnText(0); row != "ok")
        {
            problems.push_back(std::move(row));
        }
    }

    return problems;
}

std::vector<std::string> JsonStore::QuickCheckLocked() const
{
    return RunCheckLocked("PRAGMA quick_check;");
}

std::vector<std::string> JsonStore::IntegrityCheck() const
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return {"store is not open"};
    }

    return RunCheckLocked("PRAGMA integrity_check;");
}

void JsonStore::Checkpoint()
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    if (mDb == nullptr)
    {
        return;
    }

    // Both are best effort: a checkpoint that cannot run (another connection is
    // mid-read) just means the -wal file stays large until next time.
    std::string error;
    (void)ExecLocked("PRAGMA wal_checkpoint(TRUNCATE);", error);
    (void)ExecLocked("PRAGMA optimize;", error);
}
} // namespace guitarfx::storage
