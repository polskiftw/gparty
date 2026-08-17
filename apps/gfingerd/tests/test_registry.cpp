#include "registry.hpp"

#include "database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fp = gparty::fingerprints;

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

gdupe::RemoteObject object(std::string key, std::string file_id,
                           std::string extension) {
  return {std::move(key), std::move(file_id), 1234,
          "0123456789abcdef0123456789abcdef01234567",
          "application/octet-stream", std::move(extension), 100};
}

gdupe::Fingerprint fingerprint() {
  gdupe::Fingerprint value;
  value.version = fp::kFingerprintVersion;
  value.kind = gdupe::MediaKind::StaticImage;
  value.sha256 =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  value.width = 640;
  value.height = 480;
  value.duration_ms = 0;
  value.frame_count = 1;
  value.phash = 0x123456789abcdef0ULL;
  value.perceptual256.fill(0x5a);
  value.crop_hashes = {1, 2, 3};
  return value;
}

} // namespace

int main() {
  try {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("gfingerd-registry-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto database_path = root / "gdupe.sqlite3";

    const auto still = object("gallery/still.jpg", "file-still-v1", "jpg");
    const auto gif = object("gallery/odd.gif", "file-gif-v1", "gif");
    const auto unsupported =
        object("gallery/other.bin", "file-other-v1", "bin");

    fp::Registry registry(database_path);
    registry.reconcile({still, gif, unsupported});
    require(registry.pending().size() == 2,
            "supported unfingerprinted objects must be pending");

    registry.save_fingerprint(still, fingerprint());
    {
      gdupe::Database gdupe_database(database_path);
      const auto row = gdupe_database.object(still.key);
      require(row && row->fingerprint,
              "gfingerd must write the fingerprint into gdupe's objects row");
      require(row->remote.file_id == still.file_id,
              "the saved gdupe row must retain the exact B2 file ID");
      require(row->fingerprint->version == fp::kFingerprintVersion,
              "the saved fingerprint version must match gdupe v3");
    }

    const auto deferred_copy = root / "odd.gif";
    registry.defer_gif(gif, deferred_copy,
                       "GIF frame rectangle is outside its logical canvas");
    auto status = registry.status();
    require(status.inventory_objects == 3, "inventory count mismatch");
    require(status.fully_fingerprinted == 1, "complete count mismatch");
    require(status.deferred_gifs == 1, "deferred GIF count mismatch");
    require(status.unsupported == 1, "unsupported count mismatch");
    require(status.pending_objects == 0,
            "deferred and unsupported objects must not be pending");

    auto replacement = gif;
    replacement.file_id = "file-gif-v2";
    replacement.upload_timestamp = 200;
    registry.reconcile({still, replacement, unsupported});
    status = registry.status();
    require(status.deferred_gifs == 0,
            "a replacement file ID must not inherit an old GIF deferral");
    require(status.pending_objects == 1,
            "replacement GIF must return to normal pending work");

    registry.record_failure(replacement, "decoder failure", 1);
    status = registry.status();
    require(status.failed == 1, "terminal failure count mismatch");
    require(status.pending_objects == 0,
            "terminal failures must not also be counted as pending");

    replacement.file_id = "file-gif-v3";
    replacement.upload_timestamp = 300;
    registry.reconcile({still, replacement, unsupported});
    status = registry.status();
    require(status.failed == 0,
            "a replacement file ID must not inherit a failure state");
    require(status.pending_objects == 1,
            "replacement after failure must be pending");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "gfingerd registry tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "gfingerd registry test failed: " << problem.what() << '\n';
    return 1;
  }
}
