#include "gif_decode.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

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

void test_wic_animated_gif() {
  // Two 2x1 frames: red for 100 ms, then green for 200 ms. The fixture is
  // embedded so the test requires no image utility or external decoder.
  constexpr std::string_view fixture =
      "R0lGODlhAgABAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQECgAA"
      "ACwAAAAAAgABAAAIBQABAAgIACH5BAUUAAEALAAAAAACAAEAgQD/AAAAAAAAAAAAAAgF"
      "AAEACAgAOw==";

  TempGif file;
  const auto bytes = base64_decode(fixture);
  {
    std::ofstream output(file.path(), std::ios::binary);
    require(static_cast<bool>(output), "could not create GIF fixture");
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "could not write GIF fixture");
  }

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
}

} // namespace

int main() {
  try {
    test_wic_animated_gif();
    return 0;
  } catch (...) {
    return 1;
  }
}
