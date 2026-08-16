#include "media_decode.hpp"
#include "media_test_util.hpp"
#include "nvdec_decode.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
    return 77;
  }

  try {
    gdupe_test::TempMedia media("hevc-main10-hvc1.mp4.b64", "mp4");
    const auto decoded = gdupe::decode_moving_media_static(
        media.path(), "mp4", 2,
        std::chrono::steady_clock::now() + std::chrono::seconds(30));
    if (decoded.width != 192 || decoded.height != 192)
      throw std::runtime_error("Main 10 dimensions were decoded incorrectly");
    if (decoded.sampled_frames.empty())
      throw std::runtime_error("Main 10 produced no decoded frames");
    for (const auto &frame : decoded.sampled_frames) {
      if (frame.width != 192 || frame.height != 192 ||
          frame.pixels.size() != 192U * 192U)
        throw std::runtime_error("Main 10 grayscale output is malformed");
    }

    std::cout << "HEVC Main 10 NVDEC decode passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "::error title=HEVC Main 10 NVDEC decode failed::" << e.what()
              << '\n';
    std::cerr << "HEVC Main 10 NVDEC decode failed: " << e.what() << '\n';
    return 1;
  }
}
