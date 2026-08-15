#include "media_decode.hpp"
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
  throw std::runtime_error("could not locate bundled NVDEC WebM fixture " +
                           std::string(name));
}

class TempFile {
public:
  explicit TempFile(std::string_view stem) {
    path_ = std::filesystem::temp_directory_path() /
            ("gdupe-" + std::string(stem) + "-" +
             std::to_string(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()) +
             ".webm");
  }
  ~TempFile() { std::filesystem::remove(path_); }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_fixture(std::string_view fixture_name, std::string_view codec_name) {
  std::ifstream fixture(find_fixture(fixture_name), std::ios::binary);
  require(static_cast<bool>(fixture),
          std::string("could not open ") + codec_name + " WebM fixture");
  const std::string encoded((std::istreambuf_iterator<char>(fixture)),
                            std::istreambuf_iterator<char>());
  const auto bytes = base64_decode(encoded);
  require(bytes.size() > 500,
          std::string(codec_name) + " WebM fixture did not decode from base64");

  TempFile media(codec_name);
  {
    std::ofstream output(media.path(), std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output),
            std::string("could not materialize ") + codec_name +
                " WebM fixture");
  }

  const auto decoded = gdupe::decode_moving_media_static(
      media.path(), "webm", 2,
      std::chrono::steady_clock::now() + std::chrono::seconds(30));
  require(decoded.width == 64 && decoded.height == 64,
          std::string(codec_name) + " WebM dimensions are wrong");
  require(decoded.frame_count == 2,
          std::string(codec_name) + " WebM frame count is wrong");
  require(decoded.duration_ms >= 900 && decoded.duration_ms <= 1100,
          std::string(codec_name) + " WebM duration is wrong");
  require(!decoded.sampled_frames.empty(),
          std::string(codec_name) + " WebM produced no sampled frames");
  for (const auto &frame : decoded.sampled_frames) {
    require(frame.width == 64 && frame.height == 64 &&
                frame.pixels.size() == 64U * 64U,
            std::string(codec_name) + " WebM grayscale output is malformed");
  }
}

} // namespace

int main() {
  if (!gdupe::nvdec_runtime_available()) {
    std::cout << "SKIP: NVIDIA NVDEC runtime/device not available\n";
    return 77;
  }

  try {
    test_fixture("vp8.webm.b64", "VP8");
    test_fixture("vp9.webm.b64", "VP9");
    test_fixture("av1.webm.b64", "AV1");
    std::cout << "VP8/VP9/AV1 NVDEC WebM tests passed\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "::error title=NVDEC WebM regression failed::" << problem.what()
              << '\n';
    std::cerr << "NVDEC WebM regression failed: " << problem.what() << '\n';
    return 1;
  }
}
