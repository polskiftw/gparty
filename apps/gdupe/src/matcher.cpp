#include "matcher.hpp"

#include "database.hpp"
#include "fingerprint.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace gdupe {
namespace {

bool lossless(const std::string &extension) {
  return extension == "png" || extension == "webp" || extension == "gif";
}

double bounded_score(double value) { return std::clamp(value, 0.0, 1.0); }

} // namespace

double
Matcher::sequence_distance(const std::vector<std::uint64_t> &first,
                           const std::vector<std::uint64_t> &second) const {
  if (first.empty() || second.empty())
    return 64.0;
  const auto &shorter = first.size() <= second.size() ? first : second;
  const auto &longer = first.size() <= second.size() ? second : first;
  const std::size_t minimum_segment = std::max<std::size_t>(
      2, static_cast<std::size_t>(
             std::ceil(shorter.size() * config_.minimum_moving_overlap)));
  double best = 64.0;
  for (std::size_t length = minimum_segment; length <= longer.size();
       ++length) {
    for (std::size_t start = 0; start + length <= longer.size(); ++start) {
      double total = 0.0;
      for (std::size_t index = 0; index < shorter.size(); ++index) {
        const std::size_t mapped =
            start +
            std::min(length - 1,
                     static_cast<std::size_t>(std::llround(
                         static_cast<double>(index) * (length - 1) /
                         std::max<std::size_t>(1, shorter.size() - 1))));
        total += Fingerprinter::hamming(shorter[index], longer[mapped]);
      }
      best = std::min(best, total / shorter.size());
    }
  }
  return best;
}

std::optional<CandidateEdge>
Matcher::compare(const InventoryObject &first,
                 const InventoryObject &second) const {
  if (!first.fingerprint || !second.fingerprint)
    return std::nullopt;
  const auto &a = *first.fingerprint;
  const auto &b = *second.fingerprint;
  if (a.sha256 == b.sha256)
    return std::nullopt;
  const bool a_moving = a.kind != MediaKind::StaticImage;
  const bool b_moving = b.kind != MediaKind::StaticImage;
  if (a_moving != b_moving)
    return std::nullopt;
  const int phash = Fingerprinter::hamming(a.phash, b.phash);
  const int gradient = Fingerprinter::hamming(a.gradient256, b.gradient256);

  if (!a_moving) {
    if (phash <= config_.static_phash_distance &&
        gradient <= config_.static_gradient_distance) {
      const double score =
          bounded_score(1.0 - (0.55 * phash / 64.0 + 0.45 * gradient / 256.0));
      if (score >= config_.minimum_score)
        return CandidateEdge{first.remote.key, second.remote.key, score,
                             "same image after resize or recompression"};
    }
    int crop = 64;
    for (const auto left : a.crop_hashes)
      for (const auto right : b.crop_hashes)
        crop = std::min(crop, Fingerprinter::hamming(left, right));
    if (crop <= config_.crop_phash_distance &&
        gradient <= config_.crop_gradient_distance) {
      const double score =
          bounded_score(0.96 - 0.45 * crop / 64.0 - 0.25 * gradient / 256.0);
      if (score >= config_.minimum_score)
        return CandidateEdge{first.remote.key, second.remote.key, score,
                             "convincing crop or reframe"};
    }
    return std::nullopt;
  }

  if (phash > config_.moving_phash_distance ||
      gradient > config_.crop_gradient_distance)
    return std::nullopt;
  const double timeline = sequence_distance(a.timeline, b.timeline);
  if (timeline > config_.moving_timeline_distance)
    return std::nullopt;
  double duration_ratio = 1.0;
  if (a.duration_ms > 0 && b.duration_ms > 0)
    duration_ratio =
        static_cast<double>(std::min(a.duration_ms, b.duration_ms)) /
        std::max(a.duration_ms, b.duration_ms);
  if (duration_ratio < config_.minimum_moving_overlap &&
      timeline > config_.moving_timeline_distance * 0.65)
    return std::nullopt;
  const double score =
      bounded_score(1.0 - 0.30 * phash / 64.0 - 0.25 * gradient / 256.0 -
                    0.45 * timeline / 64.0);
  if (score < config_.minimum_score)
    return std::nullopt;
  const std::string evidence =
      duration_ratio < 0.9 ? "strongly matched moving-media excerpt"
                           : "same moving media after re-encode or resize";
  return CandidateEdge{first.remote.key, second.remote.key, score, evidence};
}

