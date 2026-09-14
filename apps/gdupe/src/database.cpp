#include "database.hpp"

#include <windows.h>
#include <sddl.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

namespace gdupe {
namespace {

constexpr wchar_t kSharedDatabaseMutexName[] =
    L"Global\\GPartyGfingerdDatabase";

struct LocalMemoryDeleter {
  void operator()(void *value) const noexcept {
    if (value)
      LocalFree(value);
  }
};

class SharedDatabaseWriteGuard {
public:
  SharedDatabaseWriteGuard() {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)", SDDL_REVISION_1,
            &raw, nullptr))
      throw std::runtime_error(
          "Windows could not create the shared database lock policy");
    std::unique_ptr<void, LocalMemoryDeleter> descriptor(raw);
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.get();
    handle_ =
        CreateMutexW(&attributes, FALSE, kSharedDatabaseMutexName);
    if (!handle_)
      throw std::runtime_error(
          "Windows could not create the shared database lock");
    const DWORD waited = WaitForSingleObject(handle_, INFINITE);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
      CloseHandle(handle_);
      handle_ = nullptr;
      throw std::runtime_error(
          "Windows could not acquire the shared database lock");
    }
    owns_ = true;
  }

  ~SharedDatabaseWriteGuard() {
    if (owns_)
      ReleaseMutex(handle_);
    if (handle_)
      CloseHandle(handle_);
  }

  SharedDatabaseWriteGuard(const SharedDatabaseWriteGuard &) = delete;
  SharedDatabaseWriteGuard &operator=(const SharedDatabaseWriteGuard &) =
      delete;

private:
  HANDLE handle_{};
  bool owns_{};
};

class Statement {
public:
  Statement(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("SQLite prepare failed: ") +
                               sqlite3_errmsg(db));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  sqlite3_stmt *get() const { return statement_; }
  void reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
  }
  void done() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw std::runtime_error(std::string("SQLite statement failed: ") +
                               sqlite3_errmsg(db_));
    }
  }

private:
  sqlite3 *db_{};
  sqlite3_stmt *statement_{};
};

class Transaction {
public:
  explicit Transaction(sqlite3 *db) : db_(db) {
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error(std::string("SQLite transaction failed: ") +
                               sqlite3_errmsg(db_));
    }
  }
  ~Transaction() {
    if (!committed_)
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  void commit() {
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("SQLite commit failed: ") +
                               sqlite3_errmsg(db_));
    }
    committed_ = true;
  }

private:
  sqlite3 *db_{};
  bool committed_{};
};

void bind_text(sqlite3_stmt *statement, int index, const std::string &value) {
  if (sqlite3_bind_text(statement, index, value.c_str(),
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("SQLite text binding failed");
  }
}

std::string column_text(sqlite3_stmt *statement, int index) {
  const auto *value = sqlite3_column_text(statement, index);
  return value == nullptr ? std::string{}
                          : reinterpret_cast<const char *>(value);
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const std::vector<T> &values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(T));
  if (!values.empty())
    std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

template <typename T>
std::vector<T> vector_from_blob(sqlite3_stmt *statement, int column) {
  const int size = sqlite3_column_bytes(statement, column);
  const void *data = sqlite3_column_blob(statement, column);
  if (size <= 0 || data == nullptr)
    return {};
  if (size % static_cast<int>(sizeof(T)) != 0)
    throw std::runtime_error("Fingerprint blob has an invalid size");
  std::vector<T> values(static_cast<std::size_t>(size) / sizeof(T));
  std::memcpy(values.data(), data, static_cast<std::size_t>(size));
  return values;
}

void bind_blob(sqlite3_stmt *statement, int index, const void *data,
               std::size_t size) {
  if (sqlite3_bind_blob(statement, index, data, static_cast<int>(size),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("SQLite blob binding failed");
  }
}

Fingerprint read_fingerprint(sqlite3_stmt *statement, int offset) {
  Fingerprint value;
  value.version = sqlite3_column_int(statement, offset);
  value.kind =
      static_cast<MediaKind>(sqlite3_column_int(statement, offset + 1));
  value.sha256 = column_text(statement, offset + 2);
  value.width = sqlite3_column_int(statement, offset + 3);
  value.height = sqlite3_column_int(statement, offset + 4);
  value.duration_ms = sqlite3_column_int64(statement, offset + 5);
  value.frame_count = sqlite3_column_int64(statement, offset + 6);
  value.phash =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, offset + 7));
  const int hash256_size = sqlite3_column_bytes(statement, offset + 8);
  const void *hash256 = sqlite3_column_blob(statement, offset + 8);
  if (hash256_size != static_cast<int>(value.perceptual256.size()) ||
      hash256 == nullptr) {
    throw std::runtime_error("perceptual256 fingerprint has an invalid size");
  }
  std::memcpy(value.perceptual256.data(), hash256, value.perceptual256.size());
  value.crop_hashes = vector_from_blob<std::uint64_t>(statement, offset + 9);
  value.timeline = vector_from_blob<std::uint64_t>(statement, offset + 10);
  return value;
}

