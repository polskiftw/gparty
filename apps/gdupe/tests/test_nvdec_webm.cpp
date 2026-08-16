#include "media_decode.hpp"
#include "media_test_util.hpp"
#include "nvdec_decode.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using gdupe_test::TempMedia;
using gdupe_test::require;

void test_fixture(std::string_view fixture_name, std::string_view codec_name) {
  const std::string codec(codec_name);
  TempMedia media(fixture_name, "webm");
  require(media.size() > 500,
          codec + " WebM fixture did not decode from base64");

  const auto decoded = gdupe::decode_moving_media_static(
      media.path(), "webm", 2,
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  require(decoded.width == 128 && decoded.height == 128,
          codec + " WebM dimensions are wrong");
  require(decoded.frame_count == 2, codec + " WebM frame count is wrong");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          codec + " WebM duration is wrong");
  require(!decoded.sampled_frames.empty(),
          codec + " WebM produced no sampled frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 128 && frame.height == 128 &&
                frame.pixels.size() == 128U * 128U,
            codec + " WebM grayscale output is malformed");
  }
}

} // namespace

int main() {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
    return 77;
  }

  try {
    test_fixture("vp8.webm.b64", "VP8");
    test_fixture("vp9.webm.b64", "VP9");
    test_fixture("av1.webm.b64", "AV1");
    std::cout << "VP8/VP9/AV1 NVDEC WebM tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "::error title=NVDEC WebM regression failed::" << problem.what()
              << '\n';
    std::cerr << "NVDEC WebM regression failed: " << problem.what() << '\n';
    return 1;
  }
}
