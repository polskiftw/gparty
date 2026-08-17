#include "registry.hpp"

#include "component_versions.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace gparty::fingerprints {
namespace {

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
  void reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
  }
  void done() {
    if (sqlite3_step(statement_) != SQLITE_DONE)
      throw std::runtime_error(std::string("SQLite statement failed: ") +
                               sqlite3_errmsg(db_));
  }

private:
  sqlite3 *db_{};
  sqlite3_stmt *statement_{};
};

class Transaction {
public:
  explicit Transaction(sqlite3 *db) : db_(db) {
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
        SQLITE_OK)
      throw std::runtime_error(std::string("SQLite transaction failed: ") +
                               sqlite3_errmsg(db_));
  }
  ~Transaction() {
    if (!committed_)
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  void commit() {
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("SQLite commit failed: ") +
                               sqlite3_errmsg(db_));
    committed_ = true;
  }

private:
  sqlite3 *db_{};
  bool committed_{};
};

void bind_text(sqlite3_stmt *statement, int index, std::string_view value) {
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK)
    throw std::runtime_error("SQLite text binding failed");
}

void bind_blob(sqlite3_stmt *statement, int index, const void *data,
               std::size_t size) {
  if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    throw std::runtime_error("Fingerprint component is unexpectedly large");
  if (sqlite3_bind_blob(statement, index, data, static_cast<int>(size),
                        SQLITE_TRANSIENT) != SQLITE_OK)
    throw std::runtime_error("SQLite blob binding failed");
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *value = sqlite3_column_text(statement, column);
  return value == nullptr ? std::string{}
                          : reinterpret_cast<const char *>(value);
}

std::vector<std::uint8_t> column_blob(sqlite3_stmt *statement, int column) {
  const int size = sqlite3_column_bytes(statement, column);
  const void *data = sqlite3_column_blob(statement, column);
  if (size <= 0 || data == nullptr)
    return {};
  std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
  std::memcpy(result.data(), data, result.size());
  return result;
}

bool hex_digest(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isxdigit(c) != 0;
         });
}

std::vector<std::uint8_t> little_endian(std::uint64_t value) {
  std::vector<std::uint8_t> bytes(8);
  for (unsigned int offset = 0; offset < 8; ++offset)
    bytes[offset] = static_cast<std::uint8_t>(value >> (offset * 8U));
  return bytes;
}

std::vector<std::uint8_t>
little_endian(const std::vector<std::uint64_t> &values) {
  std::vector<std::uint8_t> bytes(values.size() * 8);
  for (std::size_t index = 0; index < values.size(); ++index)
    for (unsigned int offset = 0; offset < 8; ++offset)
      bytes[index * 8 + offset] =
          static_cast<std::uint8_t>(values[index] >> (offset * 8U));
  return bytes;
}

std::vector<std::uint8_t> media_payload(const gdupe::Fingerprint &value) {
  const auto json = nlohmann::json{
      {"kind", static_cast<int>(value.kind)},
      {"width", value.width},
      {"height", value.height},
      {"duration_ms", value.duration_ms},
      {"frame_count", value.frame_count}};
  const std::string text = json.dump();
  return {text.begin(), text.end()};
}

std::vector<std::uint8_t> text_payload(std::string_view value) {
  return {value.begin(), value.end()};
}

template <typename Callback>
void for_required(const std::string &extension, Callback callback) {
  if (moving_extension(extension)) {
    for (const auto component : kMovingComponents)
      callback(component);
  } else {
    for (const auto component : kStaticComponents)
      callback(component);
  }
}

