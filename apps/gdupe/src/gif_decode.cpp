#include "gif_decode.hpp"

#include "wic_gif.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gdupe {
namespace {

std::vector<std::size_t>
choose_samples(const std::vector<std::int64_t> &starts,
               std::int64_t duration_ns, std::size_t sample_count) {
  const std::size_t frame_count = starts.size();
  const std::size_t wanted = std::min(sample_count, frame_count);
  std::vector<std::size_t> indices;
  indices.reserve(wanted);
  std::size_t previous = std::numeric_limits<std::size_t>::max();
  for (std::size_t sample = 0; sample < wanted; ++sample) {
    std::size_t index = 0;
    if (duration_ns > 0) {
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns) * sample / wanted);
      const auto upper = std::upper_bound(starts.begin(), starts.end(), target);
      index = upper == starts.begin()
                  ? 0
                  : static_cast<std::size_t>((upper - starts.begin()) - 1);
    } else {
      index = (sample * frame_count) / wanted;
    }
    index = std::min(index, frame_count - 1);
    if (index == previous && index + 1 < frame_count)
      ++index;
    indices.push_back(index);
    previous = index;
  }
  return indices;
}

DecodedGrayFrame gray_frame(const WicGifFrameView &frame) {
  DecodedGrayFrame result;
  result.width = frame.width;
  result.height = frame.height;
  result.timestamp_ns = frame.timestamp_ns;
  result.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height);
  for (int y = 0; y < frame.height; ++y) {
    const auto *source = frame.premultiplied_bgra.data() +
                         static_cast<std::size_t>(y) * frame.width * 4;
    auto *destination = result.pixels.data() +
                        static_cast<std::size_t>(y) * frame.width;
    for (int x = 0; x < frame.width; ++x) {
      const auto *pixel = source + static_cast<std::size_t>(x) * 4;
      destination[x] = static_cast<std::uint8_t>(
          (29U * pixel[0] + 150U * pixel[1] + 77U * pixel[2] + 128U) >> 8U);
    }
  }
  return result;
}

} // namespace

DecodedMovingMedia decode_gif_static(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  if (sample_count == 0)
    throw std::runtime_error("GIF sample count must be positive");
  WicGifDecoder decoder(path);
  const auto &info = decoder.info();
  const auto sample_indices = choose_samples(
      info.frame_starts_ns, info.duration_ms * 1'000'000, sample_count);

  DecodedMovingMedia result;
  result.width = info.width;
  result.height = info.height;
  result.duration_ms = info.duration_ms;
  result.frame_count = info.frame_count;
  result.sampled_frames.reserve(sample_indices.size());
  std::size_t sample_cursor = 0;
  decoder.decode(deadline, [&](const WicGifFrameView &frame) {
    while (sample_cursor < sample_indices.size() &&
           sample_indices[sample_cursor] == frame.index) {
      result.sampled_frames.push_back(gray_frame(frame));
      ++sample_cursor;
    }
    return sample_cursor < sample_indices.size();
  });
  if (result.sampled_frames.empty())
    throw std::runtime_error("GIF contained no sampled frames");
  return result;
}

} // namespace gdupe
