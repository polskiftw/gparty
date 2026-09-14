#include "gif_decode.hpp"
#include "wic_gif.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kAnimatedFixture =
    "R0lGODlhAgABAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQECgAA"
    "ACwAAAAAAgABAAAIBQABAAgIACH5BAUUAAEALAAAAAACAAEAgQD/AAAAAAAAAAAAAAgF"
    "AAEACAgAOw==";

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> result;
  unsigned accumulator = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=')
      break;
    const auto position = alphabet.find(c);
    if (position == std::string_view::npos)
      continue;
    accumulator = (accumulator << 6U) | static_cast<unsigned>(position);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return result;
}

class TempGif {
public:
  TempGif() {
    path_ = std::filesystem::temp_directory_path() /
            ("gdupe-wic-gif-" +
             std::to_string(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()) +
             ".gif");
  }
  ~TempGif() { std::filesystem::remove(path_); }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_gif(const std::filesystem::path &path,
               const std::vector<std::uint8_t> &bytes) {
  std::ofstream output(path, std::ios::binary);
  require(static_cast<bool>(output), "could not create GIF fixture");
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "could not write GIF fixture");
}

void test_wic_animated_gif() {
  // Two 2x1 frames: red for 100 ms, then green for 200 ms. The fixture is
  // embedded so the test requires no image utility or external decoder.
  TempGif file;
  write_gif(file.path(), base64_decode(kAnimatedFixture));

  const auto decoded = gdupe::decode_gif_static(
      file.path(), 2, std::chrono::steady_clock::now() + std::chrono::seconds(5));
  require(decoded.width == 2 && decoded.height == 1,
          "WIC GIF canvas dimensions changed");
  require(decoded.frame_count == 2, "WIC GIF frame count changed");
  require(decoded.duration_ms == 300, "WIC GIF timing changed");
  require(decoded.sampled_frames.size() == 2,
          "WIC GIF sampling did not retain both frames");
  require(decoded.sampled_frames[0].timestamp_ns == 0,
          "WIC GIF first timestamp changed");
  require(decoded.sampled_frames[1].timestamp_ns == 100'000'000,
          "WIC GIF second timestamp changed");
  require(decoded.sampled_frames[0].pixels.size() == 2 &&
              decoded.sampled_frames[1].pixels.size() == 2,
          "WIC GIF grayscale frame size changed");
  require(decoded.sampled_frames[0].pixels[0] == 77 &&
              decoded.sampled_frames[1].pixels[0] == 149,
          "WIC GIF composed frame colors changed");

  gdupe::WicGifDecoder preview_decoder(file.path());
  require(preview_decoder.info().logical_width == 2 &&
              preview_decoder.info().logical_height == 1,
          "WIC GIF logical canvas dimensions changed");
  require(preview_decoder.info().normalizations.empty(),
          "conforming GIF unexpectedly required geometry normalization");
  require(preview_decoder.info().frame_count == 2,
          "WIC GIF preview frame count changed");
  std::vector<std::array<std::uint8_t, 4>> preview_pixels;
  std::vector<std::uint64_t> preview_delays;
  preview_decoder.decode(
      std::chrono::steady_clock::now() + std::chrono::seconds(5),
      [&](const gdupe::WicGifFrameView &frame) {
        require(frame.premultiplied_bgra.size() == 8,
                "WIC GIF preview canvas size changed");
        preview_pixels.push_back({frame.premultiplied_bgra[0],
                                  frame.premultiplied_bgra[1],
                                  frame.premultiplied_bgra[2],
                                  frame.premultiplied_bgra[3]});
        preview_delays.push_back(frame.delay_ms);
        return true;
      });
  require(preview_pixels.size() == 2 && preview_delays.size() == 2,
          "WIC GIF preview did not retain both frames");
  require(preview_pixels[0] == std::array<std::uint8_t, 4>{0, 0, 255, 255} &&
              preview_pixels[1] ==
                  std::array<std::uint8_t, 4>{0, 255, 0, 255},
          "WIC GIF preview composed colors changed");
  require(preview_delays[0] == 100 && preview_delays[1] == 200,
          "WIC GIF preview timing changed");
}

void test_frame_extending_beyond_logical_canvas() {
  // Deliberately make the logical screen 1x1 while retaining the fixture's
  // decodable 2x1 image descriptors. Real-world GIFs with this malformed
  // geometry should preserve their WIC-decoded pixels by expanding the
  // effective compositing canvas rather than aborting the whole library scan.
  auto bytes = base64_decode(kAnimatedFixture);
  require(bytes.size() > 9, "GIF fixture is unexpectedly short");
  bytes[6] = 1;
  bytes[7] = 0;

  TempGif file;
  write_gif(file.path(), bytes);

  gdupe::WicGifDecoder decoder(file.path());
  const auto &info = decoder.info();
  require(info.logical_width == 1 && info.logical_height == 1,
          "malformed GIF logical dimensions were not preserved");
  require(info.width == 2 && info.height == 1,
          "malformed GIF effective canvas was not expanded");
  require(!info.normalizations.empty(),
          "malformed GIF normalization was not reported");
  require(info.normalizations.front().expanded_canvas,
          "malformed GIF canvas expansion was not diagnosed");
  require(info.normalizations.front().decoded_width == 2,
          "malformed GIF decoded frame width changed");

  std::size_t frames = 0;
  decoder.decode(std::chrono::steady_clock::now() + std::chrono::seconds(5),
                 [&](const gdupe::WicGifFrameView &frame) {
                   require(frame.width == 2 && frame.height == 1,
                           "normalized GIF preview canvas changed");
                   require(frame.premultiplied_bgra.size() == 8,
                           "normalized GIF preview pixel count changed");
                   ++frames;
                   return true;
                 });
  require(frames == 2, "normalized GIF did not decode both frames");

  const auto fingerprint_input = gdupe::decode_gif_static(
      file.path(), 2, std::chrono::steady_clock::now() + std::chrono::seconds(5));
  require(fingerprint_input.width == 2 && fingerprint_input.height == 1,
          "normalized GIF fingerprint canvas changed");
  require(fingerprint_input.sampled_frames.size() == 2,
          "normalized GIF fingerprint sampling failed");
}

} // namespace

int main() {
  try {
    test_wic_animated_gif();
    test_frame_extending_beyond_logical_canvas();
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << problem.what() << '\n';
    return 1;
  }
}
