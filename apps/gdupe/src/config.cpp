#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

namespace gdupe {
namespace {

std::string required_environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    throw std::runtime_error(
        std::string("Required environment variable is missing: ") + name);
  }
  return value;
}

std::filesystem::path expand_path(std::string value,
                                  const std::filesystem::path &base) {
  const char *local = std::getenv("LOCALAPPDATA");
  const std::string token = "%LOCALAPPDATA%";
  if (value.rfind(token, 0) == 0) {
    if (local == nullptr || std::string(local).empty()) {
      throw std::runtime_error(
          "LOCALAPPDATA is unavailable while expanding a configured path");
    }
    value.replace(0, token.size(), local);
  }
  auto result = std::filesystem::path(value);
  if (result.is_relative())
    result = base / result;
  return result.lexically_normal();
}

template <typename T>
T value_or(const nlohmann::json &object, const char *key, T fallback) {
  return object.contains(key) ? object.at(key).get<T>() : std::move(fallback);
}

} // namespace

Config Config::load(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("Cannot open gdupe configuration: " +
                             path.string());
  nlohmann::json root;
  stream >> root;
  const auto &b2 = root.at("b2");
  const auto &storage = root.at("storage");
  const auto &tools = root.at("tools");
  const auto &fingerprints = root.at("fingerprints");
  const auto &matching = root.at("matching");
  const auto &survivor = root.at("survivor");

  Config config;
  config.bucket_name = b2.at("bucket_name").get<std::string>();
  config.canonical_prefix =
      value_or(b2, "canonical_prefix", config.canonical_prefix);
  config.canonical_index_key =
      value_or(b2, "canonical_index_key", config.canonical_index_key);
  config.maximum_attempts =
      value_or(b2, "maximum_attempts", config.maximum_attempts);
  config.database_path = expand_path(
      storage.at("database_path").get<std::string>(), path.parent_path());
  config.cache_directory = expand_path(
      storage.at("cache_directory").get<std::string>(), path.parent_path());
  config.ffmpeg_path = expand_path(tools.at("ffmpeg_path").get<std::string>(),
                                   path.parent_path());
  config.ffprobe_path = expand_path(tools.at("ffprobe_path").get<std::string>(),
                                    path.parent_path());
  config.keep_media_cache = value_or(storage, "keep_media_cache", false);
  config.fingerprint_version = value_or(fingerprints, "version", 1);
  config.fingerprint_threads = value_or(fingerprints, "worker_threads", 4);
  config.video_sample_frames =
      value_or(fingerprints, "video_sample_frames", 48);
  config.gif_sample_frames = value_or(fingerprints, "gif_sample_frames", 32);
  config.minimum_moving_overlap =
      value_or(fingerprints, "minimum_moving_overlap", 0.60);
  config.static_phash_distance =
      value_or(matching, "static_phash_distance", 10);
  config.static_hash256_distance =
      value_or(matching, "static_perceptual256_distance", 48);
  config.crop_phash_distance = value_or(matching, "crop_phash_distance", 7);
  config.crop_hash256_distance =
      value_or(matching, "crop_perceptual256_distance", 64);
  config.moving_phash_distance =
      value_or(matching, "moving_phash_distance", 12);
  config.moving_timeline_distance =
      value_or(matching, "moving_timeline_average_distance", 12.0);
  config.minimum_score = value_or(matching, "minimum_score", 0.76);
  config.worker_threads = value_or(matching, "worker_threads", 0);
  config.resolution_weight = value_or(survivor, "resolution_weight", 1.0);
  config.duration_weight = value_or(survivor, "duration_weight", 0.9);
  config.lossless_bonus = value_or(survivor, "lossless_bonus", 0.12);
  config.size_weight = value_or(survivor, "size_weight", 0.08);
  config.key_id = required_environment("B2_KEY_ID");
  config.application_key = required_environment("B2_APPLICATION_KEY");
  config.validate();
  return config;
}

void Config::validate() const {
  if (bucket_name.empty())
    throw std::runtime_error("b2.bucket_name must not be empty");
  if (canonical_prefix.empty() || canonical_prefix.back() != '/' ||
      canonical_prefix.front() == '/') {
    throw std::runtime_error(
        "b2.canonical_prefix must be a relative prefix ending in /");
  }
  if (canonical_index_key.empty() ||
      canonical_index_key.rfind(canonical_prefix, 0) == 0) {
    throw std::runtime_error(
        "The canonical index must be outside the media prefix");
  }
  if (maximum_attempts < 1 || maximum_attempts > 12)
    throw std::runtime_error("maximum_attempts must be 1..12");
  if (ffmpeg_path.empty() || ffprobe_path.empty())
    throw std::runtime_error("FFmpeg tool paths must not be empty");
  if (fingerprint_version < 1 || fingerprint_threads < 1 ||
      fingerprint_threads > 16 || video_sample_frames < 4 ||
      gif_sample_frames < 4) {
    throw std::runtime_error("Fingerprint configuration is invalid");
  }
  if (minimum_moving_overlap < 0.5 || minimum_moving_overlap > 1.0 ||
      minimum_score < 0.5 || minimum_score > 1.0) {
    throw std::runtime_error("Matching ratios are outside conservative bounds");
  }
  if (static_phash_distance < 0 || static_phash_distance > 11 ||
      crop_phash_distance < 0 || crop_phash_distance > 7 ||
      moving_phash_distance < 0 || moving_phash_distance > 15 ||
      static_hash256_distance < 0 || static_hash256_distance > 64 ||
      crop_hash256_distance < 0 || crop_hash256_distance > 96) {
    throw std::runtime_error("Matching distances are outside indexed bounds");
  }
  if (worker_threads < 0 || worker_threads > 128)
    throw std::runtime_error("worker_threads must be 0..128");
}

std::filesystem::path
default_config_path(const std::filesystem::path &executable) {
  const auto adjacent = executable.parent_path() / "config" / "gdupe.json";
  if (std::filesystem::exists(adjacent))
    return adjacent;
  return executable.parent_path() / "config" / "gdupe.example.json";
}

} // namespace gdupe
