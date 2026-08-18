#include "engine.hpp"
#include "crypto_hash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gdupe {
namespace {

constexpr const char *kObjectCacheDirectory = "objects-v1";
constexpr const char *kCancelled = "Operation cancelled";
constexpr std::size_t kValidationProgressStride = 256;
constexpr std::size_t kDeleteProgressStride = 8;
constexpr std::size_t kMaximumConcurrentDeletes = 16;

std::string stable_name(const std::string &value) {
  return sha256(value).substr(0, 32);
}

std::string operation_id() {
  static thread_local std::mt19937_64 generator(std::random_device{}());
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16)
         << static_cast<std::uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count())
         << std::setw(16) << generator();
  return output.str();
}

void report(const Engine::Progress &progress, const std::string &phase,
            std::size_t completed = 0, std::size_t total = 0) {
  if (progress)
    progress(phase, completed, total);
}

} // namespace

Engine::Engine(Config config)
    : config_(std::move(config)), database_(config_.database_path),
      b2_(config_), matcher_(config_) {
  std::filesystem::create_directories(config_.cache_directory);
  std::filesystem::create_directories(config_.cache_directory /
                                      kObjectCacheDirectory);
}

void Engine::request_cancel() noexcept {
  cancel_requested_.store(true, std::memory_order_relaxed);
  b2_.request_cancel();
}

void Engine::begin_operation() {
  if (cancel_requested_.load(std::memory_order_relaxed))
    throw std::runtime_error(kCancelled);
  b2_.clear_cancel();
}

void Engine::throw_if_cancelled() const {
  if (cancel_requested_.load(std::memory_order_relaxed))
    throw std::runtime_error(kCancelled);
}

std::filesystem::path Engine::cache_path(const RemoteObject &object) const {
  return config_.cache_directory / kObjectCacheDirectory /
         (stable_name(object.file_id) +
          (object.extension.empty() ? std::string{} : "." + object.extension));
}

void Engine::recover_operations(Progress progress) {
  const auto pending = database_.pending_operations();
  if (pending.empty())
    return;
  report(progress, "Repairing an interrupted library update", 0,
         pending.size());
  std::size_t completed = 0;
  for (const auto &operation : pending) {
    throw_if_cancelled();
    if (operation.state == "prepared") {
      if (b2_.version_exists(operation.target_key, operation.target_file_id)) {
        b2_.delete_version(operation.target_key, operation.target_file_id);
      }
      database_.update_operation(operation.id, "remote_deleted");
    } else if (operation.state != "remote_deleted") {
      throw std::runtime_error(
          "Recovery journal contains an unknown operation state");
    }
    report(progress, "Repairing an interrupted library update", ++completed,
           pending.size());
  }
  throw_if_cancelled();
  auto stable = b2_.stable_inventory(config_.canonical_prefix);
  throw_if_cancelled();
  stable = b2_.settle_canonical_index(stable);
  throw_if_cancelled();
  database_.reconcile_inventory(stable, config_.fingerprint_version);
  std::vector<std::string> ids;
  ids.reserve(pending.size());
  for (const auto &operation : pending)
    ids.push_back(operation.id);
  database_.complete_operations(ids);
}

std::filesystem::path Engine::materialize(const std::string &key) {
  std::scoped_lock lock(mutex_);
  begin_operation();
  const auto found =
      std::find_if(inventory_.begin(), inventory_.end(),
                   [&](const auto &item) { return item.remote.key == key; });
  if (found == inventory_.end())
    throw std::runtime_error(
        "The selected media is no longer in the inventory");
  const auto path = cache_path(found->remote);
  b2_.download_to(found->remote, path);
  throw_if_cancelled();
  return path;
}

