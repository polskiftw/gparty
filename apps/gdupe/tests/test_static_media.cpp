#include "media_decode.hpp"
#include "media_test_util.hpp"
#include "nvdec_decode.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using gdupe_test::TempMedia;
using gdupe_test::require;

void test_h264_avc1_mp4() {
  TempMedia media("h264-avc1.mp4.b64", "mp4");
  require(media.size() > 1000,
          "H.264/AVC fixture did not decode from base64");

  const auto decoded = gdupe::decode_moving_media_static(
      media.path(), "mp4", 2,
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  require(decoded.width == 64 && decoded.height == 64,
          "H.264/AVC avc1 decode dimensions are wrong");
  require(decoded.frame_count == 2, "H.264/AVC avc1 frame count is wrong");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          "H.264/AVC avc1 duration is wrong");
  require(!decoded.sampled_frames.empty(),
          "H.264/AVC avc1 produced no sampled grayscale frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 64 && frame.height == 64 &&
                frame.pixels.size() == 64U * 64U,
            "H.264/AVC avc1 grayscale output is malformed");
  }
}

void test_hevc_main_hvc1_mp4() {
  TempMedia media("hevc-main-hvc1.mp4.b64", "mp4");
  require(media.size() > 1000,
          "HEVC Main fixture did not decode from base64");

  const auto decoded = gdupe::decode_moving_media_static(
      media.path(), "mp4", 2,
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  require(decoded.width == 192 && decoded.height == 192,
          "HEVC Main hvc1 decode dimensions are wrong");
  require(decoded.frame_count == 2, "HEVC Main hvc1 frame count is wrong");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          "HEVC Main hvc1 duration is wrong");
  require(!decoded.sampled_frames.empty(),
          "HEVC Main hvc1 produced no sampled grayscale frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 192 && frame.height == 192 &&
                frame.pixels.size() == 192U * 192U,
            "HEVC Main hvc1 grayscale output is malformed");
  }
}

} // namespace

int main() {
  try {
    if (!gdupe::nvdec_runtime_available()) {
      std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
      return 77;
    }
    test_h264_avc1_mp4();
    test_hevc_main_hvc1_mp4();
    std::cout << "H.264/AVC and HEVC Main NVDEC tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "H.264/AVC + HEVC Main NVDEC test failure: " << e.what()
              << '\n';
    return 1;
  }
}
