#pragma once

#include <filesystem>
#include <string>

namespace gdupe {

struct Config {
  std::string bucket_name;
  std::string canonical_prefix{"gallery/"};
  std::string canonical_index_key{"_internal/gdupe/canonical-index-v1.json"};
  int maximum_attempts{6};
  std::filesystem::path database_path;
  std::filesystem::path cache_directory;
  bool keep_media_cache{false};
  int fingerprint_version{3};
  int fingerprint_threads{4};
  int video_sample_frames{48};
  int gif_sample_frames{32};
  double minimum_moving_overlap{0.60};
  int static_phash_distance{10};
  int static_hash256_distance{48};
  int crop_phash_distance{7};
  int crop_hash256_distance{64};
  int moving_phash_distance{12};
  double moving_timeline_distance{12.0};
  double minimum_score{0.76};
  int worker_threads{0};
  double resolution_weight{1.0};
  double duration_weight{0.9};
  double lossless_bonus{0.12};
  double size_weight{0.08};
  std::string key_id;
  std::string application_key;

  static Config load(const std::filesystem::path &path);
  void validate() const;
};

std::filesystem::path
default_config_path(const std::filesystem::path &executable);

} // namespace gdupe
