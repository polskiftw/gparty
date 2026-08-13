#include "config.hpp"
#include "database.hpp"
#include "fingerprint.hpp"
#include "matcher.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <QByteArray>
#include <QCoreApplication>
#include <QTemporaryDir>
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
  const auto hash256 = gdupe::Fingerprinter::perceptual_hash256(image);
  require(hash256.size() == 32, "perceptual hash must be 256 bits");
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
    remote[1].file_id = "id-b2";
    db.reconcile_inventory(remote, 1);
    require(db.exclusions().empty(),
            "an exclusion survived a changed B2 object version");
    gdupe::Operation operation{"op-1",  "manual_delete", "gallery/b.jpg",
                               "id-b2", "prepared",      {}};
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
  require(queue.size() == 1 && queue.front().left.remote.key == "gallery/a.jpg",
          "recommended survivor must be shown on the left");
}

void test_crop_and_excerpt_matching() {
  gdupe::Config config;
  gdupe::Matcher matcher(config);

  auto original = object("gallery/original.jpg", "original", 2000,
                         fingerprint(0, 1800, 1200, "crop-a"));
  auto cropped = object("gallery/cropped.jpg", "cropped", 1200,
                        fingerprint(0xffff, 900, 700, "crop-b"));
  original.fingerprint->crop_hashes = {0xffff};
  cropped.fingerprint->crop_hashes = {0xaaaaaaaaaaaaaaaaULL};
  auto crop_edges = matcher.find_candidates({original, cropped}, {});
  require(crop_edges.size() == 1 &&
              crop_edges.front().evidence == "convincing crop or reframe",
          "full-to-crop matching did not identify a convincing crop");

  auto short_video = object("gallery/short.mp4", "short", 1000,
                            fingerprint(0, 1280, 720, "video-a"));
  auto long_video = object("gallery/long.mp4", "long", 2000,
                           fingerprint(0, 1280, 720, "video-b"));
  short_video.remote.extension = "mp4";
  long_video.remote.extension = "mp4";
  short_video.fingerprint->kind = gdupe::MediaKind::Video;
  long_video.fingerprint->kind = gdupe::MediaKind::Video;
  short_video.fingerprint->duration_ms = 6000;
  long_video.fingerprint->duration_ms = 10000;
  long_video.fingerprint->timeline.resize(48);
  for (std::size_t i = 0; i < 48; ++i)
    long_video.fingerprint->timeline[i] = std::uint64_t{1} << (i % 32);
  short_video.fingerprint->timeline.resize(48);
  for (std::size_t i = 0; i < 48; ++i) {
    const std::size_t mapped =
        10 + static_cast<std::size_t>(std::llround(i * 28.0 / 47.0));
    short_video.fingerprint->timeline[i] =
        long_video.fingerprint->timeline[mapped];
  }
  auto video_edges = matcher.find_candidates({short_video, long_video}, {});
  require(video_edges.size() == 1 &&
              video_edges.front().evidence ==
                  "strongly matched moving-media excerpt",
          "duration-aware timeline matching did not identify an excerpt");
}

void test_index_threshold_boundaries() {
  gdupe::Config config;
  config.static_phash_distance = 11;
  gdupe::Matcher matcher(config);
  auto first = object("gallery/index-a.jpg", "index-a", 1000,
                      fingerprint(0, 1000, 1000, "index-a"));
  const std::uint64_t static_boundary =
      0x7ULL | (0x7ULL << 16U) | (0x7ULL << 32U) | (0x3ULL << 48U);
  auto second = object("gallery/index-b.jpg", "index-b", 900,
                       fingerprint(static_boundary, 900, 900, "index-b"));
  const auto edges = matcher.find_candidates({first, second}, {});
  require(edges.size() == 1,
          "multi-index omitted a pair at the configured pHash boundary");

  config.moving_phash_distance = 15;
  gdupe::Matcher moving_matcher(config);
  auto moving_first = object("gallery/index-a.mp4", "moving-a", 1000,
                             fingerprint(0, 1280, 720, "moving-a"));
  const std::uint64_t moving_boundary =
      0xfULL | (0xfULL << 16U) | (0xfULL << 32U) | (0x7ULL << 48U);
  auto moving_second =
      object("gallery/index-b.mp4", "moving-b", 900,
             fingerprint(moving_boundary, 1280, 720, "moving-b"));
  for (auto *item : {&moving_first, &moving_second}) {
    item->remote.extension = "mp4";
    item->fingerprint->kind = gdupe::MediaKind::Video;
    item->fingerprint->duration_ms = 10'000;
    item->fingerprint->timeline = {1, 2, 4, 8};
  }
  const auto moving_edges =
      moving_matcher.find_candidates({moving_first, moving_second}, {});
  require(moving_edges.size() == 1,
          "multi-index omitted moving media at its configured boundary");
}

