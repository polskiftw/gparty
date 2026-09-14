#include "gdupe_store.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <sqlite3.h>

namespace gparty::fingerprints {
namespace {

constexpr wchar_t kDatabaseMutexName[] = L"Global\\GPartyGfingerdDatabase";

struct LocalMemoryDeleter {
  void operator()(void *value) const noexcept {
    if (value)
      LocalFree(value);
  }
};

class DatabaseWriteGuard {
public:
  DatabaseWriteGuard() {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)", SDDL_REVISION_1,
            &raw, nullptr))
      throw std::runtime_error(
          "Windows could not create the gfingerd database lock policy");
    std::unique_ptr<void, LocalMemoryDeleter> descriptor(raw);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.get();
    handle_ = CreateMutexW(&attributes, FALSE, kDatabaseMutexName);
    if (!handle_)
      throw std::runtime_error(
          "Windows could not create the gfingerd database lock");
    const DWORD waited = WaitForSingleObject(handle_, INFINITE);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error(
          "Windows could not acquire the gfingerd database lock");
    }
    owns_ = true;
  }

  ~DatabaseWriteGuard() {
    if (owns_)
      ReleaseMutex(handle_);
    if (handle_)
      CloseHandle(handle_);
  }

  DatabaseWriteGuard(const DatabaseWriteGuard &) = delete;
  DatabaseWriteGuard &operator=(const DatabaseWriteGuard &) = delete;

private:
  HANDLE handle_{};
  bool owns_{};
};

class Statement {
public:
  Statement(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("SQLite prepare failed: ") +
                               sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  sqlite3_stmt *get() const { return statement_; }
  void done() {
    if (sqlite3_step(statement_) != SQLITE_DONE)
      throw std::runtime_error(std::string("SQLite statement failed: ") +
                               sqlite3_errmsg(db_));
  }

private:
  sqlite3 *db_{};
  sqlite3_stmt *statement_{};
};

void bind_text(sqlite3_stmt *statement, int index, std::string_view value) {
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK)
    throw std::runtime_error("SQLite text binding failed");
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *value = sqlite3_column_text(statement, column);
  return value == nullptr ? std::string{}
                          : reinterpret_cast<const char *>(value);
}

std::filesystem::path
require_existing_database(const std::filesystem::path &path) {
  std::error_code error;
  if (std::filesystem::is_regular_file(path, error))
    return path;
  if (error)
    throw std::runtime_error("Cannot inspect the existing gdupe database: " +
                             error.message());
  throw std::runtime_error(
      "gfingerd requires the existing gdupe database: " + path.string());
}

bool same_identity(const gdupe::RemoteObject &first,
                   const gdupe::RemoteObject &second) {
  if (first.key != second.key || first.file_id != second.file_id ||
      first.size != second.size)
    return false;
  const bool first_has_sha1 = first.sha1.size() == 40;
  const bool second_has_sha1 = second.sha1.size() == 40;
  return !first_has_sha1 || !second_has_sha1 || first.sha1 == second.sha1;
}

struct FailureState {
  std::string state;
  std::int64_t retry_after{};
};

using FailureMap = std::unordered_map<std::string, FailureState>;
using FileIdSet = std::unordered_set<std::string>;

FailureMap load_failures(sqlite3 *db) {
  FailureMap result;
  Statement statement(
      db, "SELECT file_id,state,retry_after FROM gfingerd_failures");
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    result.emplace(column_text(statement.get(), 0),
                   FailureState{column_text(statement.get(), 1),
                                sqlite3_column_int64(statement.get(), 2)});
  }
  return result;
}

FileIdSet load_deferred(sqlite3 *db) {
  FileIdSet result;
  Statement statement(db, "SELECT file_id FROM gfingerd_deferred_gifs");
  while (sqlite3_step(statement.get()) == SQLITE_ROW)
    result.insert(column_text(statement.get(), 0));
  return result;
}

