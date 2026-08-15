#include "preview_decode.hpp"
#include "nvdec_decode.hpp"
#include "video_demux.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace gdupe {

PreviewDecodeResult decode_video_preview_once(
    const std::filesystem::path &path, const std::string &extension,
    std::stop_token stop, const PreviewFrameCallback &callback) {
  if (!callback)
    throw std::runtime_error("Video preview callback is empty");

  auto demux = open_video_demux(path, extension);
  const DemuxedVideoInfo &info = demux->info();

  PreviewDecodeResult result;
  result.duration_ns = info.duration_ns;
  NvdecPacketDecoder decoder(info.codec, NvdecOutput::preview);
  if (!info.codec_header.empty())
    decoder.feed_header(info.codec_header);

  bool keep_going = !stop.stop_requested();
  std::int64_t last_timestamp_ns = 0;
  const NvdecBgraFrameCallback on_frame = [&](NvdecBgraFrame frame) {
    if (!keep_going || stop.stop_requested()) {
      keep_going = false;
      return;
    }

    const std::int64_t timestamp_ns =
        std::max(last_timestamp_ns, std::max<std::int64_t>(0, frame.timestamp));
    last_timestamp_ns = timestamp_ns;

    PreviewDecodedFrame converted;
    converted.width = frame.width;
    converted.height = frame.height;
    converted.timestamp_ns = timestamp_ns;
    converted.premultiplied_bgra = std::move(frame.pixels);
    ++result.decoded_frames;
    keep_going = callback(std::move(converted));
  };

  const bool reached_end = demux->visit_packets([&](DemuxedVideoPacket packet) {
    if (!keep_going || stop.stop_requested()) {
      keep_going = false;
      return false;
    }
    decoder.decode_bgra(packet.bytes, packet.timestamp_ns, on_frame);
    return keep_going && !stop.stop_requested();
  });

  if (reached_end && keep_going && !stop.stop_requested())
    decoder.flush_bgra(on_frame);
  if (result.decoded_frames == 0 && !stop.stop_requested())
    throw std::runtime_error(
        "Moving-media file contained no NVDEC-decodable preview frames");
  return result;
}

} // namespace gdupe
