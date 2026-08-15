#pragma once

#include "nvdec_decode.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace gdupe {

struct DemuxedVideoInfo {
  NvdecCodec codec{NvdecCodec::h264};
  int width{};
  int height{};
  std::int64_t duration_ns{};
  // Zero means the container path does not cheaply expose a trustworthy total.
  std::int64_t frame_count{};
  // H.264/HEVC use Annex-B parameter sets. AV1/WebM uses configOBUs from
  // CodecPrivate. VP8/VP9 normally leave this empty.
  std::vector<std::uint8_t> codec_header;
};

struct DemuxedVideoPacket {
  std::vector<std::uint8_t> bytes;
  std::int64_t timestamp_ns{};
};

// Return false to stop walking compressed packets early.
using DemuxedVideoPacketCallback =
    std::function<bool(DemuxedVideoPacket packet)>;

class VideoDemux {
public:
  virtual ~VideoDemux() = default;

  [[nodiscard]] virtual const DemuxedVideoInfo &info() const noexcept = 0;

  // Walk packets in decode order. Returns true at natural end-of-stream and
  // false when the callback requests an early stop.
  virtual bool visit_packets(const DemuxedVideoPacketCallback &callback) = 0;
};

std::unique_ptr<VideoDemux>
open_video_demux(const std::filesystem::path &path, std::string_view extension);

} // namespace gdupe
