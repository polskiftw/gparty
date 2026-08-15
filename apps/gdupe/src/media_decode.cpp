#include "media_decode.hpp"
#include "nvdec_decode.hpp"
#include "video_demux.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gdupe {
namespace {

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("Moving-media decoding exceeded its deadline");
}

class FrameSampler {
public:
  FrameSampler(std::size_t maximum, std::int64_t duration_ns)
      : maximum_(maximum), duration_ns_(std::max<std::int64_t>(0, duration_ns)) {
    if (maximum_ == 0)
      throw std::runtime_error("Moving-media sample count must be positive");
    frames_.reserve(maximum_);
  }

  void submit(NvdecGrayFrame frame) {
    ++decoded_count_;
    width_ = std::max(width_, frame.width);
    height_ = std::max(height_, frame.height);
    if (frames_.size() >= maximum_)
      return;

    const std::int64_t timestamp_ns =
        std::max<std::int64_t>(0, frame.timestamp);
    if (duration_ns_ > 0) {
      const long double fraction = static_cast<long double>(frames_.size()) /
                                   static_cast<long double>(maximum_);
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns_) * fraction);
      if (timestamp_ns < target)
        return;
    }

    DecodedGrayFrame converted;
    converted.width = frame.width;
    converted.height = frame.height;
    converted.timestamp_ns = timestamp_ns;
    converted.pixels = std::move(frame.pixels);
    frames_.push_back(std::move(converted));
  }

  [[nodiscard]] bool full() const noexcept {
    return frames_.size() >= maximum_;
  }
  [[nodiscard]] std::int64_t decoded_count() const noexcept {
    return decoded_count_;
  }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  std::vector<DecodedGrayFrame> take_frames() { return std::move(frames_); }

private:
  std::size_t maximum_{};
  std::int64_t duration_ns_{};
  std::int64_t decoded_count_{};
  int width_{};
  int height_{};
  std::vector<DecodedGrayFrame> frames_;
};

} // namespace

DecodedMovingMedia decode_moving_media_static(
    const std::filesystem::path &path, const std::string &extension,
    std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  check_deadline(deadline);
  auto demux = open_video_demux(path, extension);
  const DemuxedVideoInfo &info = demux->info();

  FrameSampler sampler(sample_count, info.duration_ns);
  NvdecPacketDecoder decoder(info.codec);
  if (!info.codec_header.empty())
    decoder.feed_header(info.codec_header);

  const NvdecFrameCallback on_frame = [&](NvdecGrayFrame frame) {
    check_deadline(deadline);
    sampler.submit(std::move(frame));
  };

  demux->visit_packets([&](DemuxedVideoPacket packet) {
    check_deadline(deadline);
    decoder.decode(packet.bytes, packet.timestamp_ns, on_frame);
    // MP4 exposes its total sample count up front, so once every requested
    // fingerprint sample is retained there is no value in walking the rest of
    // the file. WebM keeps walking to preserve its decoded-frame count.
    return info.frame_count == 0 || !sampler.full();
  });
  decoder.flush(on_frame);

  DecodedMovingMedia result;
  result.width = std::max(info.width, std::max(decoder.width(), sampler.width()));
  result.height =
      std::max(info.height, std::max(decoder.height(), sampler.height()));
  result.duration_ms = info.duration_ns / 1'000'000;
  result.frame_count =
      info.frame_count > 0 ? info.frame_count : sampler.decoded_count();
  result.sampled_frames = sampler.take_frames();
  if (result.sampled_frames.empty())
    throw std::runtime_error(
        "Moving-media file contained no NVDEC-decodable video frames");
  return result;
}

} // namespace gdupe
