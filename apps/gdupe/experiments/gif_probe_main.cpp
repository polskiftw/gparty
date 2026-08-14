#include "gif_decode.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  constexpr std::array<unsigned char, 34> gif{
      0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,
      0x00,0x00,0x00,0x00,0xff,0xff,0xff,0x2c,0x00,0x00,0x00,0x00,
      0x01,0x00,0x01,0x00,0x00,0x02,0x01,0x4c,0x00,0x3b};

  const auto path = std::filesystem::current_path() / "gif-probe.gif";
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(gif.data()), gif.size());
  }

  const auto decoded = gdupe::decode_gif_static(
      path, 4, std::chrono::steady_clock::now() + std::chrono::seconds(10));
  std::filesystem::remove(path);

  if (decoded.width != 1 || decoded.height != 1 || decoded.frame_count != 1 ||
      decoded.sampled_frames.size() != 1 ||
      decoded.sampled_frames.front().pixels.size() != 1) {
    std::cerr << "unexpected decoded GIF shape\n";
    return 1;
  }
  std::cout << "static FLTK GIF decode passed\n";
  return 0;
}