bool component_exists(sqlite3 *db, const std::string &file_id,
                      ComponentVersion component) {
  Statement statement(
      db, "SELECT 1 FROM fingerprint_components WHERE file_id=? AND "
          "component=? AND algorithm_version=?");
  bind_text(statement.get(), 1, file_id);
  bind_text(statement.get(), 2, component.name);
  sqlite3_bind_int(statement.get(), 3, component.version);
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::size_t missing_count(sqlite3 *db, const std::string &file_id,
                          const std::string &extension) {
  std::size_t missing = 0;
  for_required(extension, [&](ComponentVersion component) {
    if (!component_exists(db, file_id, component))
      ++missing;
  });
  return missing;
}

void upsert_component(sqlite3 *db, const std::string &file_id,
                      ComponentVersion component,
                      const std::vector<std::uint8_t> &payload,
                      std::string_view source, bool preserve_existing) {
  const char *sql = preserve_existing
                        ? "INSERT OR IGNORE INTO fingerprint_components("
                          "file_id,component,algorithm_version,payload,source) "
                          "VALUES(?,?,?,?,?)"
                        : "INSERT INTO fingerprint_components("
                          "file_id,component,algorithm_version,payload,source) "
                          "VALUES(?,?,?,?,?) ON CONFLICT(file_id,component,"
                          "algorithm_version) DO UPDATE SET "
                          "payload=excluded.payload,computed_at=unixepoch(),"
                          "source=excluded.source";
  Statement statement(db, sql);
  bind_text(statement.get(), 1, file_id);
  bind_text(statement.get(), 2, component.name);
  sqlite3_bind_int(statement.get(), 3, component.version);
  bind_blob(statement.get(), 4, payload.data(), payload.size());
  bind_text(statement.get(), 5, source);
  statement.done();
}

bool identity_matches(sqlite3 *db, const std::string &key,
                      const std::string &file_id, std::uint64_t size,
                      const std::string &sha1) {
  Statement statement(db,
                      "SELECT size,sha1 FROM objects WHERE key=? AND "
                      "current_file_id=? AND present=1");
  bind_text(statement.get(), 1, key);
  bind_text(statement.get(), 2, file_id);
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return false;
  if (static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0)) !=
      size)
    return false;
  const std::string current_sha1 = column_text(statement.get(), 1);
  if (hex_digest(current_sha1, 40) && hex_digest(sha1, 40) &&
      current_sha1 != sha1)
    return false;
  return true;
}

constexpr int kSchemaVersion = 1;

} // namespace

bool supported_extension(const std::string &extension) {
  return extension == "jpg" || extension == "jpeg" || extension == "png" ||
         extension == "webp" || extension == "gif" || extension == "mp4" ||
         extension == "m4v" || extension == "webm";
}

bool moving_extension(const std::string &extension) {
  return extension == "gif" || extension == "mp4" || extension == "m4v" ||
         extension == "webm";
}

Registry::Registry(const std::filesystem::path &path) {
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
    throw std::runtime_error("Cannot open fingerprint registry: " + message);
  }
  sqlite3_busy_timeout(db_, 30'000);
  execute("PRAGMA journal_mode=WAL");
  execute("PRAGMA synchronous=FULL");
  execute("PRAGMA foreign_keys=ON");
  initialize_schema();
}

Registry::~Registry() {
  if (db_)
    sqlite3_close(db_);
}

void Registry::execute(const char *sql) const {
  char *error = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db_);
    sqlite3_free(error);
    throw std::runtime_error("SQLite execution failed: " + message);
  }
}

