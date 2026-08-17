#pragma once

#include <filesystem>
#include <string>

namespace gparty::fingerprints {

struct Config {
  std::string bucket_name{"gooning-party-media-b2"};
  std::string canonical_prefix{"gallery/"};
  int maximum_attempts{6};
  int polling_seconds{600};
  int worker_threads{2};
  int download_connections{4};
  int prefetch_files{8};
  int maximum_item_attempts{5};
  std::filesystem::path database_path;
  std::filesystem::path cache_directory;
  std::filesystem::path legacy_gdupe_database;
  std::filesystem::path log_path;
  int fingerprint_version{3};
  int video_sample_frames{48};
  int gif_sample_frames{32};
  std::string key_id;
  std::string application_key;

  static Config defaults();
  static Config load(const std::filesystem::path &path);
  static Config load_user_or_defaults();
  void save_user() const;
  void validate() const;
};

std::filesystem::path user_config_path();

} // namespace gparty::fingerprints
