#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gdupe {

struct RgbImage {
  int width{};
  int height{};
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept {
    return width <= 0 || height <= 0 || pixels.empty();
  }
};

RgbImage decode_static_image(const std::filesystem::path &path,
                             const std::string &extension);

} // namespace gdupe