void test_external_ffmpeg() {
  const char *ffmpeg = std::getenv("GDUPE_TEST_FFMPEG");
  const char *ffprobe = std::getenv("GDUPE_TEST_FFPROBE");
  if (ffmpeg == nullptr || ffprobe == nullptr)
    return;

  QTemporaryDir directory;
  require(directory.isValid(), "could not create FFmpeg test directory");
  const QString video = directory.filePath("sample.mp4");

  // A tiny two-frame H.264/MP4 fixture is embedded instead of asking the
  // runtime under test to encode its own fixture. That keeps the production
  // FFmpeg build decode-only except for the PNG frame encoder gdupe uses.
  const QByteArray fixture = QByteArray::fromBase64(QByteArray(
      "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMobW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAAfQAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAlN0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAAfQAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAH0AAAAAAABAAAAAAHLbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAIABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABdm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAATZzdGJsAAAAtnN0c2QAAAAAAAAAAQAAAKZhdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAABAAEABIAAAASAAAAAAAAAABFUxhdmM2MS4xOS4xMDEgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAALGF2Y0MBQsAK/+EAFWdCwAraewEQAAADABAAAAMAiPEiagEABGjOD8gAAAAQcGFzcAAAAAEAAAABAAAAFGJ0cnQAAAAAAAAncAAAAAAAAAAYc3R0cwAAAAAAAAABAAAAAgAAEAAAAAAUc3RzcwAAAAAAAAABAAAAAQAAABxzdHNjAAAAAAAAAAEAAAABAAAAAgAAAAEAAAAcc3RzegAAAAAAAAAAAAAAAgAAAm4AAAAJAAAAFHN0Y28AAAAAAAAAAQAAA1gAAABhdWR0YQAAAFltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABtZGlyYXBwbAAAAAAAAAAAAAAAACxpbHN0AAAAJKl0b28AAAAcZGF0YQAAAAEAAAAATGF2ZjYxLjcuMTAzAAAACGZyZWUAAAJ/bWRhdAAAAlMGBf//T9xF6b3m2Ui3lizYINkj7u94MjY0IC0gY29yZSAxNjQgcjMxMDggMzFlMTlmOSAtIEguMjY0L01QRUctNCBBVkMgY29kZWMgLSBDb3B5bGVmdCAyMDAzLTIwMjMgLSBodHRwOi8vd3d3LnZpZGVvbGFuLm9yZy94MjY0Lmh0bWwgLSBvcHRpb25zOiBjYWJhYz0wIHJlZj0xIGRlYmxvY2s9MDowOjAgYW5hbHlzZT0wOjAgbWU9ZGlhIHN1Ym1lPTAgcHN5PTEgcHN5X3JkPTEuMDA6MC4wMiBtaXhlZF9yZWY9MCBtZV9yYW5nZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTAgOHg4ZGN0PTAgcWNtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0xIGNocm9tYV9xcF9vZmZzZXQ9MCB0aHJlYWRzPTEgbG9va2FoZWFkX3RocmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MjUwIGtleWludF9taW49NCBzY2VuZWN1dD0wIGludHJhX3JlZnJlc2g9MCByYz1jcmYgbWJ0cmVlPTAgY3JmPTIzLjAgcWNvbXA9MC42MCBxcG1pbj0wIHFwbWF4PTY5IHFwc3RlcD00IGlwX3JhdGlvPTEuNDAgYXE9MACAAAAAE2WIhDoRigACGPHAAED2OAAIeWAAAAAFQZogEKU="));
  require(!fixture.isEmpty(), "embedded H.264 fixture did not decode from base64");
  {
    std::ofstream output(std::filesystem::path(video.toStdWString()),
                         std::ios::binary);
    require(output.good(), "could not create embedded FFmpeg fixture");
    output.write(fixture.constData(),
                 static_cast<std::streamsize>(fixture.size()));
    require(output.good(), "could not write embedded FFmpeg fixture");
  }

  gdupe::Config config;
  config.ffmpeg_path = ffmpeg;
  config.ffprobe_path = ffprobe;
  config.cache_directory =
      std::filesystem::path(directory.path().toStdWString());
  config.video_sample_frames = 4;
  const auto result = gdupe::Fingerprinter(config).compute(
      std::filesystem::path(video.toStdWString()), "mp4");
  require(result.kind == gdupe::MediaKind::Video && result.width == 16 &&
              result.height == 16 && result.duration_ms >= 400 &&
              result.timeline.size() >= 2,
          "external FFmpeg fingerprinting returned incomplete metadata");
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    test_hashes();
    test_database();
    test_consolidated_process_all();
    test_crop_and_excerpt_matching();
    test_index_threshold_boundaries();
    test_external_ffmpeg();
    std::cout << "gdupe core tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe core test failure: " << problem.what() << '\n';
    return 1;
  }
}
