#include "media_test_util.hpp"
#include "video_demux.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using gdupe_test::TempMedia;
using gdupe_test::require;

const char *codec_name(gdupe::NvdecCodec codec) {
  switch (codec) {
  case gdupe::NvdecCodec::h264:
    return "H.264";
  case gdupe::NvdecCodec::hevc:
    return "HEVC";
  case gdupe::NvdecCodec::vp8:
    return "VP8";
  case gdupe::NvdecCodec::vp9:
    return "VP9";
  case gdupe::NvdecCodec::av1:
    return "AV1";
  }
  return "unknown";
}

void test_fixture(std::string_view fixture, std::string_view extension,
                  gdupe::NvdecCodec expected_codec,
                  std::int64_t expected_frame_count) {
  TempMedia media(fixture, extension);
  auto demux = gdupe::open_video_demux(media.path(), extension);
  const auto &info = demux->info();

  require(info.codec == expected_codec,
          std::string("wrong codec for ") + codec_name(expected_codec));
  require(info.duration_ns >= 900'000'000 && info.duration_ns <= 1'100'000'000,
          std::string("wrong duration for ") + codec_name(expected_codec));
  require(info.frame_count == expected_frame_count,
          std::string("wrong container frame count for ") +
              codec_name(expected_codec));
  if (expected_codec == gdupe::NvdecCodec::h264 ||
      expected_codec == gdupe::NvdecCodec::hevc)
    require(!info.codec_header.empty(),
            std::string("missing codec header for ") + codec_name(expected_codec));

  std::size_t packets = 0;
  std::int64_t last_timestamp = 0;
  const bool reached_end = demux->visit_packets([&](gdupe::DemuxedVideoPacket packet) {
    require(!packet.bytes.empty(),
            std::string("empty packet for ") + codec_name(expected_codec));
    require(packet.timestamp_ns >= 0,
            std::string("negative timestamp for ") + codec_name(expected_codec));
    require(packet.timestamp_ns >= last_timestamp,
            std::string("non-monotonic fixture timestamp for ") +
                codec_name(expected_codec));
    last_timestamp = packet.timestamp_ns;
    ++packets;
    return true;
  });
  require(reached_end,
          std::string("demux stopped early for ") + codec_name(expected_codec));
  require(packets == 2,
          std::string("wrong packet count for ") + codec_name(expected_codec));
}

void test_early_stop() {
  TempMedia media("h264-avc1.mp4.b64", "mp4");
  auto demux = gdupe::open_video_demux(media.path(), "mp4");
  std::size_t packets = 0;
  const bool reached_end = demux->visit_packets([&](gdupe::DemuxedVideoPacket) {
    ++packets;
    return false;
  });
  require(!reached_end && packets == 1,
          "demux callback did not stop packet walking immediately");
}

} // namespace

int main() {
  try {
    test_fixture("h264-avc1.mp4.b64", "mp4", gdupe::NvdecCodec::h264, 2);
    test_fixture("hevc-main-hvc1.mp4.b64", "mp4", gdupe::NvdecCodec::hevc, 2);
    test_fixture("hevc-main10-hvc1.mp4.b64", "mp4", gdupe::NvdecCodec::hevc, 2);
    test_fixture("vp8.webm.b64", "webm", gdupe::NvdecCodec::vp8, 0);
    test_fixture("vp9.webm.b64", "webm", gdupe::NvdecCodec::vp9, 0);
    test_fixture("av1.webm.b64", "webm", gdupe::NvdecCodec::av1, 0);
    test_early_stop();
    std::cout << "shared video demux tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "shared video demux test failure: " << problem.what() << '\n';
    return 1;
  }
}
