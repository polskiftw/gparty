#include "runtime_stats.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace gparty::fingerprints {

RuntimeStats::RuntimeStats(std::filesystem::path path,
                           std::size_t download_count,
                           std::size_t fingerprint_count,
                           std::size_t prefetch_capacity, bool boot_worker)
    : path_(std::move(path)), downloads_(download_count),
      fingerprints_(fingerprint_count), prefetch_capacity_(prefetch_capacity),
      boot_worker_(boot_worker) {
  process_started_unix_ms_ =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  std::filesystem::create_directories(path_.parent_path());
  std::scoped_lock lock(mutex_);
  publish_locked(true);
}

RuntimeStats::~RuntimeStats() {
  try {
    std::scoped_lock lock(mutex_);
    state_ = "stopped";
    for (auto &slot : downloads_)
      slot = {};
    for (auto &slot : fingerprints_)
      slot = {};
    prefetch_ready_ = 0;
    publish_locked(true);
  } catch (...) {
  }
}

void RuntimeStats::set_state(std::string state) {
  std::scoped_lock lock(mutex_);
  state_ = std::move(state);
  publish_locked(true);
}

void RuntimeStats::download_begin(std::size_t connection,
                                  const std::string &key) {
  std::scoped_lock lock(mutex_);
  if (connection >= downloads_.size())
    return;
  auto &slot = downloads_[connection];
  slot = {};
  slot.active = true;
  slot.key = key;
  slot.last_sample = std::chrono::steady_clock::now();
  publish_locked(true);
}

void RuntimeStats::download_progress(std::size_t connection,
                                     std::uint64_t downloaded,
                                     std::uint64_t) {
  std::scoped_lock lock(mutex_);
  if (connection >= downloads_.size() || !downloads_[connection].active)
    return;
  auto &slot = downloads_[connection];
  const auto now = std::chrono::steady_clock::now();
  if (downloaded < slot.downloaded)
    slot.downloaded = 0;
  const auto delta = downloaded - slot.downloaded;
  const auto elapsed =
      std::chrono::duration<double>(now - slot.last_sample).count();
  session_bytes_ += delta;
  if (elapsed >= 0.2) {
    const auto sample_delta = downloaded >= slot.sample_downloaded
                                  ? downloaded - slot.sample_downloaded
                                  : downloaded;
    const double instant = static_cast<double>(sample_delta) / elapsed;
    slot.bytes_per_second = slot.bytes_per_second == 0.0
                                ? instant
                                : slot.bytes_per_second * 0.65 + instant * 0.35;
    slot.last_sample = now;
    slot.sample_downloaded = downloaded;
  }
  slot.downloaded = downloaded;
  publish_locked(false);
}

void RuntimeStats::download_finish(std::size_t connection) {
  std::scoped_lock lock(mutex_);
  if (connection >= downloads_.size())
    return;
  downloads_[connection] = {};
  publish_locked(true);
}

void RuntimeStats::item_failed() {
  std::scoped_lock lock(mutex_);
  ++failed_;
  publish_locked(true);
}

void RuntimeStats::fingerprint_begin(std::size_t worker,
                                     const std::string &key) {
  std::scoped_lock lock(mutex_);
  if (worker >= fingerprints_.size())
    return;
  fingerprints_[worker] = {true, key};
  publish_locked(true);
}

void RuntimeStats::fingerprint_finish(std::size_t worker, bool completed,
                                      bool failed) {
  std::scoped_lock lock(mutex_);
  if (worker >= fingerprints_.size())
    return;
  fingerprints_[worker] = {};
  if (completed)
    ++completed_;
  if (failed)
    ++failed_;
  publish_locked(true);
}

void RuntimeStats::set_prefetch_ready(std::size_t ready) {
  std::scoped_lock lock(mutex_);
  prefetch_ready_ = (std::min)(ready, prefetch_capacity_);
  publish_locked(true);
}

void RuntimeStats::heartbeat() {
  std::scoped_lock lock(mutex_);
  publish_locked(true);
}

void RuntimeStats::mark_nvdec_ready() {
  std::scoped_lock lock(mutex_);
  if (nvdec_ready_unix_ms_ == 0) {
    nvdec_ready_unix_ms_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
  }
  publish_locked(true);
}

void RuntimeStats::publish_locked(bool force) {
  const auto steady_now = std::chrono::steady_clock::now();
  if (!force && last_publish_ != std::chrono::steady_clock::time_point{} &&
      steady_now - last_publish_ < std::chrono::milliseconds(250))
    return;
  last_publish_ = steady_now;

  int active_downloads = 0;
  int active_fingerprints = 0;
  double speed = 0.0;
  std::vector<std::string> download_files;
  std::vector<std::string> fingerprint_files;
  for (const auto &slot : downloads_) {
    if (!slot.active)
      continue;
    ++active_downloads;
    speed += slot.bytes_per_second;
    download_files.push_back(slot.key);
  }
  for (const auto &slot : fingerprints_) {
    if (!slot.active)
      continue;
    ++active_fingerprints;
    fingerprint_files.push_back(slot.key);
  }
  const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  const nlohmann::json value{
      {"state", state_},
      {"updated_unix_ms", unix_ms},
      {"launch_mode", boot_worker_ ? "boot" : "manual"},
      {"process_started_unix_ms", process_started_unix_ms_},
      {"nvdec_ready_unix_ms", nvdec_ready_unix_ms_},
      {"configured_download_connections", downloads_.size()},
      {"active_downloads", active_downloads},
      {"configured_fingerprint_workers", fingerprints_.size()},
      {"active_fingerprint_workers", active_fingerprints},
      {"prefetch_ready", prefetch_ready_},
      {"prefetch_capacity", prefetch_capacity_},
      {"bytes_per_second", speed},
      {"session_bytes", session_bytes_},
      {"completed_session", completed_},
      {"failed_session", failed_},
      {"current_download_files", download_files},
      {"current_fingerprint_files", fingerprint_files}};
  const auto temporary = path_.string() + ".new";
  {
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream)
      return;
    stream << value.dump() << '\n';
    if (!stream)
      return;
  }
  std::error_code error;
  std::filesystem::remove(path_, error);
  std::filesystem::rename(temporary, path_, error);
}

} // namespace gparty::fingerprints
