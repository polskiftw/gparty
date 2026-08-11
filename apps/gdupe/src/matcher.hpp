#pragma once

#include "config.hpp"
#include "model.hpp"

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdupe {

class Matcher {
public:
  using Progress =
      std::function<void(std::size_t completed, std::size_t total)>;
  explicit Matcher(const Config &config) : config_(config) {}

  std::vector<CandidateEdge> find_candidates(
      const std::vector<InventoryObject> &inventory,
      const std::set<std::pair<std::string, std::string>> &exclusions,
      Progress progress = {}) const;
  std::vector<ReviewPair>
  build_queue(const std::vector<InventoryObject> &inventory,
              const std::vector<CandidateEdge> &edges,
              std::uint64_t generation) const;
  std::vector<std::string>
  process_all_deletions(const std::vector<InventoryObject> &inventory,
                        const std::vector<CandidateEdge> &edges) const;
  double survivor_quality(const InventoryObject &object) const;

private:
  Config config_;
  std::optional<CandidateEdge> compare(const InventoryObject &first,
                                       const InventoryObject &second) const;
  double sequence_distance(const std::vector<std::uint64_t> &first,
                           const std::vector<std::uint64_t> &second) const;
};

} // namespace gdupe
