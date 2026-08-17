#include "runtime_stats.hpp"

#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace gparty::fingerprints {

RuntimeStats::RuntimeStats(std::filesystem::path path, std::size_t worker_count)
    : path_(std::move(path)), slots_(worker_count) {
  std::filesystem::create_directories(path_.parent_path());
  std::scoped_lock lock(mutex_);
  publish_locked(true);
}

RuntimeStats::~RuntimeStats() {
  try {
    std::scoped_lock lock(mutex_);
    state_ = "stopped";
    for (auto &slot : slots_)
      slot = {};
    publish_locked(true);
  } catch (...) {
  }
}

void RuntimeStats::set_state(std::string state) {
  std::scoped_lock lock(mutex_);
  state_ = std::move(state);
  publish_locked(true);
}

void RuntimeStats::begin(std::size_t worker, const std::string &key) {
  std::scoped_lock lock(mutex_);
  if (worker >= slots_.size())
    return;
  auto &slot = slots_[worker];
  slot = {};
  slot.active = true;
  slot.key = key;
  slot.last_sample = std::chrono::steady_clock::now();
  publish_locked(true);
}

void RuntimeStats::progress(std::size_t worker, std::uint64_t downloaded,
                            std::uint64_t total) {
  std::scoped_lock lock(mutex_);
  if (worker >= slots_.size() || !slots_[worker].active)
    return;
  auto &slot = slots_[worker];
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
  slot.total = total;
  publish_locked(false);
}

void RuntimeStats::finish(std::size_t worker, bool succeeded) {
  std::scoped_lock lock(mutex_);
  if (worker >= slots_.size())
    return;
  slots_[worker] = {};
  if (succeeded)
    ++completed_;
  else
    ++failed_;
  publish_locked(true);
}

void RuntimeStats::download_finished(std::size_t worker) {
  std::scoped_lock lock(mutex_);
  if (worker >= slots_.size())
    return;
  slots_[worker].bytes_per_second = 0.0;
  publish_locked(true);
}

void RuntimeStats::cancel(std::size_t worker) {
  std::scoped_lock lock(mutex_);
  if (worker >= slots_.size())
    return;
  slots_[worker] = {};
  publish_locked(true);
}

void RuntimeStats::heartbeat() {
  std::scoped_lock lock(mutex_);
  publish_locked(true);
}

void RuntimeStats::publish_locked(bool force) {
  const auto steady_now = std::chrono::steady_clock::now();
  if (!force && last_publish_ != std::chrono::steady_clock::time_point{} &&
      steady_now - last_publish_ < std::chrono::milliseconds(250))
    return;
  last_publish_ = steady_now;

  int active = 0;
  double speed = 0.0;
  std::vector<std::string> files;
  for (const auto &slot : slots_) {
    if (!slot.active)
      continue;
    ++active;
    speed += slot.bytes_per_second;
    files.push_back(slot.key);
  }
  const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  const nlohmann::json value{{"state", state_},
                             {"updated_unix_ms", unix_ms},
                             {"configured_workers", slots_.size()},
                             {"active_workers", active},
                             {"bytes_per_second", speed},
                             {"session_bytes", session_bytes_},
                             {"completed_session", completed_},
                             {"failed_session", failed_},
                             {"current_files", files}};
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
