#pragma once

#include "preview_decode.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gdupe_test {

inline void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

inline std::vector<std::uint8_t> base64_decode(std::string_view text) {
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

inline std::filesystem::path find_fixture(std::string_view name) {
  const auto source = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / std::string(name);
  if (std::filesystem::exists(source))
    return source;
  const auto bundled = std::filesystem::current_path() / "fixtures" /
                       std::string(name);
  if (std::filesystem::exists(bundled))
    return bundled;
  throw std::runtime_error("could not locate preview fixture " +
                           std::string(name));
}

class TempMedia {
public:
  TempMedia(std::string_view fixture_name, std::string_view extension) {
    make_path(extension, "fixture");
    std::ifstream input(find_fixture(fixture_name), std::ios::binary);
    require(static_cast<bool>(input), "could not open preview fixture");
    const std::string encoded((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    const auto bytes = base64_decode(encoded);
    std::ofstream output(path_, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "could not materialize preview fixture");
  }

  explicit TempMedia(std::string_view extension) {
    make_path(extension, "invalid");
    std::ofstream output(path_, std::ios::binary);
    output << "not a media container";
  }

  ~TempMedia() { std::filesystem::remove(path_); }
  const std::filesystem::path &path() const noexcept { return path_; }

private:
  void make_path(std::string_view extension, std::string_view kind) {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    path_ = std::filesystem::temp_directory_path() /
            ("gdupe-preview-" + std::string(kind) + "-" +
             std::to_string(stamp) + "." + std::string(extension));
  }

  std::filesystem::path path_;
};

} // namespace gdupe_test
