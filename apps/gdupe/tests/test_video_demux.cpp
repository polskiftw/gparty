#include "media_test_util.hpp"
#include "video_demux.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

template <typename Action>
void run_case(std::string_view name, Action &&action) {
  try {
    std::forward<Action>(action)();
  } catch (const std::exception &problem) {
    throw std::runtime_error(std::string(name) + ": " + problem.what());
  }
}

void test_fixture(std::string_view fixture, std::string_view extension,
                  gdupe::NvdecCodec expected_codec,
                  std::int64_t expected_frame_count, int expected_width,
                  int expected_height) {
  TempMedia media(fixture, extension);
  auto demux = gdupe::open_video_demux(media.path(), extension);
  const auto &info = demux->info();

  require(info.codec == expected_codec,
          std::string("wrong codec for ") + codec_name(expected_codec));
  require(info.width == expected_width && info.height == expected_height,
          std::string("wrong dimensions for ") + codec_name(expected_codec));
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

void test_early_stop(std::string_view fixture, std::string_view extension) {
  TempMedia media(fixture, extension);
  auto demux = gdupe::open_video_demux(media.path(), extension);
  std::size_t packets = 0;
  const bool reached_end = demux->visit_packets([&](gdupe::DemuxedVideoPacket) {
    ++packets;
    return false;
  });
  require(!reached_end && packets == 1,
          std::string(extension) +
              " demux callback did not stop packet walking immediately");
}

} // namespace

int main() {
  try {
    run_case("h264-avc1.mp4.b64", [] {
      test_fixture("h264-avc1.mp4.b64", "mp4", gdupe::NvdecCodec::h264, 2,
                   64, 64);
    });
    run_case("hevc-main-hvc1.mp4.b64", [] {
      test_fixture("hevc-main-hvc1.mp4.b64", "mp4", gdupe::NvdecCodec::hevc,
                   2, 192, 192);
    });
    run_case("hevc-main10-hvc1.mp4.b64", [] {
      test_fixture("hevc-main10-hvc1.mp4.b64", "mp4",
                   gdupe::NvdecCodec::hevc, 2, 192, 192);
    });
    run_case("vp8.webm.b64", [] {
      test_fixture("vp8.webm.b64", "webm", gdupe::NvdecCodec::vp8, 0, 128,
                   128);
    });
    run_case("vp9.webm.b64", [] {
      test_fixture("vp9.webm.b64", "webm", gdupe::NvdecCodec::vp9, 0, 128,
                   128);
    });
    run_case("av1.webm.b64", [] {
      test_fixture("av1.webm.b64", "webm", gdupe::NvdecCodec::av1, 0, 128,
                   128);
    });
    run_case("h264 early-stop", [] {
      test_early_stop("h264-avc1.mp4.b64", "mp4");
    });
    run_case("vp9 early-stop", [] {
      test_early_stop("vp9.webm.b64", "webm");
    });
    std::cout << "shared video demux tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "shared video demux test failure: " << problem.what() << '\n';
    return 1;
  }
}
