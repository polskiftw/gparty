#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace gdupe {

struct PreviewDecodedFrame {
  int width{};
  int height{};
  std::int64_t timestamp_ns{};
  std::vector<std::uint8_t> premultiplied_bgra;
};

struct PreviewDecodeResult {
  std::int64_t duration_ns{};
  std::int64_t decoded_frames{};
};

using PreviewFrameCallback = std::function<bool(PreviewDecodedFrame)>;

// Decode one complete pass of a video with the same container and NVDEC
// boundary used by fingerprinting. Returning false from callback or requesting
// the stop token ends the pass without flushing additional display frames.
PreviewDecodeResult decode_video_preview_once(
    const std::filesystem::path &path, const std::string &extension,
    std::stop_token stop, const PreviewFrameCallback &callback);

} // namespace gdupe
