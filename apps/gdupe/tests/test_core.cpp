#include "config.hpp"
#include "database.hpp"
#include "fingerprint.hpp"
#include "matcher.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

gdupe::Fingerprint fingerprint(std::uint64_t phash, int width, int height,
                               const std::string &sha) {
  gdupe::Fingerprint value;
  value.version = 1;
  value.kind = gdupe::MediaKind::StaticImage;
  value.sha256 = sha;
  value.width = width;
  value.height = height;
  value.frame_count = 1;
  value.phash = phash;
  value.crop_hashes = {phash};
  return value;
}

gdupe::InventoryObject object(std::string key, std::string id,
                              std::uint64_t size, gdupe::Fingerprint value) {
  return {{std::move(key), std::move(id), size, std::string(40, 'a'),
           "image/jpeg", "jpg"},
          std::move(value)};
}

void test_hashes() {
  cv::Mat image(300, 500, CV_8UC3, cv::Scalar(22, 32, 42));
  cv::rectangle(image, {80, 40, 260, 210}, cv::Scalar(230, 210, 180), -1);
  cv::circle(image, {330, 180}, 54, cv::Scalar(40, 80, 220), -1);
  cv::Mat resized;
  cv::resize(image, resized, {800, 480}, 0, 0, cv::INTER_CUBIC);
  const auto first = gdupe::Fingerprinter::perceptual_hash(image);
  const auto second = gdupe::Fingerprinter::perceptual_hash(resized);
  require(gdupe::Fingerprinter::hamming(first, second) <= 10,
          "pHash should tolerate resizing");
  const auto gradient = gdupe::Fingerprinter::gradient_hash(image);
  require(gradient.size() == 32, "gradient hash must be 256 bits");
  require(gdupe::Fingerprinter::crop_hashes(image).size() >= 7,
          "crop-aware fingerprint set is incomplete");
}

void test_database() {
  const auto path =
      std::filesystem::temp_directory_path() / "gdupe-core-test.sqlite3";
  std::filesystem::remove(path);
  {
    gdupe::Database db(path);
    std::vector<gdupe::RemoteObject> remote = {
        {"gallery/a.jpg", "id-a", 100, std::string(40, 'a'), "image/jpeg",
         "jpg"},
        {"gallery/b.jpg", "id-b", 90, std::string(40, 'b'), "image/jpeg",
         "jpg"}};
    db.reconcile_inventory(remote, 1);
    db.save_fingerprint("gallery/a.jpg", "id-a",
                        fingerprint(7, 100, 100, "sha-a"));
    require(db.object("gallery/a.jpg")->fingerprint.has_value(),
            "fingerprint did not round-trip");
    db.exclude_pair("gallery/b.jpg", "gallery/a.jpg");
    require(db.exclusions().contains({"gallery/a.jpg", "gallery/b.jpg"}),
            "pair exclusion is not durable");
    gdupe::Operation operation{"op-1", "manual_delete", "gallery/b.jpg",
                               "id-b", "prepared",      {}};
    db.prepare_operations({operation});
    db.update_operation("op-1", "remote_deleted");
    require(db.pending_operations().front().state == "remote_deleted",
            "operation journal did not advance");
    db.complete_operations({"op-1"});
    require(db.pending_operations().empty(),
            "completed operation remained pending");
    remote.erase(remote.begin() + 1);
    db.reconcile_inventory(remote, 1);
    require(!db.object("gallery/b.jpg"),
            "missing remote object remained in inventory");
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

void test_consolidated_process_all() {
  gdupe::Config config;
  gdupe::Matcher matcher(config);
  auto a = object("gallery/a.jpg", "a", 1000, fingerprint(1, 1600, 1200, "1"));
  auto b = object("gallery/b.jpg", "b", 900, fingerprint(1, 1200, 900, "2"));
  auto c = object("gallery/c.jpg", "c", 800, fingerprint(1, 1000, 800, "3"));
  std::vector<gdupe::InventoryObject> inventory = {a, b, c};
  std::vector<gdupe::CandidateEdge> edges = {
      {"gallery/a.jpg", "gallery/b.jpg", .95, "near"},
      {"gallery/b.jpg", "gallery/c.jpg", .94, "near"}};
  const auto deletions = matcher.process_all_deletions(inventory, edges);
  require(deletions.size() == 1 && deletions.front() == "gallery/b.jpg",
          "Process All must not delete a transitive-only alternative");
  const auto queue = matcher.build_queue(inventory, edges, 4);
  require(queue.front().left.remote.key == "gallery/a.jpg",
          "recommended survivor must be shown on the left");
}

} // namespace

int main() {
  try {
    test_hashes();
    test_database();
    test_consolidated_process_all();
    std::cout << "gdupe core tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe core test failure: " << problem.what() << '\n';
    return 1;
  }
}
