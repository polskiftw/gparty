#include "mp4_decode.hpp"
#include "nvdec_decode.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> base64_decode(std::string_view text) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> result;
  unsigned accumulator = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=')
      break;
    const auto p = alphabet.find(c);
    if (p == std::string_view::npos)
      continue;
    accumulator = (accumulator << 6U) | static_cast<unsigned>(p);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
    }
  }
  return result;
}

} // namespace

int main() {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
    return 77;
  }

  try {
    const auto fixture_path = std::filesystem::path(__FILE__).parent_path() /
                              "fixtures/hevc-main10-hvc1.mp4.b64";
    std::ifstream fixture(fixture_path, std::ios::binary);
    if (!fixture)
      throw std::runtime_error("could not open Main 10 test fixture");
    const std::string encoded((std::istreambuf_iterator<char>(fixture)),
                              std::istreambuf_iterator<char>());
    const auto bytes = base64_decode(encoded);

    const auto media_path = std::filesystem::temp_directory_path() /
                            "gdupe-hevc-main10-hvc1.mp4";
    {
      std::ofstream out(media_path, std::ios::binary);
      out.write(reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      if (!out)
        throw std::runtime_error("could not materialize Main 10 test fixture");
    }

    try {
      const auto decoded = gdupe::decode_mp4_static(
          media_path, 2,
          std::chrono::steady_clock::now() + std::chrono::seconds(30));
      std::filesystem::remove(media_path);
      if (decoded.width != 128 || decoded.height != 128)
        throw std::runtime_error("Main 10 dimensions were decoded incorrectly");
      if (decoded.sampled_frames.empty())
        throw std::runtime_error("Main 10 produced no decoded frames");
      for (const auto &frame : decoded.sampled_frames) {
        if (frame.width != 128 || frame.height != 128 ||
            frame.pixels.size() != 128U * 128U)
          throw std::runtime_error("Main 10 grayscale output is malformed");
      }
    } catch (...) {
      std::filesystem::remove(media_path);
      throw;
    }

    std::cout << "HEVC Main 10 NVDEC decode passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "::error title=HEVC Main 10 NVDEC decode failed::" << e.what()
              << '\n';
    std::cerr << "HEVC Main 10 NVDEC decode failed: " << e.what() << '\n';
    return 1;
  }
}
