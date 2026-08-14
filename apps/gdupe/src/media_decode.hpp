#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gdupe {

struct DecodedGrayFrame {
  int width{};
  int height{};
  std::int64_t timestamp_ns{};
  std::vector<std::uint8_t> pixels;
};

struct DecodedMovingMedia {
  int width{};
  int height{};
  std::int64_t duration_ms{};
  std::int64_t frame_count{};
  std::vector<DecodedGrayFrame> sampled_frames;
};

// Decode a moving-media file using only statically linked codec/container
// libraries. |sample_count| is the maximum number of representative frames
// retained for fingerprinting; decoders may process additional frames to
// preserve inter-frame codec state. The fingerprint database is defined by
// this decoder path and does not attempt FFmpeg hash compatibility.
DecodedMovingMedia decode_moving_media_static(
    const std::filesystem::path &path, const std::string &extension,
    std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline);

} // namespace gdupe
