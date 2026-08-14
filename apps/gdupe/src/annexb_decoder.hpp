#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gdupe {

struct AnnexBGrayFrame {
  int width{};
  int height{};
  std::uint32_t timestamp_token{};
  std::vector<std::uint8_t> pixels;
};

using AnnexBFrameCallback = std::function<void(AnnexBGrayFrame)>;

class AnnexBDecoder {
public:
  virtual ~AnnexBDecoder() = default;

  // Feed codec parameter sets in Annex-B form, discover the coded/display
  // dimensions, switch to frame output mode, and allocate planar output.
  virtual void initialize(std::span<const std::uint8_t> header) = 0;

  // Feed one complete MP4 access unit after conversion to Annex-B. Output may
  // be delayed/reordered; timestamp_token follows the frame through the codec.
  virtual void decode(std::span<const std::uint8_t> access_unit,
                      std::uint32_t timestamp_token,
                      const AnnexBFrameCallback &callback) = 0;

  // Drain delayed display frames after the final access unit.
  virtual void flush(const AnnexBFrameCallback &callback) = 0;

  [[nodiscard]] virtual int width() const noexcept = 0;
  [[nodiscard]] virtual int height() const noexcept = 0;
};

std::unique_ptr<AnnexBDecoder> make_h264_annexb_decoder();
std::unique_ptr<AnnexBDecoder> make_hevc_annexb_decoder();

} // namespace gdupe
