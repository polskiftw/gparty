#pragma once

#include "model.hpp"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace gparty::fingerprints {

struct AdoptionSummary {
  std::size_t rows_scanned{};
  std::size_t matching_rows{};
  std::size_t components_imported{};
};

struct RegistryStatus {
  std::size_t inventory_objects{};
  std::size_t fully_fingerprinted{};
  std::size_t pending_objects{};
  std::size_t pending_components{};
  std::size_t unsupported{};
  std::size_t failed{};
  std::string last_successful_scan;
  std::string currently_processing;
};

struct PendingObject {
  gdupe::RemoteObject remote;
  std::size_t missing_components{};
};

class Registry {
public:
  explicit Registry(const std::filesystem::path &path);
  ~Registry();
  Registry(const Registry &) = delete;
  Registry &operator=(const Registry &) = delete;

  void reconcile(const std::vector<gdupe::RemoteObject> &objects);
  AdoptionSummary adopt_gdupe_v3(const std::filesystem::path &legacy_path);
  std::vector<PendingObject> pending(std::size_t limit = 0) const;
  void save_fingerprint(const gdupe::RemoteObject &object,
                        const gdupe::Fingerprint &fingerprint,
                        const std::string &source = "computed");
  void record_failure(const gdupe::RemoteObject &object,
                      const std::string &error, int maximum_attempts);
  void clear_failure(const std::string &file_id);
  RegistryStatus status() const;
  void set_metadata(const std::string &key, const std::string &value);
  std::string metadata(const std::string &key) const;

private:
  sqlite3 *db_{};
  mutable std::mutex mutex_;

  void initialize_schema();
  void execute(const char *sql) const;
};

bool supported_extension(const std::string &extension);
bool moving_extension(const std::string &extension);

} // namespace gparty::fingerprints
