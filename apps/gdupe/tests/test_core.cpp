#include "config.hpp"
#include "crypto_hash.hpp"
#include "database.hpp"
#include "fingerprint.hpp"
#include "matcher.hpp"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> result;
  unsigned int accumulator = 0;
  int bits = 0;
  for (const char character : text) {
    if (character == '=')
      break;
    const auto position = alphabet.find(character);
    if (position == std::string_view::npos)
      continue;
    accumulator = (accumulator << 6U) | static_cast<unsigned int>(position);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return result;
}

class TempDirectory {
public:
  TempDirectory() {
    const auto token = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    path_ = std::filesystem::temp_directory_path() / ("gdupe-test-" + token);
    if (!std::filesystem::create_directory(path_))
      throw std::runtime_error("could not create test directory");
  }
  ~TempDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_fixture(const std::filesystem::path &path, std::string_view base64) {
  const auto bytes = base64_decode(base64);
  require(!bytes.empty(), "embedded fixture did not decode from base64");
  std::ofstream output(path, std::ios::binary);
  require(output.good(), "could not create embedded fixture");
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(output.good(), "could not write embedded fixture");
}

void test_crypto_hashes() {
  require(gdupe::sha1("abc") ==
              "a9993e364706816aba3e25717850c26c9cd0d89d",
          "Windows CNG SHA-1 returned the wrong digest");
  require(gdupe::sha256("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "Windows CNG SHA-256 returned the wrong digest");
  TempDirectory directory;
  const auto path = directory.path() / "hash.txt";
  {
    std::ofstream output(path, std::ios::binary);
    output << "abc";
  }
  require(gdupe::sha1_file(path) ==
              "a9993e364706816aba3e25717850c26c9cd0d89d" &&
              gdupe::sha256_file(path) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "Windows CNG file hashing returned the wrong digest");
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
  TempDirectory directory;
  const auto original_path = directory.path() / "original.png";
  const auto resized_path = directory.path() / "resized.png";
  write_fixture(original_path,
                "iVBORw0KGgoAAAANSUhEUgAAABAAAAAMCAIAAADkharWAAAAMklEQVR42mO8c+cOA27g5OSEJsLEQCKgvQYW/NI2NjbI3CNHjgxCP5CsgVFOTm6QOQkA/lQGzdRVa7QAAAAASUVORK5CYII=");
  write_fixture(resized_path,
                "iVBORw0KGgoAAAANSUhEUgAAACAAAAAYCAIAAAAUMWhjAAAAWUlEQVR42mO8c+cOAwXAyckJvwImBhqDUQtGgAUsFOq3sbHBFDxy5Ah1fFBXV0fQViaqm45mx2gqGrVgIC1oamrCIwvPaxT5AJcdyDmZUU5ObjSSRy2gDAAAOwQS5Xwxv6cAAAAASUVORK5CYII=");
  gdupe::Config config;
  const gdupe::Fingerprinter fingerprinter(config);
  const auto first = fingerprinter.compute(original_path, "png");
  const auto second = fingerprinter.compute(resized_path, "png");
  require(gdupe::Fingerprinter::hamming(first.phash, second.phash) <= 10,
          "pHash should tolerate resizing");
  require(first.perceptual256.size() == 32,
          "perceptual hash must be 256 bits");
  require(first.crop_hashes.size() == 7,
          "crop-aware fingerprint set is incomplete");

  const auto jpeg_path = directory.path() / "static.jpg";
  const auto webp_path = directory.path() / "static.webp";
  write_fixture(jpeg_path,
                "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAwAEADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD85K+hP+GAfj1/0In/AJWLD/4/Xz3X9CFAH45/8MA/Hr/oRP8AysWH/wAfo/4YB+PX/Qif+Viw/wDj9fsZRQB+Of8AwwD8ev8AoRP/ACsWH/x+vPfi18APHvwM/sr/AITfQf7E/tTzfsf+mW9x5vlbPM/1Uj4x5idcZzxnBr9zq/Pf/grP/wA0r/7iv/tnQB+e9FFFABX9CFfz31/QhQAUUUUAFfCP/BTnQhr+s/CeB2UQxjVZZQc/MoNkCox65A9q+7q+Ef8Agpzro0DWfhPO6qYZBqsUpOflUmyJYY9MA+9AHyvZ2cOn20dvbxrFDGMKi9v8+tec/EHwvBpXlX9mqQwSN5bwrnhzlsjtjAxgYxgevHo1neQ6hbR3FvIssMgyrr3/AM+lec/EHxRBqvlWFmyTQRt5jzLnlxlcDtjBzkZzkenIBxdf0IV/PfRQB/QhRX899FAH9CFfnv8A8FZ/+aV/9xX/ANs6/PeigAooooA//9k=");
  write_fixture(webp_path,
                "UklGRqgAAABXRUJQVlA4IJwAAAAQBgCdASpAADAAPi0Sh0KhoQ6szgAMAWJaQAD4KPQDiRJjhuijw7oWNA/vTpcJd6A2mGAPLleTmcAA/v/ldn/8SP//ULmf/0egmv4axR1SUQpN/1StzkIebYPmyJkOhttaKlJvp3k46/+T42k7L2chlvDbnqAyXRPPCdrl8AKQoQjzD8XjcBIVYMhl3ltTx6ajBnyA+QSwKzOAAAA=");
  for (const auto &[path, extension] :
       {std::pair{jpeg_path, "jpg"}, std::pair{webp_path, "webp"}}) {
    const auto decoded = fingerprinter.compute(path, extension);
    require(decoded.width == 64 && decoded.height == 48,
            "static image decoder returned incorrect dimensions");
  }
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

void test_static_h264_mp4_decode() {
  TempDirectory directory;
  const auto video = directory.path() / "sample.mp4";

  // A tiny two-frame H.264/MP4 fixture exercises the static MP4 demux and AVC
  // decoder path end-to-end without relying on any external media framework.
  write_fixture(video,
      "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMobW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAAfQAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAlN0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAAfQAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAH0AAAAAAABAAAAAAHLbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAIABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABdm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAATZzdGJsAAAAtnN0c2QAAAAAAAAAAQAAAKZhdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAABAAEABIAAAASAAAAAAAAAABFUxhdmM2MS4xOS4xMDEgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAALGF2Y0MBQsAK/+EAFWdCwAraewEQAAADABAAAAMAiPEiagEABGjOD8gAAAAQcGFzcAAAAAEAAAABAAAAFGJ0cnQAAAAAAAAncAAAAAAAAAAYc3R0cwAAAAAAAAABAAAAAgAAEAAAAAAUc3RzcwAAAAAAAAABAAAAAQAAABxzdHNjAAAAAAAAAAEAAAABAAAAAgAAAAEAAAAcc3RzegAAAAAAAAAAAAAAAgAAAm4AAAAJAAAAFHN0Y28AAAAAAAAAAQAAA1gAAABhdWR0YQAAAFltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABtZGlyYXBwbAAAAAAAAAAAAAAAACxpbHN0AAAAJKl0b28AAAAcZGF0YQAAAAEAAAAATGF2ZjYxLjcuMTAzAAAACGZyZWUAAAJ/bWRhdAAAAlMGBf//T9xF6b3m2Ui3lizYINkj7u94MjY0IC0gY29yZSAxNjQgcjMxMDggMzFlMTlmOSAtIEguMjY0L01QRUctNCBBVkMgY29kZWMgLSBDb3B5bGVmdCAyMDAzLTIwMjMgLSBodHRwOi8vd3d3LnZpZGVvbGFuLm9yZy94MjY0Lmh0bWwgLSBvcHRpb25zOiBjYWJhYz0wIHJlZj0xIGRlYmxvY2s9MDowOjAgYW5hbHlzZT0wOjAgbWU9ZGlhIHN1Ym1lPTAgcHN5PTEgcHN5X3JkPTEuMDA6MC4wMiBtaXhlZF9yZWY9MCBtZV9yYW5nZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTAgOHg4ZGN0PTAgcWNtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0xIGNocm9tYV9xcF9vZmZzZXQ9MCB0aHJlYWRzPTEgbG9va2FoZWFkX3RocmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MjUwIGtleWludF9taW49NCBzY2VuZWN1dD0wIGludHJhX3JlZnJlc2g9MCByYz1jcmYgbWJ0cmVlPTAgY3JmPTIzLjAgcWNvbXA9MC42MCBxcG1pbj0wIHFwbWF4PTY5IHFwc3RlcD00IGlwX3JhdGlvPTEuNDAgYXE9MACAAAAAE2WIhDoRigACGPHAAED2OAAIeWAAAAAFQZogEKU=");

  gdupe::Config config;
  config.cache_directory = directory.path();
  config.video_sample_frames = 4;
  const auto result = gdupe::Fingerprinter(config).compute(video, "mp4");
  require(result.kind == gdupe::MediaKind::Video && result.width == 16 &&
              result.height == 16 && result.duration_ms >= 400 &&
              result.timeline.size() >= 2,
          "static H.264/MP4 fingerprinting returned incomplete metadata");
}

} // namespace

int main() {
  try {
    test_crypto_hashes();
    test_hashes();
    test_database();
    test_consolidated_process_all();
    test_crop_and_excerpt_matching();
    test_index_threshold_boundaries();
    test_static_h264_mp4_decode();
    std::cout << "gdupe core tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "gdupe core test failure: " << problem.what() << '\n';
    return 1;
  }
}
