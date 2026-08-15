#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gdupe {

enum class NvdecCodec {
  h264,
  hevc,
  vp8,
  vp9,
  av1,
};

enum class NvdecOutput {
  fingerprint,
  preview,
};

struct NvdecGrayFrame {
  int width{};
  int height{};
  std::int64_t timestamp{};
  std::vector<std::uint8_t> pixels;
};

struct NvdecBgraFrame {
  int width{};
  int height{};
  std::int64_t timestamp{};
  std::vector<std::uint8_t> pixels;
};

using NvdecFrameCallback = std::function<void(NvdecGrayFrame)>;
using NvdecBgraFrameCallback = std::function<void(NvdecBgraFrame)>;

class NvdecPacketDecoder {
public:
  explicit NvdecPacketDecoder(
      NvdecCodec codec, NvdecOutput output = NvdecOutput::fingerprint);
  ~NvdecPacketDecoder();

  NvdecPacketDecoder(const NvdecPacketDecoder &) = delete;
  NvdecPacketDecoder &operator=(const NvdecPacketDecoder &) = delete;
  NvdecPacketDecoder(NvdecPacketDecoder &&) noexcept;
  NvdecPacketDecoder &operator=(NvdecPacketDecoder &&) noexcept;

  // Feed codec configuration bytes before compressed pictures. H.264/HEVC
  // callers use Annex-B parameter sets; WebM codecs normally need no separate
  // header and can begin with decode()/decode_bgra().
  void feed_header(std::span<const std::uint8_t> bytes);

  // Fingerprint path. timestamp is an opaque signed 64-bit value carried
  // through NVDEC display reordering unchanged.
  void decode(std::span<const std::uint8_t> bytes, std::int64_t timestamp,
              const NvdecFrameCallback &callback);
  void flush(const NvdecFrameCallback &callback);

  // Preview path. The decoder must have been constructed with
  // NvdecOutput::preview. Output is opaque premultiplied BGRA8 suitable for
  // Direct2D. Common 4:2:0 NV12 and P016 surfaces are supported.
  void decode_bgra(std::span<const std::uint8_t> bytes,
                   std::int64_t timestamp,
                   const NvdecBgraFrameCallback &callback);
  void flush_bgra(const NvdecBgraFrameCallback &callback);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// True when the NVIDIA CUDA/NVDEC driver runtime can be loaded and at least
// one CUDA device is visible. This intentionally does not claim that a
// particular codec/profile is supported; each decoder validates that with
// cuvidGetDecoderCaps when its sequence header is parsed.
[[nodiscard]] bool nvdec_runtime_available() noexcept;

} // namespace gdupe