void Registry::initialize_schema() {
  Statement version(db_, "PRAGMA user_version");
  if (sqlite3_step(version.get()) != SQLITE_ROW)
    throw std::runtime_error("Cannot read fingerprint schema version");
  const int current = sqlite3_column_int(version.get(), 0);
  if (current != 0 && current != kSchemaVersion)
    throw std::runtime_error("Unsupported fingerprint schema version " +
                             std::to_string(current));
  execute(R"SQL(
CREATE TABLE IF NOT EXISTS object_versions(
  file_id TEXT PRIMARY KEY,
  key TEXT NOT NULL,
  size INTEGER NOT NULL CHECK(size > 0),
  sha1 TEXT NOT NULL,
  content_type TEXT NOT NULL,
  extension TEXT NOT NULL,
  upload_timestamp INTEGER NOT NULL DEFAULT 0,
  first_seen INTEGER NOT NULL DEFAULT(unixepoch()),
  last_seen INTEGER NOT NULL DEFAULT(unixepoch()),
  present INTEGER NOT NULL DEFAULT 1 CHECK(present IN (0,1))
);
CREATE TABLE IF NOT EXISTS objects(
  key TEXT PRIMARY KEY,
  current_file_id TEXT NOT NULL REFERENCES object_versions(file_id),
  size INTEGER NOT NULL CHECK(size > 0),
  sha1 TEXT NOT NULL,
  content_type TEXT NOT NULL,
  extension TEXT NOT NULL,
  upload_timestamp INTEGER NOT NULL DEFAULT 0,
  first_seen INTEGER NOT NULL DEFAULT(unixepoch()),
  last_seen INTEGER NOT NULL DEFAULT(unixepoch()),
  present INTEGER NOT NULL DEFAULT 1 CHECK(present IN (0,1))
);
CREATE INDEX IF NOT EXISTS objects_current_file_id ON objects(current_file_id);
CREATE TABLE IF NOT EXISTS fingerprint_components(
  file_id TEXT NOT NULL REFERENCES object_versions(file_id),
  component TEXT NOT NULL,
  algorithm_version INTEGER NOT NULL CHECK(algorithm_version > 0),
  payload BLOB NOT NULL,
  computed_at INTEGER NOT NULL DEFAULT(unixepoch()),
  source TEXT NOT NULL,
  PRIMARY KEY(file_id,component,algorithm_version)
);
CREATE TABLE IF NOT EXISTS failures(
  file_id TEXT PRIMARY KEY REFERENCES object_versions(file_id),
  state TEXT NOT NULL CHECK(state IN ('retry','failed','unsupported')),
  attempt_count INTEGER NOT NULL DEFAULT 0,
  last_error TEXT NOT NULL,
  retry_after INTEGER NOT NULL DEFAULT 0,
  profile_version INTEGER NOT NULL DEFAULT 1,
  updated_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS deferred_gifs(
  file_id TEXT PRIMARY KEY REFERENCES object_versions(file_id),
  local_path TEXT NOT NULL,
  reason TEXT NOT NULL,
  deferred_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS adoption_runs(
  source_path TEXT PRIMARY KEY,
  source_schema_version INTEGER NOT NULL,
  source_fingerprint_version INTEGER NOT NULL,
  rows_scanned INTEGER NOT NULL,
  matching_rows INTEGER NOT NULL,
  components_imported INTEGER NOT NULL,
  completed_at INTEGER NOT NULL DEFAULT(unixepoch())
);
CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
PRAGMA user_version=1;
)SQL");
}

void Registry::reconcile(const std::vector<gdupe::RemoteObject> &objects) {
  std::scoped_lock lock(mutex_);
  Transaction transaction(db_);
  execute("UPDATE objects SET present=0; UPDATE object_versions SET present=0");
  Statement version(db_, R"SQL(
INSERT INTO object_versions(file_id,key,size,sha1,content_type,extension,upload_timestamp,present)
VALUES(?,?,?,?,?,?,?,1)
ON CONFLICT(file_id) DO UPDATE SET last_seen=unixepoch(),present=1,
  content_type=excluded.content_type,extension=excluded.extension,
  upload_timestamp=excluded.upload_timestamp
WHERE object_versions.key=excluded.key AND object_versions.size=excluded.size
  AND object_versions.sha1=excluded.sha1
)SQL");
  Statement object(db_, R"SQL(
INSERT INTO objects(key,current_file_id,size,sha1,content_type,extension,upload_timestamp,present)
VALUES(?,?,?,?,?,?,?,1)
ON CONFLICT(key) DO UPDATE SET current_file_id=excluded.current_file_id,
  size=excluded.size,sha1=excluded.sha1,content_type=excluded.content_type,
  extension=excluded.extension,upload_timestamp=excluded.upload_timestamp,
  last_seen=unixepoch(),present=1
)SQL");
  Statement unsupported(db_, R"SQL(
INSERT INTO failures(file_id,state,attempt_count,last_error,retry_after)
VALUES(?,'unsupported',0,?,0)
ON CONFLICT(file_id) DO UPDATE SET state='unsupported',attempt_count=0,
  last_error=excluded.last_error,retry_after=0,updated_at=unixepoch()
)SQL");
  for (const auto &item : objects) {
    if (item.key.empty() || item.file_id.empty() || item.size == 0)
      throw std::runtime_error("B2 returned an invalid object identity");
    version.reset();
    bind_text(version.get(), 1, item.file_id);
    bind_text(version.get(), 2, item.key);
    sqlite3_bind_int64(version.get(), 3,
                       static_cast<sqlite3_int64>(item.size));
    bind_text(version.get(), 4, item.sha1);
    bind_text(version.get(), 5, item.content_type);
    bind_text(version.get(), 6, item.extension);
    sqlite3_bind_int64(version.get(), 7, item.upload_timestamp);
    version.done();
    if (sqlite3_changes(db_) != 1)
      throw std::runtime_error("A B2 file ID changed identity unexpectedly");

    object.reset();
    bind_text(object.get(), 1, item.key);
    bind_text(object.get(), 2, item.file_id);
    sqlite3_bind_int64(object.get(), 3,
                       static_cast<sqlite3_int64>(item.size));
    bind_text(object.get(), 4, item.sha1);
    bind_text(object.get(), 5, item.content_type);
    bind_text(object.get(), 6, item.extension);
    sqlite3_bind_int64(object.get(), 7, item.upload_timestamp);
    object.done();

    if (!supported_extension(item.extension)) {
      unsupported.reset();
      bind_text(unsupported.get(), 1, item.file_id);
      bind_text(unsupported.get(), 2,
                "Unsupported media extension: " + item.extension);
      unsupported.done();
    }
  }
  Statement scan(db_, R"SQL(
INSERT INTO metadata(key,value) VALUES('last_successful_scan',datetime('now'))
ON CONFLICT(key) DO UPDATE SET value=excluded.value
)SQL");
  scan.done();
  transaction.commit();
}

AdoptionSummary
Registry::adopt_gdupe_v3(const std::filesystem::path &legacy_path) {
  AdoptionSummary summary;
  if (legacy_path.empty() || !std::filesystem::is_regular_file(legacy_path))
    return summary;
  sqlite3 *legacy = nullptr;
  if (sqlite3_open_v2(legacy_path.string().c_str(), &legacy,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string error = legacy ? sqlite3_errmsg(legacy) : "unknown";
    if (legacy)
      sqlite3_close(legacy);
    throw std::runtime_error("Cannot open existing gdupe database read-only: " +
                             error);
  }
  struct LegacyCloser {
    void operator()(sqlite3 *value) const { sqlite3_close(value); }
  };
  std::unique_ptr<sqlite3, LegacyCloser> close_legacy(legacy);
  sqlite3_exec(legacy, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);

  Statement source(legacy, R"SQL(
SELECT key,file_id,size,sha1,media_kind,sha256,width,height,duration_ms,
       frame_count,phash,perceptual256,crop_hashes,timeline
FROM objects WHERE fp_version=3 AND file_id IS NOT NULL
)SQL");

  std::scoped_lock lock(mutex_);
  execute("BEGIN IMMEDIATE");
  bool open_transaction = true;
  try {
    std::size_t batch_rows = 0;
    while (sqlite3_step(source.get()) == SQLITE_ROW) {
      ++summary.rows_scanned;
      const std::string key = column_text(source.get(), 0);
      const std::string file_id = column_text(source.get(), 1);
      const auto size =
          static_cast<std::uint64_t>(sqlite3_column_int64(source.get(), 2));
      const std::string sha1 = column_text(source.get(), 3);
      if (!identity_matches(db_, key, file_id, size, sha1))
        continue;
      ++summary.matching_rows;

      const int kind = sqlite3_column_int(source.get(), 4);
      const std::string sha256 = column_text(source.get(), 5);
      const int width = sqlite3_column_int(source.get(), 6);
      const int height = sqlite3_column_int(source.get(), 7);
      const auto duration = sqlite3_column_int64(source.get(), 8);
      const auto frames = sqlite3_column_int64(source.get(), 9);
      const bool valid_media = kind >= static_cast<int>(gdupe::MediaKind::StaticImage) &&
                               kind <= static_cast<int>(gdupe::MediaKind::Video) &&
                               width > 0 && height > 0 && duration >= 0 && frames > 0;
      if (valid_media) {
        gdupe::Fingerprint value;
        value.kind = static_cast<gdupe::MediaKind>(kind);
        value.width = width;
        value.height = height;
        value.duration_ms = duration;
        value.frame_count = frames;
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kMediaInfo, media_payload(value),
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }
      if (hex_digest(sha256, 64)) {
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kSha256, text_payload(sha256),
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }
      if (sqlite3_column_type(source.get(), 10) != SQLITE_NULL) {
        const auto value = static_cast<std::uint64_t>(
            sqlite3_column_int64(source.get(), 10));
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kPhash64, little_endian(value),
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }
      const auto hash256 = column_blob(source.get(), 11);
      if (hash256.size() == 32) {
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kPerceptual256, hash256,
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }
      const auto crops = column_blob(source.get(), 12);
      if (!crops.empty() && crops.size() % 8 == 0) {
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kCropPhash64, crops,
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }
      const auto timeline = column_blob(source.get(), 13);
      if ((kind == static_cast<int>(gdupe::MediaKind::AnimatedImage) ||
           kind == static_cast<int>(gdupe::MediaKind::Video)) &&
          !timeline.empty() && timeline.size() % 8 == 0) {
        const int before = sqlite3_total_changes(db_);
        upsert_component(db_, file_id, kTimelinePhash64, timeline,
                         "gdupe-v3-adoption", true);
        summary.components_imported +=
            sqlite3_total_changes(db_) > before ? 1U : 0U;
      }

      if (++batch_rows == 500) {
        execute("COMMIT");
        execute("BEGIN IMMEDIATE");
        batch_rows = 0;
      }
    }
    Statement run(db_, R"SQL(
INSERT INTO adoption_runs(source_path,source_schema_version,
  source_fingerprint_version,rows_scanned,matching_rows,components_imported)
VALUES(?,1,3,?,?,?)
ON CONFLICT(source_path) DO UPDATE SET rows_scanned=excluded.rows_scanned,
  matching_rows=excluded.matching_rows,
  components_imported=adoption_runs.components_imported+excluded.components_imported,
  completed_at=unixepoch()
)SQL");
    bind_text(run.get(), 1, legacy_path.string());
    sqlite3_bind_int64(run.get(), 2,
                       static_cast<sqlite3_int64>(summary.rows_scanned));
    sqlite3_bind_int64(run.get(), 3,
                       static_cast<sqlite3_int64>(summary.matching_rows));
    sqlite3_bind_int64(
        run.get(), 4,
        static_cast<sqlite3_int64>(summary.components_imported));
    run.done();
    execute("COMMIT");
    open_transaction = false;
  } catch (...) {
    if (open_transaction)
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  return summary;
}

std::vector<PendingObject> Registry::pending(std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  Statement statement(db_, R"SQL(
SELECT o.key,o.current_file_id,o.size,o.sha1,o.content_type,o.extension,
       o.upload_timestamp,COALESCE(f.state,''),COALESCE(f.retry_after,0),
       CASE WHEN d.file_id IS NULL THEN 0 ELSE 1 END
FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id
LEFT JOIN deferred_gifs d ON d.file_id=o.current_file_id
WHERE o.present=1 ORDER BY o.upload_timestamp,o.key
)SQL");
  std::vector<PendingObject> result;
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const std::string extension = column_text(statement.get(), 5);
    if (!supported_extension(extension))
      continue;
    const std::string state = column_text(statement.get(), 7);
    const auto retry_after = sqlite3_column_int64(statement.get(), 8);
    const bool deferred_gif = sqlite3_column_int(statement.get(), 9) != 0;
    if (state == "failed" || state == "unsupported" || deferred_gif ||
        retry_after > now)
      continue;
    gdupe::RemoteObject object{
        column_text(statement.get(), 0), column_text(statement.get(), 1),
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 2)),
        column_text(statement.get(), 3), column_text(statement.get(), 4),
        extension, sqlite3_column_int64(statement.get(), 6)};
    const std::size_t missing = missing_count(db_, object.file_id, extension);
    if (missing != 0)
      result.push_back({std::move(object), missing});
    if (limit != 0 && result.size() >= limit)
      break;
  }
  return result;
}

void Registry::save_fingerprint(const gdupe::RemoteObject &object,
                                const gdupe::Fingerprint &fingerprint,
                                const std::string &source) {
  if (fingerprint.version != kCompatibleGdupeFingerprintVersion)
    throw std::runtime_error("Unexpected fingerprint algorithm version");
  if (!hex_digest(fingerprint.sha256, 64) || fingerprint.width <= 0 ||
      fingerprint.height <= 0 || fingerprint.frame_count <= 0 ||
      fingerprint.crop_hashes.empty() ||
      (moving_extension(object.extension) && fingerprint.timeline.empty()))
    throw std::runtime_error("Computed fingerprint is structurally incomplete");
  std::scoped_lock lock(mutex_);
  Transaction transaction(db_);
  if (!identity_matches(db_, object.key, object.file_id, object.size,
                        object.sha1))
    throw std::runtime_error(
        "Object changed while its fingerprint was being saved");

  const auto store_if_missing = [&](ComponentVersion component,
                                    const std::vector<std::uint8_t> &payload) {
    if (!component_exists(db_, object.file_id, component))
      upsert_component(db_, object.file_id, component, payload, source, false);
  };
  store_if_missing(kMediaInfo, media_payload(fingerprint));
  store_if_missing(kSha256, text_payload(fingerprint.sha256));
  store_if_missing(kPhash64, little_endian(fingerprint.phash));
  store_if_missing(kPerceptual256,
                   {fingerprint.perceptual256.begin(),
                    fingerprint.perceptual256.end()});
  store_if_missing(kCropPhash64, little_endian(fingerprint.crop_hashes));
  if (moving_extension(object.extension))
    store_if_missing(kTimelinePhash64, little_endian(fingerprint.timeline));
  Statement clear(db_, "DELETE FROM failures WHERE file_id=?");
  bind_text(clear.get(), 1, object.file_id);
  clear.done();
  Statement clear_deferred(db_, "DELETE FROM deferred_gifs WHERE file_id=?");
  bind_text(clear_deferred.get(), 1, object.file_id);
  clear_deferred.done();
  transaction.commit();
}

void Registry::record_failure(const gdupe::RemoteObject &object,
                              const std::string &error,
                              int maximum_attempts) {
  std::scoped_lock lock(mutex_);
  Statement previous(db_,
                     "SELECT attempt_count FROM failures WHERE file_id=?");
  bind_text(previous.get(), 1, object.file_id);
  int attempts = 1;
  if (sqlite3_step(previous.get()) == SQLITE_ROW)
    attempts = sqlite3_column_int(previous.get(), 0) + 1;
  const bool terminal = attempts >= maximum_attempts;
  const int exponent = std::min(12, std::max(0, attempts - 1));
  const auto delay = std::min<std::int64_t>(21'600, 30LL << exponent);
  Statement statement(db_, R"SQL(
INSERT INTO failures(file_id,state,attempt_count,last_error,retry_after)
VALUES(?,?,?,?,unixepoch()+?)
ON CONFLICT(file_id) DO UPDATE SET state=excluded.state,
  attempt_count=excluded.attempt_count,last_error=excluded.last_error,
  retry_after=excluded.retry_after,updated_at=unixepoch()
)SQL");
  bind_text(statement.get(), 1, object.file_id);
  bind_text(statement.get(), 2, terminal ? "failed" : "retry");
  sqlite3_bind_int(statement.get(), 3, attempts);
  bind_text(statement.get(), 4, error.substr(0, 4000));
  sqlite3_bind_int64(statement.get(), 5, delay);
  statement.done();
}

void Registry::defer_gif(const gdupe::RemoteObject &object,
                         const std::filesystem::path &local_path,
                         const std::string &reason) {
  if (object.extension != "gif")
    throw std::runtime_error("Only GIF objects may be deferred as malformed GIFs");
  if (local_path.empty())
    throw std::runtime_error("Deferred GIF local path is empty");
  std::scoped_lock lock(mutex_);
  Transaction transaction(db_);
  if (!identity_matches(db_, object.key, object.file_id, object.size, object.sha1))
    throw std::runtime_error("Object changed while malformed GIF was being deferred");
  Statement statement(db_, R"SQL(
INSERT INTO deferred_gifs(file_id,local_path,reason,deferred_at)
VALUES(?,?,?,unixepoch())
ON CONFLICT(file_id) DO UPDATE SET local_path=excluded.local_path,
  reason=excluded.reason,deferred_at=unixepoch()
)SQL");
  bind_text(statement.get(), 1, object.file_id);
  bind_text(statement.get(), 2, local_path.string());
  bind_text(statement.get(), 3, reason.substr(0, 4000));
  statement.done();
  Statement clear(db_, "DELETE FROM failures WHERE file_id=?");
  bind_text(clear.get(), 1, object.file_id);
  clear.done();
  transaction.commit();
}

void Registry::clear_failure(const std::string &file_id) {
  std::scoped_lock lock(mutex_);
  Statement statement(db_, "DELETE FROM failures WHERE file_id=?");
  bind_text(statement.get(), 1, file_id);
  statement.done();
}

RegistryStatus Registry::status() const {
  std::scoped_lock lock(mutex_);
  RegistryStatus result;
  Statement objects(db_, R"SQL(
SELECT o.key,o.current_file_id,o.extension,COALESCE(f.state,''),
       CASE WHEN d.file_id IS NULL THEN 0 ELSE 1 END
FROM objects o LEFT JOIN failures f ON f.file_id=o.current_file_id
LEFT JOIN deferred_gifs d ON d.file_id=o.current_file_id
WHERE o.present=1
)SQL");
  while (sqlite3_step(objects.get()) == SQLITE_ROW) {
    ++result.inventory_objects;
    const std::string file_id = column_text(objects.get(), 1);
    const std::string extension = column_text(objects.get(), 2);
    const std::string failure = column_text(objects.get(), 3);
    const bool deferred_gif = sqlite3_column_int(objects.get(), 4) != 0;
    if (!supported_extension(extension) || failure == "unsupported") {
      ++result.unsupported;
      continue;
    }
    if (deferred_gif) {
      ++result.deferred_gifs;
      continue;
    }
    if (failure == "failed")
      ++result.failed;
    const std::size_t missing = missing_count(db_, file_id, extension);
    if (missing == 0)
      ++result.fully_fingerprinted;
    else {
      ++result.pending_objects;
      result.pending_components += missing;
    }
  }
  Statement metadata_statement(db_,
                               "SELECT key,value FROM metadata WHERE key IN "
                               "('last_successful_scan','currently_processing')");
  while (sqlite3_step(metadata_statement.get()) == SQLITE_ROW) {
    const std::string key = column_text(metadata_statement.get(), 0);
    if (key == "last_successful_scan")
      result.last_successful_scan = column_text(metadata_statement.get(), 1);
    else
      result.currently_processing = column_text(metadata_statement.get(), 1);
  }
  return result;
}

void Registry::set_metadata(const std::string &key, const std::string &value) {
  std::scoped_lock lock(mutex_);
  Statement statement(db_, R"SQL(
INSERT INTO metadata(key,value) VALUES(?,?)
ON CONFLICT(key) DO UPDATE SET value=excluded.value
)SQL");
  bind_text(statement.get(), 1, key);
  bind_text(statement.get(), 2, value);
  statement.done();
}

std::string Registry::metadata(const std::string &key) const {
  std::scoped_lock lock(mutex_);
  Statement statement(db_, "SELECT value FROM metadata WHERE key=?");
  bind_text(statement.get(), 1, key);
  return sqlite3_step(statement.get()) == SQLITE_ROW
             ? column_text(statement.get(), 0)
             : std::string{};
}

} // namespace gparty::fingerprints
