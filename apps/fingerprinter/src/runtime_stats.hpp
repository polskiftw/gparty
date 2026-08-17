#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gparty::fingerprints {

class RuntimeStats {
public:
  RuntimeStats(std::filesystem::path path, std::size_t worker_count);
  ~RuntimeStats();

  void set_state(std::string state);
  void begin(std::size_t worker, const std::string &key);
  void progress(std::size_t worker, std::uint64_t downloaded,
                std::uint64_t total);
  void download_finished(std::size_t worker);
  void finish(std::size_t worker, bool succeeded);
  void cancel(std::size_t worker);
  void heartbeat();

private:
  struct Slot {
    bool active{};
    std::string key;
    std::uint64_t downloaded{};
    std::uint64_t sample_downloaded{};
    std::uint64_t total{};
    double bytes_per_second{};
    std::chrono::steady_clock::time_point last_sample{};
  };

  std::filesystem::path path_;
  std::vector<Slot> slots_;
  std::mutex mutex_;
  std::string state_{"starting"};
  std::uint64_t session_bytes_{};
  std::uint64_t completed_{};
  std::uint64_t failed_{};
  std::chrono::steady_clock::time_point last_publish_{};

  void publish_locked(bool force);
};

} // namespace gparty::fingerprints
