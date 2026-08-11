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

struct IndexedHash {
  std::uint32_t owner{};
  std::uint64_t hash{};
};

class HammingIndex {
public:
  void add(std::uint64_t hash, std::uint32_t owner) {
    for (unsigned int band = 0; band < 4; ++band)
      buckets_[key(band, segment(hash, band))].push_back({owner, hash});
  }

  template <typename Callback>
  void query(std::uint64_t hash, std::uint32_t owner, int maximum_distance,
             int segment_distance, Callback callback) const {
    for (unsigned int band = 0; band < 4; ++band) {
      const std::uint16_t value = segment(hash, band);
      visit_neighbors(value, segment_distance, [&](std::uint16_t neighbor) {
        const auto found = buckets_.find(key(band, neighbor));
        if (found == buckets_.end())
          return;
        for (const auto &entry : found->second)
          if (entry.owner != owner &&
              Fingerprinter::hamming(hash, entry.hash) <= maximum_distance)
            callback(entry.owner);
      });
    }
  }

private:
  static std::uint16_t segment(std::uint64_t hash, unsigned int band) {
    return static_cast<std::uint16_t>((hash >> (band * 16U)) & 0xffffU);
  }

  static std::uint32_t key(unsigned int band, std::uint16_t value) {
    return (band << 16U) | value;
  }

  template <typename Callback>
  static void visit_neighbors(std::uint16_t value, int distance,
                              Callback callback) {
    callback(value);
    if (distance < 1)
      return;
    for (unsigned int first = 0; first < 16; ++first) {
      callback(static_cast<std::uint16_t>(value ^ (1U << first)));
      if (distance < 2)
        continue;
      for (unsigned int second = first + 1; second < 16; ++second) {
        callback(
            static_cast<std::uint16_t>(value ^ (1U << first) ^ (1U << second)));
        if (distance < 3)
          continue;
        for (unsigned int third = second + 1; third < 16; ++third)
          callback(static_cast<std::uint16_t>(value ^ (1U << first) ^
                                              (1U << second) ^ (1U << third)));
      }
    }
  }

  std::unordered_map<std::uint32_t, std::vector<IndexedHash>> buckets_;
};

} // namespace