std::string load_last_scan(sqlite3 *db) {
  Statement statement(
      db,
      "SELECT value FROM gfingerd_metadata WHERE key='last_successful_scan'");
  return sqlite3_step(statement.get()) == SQLITE_ROW
             ? column_text(statement.get(), 0)
             : std::string{};
}

} // namespace

bool supported_extension(const std::string &extension) {
  return extension == "jpg" || extension == "jpeg" || extension == "png" ||
         extension == "webp" || extension == "gif" || extension == "mp4" ||
         extension == "m4v" || extension == "webm";
}

GdupeStore::GdupeStore(const std::filesystem::path &path) {
  DatabaseWriteGuard database_guard;
  const auto existing = require_existing_database(path);
  database_ = std::make_unique<gdupe::Database>(existing);
  if (sqlite3_open_v2(existing.string().c_str(), &ops_db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string message =
        ops_db_ ? sqlite3_errmsg(ops_db_) : "unknown error";
    if (ops_db_)
      sqlite3_close(ops_db_);
    ops_db_ = nullptr;
    throw std::runtime_error("Cannot open gdupe database for gfingerd state: " +
                             message);
  }
  sqlite3_busy_timeout(ops_db_, 30'000);
  execute("PRAGMA journal_mode=WAL");
  execute("PRAGMA synchronous=FULL");
  initialize_operational_schema();
}

GdupeStore::~GdupeStore() {
  if (ops_db_)
    sqlite3_close(ops_db_);
}

void GdupeStore::execute(const char *sql) const {
  char *error = nullptr;
  if (sqlite3_exec(ops_db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(ops_db_);
    sqlite3_free(error);
    throw std::runtime_error("SQLite execution failed: " + message);
  }
}

void GdupeStore::initialize_operational_schema() {
  execute(R"SQL(
CREATE TABLE IF NOT EXISTS gfingerd_failures(
  file_id TEXT PRIMARY KEY,
  object_key TEXT NOT NULL,
  state TEXT NOT NULL CHECK(state IN ('retry','failed')),
  attempt_count INTEGER NOT NULL DEFAULT 0,
  last_error TEXT NOT NULL,
  retry_after INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS gfingerd_deferred_gifs(
  file_id TEXT PRIMARY KEY,
  object_key TEXT NOT NULL,
  local_path TEXT NOT NULL,
  reason TEXT NOT NULL,
  deferred_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS gfingerd_metadata(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)SQL");
}

bool GdupeStore::current_identity(const gdupe::RemoteObject &object) const {
  const auto current = database_->object(object.key);
  return current && same_identity(current->remote, object);
}

void GdupeStore::reconcile(const std::vector<gdupe::RemoteObject> &objects) {
  DatabaseWriteGuard database_guard;
  database_->reconcile_inventory(objects, kFingerprintVersion);
  std::scoped_lock lock(ops_mutex_);
  execute(R"SQL(
DELETE FROM gfingerd_failures
WHERE NOT EXISTS(
  SELECT 1 FROM objects
  WHERE objects.key=gfingerd_failures.object_key
    AND objects.file_id=gfingerd_failures.file_id
);
DELETE FROM gfingerd_deferred_gifs
WHERE NOT EXISTS(
  SELECT 1 FROM objects
  WHERE objects.key=gfingerd_deferred_gifs.object_key
    AND objects.file_id=gfingerd_deferred_gifs.file_id
);
INSERT INTO gfingerd_metadata(key,value)
VALUES('last_successful_scan',datetime('now'))
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
)SQL");
}

std::vector<PendingObject> GdupeStore::pending(std::size_t limit) const {
  const auto inventory = database_->inventory();
  FailureMap failures;
  FileIdSet deferred;
  {
    std::scoped_lock lock(ops_mutex_);
    failures = load_failures(ops_db_);
    deferred = load_deferred(ops_db_);
  }
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::vector<PendingObject> result;
  if (limit != 0)
    result.reserve((std::min)(limit, inventory.size()));
  for (const auto &item : inventory) {
    if (!supported_extension(item.remote.extension) || item.fingerprint)
      continue;
    if (deferred.contains(item.remote.file_id))
      continue;
    const auto failure = failures.find(item.remote.file_id);
    if (failure != failures.end()) {
      if (failure->second.state == "failed" ||
          failure->second.retry_after > now)
        continue;
    }
    result.push_back({item.remote});
    if (limit != 0 && result.size() >= limit)
      break;
  }
  return result;
}

void GdupeStore::save_fingerprint(const gdupe::RemoteObject &object,
                                  const gdupe::Fingerprint &fingerprint) {
  if (fingerprint.version != kFingerprintVersion)
    throw std::runtime_error(
        "gfingerd produced an incompatible fingerprint version");
  DatabaseWriteGuard database_guard;
  database_->save_fingerprint(object.key, object.file_id, fingerprint);
  std::scoped_lock lock(ops_mutex_);
  Statement clear_failure(ops_db_,
                          "DELETE FROM gfingerd_failures WHERE file_id=?");
  bind_text(clear_failure.get(), 1, object.file_id);
  clear_failure.done();
  Statement clear_deferred(
      ops_db_, "DELETE FROM gfingerd_deferred_gifs WHERE file_id=?");
  bind_text(clear_deferred.get(), 1, object.file_id);
  clear_deferred.done();
}

void GdupeStore::record_failure(const gdupe::RemoteObject &object,
                                const std::string &error,
                                int maximum_attempts) {
  DatabaseWriteGuard database_guard;
  if (!current_identity(object))
    return;
  std::scoped_lock lock(ops_mutex_);
  Statement previous(
      ops_db_,
      "SELECT attempt_count FROM gfingerd_failures WHERE file_id=?");
  bind_text(previous.get(), 1, object.file_id);
  int attempts = 1;
  if (sqlite3_step(previous.get()) == SQLITE_ROW)
    attempts = sqlite3_column_int(previous.get(), 0) + 1;
  const bool terminal = attempts >= maximum_attempts;
  const int exponent = (std::min)(12, (std::max)(0, attempts - 1));
  const auto delay = (std::min<std::int64_t>)(21'600, 30LL << exponent);
  Statement statement(ops_db_, R"SQL(
INSERT INTO gfingerd_failures(
  file_id,object_key,state,attempt_count,last_error,retry_after)
VALUES(?,?,?,?,?,unixepoch()+?)
ON CONFLICT(file_id) DO UPDATE SET
  object_key=excluded.object_key,
  state=excluded.state,
  attempt_count=excluded.attempt_count,
  last_error=excluded.last_error,
  retry_after=excluded.retry_after,
  updated_at=unixepoch()
)SQL");
  bind_text(statement.get(), 1, object.file_id);
  bind_text(statement.get(), 2, object.key);
  bind_text(statement.get(), 3, terminal ? "failed" : "retry");
  sqlite3_bind_int(statement.get(), 4, attempts);
  bind_text(statement.get(), 5, error.substr(0, 4000));
  sqlite3_bind_int64(statement.get(), 6, delay);
  statement.done();
}

void GdupeStore::defer_gif(const gdupe::RemoteObject &object,
                           const std::filesystem::path &local_path,
                           const std::string &reason) {
  if (object.extension != "gif")
    throw std::runtime_error(
        "Only GIF objects may be deferred as malformed GIFs");
  if (local_path.empty())
    throw std::runtime_error("Deferred GIF local path is empty");
  DatabaseWriteGuard database_guard;
  if (!current_identity(object))
    throw std::runtime_error(
        "Object changed while malformed GIF was being deferred");
  std::scoped_lock lock(ops_mutex_);
  execute("BEGIN IMMEDIATE");
  try {
    Statement statement(ops_db_, R"SQL(
INSERT INTO gfingerd_deferred_gifs(
  file_id,object_key,local_path,reason,deferred_at)
VALUES(?,?,?,?,unixepoch())
ON CONFLICT(file_id) DO UPDATE SET
  object_key=excluded.object_key,
  local_path=excluded.local_path,
  reason=excluded.reason,
  deferred_at=unixepoch()
)SQL");
    bind_text(statement.get(), 1, object.file_id);
    bind_text(statement.get(), 2, object.key);
    bind_text(statement.get(), 3, local_path.string());
    bind_text(statement.get(), 4, reason.substr(0, 4000));
    statement.done();
    Statement clear(ops_db_,
                    "DELETE FROM gfingerd_failures WHERE file_id=?");
    bind_text(clear.get(), 1, object.file_id);
    clear.done();
    execute("COMMIT");
  } catch (...) {
    sqlite3_exec(ops_db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

GdupeStoreStatus GdupeStore::status() const {
  std::scoped_lock lock(ops_mutex_);
  Statement counts(ops_db_, R"SQL(
SELECT
  COUNT(*),
  COALESCE(SUM(CASE WHEN extension NOT IN
    ('jpg','jpeg','png','webp','gif','mp4','m4v','webm') THEN 1 ELSE 0 END),0),
  COALESCE(SUM(CASE WHEN extension IN
    ('jpg','jpeg','png','webp','gif','mp4','m4v','webm')
    AND EXISTS(SELECT 1 FROM gfingerd_deferred_gifs d
               WHERE d.file_id=objects.file_id AND d.object_key=objects.key)
    THEN 1 ELSE 0 END),0),
  COALESCE(SUM(CASE WHEN extension IN
    ('jpg','jpeg','png','webp','gif','mp4','m4v','webm')
    AND NOT EXISTS(SELECT 1 FROM gfingerd_deferred_gifs d
                   WHERE d.file_id=objects.file_id AND d.object_key=objects.key)
    AND EXISTS(SELECT 1 FROM gfingerd_failures f
               WHERE f.file_id=objects.file_id AND f.object_key=objects.key
                 AND f.state='failed')
    THEN 1 ELSE 0 END),0),
  COALESCE(SUM(CASE WHEN extension IN
    ('jpg','jpeg','png','webp','gif','mp4','m4v','webm')
    AND NOT EXISTS(SELECT 1 FROM gfingerd_deferred_gifs d
                   WHERE d.file_id=objects.file_id AND d.object_key=objects.key)
    AND NOT EXISTS(SELECT 1 FROM gfingerd_failures f
                   WHERE f.file_id=objects.file_id AND f.object_key=objects.key
                     AND f.state='failed')
    AND fp_version IS NOT NULL
    THEN 1 ELSE 0 END),0),
  COALESCE(SUM(CASE WHEN extension IN
    ('jpg','jpeg','png','webp','gif','mp4','m4v','webm')
    AND NOT EXISTS(SELECT 1 FROM gfingerd_deferred_gifs d
                   WHERE d.file_id=objects.file_id AND d.object_key=objects.key)
    AND NOT EXISTS(SELECT 1 FROM gfingerd_failures f
                   WHERE f.file_id=objects.file_id AND f.object_key=objects.key
                     AND f.state='failed')
    AND fp_version IS NULL
    THEN 1 ELSE 0 END),0)
FROM objects
)SQL");
  if (sqlite3_step(counts.get()) != SQLITE_ROW)
    throw std::runtime_error("Could not read gfingerd database status");

  GdupeStoreStatus result;
  result.inventory_objects =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 0));
  result.unsupported =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 1));
  result.deferred_gifs =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 2));
  result.failed =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 3));
  result.fully_fingerprinted =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 4));
  result.pending_objects =
      static_cast<std::size_t>(sqlite3_column_int64(counts.get(), 5));
  result.last_successful_scan = load_last_scan(ops_db_);
  return result;
}

} // namespace gparty::fingerprints
