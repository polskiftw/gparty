#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gdupe {

enum class MediaKind : std::uint8_t {
  StaticImage = 1,
  AnimatedImage = 2,
  Video = 3
};

struct RemoteObject {
  std::string key;
  std::string file_id;
  std::uint64_t size{};
  std::string sha1;
  std::string content_type;
  std::string extension;
  std::int64_t upload_timestamp{};

  auto identity() const -> std::string {
    return file_id + ":" + std::to_string(size) + ":" + sha1;
  }
};

struct Fingerprint {
  int version{};
  MediaKind kind{MediaKind::StaticImage};
  std::string sha256;
  int width{};
  int height{};
  std::int64_t duration_ms{};
  std::int64_t frame_count{};
  std::uint64_t phash{};
  std::array<std::uint8_t, 32> perceptual256{};
  std::vector<std::uint64_t> crop_hashes;
  std::vector<std::uint64_t> timeline;
};

struct InventoryObject {
  RemoteObject remote;
  std::optional<Fingerprint> fingerprint;
};

struct CandidateEdge {
  std::string a;
  std::string b;
  double score{};
  std::string evidence;
};

struct ReviewPair {
  InventoryObject left;
  InventoryObject right;
  double score{};
  std::string evidence;
  std::uint64_t generation{};
};

struct Operation {
  std::string id;
  std::string kind;
  std::string target_key;
  std::string target_file_id;
  std::string state;
  std::string error;
};

struct StartupSummary {
  std::size_t inventory_count{};
  std::size_t reused_fingerprints{};
  std::size_t computed_fingerprints{};
  std::size_t exact_deleted{};
  std::size_t review_candidates{};
};

} // namespace gdupe
