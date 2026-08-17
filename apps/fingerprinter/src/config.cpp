#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace gparty::fingerprints {
namespace {

std::filesystem::path expand_path(std::string value,
                                  const std::filesystem::path &base) {
  const char *local = std::getenv("LOCALAPPDATA");
  constexpr std::string_view token = "%LOCALAPPDATA%";
  if (value.rfind(token, 0) == 0) {
    if (local == nullptr || std::string(local).empty())
      throw std::runtime_error("LOCALAPPDATA is unavailable");
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
    throw std::runtime_error("Cannot open fingerprinter configuration: " +
                             path.string());
  nlohmann::json root;
  stream >> root;
  const auto &b2 = root.at("b2");
  const auto &storage = root.at("storage");
  const auto &runtime = root.value("runtime", nlohmann::json::object());
  const auto &fingerprints =
      root.value("fingerprints", nlohmann::json::object());

  Config config;
  config.bucket_name = b2.at("bucket_name").get<std::string>();
  config.canonical_prefix =
      value_or(b2, "canonical_prefix", config.canonical_prefix);
  config.maximum_attempts =
      value_or(b2, "maximum_attempts", config.maximum_attempts);
  config.polling_seconds =
      value_or(runtime, "polling_seconds", config.polling_seconds);
  config.maximum_item_attempts = value_or(
      runtime, "maximum_item_attempts", config.maximum_item_attempts);
  config.database_path = expand_path(
      storage.at("database_path").get<std::string>(), path.parent_path());
  config.cache_directory = expand_path(
      storage.at("cache_directory").get<std::string>(), path.parent_path());
  config.legacy_gdupe_database = expand_path(
      storage.at("legacy_gdupe_database").get<std::string>(),
      path.parent_path());
  config.log_path = expand_path(storage.at("log_path").get<std::string>(),
                                path.parent_path());
  config.video_sample_frames = value_or(
      fingerprints, "video_sample_frames", config.video_sample_frames);
  config.gif_sample_frames =
      value_or(fingerprints, "gif_sample_frames", config.gif_sample_frames);
  config.validate();
  return config;
}

void Config::validate() const {
  if (bucket_name.empty())
    throw std::runtime_error("b2.bucket_name must not be empty");
  if (canonical_prefix.empty() || canonical_prefix.front() == '/' ||
      canonical_prefix.back() != '/')
    throw std::runtime_error(
        "b2.canonical_prefix must be relative and end in /");
  if (maximum_attempts < 1 || maximum_attempts > 12 ||
      maximum_item_attempts < 1 || maximum_item_attempts > 20)
    throw std::runtime_error("Retry limits are outside safe bounds");
  if (polling_seconds < 30 || polling_seconds > 86'400)
    throw std::runtime_error("runtime.polling_seconds must be 30..86400");
  if (video_sample_frames < 4 || gif_sample_frames < 4)
    throw std::runtime_error("Fingerprint sample counts must be at least 4");
  if (database_path.empty() || cache_directory.empty() || log_path.empty())
    throw std::runtime_error("Storage paths must not be empty");
}

std::filesystem::path
default_config_path(const std::filesystem::path &executable) {
  const auto adjacent =
      executable.parent_path() / "config" / "fingerprinter.json";
  if (std::filesystem::exists(adjacent))
    return adjacent;
  return executable.parent_path() / "config" / "fingerprinter.example.json";
}

} // namespace gparty::fingerprints
