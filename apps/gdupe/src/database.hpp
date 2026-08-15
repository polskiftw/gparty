#pragma once

#include "model.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace gdupe {

class Database {
public:
  explicit Database(const std::filesystem::path &path);
  ~Database();
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  void reconcile_inventory(const std::vector<RemoteObject> &remote,
                           int fingerprint_version);
  std::vector<InventoryObject> inventory() const;
  std::optional<InventoryObject> object(const std::string &key) const;
  void save_fingerprint(const std::string &key, const std::string &file_id,
                        const Fingerprint &value);

  void exclude_pair(const std::string &first, const std::string &second);
  std::set<std::pair<std::string, std::string>> exclusions() const;

  void prepare_operations(const std::vector<Operation> &operations);
  void update_operation(const std::string &id, const std::string &state,
                        const std::string &error = {});
  std::vector<Operation> pending_operations() const;
  void complete_operations(const std::vector<std::string> &ids);

  std::string metadata(const std::string &key) const;
  void set_metadata(const std::string &key, const std::string &value);
  std::uint64_t advance_queue_generation();

private:
  sqlite3 *db_{};
  std::mutex fingerprint_write_mutex_;
  void execute(const char *sql) const;
  void initialize_schema();
};

std::pair<std::string, std::string> ordered_pair(std::string first,
                                                 std::string second);

} // namespace gdupe