std::vector<CandidateEdge> Matcher::find_candidates(
    const std::vector<InventoryObject> &inventory,
    const std::set<std::pair<std::string, std::string>> &exclusions,
    Progress progress) const {
  const std::size_t total =
      inventory.size() < 2 ? 0 : inventory.size() * (inventory.size() - 1) / 2;
  if (total == 0)
    return {};
  std::unordered_map<std::string, std::uint32_t> index_by_key;
  index_by_key.reserve(inventory.size());
  for (std::uint32_t index = 0; index < inventory.size(); ++index)
    index_by_key.emplace(inventory[index].remote.key, index);
  std::unordered_set<std::uint64_t> excluded_indices;
  excluded_indices.reserve(exclusions.size());
  for (const auto &[first, second] : exclusions) {
    const auto a = index_by_key.find(first), b = index_by_key.find(second);
    if (a == index_by_key.end() || b == index_by_key.end())
      continue;
    const auto low = std::min(a->second, b->second),
               high = std::max(a->second, b->second);
    excluded_indices.insert((static_cast<std::uint64_t>(low) << 32U) | high);
  }
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> completed{0};
  std::mutex output_mutex;
  std::vector<CandidateEdge> output;
  const unsigned int configured =
      config_.worker_threads > 0
          ? static_cast<unsigned int>(config_.worker_threads)
          : std::thread::hardware_concurrency();
  const unsigned int threads = std::max(1U, configured);
  std::vector<std::jthread> workers;
  workers.reserve(threads);
  for (unsigned int worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&] {
      std::vector<CandidateEdge> local;
      while (true) {
        const std::size_t i = next.fetch_add(1);
        if (i >= inventory.size())
          break;
        for (std::size_t j = i + 1; j < inventory.size(); ++j) {
          const auto packed = (static_cast<std::uint64_t>(i) << 32U) |
                              static_cast<std::uint64_t>(j);
          if (!excluded_indices.contains(packed)) {
            if (auto candidate = compare(inventory[i], inventory[j]))
              local.push_back(std::move(*candidate));
          }
        }
        const std::size_t now = completed.fetch_add(inventory.size() - i - 1) +
                                inventory.size() - i - 1;
        if (progress && (i % 256 == 0 || now == total))
          progress(now, total);
      }
      std::scoped_lock lock(output_mutex);
      output.insert(output.end(), std::make_move_iterator(local.begin()),
                    std::make_move_iterator(local.end()));
    });
  }
  workers.clear();
  std::sort(
      output.begin(), output.end(), [](const auto &left, const auto &right) {
        if (left.score != right.score)
          return left.score > right.score;
        return ordered_pair(left.a, left.b) < ordered_pair(right.a, right.b);
      });
  return output;
}

double Matcher::survivor_quality(const InventoryObject &object) const {
  if (!object.fingerprint)
    return -std::numeric_limits<double>::infinity();
  const auto &value = *object.fingerprint;
  const double pixels =
      std::max(1.0, static_cast<double>(value.width) * value.height);
  const double duration = std::max(1.0, static_cast<double>(value.duration_ms));
  return config_.resolution_weight * std::log2(pixels) +
         (value.kind == MediaKind::StaticImage
              ? 0.0
              : config_.duration_weight * std::log2(duration)) +
         (lossless(object.remote.extension) ? config_.lossless_bonus : 0.0) +
         config_.size_weight *
             std::log2(std::max<std::uint64_t>(1, object.remote.size));
}

std::vector<ReviewPair>
Matcher::build_queue(const std::vector<InventoryObject> &inventory,
                     const std::vector<CandidateEdge> &edges,
                     std::uint64_t generation) const {
  std::unordered_map<std::string, const InventoryObject *> by_key;
  for (const auto &item : inventory)
    by_key.emplace(item.remote.key, &item);
  std::unordered_map<std::string, std::unordered_set<std::string>> neighbors;
  std::map<std::pair<std::string, std::string>, const CandidateEdge *> by_pair;
  for (const auto &edge : edges) {
    neighbors[edge.a].insert(edge.b);
    neighbors[edge.b].insert(edge.a);
    by_pair[ordered_pair(edge.a, edge.b)] = &edge;
  }
  std::vector<std::string> order;
  for (const auto &[key, _] : neighbors)
    order.push_back(key);
  std::sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
    const double qa = survivor_quality(*by_key.at(a)),
                 qb = survivor_quality(*by_key.at(b));
    return qa != qb ? qa > qb : a < b;
  });
  std::unordered_set<std::string> active(order.begin(), order.end());
  std::vector<ReviewPair> queue;
  for (const auto &survivor : order) {
    if (!active.contains(survivor))
      continue;
    active.erase(survivor);
    std::vector<std::string> redundant;
    for (const auto &key : neighbors[survivor])
      if (active.contains(key))
        redundant.push_back(key);
    std::sort(redundant.begin(), redundant.end());
    for (const auto &key : redundant) {
      active.erase(key);
      const auto *edge = by_pair.at(ordered_pair(survivor, key));
      queue.push_back({*by_key.at(survivor), *by_key.at(key), edge->score,
                       edge->evidence, generation});
    }
  }
  return queue;
}

std::vector<std::string>
Matcher::process_all_deletions(const std::vector<InventoryObject> &inventory,
                               const std::vector<CandidateEdge> &edges) const {
  std::unordered_map<std::string, std::unordered_set<std::string>> neighbors;
  for (const auto &edge : edges) {
    neighbors[edge.a].insert(edge.b);
    neighbors[edge.b].insert(edge.a);
  }
  std::unordered_map<std::string, const InventoryObject *> by_key;
  for (const auto &item : inventory)
    by_key.emplace(item.remote.key, &item);
  std::vector<std::string> order;
  for (const auto &[key, _] : neighbors)
    order.push_back(key);
  std::sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
    const double qa = survivor_quality(*by_key.at(a));
    const double qb = survivor_quality(*by_key.at(b));
    return qa != qb ? qa > qb : a < b;
  });
  std::unordered_set<std::string> active(order.begin(), order.end());
  std::vector<std::string> deleted;
  for (const auto &survivor : order) {
    if (!active.contains(survivor))
      continue;
    active.erase(survivor);
    std::vector<std::string> redundant;
    for (const auto &neighbor : neighbors[survivor])
      if (active.contains(neighbor))
        redundant.push_back(neighbor);
    std::sort(redundant.begin(), redundant.end());
    for (const auto &key : redundant) {
      active.erase(key);
      deleted.push_back(key);
    }
  }
  return deleted;
}

} // namespace gdupe