void Engine::delete_batch(
    const std::vector<std::pair<std::string, std::string>> &requested,
    const std::string &kind, Progress progress) {
  std::vector<std::pair<std::string, std::string>> targets = requested;
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  if (targets.empty())
    return;

  throw_if_cancelled();
  report(progress, "Validating exact duplicates against B2", 0,
         targets.size());
  const auto snapshot = b2_.stable_inventory(config_.canonical_prefix);
  throw_if_cancelled();

  std::unordered_map<std::string, std::string> live_ids;
  live_ids.reserve(snapshot.size());
  for (const auto &item : snapshot)
    live_ids.emplace(item.key, item.file_id);

  std::size_t validated = 0;
  for (const auto &[key, file_id] : targets) {
    throw_if_cancelled();
    const auto found = live_ids.find(key);
    if (found == live_ids.end() || found->second != file_id)
      throw std::runtime_error("A selected B2 object changed after analysis; "
                               "no deletion was attempted");
    ++validated;
    if (validated == targets.size() ||
        validated % kValidationProgressStride == 0)
      report(progress, "Validating exact duplicates against B2", validated,
             targets.size());
  }

  throw_if_cancelled();
  const std::size_t worker_count =
      std::min(kMaximumConcurrentDeletes, targets.size());
  std::vector<std::unique_ptr<B2Client>> delete_clients;
  delete_clients.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    throw_if_cancelled();
    delete_clients.push_back(std::make_unique<B2Client>(config_));
  }

  std::vector<Operation> operations;
  operations.reserve(targets.size());
  for (const auto &[key, file_id] : targets)
    operations.push_back({operation_id(), kind, key, file_id, "prepared", {}});
  database_.prepare_operations(operations);

  try {
    report(progress, "Deleting exact B2 versions", 0, operations.size());
    std::atomic_size_t next{0};
    std::atomic_size_t completed{0};
    std::atomic_bool stop_scheduling{false};
    std::mutex state_mutex;
    std::exception_ptr failure;
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back(
          [&, client = std::move(delete_clients[worker])]() mutable {
            while (!stop_scheduling.load(std::memory_order_relaxed) &&
                   !cancel_requested_.load(std::memory_order_relaxed)) {
              const std::size_t position =
                  next.fetch_add(1, std::memory_order_relaxed);
              if (position >= operations.size())
                break;
              if (stop_scheduling.load(std::memory_order_relaxed) ||
                  cancel_requested_.load(std::memory_order_relaxed))
                break;

              auto &operation = operations[position];
              try {
                client->delete_version(operation.target_key,
                                       operation.target_file_id);
                std::scoped_lock state_lock(state_mutex);
                database_.update_operation(operation.id, "remote_deleted");
                operation.state = "remote_deleted";
                const std::size_t now =
                    completed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (now == operations.size() ||
                    now % kDeleteProgressStride == 0)
                  report(progress, "Deleting exact B2 versions", now,
                         operations.size());
              } catch (...) {
                std::scoped_lock state_lock(state_mutex);
                if (!failure)
                  failure = std::current_exception();
                stop_scheduling.store(true, std::memory_order_relaxed);
                break;
              }
            }
          });
    }
    workers.clear();

    if (failure)
      std::rethrow_exception(failure);
    throw_if_cancelled();
    report(progress, "Deleting exact B2 versions", operations.size(),
           operations.size());

    report(progress, "Consolidating the canonical B2 index");
    auto remote = b2_.stable_inventory(config_.canonical_prefix);
    throw_if_cancelled();
    remote = b2_.settle_canonical_index(remote);
    throw_if_cancelled();
    database_.reconcile_inventory(remote, config_.fingerprint_version);
    std::vector<std::string> ids;
    ids.reserve(operations.size());
    for (const auto &operation : operations)
      ids.push_back(operation.id);
    database_.complete_operations(ids);
    database_.set_metadata("last_inventory_sha256",
                           b2_.inventory_digest(remote));
  } catch (const std::exception &problem) {
    const bool cancelled = std::string(problem.what()) == kCancelled;
    if (cancelled) {
      std::vector<std::string> untouched;
      for (const auto &operation : operations)
        if (operation.state == "prepared")
          untouched.push_back(operation.id);
      if (!untouched.empty()) {
        try {
          database_.complete_operations(untouched);
        } catch (...) {
        }
      }
    }
    for (const auto &operation : operations) {
      if (cancelled && operation.state == "prepared")
        continue;
      try {
        database_.update_operation(operation.id, operation.state,
                                   problem.what());
      } catch (...) {
      }
    }
    throw;
  }
}

std::size_t Engine::cleanup_exact(Progress progress) {
  inventory_ = database_.inventory();
  std::map<std::string, std::vector<const InventoryObject *>> groups;
  for (const auto &item : inventory_)
    if (item.fingerprint)
      groups[item.fingerprint->sha256].push_back(&item);
  std::vector<std::pair<std::string, std::string>> deletions;
  for (auto &[_, items] : groups) {
    throw_if_cancelled();
    if (items.size() < 2)
      continue;
    std::sort(items.begin(), items.end(),
              [&](const auto *left, const auto *right) {
                const double a = matcher_.survivor_quality(*left),
                             b = matcher_.survivor_quality(*right);
                return a != b ? a > b : left->remote.key < right->remote.key;
              });
    for (std::size_t index = 1; index < items.size(); ++index)
      deletions.emplace_back(items[index]->remote.key,
                             items[index]->remote.file_id);
  }
  if (!deletions.empty())
    delete_batch(deletions, "automatic_exact_sha256", progress);
  inventory_ = database_.inventory();
  return deletions.size();
}

