#include "engine.hpp"
#include "crypto_hash.hpp"
#include "fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gdupe {
namespace {

constexpr int kMaximumStabilizationPasses = 8;
constexpr const char *kObjectCacheDirectory = "objects-v1";

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
  auto stable = b2_.stable_inventory(config_.canonical_prefix);
  stable = b2_.settle_canonical_index(stable);
  database_.reconcile_inventory(stable, config_.fingerprint_version);
  std::vector<std::string> ids;
  ids.reserve(pending.size());
  for (const auto &operation : pending)
    ids.push_back(operation.id);
  database_.complete_operations(ids);
}

std::filesystem::path Engine::materialize(const std::string &key) {
  std::scoped_lock lock(mutex_);
  const auto found =
      std::find_if(inventory_.begin(), inventory_.end(),
                   [&](const auto &item) { return item.remote.key == key; });
  if (found == inventory_.end())
    throw std::runtime_error(
        "The selected media is no longer in the inventory");
  const auto path = cache_path(found->remote);
  b2_.download_to(found->remote, path);
  return path;
}

std::size_t Engine::fingerprint_missing(Progress progress) {
  inventory_ = database_.inventory();
  std::vector<std::size_t> missing;
  for (std::size_t index = 0; index < inventory_.size(); ++index)
    if (!inventory_[index].fingerprint)
      missing.push_back(index);
  std::atomic<std::size_t> next{0}, completed{0};
  std::atomic<bool> failed{false};
  std::mutex failure_mutex;
  std::exception_ptr failure;
  const std::size_t worker_count =
      std::min<std::size_t>(config_.fingerprint_threads, missing.size());
  report(progress, "Fingerprinting new or changed media", 0, missing.size());
  std::vector<std::jthread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&] {
      try {
        B2Client client(config_);
        Fingerprinter fingerprinter(config_);
        while (!failed.load()) {
          const std::size_t position = next.fetch_add(1);
          if (position >= missing.size())
            break;
          auto &item = inventory_[missing[position]];
          const auto path = cache_path(item.remote);
          client.download_to(item.remote, path);
          const auto fingerprint =
              fingerprinter.compute(path, item.remote.extension);
          database_.save_fingerprint(item.remote.key, item.remote.file_id,
                                     fingerprint);
          item.fingerprint = fingerprint;
          if (!config_.keep_media_cache)
            std::filesystem::remove(path);
          const auto done = completed.fetch_add(1) + 1;
          report(progress, "Fingerprinting new or changed media", done,
                 missing.size());
        }
      } catch (...) {
        failed.store(true);
        std::scoped_lock failure_lock(failure_mutex);
        if (!failure)
          failure = std::current_exception();
      }
    });
  }
  workers.clear();
  if (failure)
    std::rethrow_exception(failure);
  return missing.size();
}

