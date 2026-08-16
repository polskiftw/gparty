#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gdupe {

enum class PreviewColorMatrix {
  bt601,
  bt709,
  bt2020,
};

struct PreviewRect {
  float left{};
  float top{};
  float right{};
  float bottom{};
};

std::vector<std::uint8_t> nv12_to_bgra(
    std::span<const std::uint8_t> luma,
    std::span<const std::uint8_t> chroma, int width, int height,
    bool full_range, PreviewColorMatrix matrix);

std::vector<std::uint8_t> p016_to_bgra(
    std::span<const std::uint16_t> luma,
    std::span<const std::uint16_t> chroma, int width, int height,
    unsigned bit_depth, bool full_range, PreviewColorMatrix matrix);

PreviewRect fit_preview_rect(int source_width, int source_height,
                             PreviewRect bounds, float inset = 12.0F);

} // namespace gdupe
