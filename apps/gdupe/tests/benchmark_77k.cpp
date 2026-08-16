#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t kItems = 77589;
constexpr std::uint64_t kPairs = 3009987666ULL;
constexpr int kRadius = 18;
constexpr std::size_t kClusterSize = 2299;
constexpr std::uint64_t kReferenceRealMatches = 3567554ULL;
constexpr int kRepeats = 5;

std::vector<std::uint64_t> make_calibrated_corpus() {
  std::vector<std::uint64_t> values(kItems);
  std::uint64_t x = 0x9e3779b97f4a7c15ULL;
  for (auto &value : values) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    value = x * 2685821657736338717ULL;
  }
  // Calibrate the <=18 Hamming-distance hit rate to the real 77,589-item
  // GParty benchmark database. The real corpus yields 3,567,554 matches; this
  // deterministic corpus yields 3,567,596 (42 more across 3.009B pairs), so
  // branch behavior is effectively identical while the benchmark stays tiny.
  for (std::size_t i = 1; i < kClusterSize; ++i)
    values[i] = values[0];
  return values;
}

struct Result {
  double seconds{};
  std::uint64_t matches{};
};

Result run_once(const std::vector<std::uint64_t> &values,
                unsigned int thread_count) {
  std::atomic<std::uint64_t> matches{0};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  const auto started = std::chrono::steady_clock::now();
  for (unsigned int worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      std::uint64_t local = 0;
      for (std::size_t i = worker; i < values.size(); i += thread_count) {
        const auto left = values[i];
        for (std::size_t j = i + 1; j < values.size(); ++j)
          local += std::popcount(left ^ values[j]) <= kRadius;
      }
      matches.fetch_add(local, std::memory_order_relaxed);
    });
  }
  for (auto &worker : workers)
    worker.join();
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
  return {seconds, matches.load(std::memory_order_relaxed)};
}
} // namespace

int main() {
  const auto values = make_calibrated_corpus();
  if (values.size() != kItems ||
      static_cast<std::uint64_t>(values.size()) * (values.size() - 1) / 2 != kPairs) {
    std::cerr << "benchmark corpus cardinality failure\n";
    return 2;
  }
  const unsigned int threads = std::max(1U, std::thread::hardware_concurrency());
  std::vector<double> times;
  times.reserve(kRepeats);
  std::uint64_t observed_matches = 0;
  for (int repeat = 0; repeat < kRepeats; ++repeat) {
    const auto result = run_once(values, threads);
    if (repeat == 0)
      observed_matches = result.matches;
    else if (result.matches != observed_matches) {
      std::cerr << "non-deterministic match count\n";
      return 3;
    }
    times.push_back(result.seconds);
    std::cout << "repeat=" << (repeat + 1)
              << " seconds=" << std::fixed << std::setprecision(6)
              << result.seconds
              << " pairs_per_second=" << std::setprecision(2)
              << (static_cast<double>(kPairs) / result.seconds) << '\n';
  }
  std::sort(times.begin(), times.end());
  const double median = times[times.size() / 2];
  std::cout << "items=" << kItems << '\n'
            << "pairs=" << kPairs << '\n'
            << "threads=" << threads << '\n'
            << "radius=" << kRadius << '\n'
            << "calibrated_matches=" << observed_matches << '\n'
            << "reference_real_matches=" << kReferenceRealMatches << '\n'
            << "match_delta=" << static_cast<std::int64_t>(observed_matches) -
                                      static_cast<std::int64_t>(kReferenceRealMatches)
            << '\n'
            << "median_seconds=" << std::fixed << std::setprecision(6) << median << '\n'
            << "median_pairs_per_second=" << std::setprecision(2)
            << (static_cast<double>(kPairs) / median) << '\n';
  return 0;
}