InventoryObject read_inventory_row(sqlite3_stmt *statement) {
  InventoryObject item;
  item.remote.key = column_text(statement, 0);
  item.remote.file_id = column_text(statement, 1);
  item.remote.size =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2));
  item.remote.sha1 = column_text(statement, 3);
  item.remote.content_type = column_text(statement, 4);
  item.remote.extension = column_text(statement, 5);
  item.remote.upload_timestamp = sqlite3_column_int64(statement, 17);
  if (sqlite3_column_type(statement, 6) != SQLITE_NULL)
    item.fingerprint = read_fingerprint(statement, 6);
  return item;
}

constexpr int kSchemaVersion = 1;
constexpr const char *inventory_select = R"SQL(
SELECT key,file_id,size,sha1,content_type,extension,
       fp_version,media_kind,sha256,width,height,duration_ms,frame_count,phash,perceptual256,crop_hashes,timeline,
       upload_timestamp
FROM objects
)SQL";

} // namespace

std::pair<std::string, std::string> ordered_pair(std::string first,
                                                 std::string second) {
  if (second < first)
    std::swap(first, second);
  return {std::move(first), std::move(second)};
}

Database::Database(const std::filesystem::path &path) {
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  if (sqlite3_open_v2(path.string().c_str(), &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown error";
    if (db_)
      sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Cannot open durable inventory: " + message);
  }
  sqlite3_busy_timeout(db_, 30000);
  SharedDatabaseWriteGuard shared_write;
  execute("PRAGMA journal_mode=WAL");
  execute("PRAGMA synchronous=FULL");
  execute("PRAGMA foreign_keys=ON");
  initialize_schema();
}

Database::~Database() {
  if (db_)
    sqlite3_close(db_);
}

void Database::execute(const char *sql) const {
  char *error = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    throw std::runtime_error("SQLite execution failed: " + message);
  }
}

