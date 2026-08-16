#include "media_test_util.hpp"
#include "nvdec_decode.hpp"
#include "video_preview.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using gdupe_test::TempMedia;
using gdupe_test::require;

std::optional<gdupe::PreviewDecodedFrame>
wait_for_frame(gdupe::VideoPreview &preview) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto frame = preview.take_latest())
      return frame;
    if (preview.failed())
      throw std::runtime_error("preview worker failed: " +
                               preview.failure_message());
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

void test_replacement_and_two_instances() {
  TempMedia h264("h264-avc1.mp4.b64", "mp4");
  TempMedia hevc("hevc-main-hvc1.mp4.b64", "mp4");
  TempMedia vp9("vp9.webm.b64", "webm");

  gdupe::VideoPreview replaceable;
  replaceable.start(h264.path(), "mp4");
  auto first = wait_for_frame(replaceable);
  require(first && first->width == 64, "H.264 preview did not start");
  replaceable.start(hevc.path(), "mp4");
  auto replacement = wait_for_frame(replaceable);
  require(replacement && replacement->width == 192,
          "candidate replacement did not publish HEVC");
  replaceable.stop();

  gdupe::VideoPreview left;
  gdupe::VideoPreview right;
  left.start(h264.path(), "mp4");
  right.start(vp9.path(), "webm");
  require(wait_for_frame(left).has_value() && wait_for_frame(right).has_value(),
          "two simultaneous preview instances did not both publish frames");
  left.stop();
  right.stop();
}

void test_bad_media_fails_closed() {
  TempMedia invalid("mp4");
  gdupe::VideoPreview preview;
  preview.start(invalid.path(), "mp4");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline && !preview.failed())
    std::this_thread::sleep_for(10ms);
  require(preview.failed() && !preview.take_latest(),
          "invalid video did not fail closed without publishing a frame");
  preview.stop();
}
} // namespace

int main() {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC unavailable for preview lifecycle tests\n";
    return 77;
  }
  try {
    test_replacement_and_two_instances();
    test_bad_media_fails_closed();
    std::cout << "NVDEC preview lifecycle tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "NVDEC preview lifecycle failure: " << problem.what() << '\n';
    return 1;
  }
}
