#include "video_preview.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gdupe {
namespace {

bool wait_until(std::stop_token stop,
                std::chrono::steady_clock::time_point target) {
  using namespace std::chrono_literals;
  while (!stop.stop_requested()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= target)
      return true;
    std::this_thread::sleep_for(
        std::min(target - now,
                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     10ms)));
  }
  return false;
}

} // namespace

VideoPreview::~VideoPreview() { stop(); }

void VideoPreview::start(const std::filesystem::path &path,
                         const std::string &extension) {
  start(path, extension, std::chrono::steady_clock::now());
}

void VideoPreview::start(
    const std::filesystem::path &path, const std::string &extension,
    std::chrono::steady_clock::time_point playback_origin) {
  stop();
  {
    std::lock_guard lock(mutex_);
    latest_.reset();
    failure_.clear();
  }
  worker_ = std::jthread(
      [this, path, extension, playback_origin](std::stop_token stop) mutable {
        run(stop, std::move(path), std::move(extension), playback_origin);
      });
}

void VideoPreview::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

std::optional<PreviewDecodedFrame> VideoPreview::take_latest() {
  std::lock_guard lock(mutex_);
  if (!latest_)
    return std::nullopt;
  auto frame = std::move(latest_);
  latest_.reset();
  return frame;
}

bool VideoPreview::failed() const {
  std::lock_guard lock(mutex_);
  return !failure_.empty();
}

std::string VideoPreview::failure_message() const {
  std::lock_guard lock(mutex_);
  return failure_;
}

void VideoPreview::run(
    std::stop_token stop, std::filesystem::path path, std::string extension,
    std::chrono::steady_clock::time_point playback_origin) {
  try {
    auto loop_started = playback_origin;
    while (!stop.stop_requested()) {
      std::int64_t first_timestamp = -1;
      std::int64_t last_relative = 0;
      std::int64_t published = 0;

      const auto result = decode_video_preview_once(
          path, extension, stop, [&](PreviewDecodedFrame frame) {
            if (stop.stop_requested())
              return false;
            const std::int64_t timestamp =
                std::max<std::int64_t>(0, frame.timestamp_ns);
            if (first_timestamp < 0)
              first_timestamp = timestamp;
            const std::int64_t relative = std::max(
                last_relative,
                std::max<std::int64_t>(0, timestamp - first_timestamp));
            if (!wait_until(stop,
                            loop_started + std::chrono::nanoseconds(relative)))
              return false;
            last_relative = relative;
            {
              std::lock_guard lock(mutex_);
              latest_ = std::move(frame);
            }
            ++published;
            return true;
          });

      if (stop.stop_requested())
        return;
      if (published == 0 || result.decoded_frames == 0)
        throw std::runtime_error("Video preview produced no frames");

      const std::int64_t loop_duration = std::max<std::int64_t>(
          result.duration_ns, last_relative + 33'000'000);
      const auto loop_end =
          loop_started + std::chrono::nanoseconds(loop_duration);
      if (!wait_until(stop, loop_end))
        return;
      loop_started = loop_end;
    }
  } catch (const std::exception &problem) {
    if (stop.stop_requested())
      return;
    std::lock_guard lock(mutex_);
    latest_.reset();
    failure_ = problem.what();
  }
}

} // namespace gdupe
