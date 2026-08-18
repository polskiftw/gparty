#pragma once

#include "b2_client.hpp"
#include "config.hpp"
#include "database.hpp"
#include "matcher.hpp"
#include "model.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gdupe {

class Engine {
public:
  using Progress = std::function<void(
      const std::string &phase, std::size_t completed, std::size_t total)>;
  explicit Engine(Config config);

  void request_cancel() noexcept;

  StartupSummary startup(Progress progress = {});
  std::vector<ReviewPair> queue() const;
  std::filesystem::path materialize(const std::string &key);

  void delete_object(const std::string &key,
                     const std::string &expected_file_id,
                     std::uint64_t generation, Progress progress = {});
  void exclude_pair(const std::string &first, const std::string &second,
                    std::uint64_t generation, Progress progress = {});
  void process_all(std::uint64_t generation, Progress progress = {});

private:
  Config config_;
  Database database_;
  B2Client b2_;
  Matcher matcher_;
  mutable std::mutex mutex_;
  std::atomic_bool cancel_requested_{};
  std::vector<InventoryObject> inventory_;
  std::vector<CandidateEdge> edges_;
  std::vector<ReviewPair> queue_;
  std::uint64_t generation_{};

  void begin_operation();
  void throw_if_cancelled() const;
  void recover_operations(Progress progress);
  std::pair<std::size_t, std::size_t>
  stabilize_inventory(Progress progress);
  std::size_t cleanup_exact(Progress progress);
  void rebuild_queue(Progress progress = {});
  void
  delete_batch(const std::vector<std::pair<std::string, std::string>> &targets,
               const std::string &kind, Progress progress);
  void check_generation(std::uint64_t generation) const;
};

} // namespace gdupe
