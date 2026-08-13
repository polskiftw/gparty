#pragma once

#include "config.hpp"
#include "model.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace gdupe {

class Fingerprinter {
public:
  explicit Fingerprinter(const Config &config) : config_(config) {}
  Fingerprint compute(const std::filesystem::path &path,
                      const std::string &extension) const;

  static int hamming(std::uint64_t first, std::uint64_t second);
  static int hamming(const std::array<std::uint8_t, 32> &first,
                     const std::array<std::uint8_t, 32> &second);

private:
  Config config_;
  Fingerprint static_image(const std::filesystem::path &path) const;
  Fingerprint moving_media(const std::filesystem::path &path, bool gif) const;
};

} // namespace gdupe
