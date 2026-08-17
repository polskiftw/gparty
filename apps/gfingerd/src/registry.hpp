#pragma once

#include "database.hpp"
#include "model.hpp"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace gparty::fingerprints {

inline constexpr int kFingerprintVersion = 3;
inline constexpr int kVideoSampleFrames = 48;
inline constexpr int kGifSampleFrames = 32;

struct RegistryStatus {
  std::size_t inventory_objects{};
  std::size_t fully_fingerprinted{};
  std::size_t pending_objects{};
  std::size_t unsupported{};
  std::size_t deferred_gifs{};
  std::size_t failed{};
  std::string last_successful_scan;
};

struct PendingObject {
  gdupe::RemoteObject remote;
};

class Registry {
public:
  explicit Registry(const std::filesystem::path &path);
  ~Registry();
  Registry(const Registry &) = delete;
  Registry &operator=(const Registry &) = delete;

  void reconcile(const std::vector<gdupe::RemoteObject> &objects);
  std::vector<PendingObject> pending(std::size_t limit = 0) const;
  void save_fingerprint(const gdupe::RemoteObject &object,
                        const gdupe::Fingerprint &fingerprint);
  void record_failure(const gdupe::RemoteObject &object,
                      const std::string &error, int maximum_attempts);
  void defer_gif(const gdupe::RemoteObject &object,
                 const std::filesystem::path &local_path,
                 const std::string &reason);
  RegistryStatus status() const;

private:
  gdupe::Database database_;
  sqlite3 *ops_db_{};
  mutable std::mutex ops_mutex_;

  void initialize_operational_schema();
  void execute(const char *sql) const;
  bool current_identity(const gdupe::RemoteObject &object) const;
};

bool supported_extension(const std::string &extension);
bool moving_extension(const std::string &extension);

} // namespace gparty::fingerprints