void Engine::delete_batch(
    const std::vector<std::pair<std::string, std::string>> &requested,
    const std::string &kind, Progress progress) {
  std::vector<std::pair<std::string, std::string>> targets = requested;
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  if (targets.empty())
    return;
  for (const auto &[key, file_id] : targets) {
    const auto current = b2_.find_object(key);
    if (!current || current->file_id != file_id)
      throw std::runtime_error("A selected B2 object changed after analysis; "
                               "no deletion was attempted");
  }
  std::vector<Operation> operations;
  operations.reserve(targets.size());
  for (const auto &[key, file_id] : targets)
    operations.push_back({operation_id(), kind, key, file_id, "prepared", {}});
  database_.prepare_operations(operations);
  try {
    std::size_t completed = 0;
    for (auto &operation : operations) {
      report(progress, "Deleting exact B2 versions", completed,
             operations.size());
      if (!b2_.version_exists(operation.target_key, operation.target_file_id))
        throw std::runtime_error(
            "A prepared B2 version disappeared before deletion");
      b2_.delete_version(operation.target_key, operation.target_file_id);
      database_.update_operation(operation.id, "remote_deleted");
      operation.state = "remote_deleted";
      report(progress, "Deleting exact B2 versions", ++completed,
             operations.size());
    }
    report(progress, "Consolidating the canonical B2 index");
    auto remote = b2_.stable_inventory(config_.canonical_prefix);
    remote = b2_.settle_canonical_index(remote);
    database_.reconcile_inventory(remote, config_.fingerprint_version);
    std::vector<std::string> ids;
    ids.reserve(operations.size());
    for (const auto &operation : operations)
      ids.push_back(operation.id);
    database_.complete_operations(ids);
    database_.set_metadata("last_inventory_sha256",
                           b2_.inventory_digest(remote));
  } catch (const std::exception &problem) {
    for (const auto &operation : operations) {
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
  std::size_t computed = 0;
  std::size_t exact = 0;
  for (int pass = 1; pass <= kMaximumStabilizationPasses; ++pass) {
    computed += fingerprint_missing(progress);
    report(progress, "Removing byte-identical copies");
    exact += cleanup_exact(progress);

    report(progress, "Verifying the final B2 inventory", pass,
           kMaximumStabilizationPasses);
    auto remote = b2_.stable_inventory(config_.canonical_prefix);
    remote = b2_.settle_canonical_index(remote);
    database_.reconcile_inventory(remote, config_.fingerprint_version);
    database_.set_metadata("last_inventory_sha256",
                           b2_.inventory_digest(remote));
    inventory_ = database_.inventory();
    const bool fully_fingerprinted =
        std::all_of(inventory_.begin(), inventory_.end(), [](const auto &item) {
          return item.fingerprint.has_value();
        });
    if (fully_fingerprinted) {
      const std::string analyzed_digest = b2_.inventory_digest(remote);
      rebuild_queue(progress);
      auto verified = b2_.stable_inventory(config_.canonical_prefix);
      verified = b2_.settle_canonical_index(verified);
      database_.reconcile_inventory(verified, config_.fingerprint_version);
      database_.set_metadata("last_inventory_sha256",
                             b2_.inventory_digest(verified));
      if (b2_.inventory_digest(verified) == analyzed_digest)
        return {computed, exact};
      inventory_ = database_.inventory();
      queue_.clear();
      edges_.clear();
    }

    report(progress, "B2 changed during analysis; synchronizing new media",
           pass, kMaximumStabilizationPasses);
  }
  throw std::runtime_error(
      "B2 kept changing throughout analysis; review remains locked until a "
      "stable, fully fingerprinted inventory can be established");
}

void Engine::rebuild_queue(Progress progress) {
  const auto object_cache =
      config_.cache_directory / kObjectCacheDirectory;
  if (!config_.keep_media_cache && std::filesystem::exists(object_cache)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(object_cache))
      if (entry.is_regular_file())
        std::filesystem::remove(entry.path());
  }
  inventory_ = database_.inventory();
  report(progress, "Comparing the surviving library");
  edges_ = matcher_.find_candidates(
      inventory_, database_.exclusions(),
      [&](std::size_t done, std::size_t total) {
        report(progress, "Comparing the surviving library", done, total);
      });
  generation_ = database_.advance_queue_generation();
  queue_ = matcher_.build_queue(inventory_, edges_, generation_);
}

StartupSummary Engine::startup(Progress progress) {
  std::scoped_lock lock(mutex_);
  report(progress, "Validating durable state");
  recover_operations(progress);
  report(progress, "Synchronizing the canonical B2 inventory");
  auto remote = b2_.stable_inventory(config_.canonical_prefix);
  remote = b2_.settle_canonical_index(remote);
  database_.reconcile_inventory(remote, config_.fingerprint_version);
  database_.set_metadata("last_inventory_sha256", b2_.inventory_digest(remote));
  const auto before = database_.inventory();
  const std::size_t reused =
      std::count_if(before.begin(), before.end(), [](const auto &item) {
        return item.fingerprint.has_value();
      });
  const auto [computed, exact] = stabilize_inventory(progress);
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
  check_generation(generation);
  const auto item = database_.object(key);
  if (!item || item->remote.file_id != expected_file_id)
    throw std::runtime_error(
        "The selected object changed after this card was shown");
  delete_batch({{key, expected_file_id}}, "manual_delete", progress);
  stabilize_inventory(progress);
}

void Engine::exclude_pair(const std::string &first, const std::string &second,
                          std::uint64_t generation, Progress progress) {
  std::scoped_lock lock(mutex_);
  check_generation(generation);
  const auto wanted = ordered_pair(first, second);
  const bool present =
      std::any_of(edges_.begin(), edges_.end(), [&](const auto &edge) {
        return ordered_pair(edge.a, edge.b) == wanted;
      });
  if (!present)
    throw std::runtime_error("That comparison is no longer pending");
  database_.exclude_pair(first, second);
  stabilize_inventory(progress);
}

void Engine::process_all(std::uint64_t generation, Progress progress) {
  std::scoped_lock lock(mutex_);
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
  stabilize_inventory(progress);
}

} // namespace gdupe
