#include "media_test_util.hpp"
#include "nvdec_decode.hpp"
#include "preview_color.hpp"
#include "preview_decode.hpp"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace {

using gdupe_test::TempMedia;
using gdupe_test::require;

std::uint64_t preview_checksum(std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t value = 1469598103934665603ULL;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

void test_cpu_helpers() {
  const std::vector<std::uint8_t> y8{16, 16, 235, 235};
  const std::vector<std::uint8_t> uv8{128, 128};
  const auto pixels8 = gdupe::nv12_to_bgra(
      y8, uv8, 2, 2, false, gdupe::PreviewColorMatrix::bt709);
  require(pixels8.size() == 16 && pixels8[0] <= 1 && pixels8[8] >= 254,
          "limited-range NV12 conversion is wrong");

  constexpr unsigned shift = 6;
  const std::vector<std::uint16_t> y10{
      static_cast<std::uint16_t>(64U << shift),
      static_cast<std::uint16_t>(64U << shift),
      static_cast<std::uint16_t>(940U << shift),
      static_cast<std::uint16_t>(940U << shift)};
  const std::vector<std::uint16_t> uv10{
      static_cast<std::uint16_t>(512U << shift),
      static_cast<std::uint16_t>(512U << shift)};
  const auto pixels10 = gdupe::p016_to_bgra(
      y10, uv10, 2, 2, 10, false, gdupe::PreviewColorMatrix::bt709);
  require(pixels10.size() == 16 && pixels10[0] <= 1 && pixels10[8] >= 254,
          "limited-range Main10/P016 conversion is wrong");

  const auto wide = gdupe::fit_preview_rect(1920, 1080, {0, 0, 500, 500}, 0);
  require(std::fabs((wide.right - wide.left) - 500.0F) < 0.01F &&
              std::fabs((wide.bottom - wide.top) - 281.25F) < 0.02F &&
              std::fabs(wide.top - 109.375F) < 0.02F,
          "16:9 aspect fit is wrong");
}

struct Spec {
  const char *fixture;
  const char *extension;
  const char *name;
  int width;
  int height;
};

void decode_fixture(const Spec &spec) {
  TempMedia media(spec.fixture, spec.extension);
  std::optional<gdupe::PreviewDecodedFrame> first;
  std::stop_source stop;
  const auto result = gdupe::decode_video_preview_once(
      media.path(), spec.extension, stop.get_token(),
      [&](gdupe::PreviewDecodedFrame frame) {
        first = std::move(frame);
        return false;
      });
  require(result.decoded_frames >= 1 && first.has_value(),
          std::string(spec.name) + " preview produced no frame");
  require(first->width == spec.width && first->height == spec.height &&
              first->premultiplied_bgra.size() ==
                  static_cast<std::size_t>(spec.width) * spec.height * 4U,
          std::string(spec.name) + " preview frame is malformed");
  const auto checksum = preview_checksum(first->premultiplied_bgra);
  require(checksum != 0, std::string(spec.name) + " preview checksum is zero");
  std::cout << spec.name << " preview checksum: 0x" << std::hex << checksum
            << std::dec << '\n';
}

} // namespace

int main() {
  try {
    test_cpu_helpers();
    if (!gdupe::nvdec_runtime_available()) {
      std::cout << "SKIP: CPU preview tests passed; NVIDIA NVDEC unavailable\n";
      return 77;
    }

    for (const Spec &spec : {
             Spec{"h264-avc1.mp4.b64", "mp4", "H.264/AVC", 64, 64},
             Spec{"hevc-main-hvc1.mp4.b64", "mp4", "HEVC Main", 192, 192},
             Spec{"hevc-main10-hvc1.mp4.b64", "mp4", "HEVC Main 10", 192,
                  192},
             Spec{"vp8.webm.b64", "webm", "VP8", 128, 128},
             Spec{"vp9.webm.b64", "webm", "VP9", 128, 128},
             Spec{"av1.webm.b64", "webm", "AV1", 128, 128},
         })
      decode_fixture(spec);

    std::cout << "NVDEC preview frame tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "NVDEC preview frame test failure: " << problem.what() << '\n';
    return 1;
  }
}
