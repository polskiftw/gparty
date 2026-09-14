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
  RuntimeStats(std::filesystem::path path, std::size_t download_count,
               std::size_t fingerprint_count, std::size_t prefetch_capacity,
               bool boot_worker);
  ~RuntimeStats();

  void set_state(std::string state);
  void download_begin(std::size_t connection, const std::string &key);
  void download_progress(std::size_t connection, std::uint64_t downloaded,
                         std::uint64_t total);
  void download_finish(std::size_t connection);
  void item_failed();
  void fingerprint_begin(std::size_t worker, const std::string &key);
  void fingerprint_finish(std::size_t worker, bool completed, bool failed);
  void set_prefetch_ready(std::size_t ready);
  void heartbeat();
  void mark_nvdec_ready();

private:
  struct DownloadSlot {
    bool active{};
    std::string key;
    std::uint64_t downloaded{};
    std::uint64_t sample_downloaded{};
    double bytes_per_second{};
    std::chrono::steady_clock::time_point last_sample{};
  };
  struct FingerprintSlot {
    bool active{};
    std::string key;
  };

  std::filesystem::path path_;
  std::vector<DownloadSlot> downloads_;
  std::vector<FingerprintSlot> fingerprints_;
  std::size_t prefetch_capacity_{};
  std::size_t prefetch_ready_{};
  std::mutex mutex_;
  std::string state_{"starting"};
  bool boot_worker_{};
  std::int64_t process_started_unix_ms_{};
  std::int64_t nvdec_ready_unix_ms_{};
  std::uint64_t session_bytes_{};
  std::uint64_t completed_{};
  std::uint64_t failed_{};
  std::chrono::steady_clock::time_point last_publish_{};

  void publish_locked(bool force);
};

} // namespace gparty::fingerprints
