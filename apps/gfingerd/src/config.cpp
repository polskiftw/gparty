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

nlohmann::json config_json(const Config &config) {
  return {
      {"b2",
       {{"bucket_name", config.bucket_name},
        {"canonical_prefix", config.canonical_prefix},
        {"maximum_attempts", config.maximum_attempts}}},
      {"runtime",
       {{"polling_seconds", config.polling_seconds},
        {"worker_threads", config.worker_threads},
        {"download_connections", config.download_connections},
        {"prefetch_files", config.prefetch_files},
        {"maximum_item_attempts", config.maximum_item_attempts}}},
      {"storage",
       {{"database_path", config.database_path.string()},
        {"cache_directory", config.cache_directory.string()},
        {"log_path", config.log_path.string()}}}};
}

} // namespace

Config Config::load(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("Cannot open gfingerd configuration: " +
                             path.string());
  nlohmann::json root;
  stream >> root;
  const auto &b2 = root.value("b2", nlohmann::json::object());
  const auto &storage = root.value("storage", nlohmann::json::object());
  const auto &runtime = root.value("runtime", nlohmann::json::object());

  Config config = defaults();
  config.bucket_name = value_or(b2, "bucket_name", config.bucket_name);
  config.canonical_prefix =
      value_or(b2, "canonical_prefix", config.canonical_prefix);
  config.maximum_attempts =
      value_or(b2, "maximum_attempts", config.maximum_attempts);
  config.polling_seconds =
      value_or(runtime, "polling_seconds", config.polling_seconds);
  config.worker_threads =
      value_or(runtime, "worker_threads", config.worker_threads);
  config.download_connections = value_or(
      runtime, "download_connections", config.download_connections);
  config.prefetch_files =
      value_or(runtime, "prefetch_files", config.prefetch_files);
  config.maximum_item_attempts = value_or(
      runtime, "maximum_item_attempts", config.maximum_item_attempts);
  if (storage.contains("database_path"))
    config.database_path = expand_path(
        storage.at("database_path").get<std::string>(), path.parent_path());
  if (storage.contains("cache_directory"))
    config.cache_directory = expand_path(
        storage.at("cache_directory").get<std::string>(), path.parent_path());
  if (storage.contains("log_path"))
    config.log_path = expand_path(
        storage.at("log_path").get<std::string>(), path.parent_path());
  config.validate();
  return config;
}

Config Config::defaults() {
  Config config;
  config.database_path =
      expand_path("%LOCALAPPDATA%/gdupe/gdupe.sqlite3", {});
  config.cache_directory =
      expand_path("%LOCALAPPDATA%/GParty/gfingerd-cache", {});
  config.log_path = expand_path("%LOCALAPPDATA%/GParty/gfingerd.log", {});
  config.validate();
  return config;
}

std::filesystem::path user_config_path() {
  return expand_path("%LOCALAPPDATA%/GParty/gfingerd.json", {});
}

Config Config::load_user_or_defaults() {
  const auto path = user_config_path();
  return std::filesystem::exists(path) ? load(path) : defaults();
}

std::string Config::serialize() const {
  validate();
  return config_json(*this).dump(2) + "\n";
}

void Config::save_user() const {
  const auto path = user_config_path();
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".new";
  {
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream)
      throw std::runtime_error("Cannot write the gfingerd configuration");
    stream << serialize();
    if (!stream)
      throw std::runtime_error("Cannot finish the gfingerd configuration");
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::rename(temporary, path);
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
  if (worker_threads < 1 || worker_threads > 16)
    throw std::runtime_error("runtime.worker_threads must be 1..16");
  if (download_connections < 1 || download_connections > 16)
    throw std::runtime_error("runtime.download_connections must be 1..16");
  if (prefetch_files < 1 || prefetch_files > 64)
    throw std::runtime_error("runtime.prefetch_files must be 1..64");
  if (database_path.empty() || cache_directory.empty() || log_path.empty())
    throw std::runtime_error("Storage paths must not be empty");
}

} // namespace gparty::fingerprints
