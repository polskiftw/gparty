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

void require(bool condition, const std::string &message) {
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

std::filesystem::path find_fixture(std::string_view name) {
  const auto source_fixture = std::filesystem::path(__FILE__).parent_path() /
                              "fixtures" / std::string(name);
  if (std::filesystem::exists(source_fixture))
    return source_fixture;
  const auto bundled_fixture = std::filesystem::current_path() / "fixtures" /
                               std::string(name);
  if (std::filesystem::exists(bundled_fixture))
    return bundled_fixture;
  throw std::runtime_error("could not locate HEVC Main test fixture");
}

class TempFile {
public:
  TempFile() {
    path_ = std::filesystem::temp_directory_path() /
            ("gdupe-hevc-main-hvc1-" +
             std::to_string(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()) +
             ".mp4");
  }
  ~TempFile() { std::filesystem::remove(path_); }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void test_hevc_main_hvc1_mp4() {
  std::ifstream fixture(find_fixture("hevc-main-hvc1.mp4.b64"),
                        std::ios::binary);
  require(static_cast<bool>(fixture), "could not open HEVC Main fixture");
  const std::string encoded((std::istreambuf_iterator<char>(fixture)),
                            std::istreambuf_iterator<char>());
  const auto bytes = base64_decode(encoded);
  require(bytes.size() > 1000, "HEVC Main fixture did not decode from base64");

  TempFile file;
  {
    std::ofstream out(file.path(), std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not materialize HEVC Main fixture");
  }

  const auto decoded = gdupe::decode_mp4_static(
      file.path(), 2,
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  require(decoded.width == 192 && decoded.height == 192,
          "HEVC Main hvc1 decode dimensions are wrong");
  require(decoded.frame_count == 2, "HEVC Main hvc1 frame count is wrong");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          "HEVC Main hvc1 duration is wrong");
  require(!decoded.sampled_frames.empty(),
          "HEVC Main hvc1 produced no sampled grayscale frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 192 && frame.height == 192 &&
                frame.pixels.size() == 192U * 192U,
            "HEVC Main hvc1 grayscale output is malformed");
  }
}

} // namespace

int main() {
  try {
    if (!gdupe::nvdec_runtime_available()) {
      std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
      return 77;
    }
    test_hevc_main_hvc1_mp4();
    std::cout << "HEVC Main NVDEC test passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "HEVC Main NVDEC test failure: " << e.what() << '\n';
    return 1;
  }
}