void Database::initialize_schema() {
  Statement version(db_, "PRAGMA user_version");
  if (sqlite3_step(version.get()) != SQLITE_ROW)
    throw std::runtime_error("Cannot read the gdupe database schema version");
  const int current_version = sqlite3_column_int(version.get(), 0);
  if (current_version != 0 && current_version != kSchemaVersion) {
    throw std::runtime_error(
        "Existing gdupe database schema version " +
        std::to_string(current_version) +
        " is unsupported; remove the old database and let gdupe rebuild it");
  }

  execute(R"SQL(
CREATE TABLE IF NOT EXISTS objects(
  key TEXT PRIMARY KEY,
  file_id TEXT NOT NULL,
  size INTEGER NOT NULL CHECK(size > 0),
  sha1 TEXT NOT NULL,
  content_type TEXT NOT NULL,
  extension TEXT NOT NULL,
  fp_version INTEGER,
  media_kind INTEGER,
  sha256 TEXT,
  width INTEGER,
  height INTEGER,
  duration_ms INTEGER,
  frame_count INTEGER,
  phash INTEGER,
  perceptual256 BLOB,
  crop_hashes BLOB,
  timeline BLOB,
  upload_timestamp INTEGER NOT NULL DEFAULT 0,
  last_seen INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE INDEX IF NOT EXISTS objects_sha256 ON objects(sha256) WHERE sha256 IS NOT NULL;
CREATE TABLE IF NOT EXISTS exclusions(
  first_key TEXT NOT NULL,
  second_key TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT(unixepoch()),
  PRIMARY KEY(first_key,second_key),
  CHECK(first_key < second_key)
);
CREATE TABLE IF NOT EXISTS operations(
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,
  target_key TEXT NOT NULL,
  target_file_id TEXT NOT NULL,
  state TEXT NOT NULL,
  error TEXT NOT NULL DEFAULT(''),
  created_at INTEGER NOT NULL DEFAULT(unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
PRAGMA user_version=1;
)SQL");
}

void Database::reconcile_inventory(const std::vector<RemoteObject> &remote,
                                   int fingerprint_version) {
  SharedDatabaseWriteGuard shared_write;
  Transaction transaction(db_);
  execute("CREATE TEMP TABLE IF NOT EXISTS seen_keys(key TEXT PRIMARY KEY); "
          "DELETE FROM seen_keys");
  Statement seen(db_, "INSERT INTO seen_keys(key) VALUES(?)");
  Statement invalidate_exclusions(
      db_, "DELETE FROM exclusions WHERE (first_key=? OR second_key=?) AND "
           "EXISTS(SELECT 1 FROM objects WHERE key=? AND file_id<>?)");
  Statement upsert(db_, R"SQL(
INSERT INTO objects(key,file_id,size,sha1,content_type,extension,upload_timestamp,last_seen)
VALUES(?,?,?,?,?,?,?,unixepoch())
ON CONFLICT(key) DO UPDATE SET
  file_id=excluded.file_id,size=excluded.size,sha1=excluded.sha1,
  content_type=excluded.content_type,extension=excluded.extension,upload_timestamp=excluded.upload_timestamp,last_seen=unixepoch(),
  fp_version=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.fp_version ELSE NULL END,
  media_kind=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.media_kind ELSE NULL END,
  sha256=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.sha256 ELSE NULL END,
  width=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.width ELSE NULL END,
  height=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.height ELSE NULL END,
  duration_ms=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.duration_ms ELSE NULL END,
  frame_count=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.frame_count ELSE NULL END,
  phash=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.phash ELSE NULL END,
  perceptual256=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.perceptual256 ELSE NULL END,
  crop_hashes=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.crop_hashes ELSE NULL END,
  timeline=CASE WHEN objects.file_id=excluded.file_id AND objects.size=excluded.size AND objects.sha1=excluded.sha1
                  THEN objects.timeline ELSE NULL END
)SQL");
  for (const auto &item : remote) {
    invalidate_exclusions.reset();
    bind_text(invalidate_exclusions.get(), 1, item.key);
    bind_text(invalidate_exclusions.get(), 2, item.key);
    bind_text(invalidate_exclusions.get(), 3, item.key);
    bind_text(invalidate_exclusions.get(), 4, item.file_id);
    invalidate_exclusions.done();
    seen.reset();
    bind_text(seen.get(), 1, item.key);
    seen.done();
    upsert.reset();
    bind_text(upsert.get(), 1, item.key);
    bind_text(upsert.get(), 2, item.file_id);
    sqlite3_bind_int64(upsert.get(), 3, static_cast<sqlite3_int64>(item.size));
    bind_text(upsert.get(), 4, item.sha1);
    bind_text(upsert.get(), 5, item.content_type);
    bind_text(upsert.get(), 6, item.extension);
    sqlite3_bind_int64(upsert.get(), 7, item.upload_timestamp);
    upsert.done();
  }
  execute("DELETE FROM exclusions WHERE first_key NOT IN (SELECT key FROM "
          "seen_keys) OR second_key NOT IN (SELECT key FROM seen_keys)");
  execute("DELETE FROM objects WHERE key NOT IN (SELECT key FROM seen_keys)");
  Statement invalidate(db_,
                       "UPDATE objects SET "
                       "fp_version=NULL,media_kind=NULL,sha256=NULL,width=NULL,"
                       "height=NULL,duration_ms=NULL,frame_count=NULL,phash="
                       "NULL,perceptual256=NULL,crop_hashes=NULL,timeline=NULL "
                       "WHERE fp_version IS NOT NULL AND fp_version<>?");
  sqlite3_bind_int(invalidate.get(), 1, fingerprint_version);
  invalidate.done();
  transaction.commit();
}

std::vector<InventoryObject> Database::inventory() const {
  Statement statement(
      db_, (std::string(inventory_select) + " ORDER BY key").c_str());
  std::vector<InventoryObject> result;
  while (sqlite3_step(statement.get()) == SQLITE_ROW)
    result.push_back(read_inventory_row(statement.get()));
  return result;
}

std::optional<InventoryObject> Database::object(const std::string &key) const {
  Statement statement(db_,
                      (std::string(inventory_select) + " WHERE key=?").c_str());
  bind_text(statement.get(), 1, key);
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return std::nullopt;
  return read_inventory_row(statement.get());
}

void Database::remove_object(const std::string &key,
                             const std::string &file_id) {
  SharedDatabaseWriteGuard shared_write;
  Transaction transaction(db_);
  Statement exclusions(
      db_, "DELETE FROM exclusions WHERE first_key=? OR second_key=?");
  bind_text(exclusions.get(), 1, key);
  bind_text(exclusions.get(), 2, key);
  exclusions.done();
  Statement object(db_, "DELETE FROM objects WHERE key=? AND file_id=?");
  bind_text(object.get(), 1, key);
  bind_text(object.get(), 2, file_id);
  object.done();
  transaction.commit();
}

void Database::save_fingerprint(const std::string &key,
                                const std::string &file_id,
                                const Fingerprint &value) {
  SharedDatabaseWriteGuard shared_write;
  std::scoped_lock lock(fingerprint_write_mutex_);
  const auto crops = bytes_of(value.crop_hashes);
  const auto timeline = bytes_of(value.timeline);
  Statement statement(db_, R"SQL(
UPDATE objects SET fp_version=?,media_kind=?,sha256=?,width=?,height=?,duration_ms=?,frame_count=?,phash=?,perceptual256=?,crop_hashes=?,timeline=?
WHERE key=? AND file_id=?
)SQL");
  sqlite3_bind_int(statement.get(), 1, value.version);
  sqlite3_bind_int(statement.get(), 2, static_cast<int>(value.kind));
  bind_text(statement.get(), 3, value.sha256);
  sqlite3_bind_int(statement.get(), 4, value.width);
  sqlite3_bind_int(statement.get(), 5, value.height);
  sqlite3_bind_int64(statement.get(), 6, value.duration_ms);
  sqlite3_bind_int64(statement.get(), 7, value.frame_count);
  sqlite3_bind_int64(statement.get(), 8,
                     static_cast<sqlite3_int64>(value.phash));
  bind_blob(statement.get(), 9, value.perceptual256.data(),
            value.perceptual256.size());
  bind_blob(statement.get(), 10, crops.data(), crops.size());
  bind_blob(statement.get(), 11, timeline.data(), timeline.size());
  bind_text(statement.get(), 12, key);
  bind_text(statement.get(), 13, file_id);
  statement.done();
  if (sqlite3_changes(db_) != 1)
    throw std::runtime_error(
        "Object changed while its fingerprint was being saved");
}

void Database::exclude_pair(const std::string &first,
                            const std::string &second) {
  SharedDatabaseWriteGuard shared_write;
  const auto [a, b] = ordered_pair(first, second);
  if (a == b)
    throw std::runtime_error("Cannot exclude an object from itself");
  Statement statement(
      db_,
      "INSERT OR IGNORE INTO exclusions(first_key,second_key) VALUES(?,?)");
  bind_text(statement.get(), 1, a);
  bind_text(statement.get(), 2, b);
  statement.done();
}

std::set<std::pair<std::string, std::string>> Database::exclusions() const {
  Statement statement(db_, "SELECT first_key,second_key FROM exclusions");
  std::set<std::pair<std::string, std::string>> result;
  while (sqlite3_step(statement.get()) == SQLITE_ROW)
    result.emplace(column_text(statement.get(), 0),
                   column_text(statement.get(), 1));
  return result;
}

void Database::prepare_operations(const std::vector<Operation> &operations) {
  SharedDatabaseWriteGuard shared_write;
  Transaction transaction(db_);
  Statement statement(
      db_,
      "INSERT INTO operations(id,kind,target_key,target_file_id,state,error) "
      "VALUES(?,?,?,?,?,'')");
  for (const auto &item : operations) {
    statement.reset();
    bind_text(statement.get(), 1, item.id);
    bind_text(statement.get(), 2, item.kind);
    bind_text(statement.get(), 3, item.target_key);
    bind_text(statement.get(), 4, item.target_file_id);
    bind_text(statement.get(), 5, item.state);
    statement.done();
  }
  transaction.commit();
}

void Database::update_operation(const std::string &id, const std::string &state,
                                const std::string &error) {
  SharedDatabaseWriteGuard shared_write;
  Statement statement(db_, "UPDATE operations SET "
                           "state=?,error=?,updated_at=unixepoch() WHERE id=?");
  bind_text(statement.get(), 1, state);
  bind_text(statement.get(), 2, error);
  bind_text(statement.get(), 3, id);
  statement.done();
  if (sqlite3_changes(db_) != 1)
    throw std::runtime_error("Recovery operation is missing");
}

std::vector<Operation> Database::pending_operations() const {
  Statement statement(db_,
                      "SELECT id,kind,target_key,target_file_id,state,error "
                      "FROM operations ORDER BY created_at,id");
  std::vector<Operation> result;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    result.push_back(
        {column_text(statement.get(), 0), column_text(statement.get(), 1),
         column_text(statement.get(), 2), column_text(statement.get(), 3),
         column_text(statement.get(), 4), column_text(statement.get(), 5)});
  }
  return result;
}

void Database::complete_operations(const std::vector<std::string> &ids) {
  SharedDatabaseWriteGuard shared_write;
  Transaction transaction(db_);
  Statement statement(db_, "DELETE FROM operations WHERE id=?");
  for (const auto &id : ids) {
    statement.reset();
    bind_text(statement.get(), 1, id);
    statement.done();
  }
  transaction.commit();
}

std::string Database::metadata(const std::string &key) const {
  Statement statement(db_, "SELECT value FROM metadata WHERE key=?");
  bind_text(statement.get(), 1, key);
  return sqlite3_step(statement.get()) == SQLITE_ROW
             ? column_text(statement.get(), 0)
             : std::string{};
}

void Database::set_metadata(const std::string &key, const std::string &value) {
  SharedDatabaseWriteGuard shared_write;
  Statement statement(db_, "INSERT INTO metadata(key,value) VALUES(?,?) ON "
                           "CONFLICT(key) DO UPDATE SET value=excluded.value");
  bind_text(statement.get(), 1, key);
  bind_text(statement.get(), 2, value);
  statement.done();
}

std::uint64_t Database::advance_queue_generation() {
  SharedDatabaseWriteGuard shared_write;
  const std::string current = metadata("queue_generation");
  const std::uint64_t generation =
      current.empty() ? 1 : std::stoull(current) + 1;
  set_metadata("queue_generation", std::to_string(generation));
  return generation;
}

} // namespace gdupe
