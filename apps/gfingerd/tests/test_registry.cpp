#include "component_versions.hpp"
#include "registry.hpp"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

class TempDirectory {
public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("gparty-fingerprint-registry-test-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void sql(sqlite3 *db, const char *text) {
  char *error = nullptr;
  if (sqlite3_exec(db, text, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

std::string bytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void create_legacy(const std::filesystem::path &path) {
  sqlite3 *db = nullptr;
  require(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK,
          "could not create legacy database");
  sql(db, R"SQL(
CREATE TABLE objects(
 key TEXT PRIMARY KEY,file_id TEXT,size INTEGER,sha1 TEXT,fp_version INTEGER,
 media_kind INTEGER,sha256 TEXT,width INTEGER,height INTEGER,duration_ms INTEGER,
 frame_count INTEGER,phash INTEGER,perceptual256 BLOB,crop_hashes BLOB,timeline BLOB
);
)SQL");
  sqlite3_stmt *statement = nullptr;
  require(sqlite3_prepare_v2(
              db,
              "INSERT INTO objects VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1,
              &statement, nullptr) == SQLITE_OK,
          "could not prepare legacy row");
  const std::array<std::uint8_t, 32> hash256{1, 2, 3, 4};
  const std::array<std::uint64_t, 7> crops{1, 2, 3, 4, 5, 6, 7};
  sqlite3_bind_text(statement, 1, "gallery/a.jpg", -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 2, "file-a", -1, SQLITE_STATIC);
  sqlite3_bind_int64(statement, 3, 100);
  sqlite3_bind_text(statement, 4, "1111111111111111111111111111111111111111", -1,
                    SQLITE_STATIC);
  sqlite3_bind_int(statement, 5,
                   gparty::fingerprints::kCompatibleGdupeFingerprintVersion);
  sqlite3_bind_int(statement, 6,
                   static_cast<int>(gdupe::MediaKind::StaticImage));
  sqlite3_bind_text(statement, 7,
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    -1, SQLITE_STATIC);
  sqlite3_bind_int(statement, 8, 640);
  sqlite3_bind_int(statement, 9, 480);
  sqlite3_bind_int64(statement, 10, 0);
  sqlite3_bind_int64(statement, 11, 1);
  sqlite3_bind_int64(statement, 12, 42);
  sqlite3_bind_blob(statement, 13, hash256.data(),
                    static_cast<int>(hash256.size()),
                    SQLITE_STATIC);
  sqlite3_bind_blob(statement, 14, crops.data(),
                    static_cast<int>(sizeof(crops)), SQLITE_STATIC);
  sqlite3_bind_blob(statement, 15, nullptr, 0, SQLITE_STATIC);
  require(sqlite3_step(statement) == SQLITE_DONE,
          "could not insert complete legacy row");
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);
  sqlite3_bind_text(statement, 1, "gallery/b.mp4", -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 2, "file-moving", -1, SQLITE_STATIC);
  sqlite3_bind_int64(statement, 3, 200);
  sqlite3_bind_text(statement, 4, "2222222222222222222222222222222222222222", -1,
                    SQLITE_STATIC);
  sqlite3_bind_int(statement, 5,
                   gparty::fingerprints::kCompatibleGdupeFingerprintVersion);
  sqlite3_bind_int(statement, 6,
                   static_cast<int>(gdupe::MediaKind::Video));
  sqlite3_bind_text(statement, 7,
                    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    -1, SQLITE_STATIC);
  require(sqlite3_step(statement) == SQLITE_DONE,
          "could not insert partial legacy row");
  sqlite3_finalize(statement);
  sqlite3_close(db);
}

gdupe::RemoteObject object(std::string file_id, std::string extension = "jpg") {
  return {"gallery/a." + extension,
          std::move(file_id),
          100,
          "1111111111111111111111111111111111111111",
          "image/jpeg",
          std::move(extension),
          1};
}

gdupe::Fingerprint fingerprint() {
  gdupe::Fingerprint value;
  value.version =
      gparty::fingerprints::kCompatibleGdupeFingerprintVersion;
  value.kind = gdupe::MediaKind::StaticImage;
  value.sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  value.width = 800;
  value.height = 600;
  value.frame_count = 1;
  value.phash = 99;
  value.perceptual256[0] = 7;
  value.crop_hashes = {1, 2, 3, 4, 5, 6, 7};
  return value;
}

void test_adoption_and_reconciliation() {
  TempDirectory directory;
  const auto registry_path = directory.path() / "fingerprints.sqlite3";
  const auto legacy_path = directory.path() / "gdupe.sqlite3";
  create_legacy(legacy_path);
  const std::string legacy_before = bytes(legacy_path);

  gparty::fingerprints::Registry registry(registry_path);
  const auto first = object("file-a");
  const gdupe::RemoteObject moving{
      "gallery/b.mp4", "file-moving", 200,
      "2222222222222222222222222222222222222222", "video/mp4", "mp4", 2};
  registry.reconcile({first, moving});
  const auto fresh = registry.pending();
  require(fresh.size() == 2 && fresh[0].missing_components == 5 &&
              fresh[1].missing_components == 6,
          "fresh objects did not require their media-specific components");

  const auto adoption = registry.adopt_gdupe_v3(legacy_path);
  require(adoption.rows_scanned == 2 && adoption.matching_rows == 2 &&
              adoption.components_imported == 6,
          "compatible gdupe v3 row was not adopted component by component");
  const auto after_adoption = registry.pending();
  require(after_adoption.size() == 1 &&
              after_adoption.front().remote.file_id == "file-moving" &&
              after_adoption.front().missing_components == 5,
          "partial legacy row did not preserve its compatible component");
  require(bytes(legacy_path) == legacy_before,
          "read-only adoption changed the old gdupe database");

  const auto repeated = registry.adopt_gdupe_v3(legacy_path);
  require(repeated.components_imported == 0 &&
              registry.pending().size() == 1,
          "adoption was not idempotent");

  auto replacement = object("file-b");
  registry.reconcile({replacement});
  require(registry.pending().size() == 1 &&
              registry.pending().front().missing_components == 5,
          "changed object version inherited the old fingerprint");
  registry.save_fingerprint(replacement, fingerprint());
  require(registry.pending().empty(),
          "computed fingerprint was not persisted completely");
  registry.reconcile({replacement});
  require(registry.pending().empty(),
          "unchanged completed object was queued on the second scan");

  registry.reconcile({});
  bool rejected = false;
  try {
    registry.save_fingerprint(replacement, fingerprint());
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  require(rejected, "a disappeared object accepted a stale fingerprint save");
}

void test_targeted_component_regeneration_and_status() {
  TempDirectory directory;
  const auto registry_path = directory.path() / "fingerprints.sqlite3";
  gparty::fingerprints::Registry registry(registry_path);
  auto current = object("file-current");
  registry.reconcile({current});
  registry.save_fingerprint(current, fingerprint());

  sqlite3 *db = nullptr;
  require(sqlite3_open(registry_path.string().c_str(), &db) == SQLITE_OK,
          "could not inspect registry");
  sql(db,
      "UPDATE fingerprint_components SET algorithm_version=999 "
      "WHERE component='phash64'");
  sqlite3_close(db);
  const auto work = registry.pending();
  require(work.size() == 1 && work.front().missing_components == 1,
          "one component version bump invalidated unrelated components");
  registry.save_fingerprint(current, fingerprint());
  require(registry.pending().empty(),
          "targeted component regeneration did not converge");

  auto unsupported = object("file-text", "txt");
  registry.reconcile({current, unsupported});
  const auto status = registry.status();
  require(status.inventory_objects == 2 && status.fully_fingerprinted == 1 &&
              status.unsupported == 1 && status.pending_objects == 0,
          "registry status did not separate unsupported current objects");
}

void test_failure_isolation() {
  TempDirectory directory;
  gparty::fingerprints::Registry registry(directory.path() / "registry.db");
  auto first = object("first");
  auto second = object("second");
  second.key = "gallery/b.jpg";
  registry.reconcile({first, second});
  registry.record_failure(first, "decoder failure", 2);
  require(registry.pending().size() == 1,
          "retry backoff did not leave unrelated backlog runnable");
  registry.record_failure(first, "decoder failure", 2);
  const auto status = registry.status();
  require(status.failed == 1 && status.pending_objects == 2,
          "durable terminal failure accounting is incorrect");
}


void test_malformed_gif_deferral() {
  TempDirectory directory;
  gparty::fingerprints::Registry registry(directory.path() / "registry.db");
  auto gif = object("gif-bad", "gif");
  registry.reconcile({gif});
  require(registry.pending().size() == 1,
          "fresh GIF was not initially pending");
  const auto saved = directory.path() / "deferred-gifs" / "saved.gif";
  registry.defer_gif(gif, saved,
                     "GIF frame rectangle is outside its logical canvas");
  require(registry.pending().empty(),
          "deferred malformed GIF was queued for fingerprinting again");
  auto status = registry.status();
  require(status.deferred_gifs == 1 && status.pending_objects == 0 &&
              status.failed == 0,
          "deferred malformed GIF was not reported as its own status");

  registry.reconcile({gif});
  require(registry.pending().empty() && registry.status().deferred_gifs == 1,
          "unchanged deferred GIF was redownloaded after reconciliation");

  auto replacement = object("gif-fixed-version", "gif");
  registry.reconcile({replacement});
  status = registry.status();
  require(registry.pending().size() == 1 && status.deferred_gifs == 0 &&
              status.pending_objects == 1,
          "new B2 version incorrectly inherited an old GIF deferral");
}

} // namespace

int main() {
  try {
    test_adoption_and_reconciliation();
    test_targeted_component_regeneration_and_status();
    test_failure_isolation();
    test_malformed_gif_deferral();
    std::cout << "fingerprint registry tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "fingerprint registry test failure: " << problem.what()
              << '\n';
    return 1;
  }
}
