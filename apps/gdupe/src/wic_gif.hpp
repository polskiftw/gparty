#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gdupe {

struct WicGifNormalization {
  std::size_t frame_index{};
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t declared_width{};
  std::uint32_t declared_height{};
  std::uint32_t decoded_width{};
  std::uint32_t decoded_height{};
  bool expanded_canvas{};
  bool size_metadata_mismatch{};
};

struct WicGifInfo {
  int logical_width{};
  int logical_height{};
  int width{};
  int height{};
  std::int64_t duration_ms{};
  std::size_t frame_count{};
  std::vector<std::int64_t> frame_starts_ns;
  std::vector<WicGifNormalization> normalizations;
};

struct WicGifFrameView {
  int width{};
  int height{};
  std::size_t index{};
  std::int64_t timestamp_ns{};
  std::uint64_t delay_ms{};
  std::span<const std::uint8_t> premultiplied_bgra;
};

class WicGifDecoder final {
public:
  explicit WicGifDecoder(const std::filesystem::path &path);
  ~WicGifDecoder();

  WicGifDecoder(const WicGifDecoder &) = delete;
  WicGifDecoder &operator=(const WicGifDecoder &) = delete;
  WicGifDecoder(WicGifDecoder &&) noexcept;
  WicGifDecoder &operator=(WicGifDecoder &&) noexcept;

  [[nodiscard]] const WicGifInfo &info() const noexcept;
  void decode(std::chrono::steady_clock::time_point deadline,
              const std::function<bool(const WicGifFrameView &)> &consumer);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gdupe