std::pair<std::size_t, std::size_t>
Engine::stabilize_inventory(Progress progress) {
  inventory_ = database_.inventory();
  report(progress, "Removing byte-identical copies");
  const std::size_t exact = cleanup_exact(progress);
  throw_if_cancelled();
  rebuild_queue(progress);
  return {0, exact};
}

void Engine::rebuild_queue(Progress progress) {
  throw_if_cancelled();
  const auto object_cache =
      config_.cache_directory / kObjectCacheDirectory;
  if (!config_.keep_media_cache && std::filesystem::exists(object_cache)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(object_cache)) {
      throw_if_cancelled();
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path());
    }
  }
  inventory_ = database_.inventory();
  report(progress, "Comparing fingerprinted media");
  edges_ = matcher_.find_candidates(
      inventory_, database_.exclusions(),
      [&](std::size_t done, std::size_t total) {
        throw_if_cancelled();
        report(progress, "Comparing fingerprinted media", done, total);
      });
  throw_if_cancelled();
  generation_ = database_.advance_queue_generation();
  queue_ = matcher_.build_queue(inventory_, edges_, generation_);
}

StartupSummary Engine::startup(Progress progress) {
  std::scoped_lock lock(mutex_);
  begin_operation();
  report(progress, "Validating durable state");
  recover_operations(progress);
  throw_if_cancelled();
  report(progress, "Loading the gfingerd inventory");
  const auto before = database_.inventory();
  const std::size_t reused =
      std::count_if(before.begin(), before.end(), [](const auto &item) {
        return item.fingerprint.has_value();
      });
  const auto [computed, exact] = stabilize_inventory(progress);
  throw_if_cancelled();
  report(progress, queue_.empty() ? "Library is clean" : "Ready for review",
         queue_.size(), queue_.size());
  return {inventory_.size(), reused, computed, exact, queue_.size()};
}

std::vector<ReviewPair> Engine::queue() const {
  std::scoped_lock lock(mutex_);
  return queue_;
}

void Engine::check_generation(std::uint64_t generation) const {
  if (generation != generation_)
    throw std::runtime_error(
        "That review card is stale; the queue has already changed");
}

void Engine::delete_object(const std::string &key,
                           const std::string &expected_file_id,
                           std::uint64_t generation, Progress progress) {
  std::scoped_lock lock(mutex_);
  begin_operation();
  check_generation(generation);
  const auto item = database_.object(key);
  if (!item || item->remote.file_id != expected_file_id)
    throw std::runtime_error(
        "The selected object changed after this card was shown");
  delete_batch({{key, expected_file_id}}, "manual_delete", progress);
  throw_if_cancelled();
  stabilize_inventory(progress);
}

void Engine::exclude_pair(const std::string &first, const std::string &second,
                          std::uint64_t generation, Progress progress) {
  std::scoped_lock lock(mutex_);
  begin_operation();
  check_generation(generation);
  const auto wanted = ordered_pair(first, second);
  const bool present =
      std::any_of(edges_.begin(), edges_.end(), [&](const auto &edge) {
        return ordered_pair(edge.a, edge.b) == wanted;
      });
  if (!present)
    throw std::runtime_error("That comparison is no longer pending");
  database_.exclude_pair(first, second);
  throw_if_cancelled();
  stabilize_inventory(progress);
}

void Engine::process_all(std::uint64_t generation, Progress progress) {
  std::scoped_lock lock(mutex_);
  begin_operation();
  check_generation(generation);
  const auto keys = matcher_.process_all_deletions(inventory_, edges_);
  std::unordered_map<std::string, std::string> ids;
  for (const auto &item : inventory_)
    ids[item.remote.key] = item.remote.file_id;
  std::vector<std::pair<std::string, std::string>> targets;
  targets.reserve(keys.size());
  for (const auto &key : keys)
    targets.emplace_back(key, ids.at(key));
  delete_batch(targets, "process_all", progress);
  throw_if_cancelled();
  stabilize_inventory(progress);
}

} // namespace gdupe