double Matcher::sequence_distance(const std::vector<std::uint64_t> &first,
                                  const std::vector<std::uint64_t> &second,
                                  double expected_duration_ratio) const {
  if (first.empty() || second.empty())
    return 64.0;
  const auto &shorter = first.size() <= second.size() ? first : second;
  const auto &longer = first.size() <= second.size() ? second : first;
  const std::size_t segment_floor = std::min<std::size_t>(2, longer.size());
  const std::size_t expected_segment =
      std::clamp<std::size_t>(static_cast<std::size_t>(std::llround(
                                  longer.size() * expected_duration_ratio)),
                              segment_floor, longer.size());
  const std::size_t minimum_segment = std::max<std::size_t>(
      segment_floor,
      static_cast<std::size_t>(std::floor(expected_segment * 0.82)));
  const std::size_t maximum_segment = std::min<std::size_t>(
      longer.size(),
      static_cast<std::size_t>(std::ceil(expected_segment * 1.18)));
  double best = 64.0;
  for (std::size_t length = minimum_segment; length <= maximum_segment;
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
  const int hash256 = Fingerprinter::hamming(a.perceptual256, b.perceptual256);

  if (!a_moving) {
    if (phash <= config_.static_phash_distance &&
        hash256 <= config_.static_hash256_distance) {
      const double score =
          bounded_score(1.0 - (0.55 * phash / 64.0 + 0.45 * hash256 / 256.0));
      if (score >= config_.minimum_score)
        return CandidateEdge{first.remote.key, second.remote.key, score,
                             "same image after resize or recompression"};
    }
    if (hash256 > config_.crop_hash256_distance)
      return std::nullopt;
    int crop = 64;
    for (const auto right : b.crop_hashes)
      crop = std::min(crop, Fingerprinter::hamming(a.phash, right));
    for (const auto left : a.crop_hashes)
      crop = std::min(crop, Fingerprinter::hamming(left, b.phash));
    for (const auto left : a.crop_hashes)
      for (const auto right : b.crop_hashes)
        crop = std::min(crop, Fingerprinter::hamming(left, right));
    if (crop <= config_.crop_phash_distance &&
        hash256 <= config_.crop_hash256_distance) {
      const double score =
          bounded_score(0.96 - 0.45 * crop / 64.0 - 0.25 * hash256 / 256.0);
      if (score >= config_.minimum_score)
        return CandidateEdge{first.remote.key, second.remote.key, score,
                             "convincing crop or reframe"};
    }
    return std::nullopt;
  }

  if (phash > config_.moving_phash_distance ||
      hash256 > config_.crop_hash256_distance)
    return std::nullopt;
  double duration_ratio = 1.0;
  if (a.duration_ms > 0 && b.duration_ms > 0)
    duration_ratio =
        static_cast<double>(std::min(a.duration_ms, b.duration_ms)) /
        std::max(a.duration_ms, b.duration_ms);
  else if (a.frame_count > 0 && b.frame_count > 0)
    duration_ratio =
        static_cast<double>(std::min(a.frame_count, b.frame_count)) /
        std::max(a.frame_count, b.frame_count);
  const double timeline =
      sequence_distance(a.timeline, b.timeline, duration_ratio);
  if (timeline > config_.moving_timeline_distance)
    return std::nullopt;
  if (duration_ratio < config_.minimum_moving_overlap &&
      timeline > config_.moving_timeline_distance * 0.65)
    return std::nullopt;
  const double score =
      bounded_score(1.0 - 0.30 * phash / 64.0 - 0.25 * hash256 / 256.0 -
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
  if (inventory.size() < 2)
    return {};
  if (inventory.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("The inventory exceeds the matcher index limit");
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

  HammingIndex static_main;
  HammingIndex static_variants;
  HammingIndex moving_main;
  for (std::uint32_t index = 0; index < inventory.size(); ++index) {
    if (!inventory[index].fingerprint)
      continue;
    const auto &fingerprint = *inventory[index].fingerprint;
    if (fingerprint.kind == MediaKind::StaticImage) {
      static_main.add(fingerprint.phash, index);
      std::vector<std::uint64_t> variants = fingerprint.crop_hashes;
      variants.push_back(fingerprint.phash);
      std::sort(variants.begin(), variants.end());
      variants.erase(std::unique(variants.begin(), variants.end()),
                     variants.end());
      for (const auto hash : variants)
        static_variants.add(hash, index);
    } else {
      moving_main.add(fingerprint.phash, index);
    }
  }

  std::unordered_set<std::uint64_t> candidate_set;
  candidate_set.reserve(inventory.size() * 4);
  const auto add_candidate = [&](std::uint32_t first, std::uint32_t second) {
    const auto low = std::min(first, second), high = std::max(first, second);
    const auto packed = (static_cast<std::uint64_t>(low) << 32U) | high;
    if (!excluded_indices.contains(packed))
      candidate_set.insert(packed);
  };
  for (std::uint32_t index = 0; index < inventory.size(); ++index) {
    if (!inventory[index].fingerprint)
      continue;
    const auto &fingerprint = *inventory[index].fingerprint;
    if (fingerprint.kind == MediaKind::StaticImage) {
      static_main.query(
          fingerprint.phash, index, config_.static_phash_distance, 2,
          [&](std::uint32_t other) { add_candidate(index, other); });
      std::vector<std::uint64_t> variants = fingerprint.crop_hashes;
      variants.push_back(fingerprint.phash);
      std::sort(variants.begin(), variants.end());
      variants.erase(std::unique(variants.begin(), variants.end()),
                     variants.end());
      for (const auto hash : variants)
        static_variants.query(
            hash, index, config_.crop_phash_distance, 1,
            [&](std::uint32_t other) { add_candidate(index, other); });
    } else {
      moving_main.query(
          fingerprint.phash, index, config_.moving_phash_distance, 3,
          [&](std::uint32_t other) { add_candidate(index, other); });
    }
    if (progress && (index % 256 == 0 || index + 1 == inventory.size()))
      progress(index + 1, inventory.size());
  }

  std::vector<std::uint64_t> candidates(candidate_set.begin(),
                                        candidate_set.end());
  std::sort(candidates.begin(), candidates.end());
  if (candidates.empty())
    return {};
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> completed{0};
  std::mutex output_mutex;
  std::vector<CandidateEdge> output;
  const unsigned int configured =
      config_.worker_threads > 0
          ? static_cast<unsigned int>(config_.worker_threads)
          : std::thread::hardware_concurrency();
  const unsigned int threads = std::min<unsigned int>(
      std::max(1U, configured),
      static_cast<unsigned int>(std::min<std::size_t>(
          candidates.size(), std::numeric_limits<unsigned int>::max())));
  std::vector<std::jthread> workers;
  workers.reserve(threads);
  for (unsigned int worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&] {
      std::vector<CandidateEdge> local;
      while (true) {
        const std::size_t position = next.fetch_add(1);
        if (position >= candidates.size())
          break;
        const auto packed = candidates[position];
        const auto first = static_cast<std::uint32_t>(packed >> 32U);
        const auto second = static_cast<std::uint32_t>(packed & 0xffffffffU);
        if (auto candidate = compare(inventory[first], inventory[second]))
          local.push_back(std::move(*candidate));
        const std::size_t now = completed.fetch_add(1) + 1;
        if (progress && (position % 256 == 0 || now == candidates.size()))
          progress(now, candidates.size());
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
