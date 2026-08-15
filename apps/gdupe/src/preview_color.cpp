#include "preview_color.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxPreviewPixels = 100'000'000ULL;

struct MatrixCoefficients {
  double kr{};
  double kb{};
};

MatrixCoefficients coefficients(PreviewColorMatrix matrix) noexcept {
  switch (matrix) {
  case PreviewColorMatrix::bt601:
    return {0.2990, 0.1140};
  case PreviewColorMatrix::bt2020:
    return {0.2627, 0.0593};
  case PreviewColorMatrix::bt709:
  default:
    return {0.2126, 0.0722};
  }
}

void validate_dimensions(int width, int height) {
  if (width <= 0 || height <= 0)
    throw std::runtime_error("Preview frame has invalid dimensions");
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > kMaxPreviewPixels ||
      pixels > std::numeric_limits<std::size_t>::max() / 4U)
    throw std::runtime_error("Preview frame exceeds gdupe's safety limit");
}

std::size_t chroma_stride_samples(int width) {
  return static_cast<std::size_t>((width + 1) / 2) * 2U;
}

std::size_t chroma_rows(int height) {
  return static_cast<std::size_t>((height + 1) / 2);
}

std::uint8_t byte_from_unit(double value) noexcept {
  const double bounded = std::clamp(value, 0.0, 1.0);
  return static_cast<std::uint8_t>(std::lround(bounded * 255.0));
}

void write_bgra(std::vector<std::uint8_t> &output, std::size_t pixel,
                double y, double cb, double cr,
                PreviewColorMatrix matrix) noexcept {
  const auto [kr, kb] = coefficients(matrix);
  const double kg = 1.0 - kr - kb;
  const double r = y + (2.0 - 2.0 * kr) * cr;
  const double b = y + (2.0 - 2.0 * kb) * cb;
  const double g = (y - kr * r - kb * b) / kg;
  const std::size_t offset = pixel * 4U;
  output[offset + 0] = byte_from_unit(b);
  output[offset + 1] = byte_from_unit(g);
  output[offset + 2] = byte_from_unit(r);
  output[offset + 3] = 255;
}

struct NormalizedYuv {
  double y{};
  double cb{};
  double cr{};
};

NormalizedYuv normalize_components(std::uint32_t y, std::uint32_t u,
                                   std::uint32_t v, unsigned bit_depth,
                                   bool full_range) {
  if (bit_depth < 8 || bit_depth > 16)
    throw std::runtime_error("Preview conversion received an invalid bit depth");

  if (full_range) {
    const double maximum =
        static_cast<double>((std::uint64_t{1} << bit_depth) - 1U);
    const double center = static_cast<double>(std::uint64_t{1}
                                              << (bit_depth - 1U));
    return {static_cast<double>(y) / maximum,
            (static_cast<double>(u) - center) / maximum,
            (static_cast<double>(v) - center) / maximum};
  }

  const std::uint32_t scale = std::uint32_t{1} << (bit_depth - 8U);
  const double y_black = static_cast<double>(16U * scale);
  const double y_range = static_cast<double>(219U * scale);
  const double c_center = static_cast<double>(128U * scale);
  const double c_range = static_cast<double>(224U * scale);
  return {(static_cast<double>(y) - y_black) / y_range,
          (static_cast<double>(u) - c_center) / c_range,
          (static_cast<double>(v) - c_center) / c_range};
}

} // namespace

std::vector<std::uint8_t> nv12_to_bgra(
    std::span<const std::uint8_t> luma,
    std::span<const std::uint8_t> chroma, int width, int height,
    bool full_range, PreviewColorMatrix matrix) {
  validate_dimensions(width, height);
  const std::size_t y_size = static_cast<std::size_t>(width) * height;
  const std::size_t uv_stride = chroma_stride_samples(width);
  const std::size_t uv_size = uv_stride * chroma_rows(height);
  if (luma.size() < y_size || chroma.size() < uv_size)
    throw std::runtime_error("NV12 preview surface is truncated");

  std::vector<std::uint8_t> output(y_size * 4U);
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const std::size_t pixel = static_cast<std::size_t>(row) * width + column;
      const std::size_t uv = static_cast<std::size_t>(row / 2) * uv_stride +
                             static_cast<std::size_t>(column / 2) * 2U;
      const auto normalized = normalize_components(
          luma[pixel], chroma[uv], chroma[uv + 1], 8, full_range);
      write_bgra(output, pixel, normalized.y, normalized.cb, normalized.cr,
                 matrix);
    }
  }
  return output;
}

std::vector<std::uint8_t> p016_to_bgra(
    std::span<const std::uint16_t> luma,
    std::span<const std::uint16_t> chroma, int width, int height,
    unsigned bit_depth, bool full_range, PreviewColorMatrix matrix) {
  validate_dimensions(width, height);
  if (bit_depth <= 8 || bit_depth > 16)
    throw std::runtime_error("P016 preview surface has an invalid bit depth");
  const std::size_t y_size = static_cast<std::size_t>(width) * height;
  const std::size_t uv_stride = chroma_stride_samples(width);
  const std::size_t uv_size = uv_stride * chroma_rows(height);
  if (luma.size() < y_size || chroma.size() < uv_size)
    throw std::runtime_error("P016 preview surface is truncated");

  const unsigned shift = 16U - bit_depth;
  const std::uint32_t maximum = (std::uint32_t{1} << bit_depth) - 1U;
  const auto nominal = [shift, maximum](std::uint16_t sample) {
    return std::min<std::uint32_t>(sample >> shift, maximum);
  };

  std::vector<std::uint8_t> output(y_size * 4U);
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const std::size_t pixel = static_cast<std::size_t>(row) * width + column;
      const std::size_t uv = static_cast<std::size_t>(row / 2) * uv_stride +
                             static_cast<std::size_t>(column / 2) * 2U;
      const auto normalized = normalize_components(
          nominal(luma[pixel]), nominal(chroma[uv]), nominal(chroma[uv + 1]),
          bit_depth, full_range);
      write_bgra(output, pixel, normalized.y, normalized.cb, normalized.cr,
                 matrix);
    }
  }
  return output;
}

PreviewRect fit_preview_rect(int source_width, int source_height,
                             PreviewRect bounds, float inset) {
  if (source_width <= 0 || source_height <= 0)
    throw std::runtime_error("Cannot fit a preview with invalid dimensions");
  const float left = bounds.left + inset;
  const float top = bounds.top + inset;
  const float right = bounds.right - inset;
  const float bottom = bounds.bottom - inset;
  const float available_width = std::max(1.0F, right - left);
  const float available_height = std::max(1.0F, bottom - top);
  const float scale =
      std::min(available_width / static_cast<float>(source_width),
               available_height / static_cast<float>(source_height));
  const float width = source_width * scale;
  const float height = source_height * scale;
  const float center_x = (bounds.left + bounds.right) * 0.5F;
  const float center_y = (bounds.top + bounds.bottom) * 0.5F;
  return {center_x - width * 0.5F, center_y - height * 0.5F,
          center_x + width * 0.5F, center_y + height * 0.5F};
}

std::uint64_t preview_checksum(std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t value = 1469598103934665603ULL;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

} // namespace gdupe
