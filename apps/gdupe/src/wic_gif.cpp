#include "wic_gif.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxFramePixels = 100'000'000ULL;

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("GIF decoding exceeded its deadline");
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

class ComApartment {
public:
  ComApartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(result))
      owns_initialization_ = true;
    else if (result != RPC_E_CHANGED_MODE)
      throw std::runtime_error("Windows COM initialization failed for GIF decoding");
  }

  ~ComApartment() {
    if (owns_initialization_)
      CoUninitialize();
  }

private:
  bool owns_initialization_{};
};

void require_hresult(HRESULT result, const char *message) {
  if (FAILED(result))
    throw std::runtime_error(message);
}

std::optional<std::uint32_t> metadata_uint(IWICMetadataQueryReader *reader,
                                           const wchar_t *query) {
  if (!reader)
    return std::nullopt;
  PROPVARIANT value;
  PropVariantInit(&value);
  const HRESULT result = reader->GetMetadataByName(query, &value);
  if (FAILED(result)) {
    PropVariantClear(&value);
    return std::nullopt;
  }
  std::optional<std::uint32_t> converted;
  switch (value.vt) {
  case VT_UI1:
    converted = value.bVal;
    break;
  case VT_UI2:
    converted = value.uiVal;
    break;
  case VT_UI4:
    converted = value.ulVal;
    break;
  case VT_UINT:
    converted = value.uintVal;
    break;
  default:
    break;
  }
  PropVariantClear(&value);
  return converted;
}

std::uint32_t required_metadata_uint(IWICMetadataQueryReader *reader,
                                     const wchar_t *query,
                                     const char *message) {
  const auto value = metadata_uint(reader, query);
  if (!value)
    throw std::runtime_error(message);
  return *value;
}

struct FrameInfo {
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t disposal{};
  std::uint32_t delay_centiseconds{};
};

std::array<std::uint8_t, 4>
logical_background(IWICImagingFactory *factory, IWICBitmapDecoder *decoder,
                   IWICMetadataQueryReader *decoder_metadata) {
  std::array<std::uint8_t, 4> background{0, 0, 0, 0};
  const auto index =
      metadata_uint(decoder_metadata, L"/logscrdesc/BackgroundColorIndex");
  if (!index || *index >= 256)
    return background;
  ComPtr<IWICPalette> palette;
  if (FAILED(factory->CreatePalette(&palette)) ||
      FAILED(decoder->CopyPalette(palette.Get())))
    return background;
  std::array<WICColor, 256> colors{};
  UINT actual = 0;
  if (FAILED(palette->GetColors(static_cast<UINT>(colors.size()), colors.data(),
                                &actual)) ||
      *index >= actual)
    return background;
  const WICColor color = colors[*index];
  background[0] = static_cast<std::uint8_t>(color & 0xffU);
  background[1] = static_cast<std::uint8_t>((color >> 8U) & 0xffU);
  background[2] = static_cast<std::uint8_t>((color >> 16U) & 0xffU);
  background[3] = static_cast<std::uint8_t>((color >> 24U) & 0xffU);
  return background;
}

void fill_canvas(std::vector<std::uint8_t> &canvas,
                 const std::array<std::uint8_t, 4> &color) {
  for (std::size_t offset = 0; offset < canvas.size(); offset += 4)
    std::copy(color.begin(), color.end(), canvas.begin() + offset);
}

void fill_rect(std::vector<std::uint8_t> &canvas, int canvas_width,
               const FrameInfo &frame,
               const std::array<std::uint8_t, 4> &color) {
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    auto *row = canvas.data() +
                (static_cast<std::size_t>(frame.top + y) * canvas_width +
                 frame.left) *
                    4;
    for (std::uint32_t x = 0; x < frame.width; ++x)
      std::copy(color.begin(), color.end(), row + x * 4);
  }
}

void composite_frame(std::vector<std::uint8_t> &canvas, int canvas_width,
                     const FrameInfo &info,
                     const std::vector<std::uint8_t> &source) {
  for (std::uint32_t y = 0; y < info.height; ++y) {
    auto *destination = canvas.data() +
                        (static_cast<std::size_t>(info.top + y) * canvas_width +
                         info.left) *
                            4;
    const auto *input = source.data() +
                        static_cast<std::size_t>(y) * info.width * 4;
    for (std::uint32_t x = 0; x < info.width; ++x) {
      const auto *src = input + static_cast<std::size_t>(x) * 4;
      auto *dst = destination + static_cast<std::size_t>(x) * 4;
      const unsigned alpha = src[3];
      if (alpha == 0)
        continue;
      if (alpha == 255) {
        std::copy_n(src, 4, dst);
        continue;
      }
      const unsigned inverse = 255U - alpha;
      for (int channel = 0; channel < 3; ++channel)
        dst[channel] = static_cast<std::uint8_t>(
            std::min(255U, static_cast<unsigned>(src[channel]) +
                               (static_cast<unsigned>(dst[channel]) * inverse +
                                127U) /
                                   255U));
      dst[3] = static_cast<std::uint8_t>(
          std::min(255U, alpha +
                             (static_cast<unsigned>(dst[3]) * inverse + 127U) /
                                 255U));
    }
  }
}

#endif

} // namespace

struct WicGifDecoder::Impl {
#ifdef _WIN32
  ComApartment apartment;
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  Microsoft::WRL::ComPtr<IWICMetadataQueryReader> decoder_metadata;
  std::vector<FrameInfo> frames;
  std::array<std::uint8_t, 4> background{};
#endif
  WicGifInfo info;

  explicit Impl(const std::filesystem::path &path) {
#ifndef _WIN32
    (void)path;
    throw std::runtime_error("GIF decoding requires Windows Imaging Component");
#else
    require_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory)),
                    "Windows Imaging Component factory creation failed");
    require_hresult(factory->CreateDecoderFromFilename(
                        path.c_str(), nullptr, GENERIC_READ,
                        WICDecodeMetadataCacheOnLoad, &decoder),
                    "Windows Imaging Component could not open GIF");
    GUID container{};
    require_hresult(decoder->GetContainerFormat(&container),
                    "Windows Imaging Component could not identify GIF");
    if (container != GUID_ContainerFormatGif)
      throw std::runtime_error(
          "Windows Imaging Component did not identify a GIF");
    UINT frame_count = 0;
    require_hresult(decoder->GetFrameCount(&frame_count),
                    "Windows Imaging Component could not enumerate GIF frames");
    if (frame_count == 0)
      throw std::runtime_error("GIF contains no decodable frames");
    require_hresult(decoder->GetMetadataQueryReader(&decoder_metadata),
                    "Windows Imaging Component could not read GIF metadata");
    const std::uint32_t width = required_metadata_uint(
        decoder_metadata.Get(), L"/logscrdesc/Width",
        "GIF logical width metadata is missing");
    const std::uint32_t height = required_metadata_uint(
        decoder_metadata.Get(), L"/logscrdesc/Height",
        "GIF logical height metadata is missing");
    if (width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > kMaxFramePixels ||
        width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
      throw std::runtime_error("GIF canvas exceeds gdupe's safety limit");
    info.width = static_cast<int>(width);
    info.height = static_cast<int>(height);
    info.frame_count = frame_count;
    info.frame_starts_ns.resize(frame_count);
    frames.resize(frame_count);
    std::int64_t duration_ns = 0;
    for (UINT index = 0; index < frame_count; ++index) {
      ComPtr<IWICBitmapFrameDecode> frame;
      require_hresult(decoder->GetFrame(index, &frame),
                      "Windows Imaging Component could not read GIF frame");
      UINT decoded_width = 0;
      UINT decoded_height = 0;
      require_hresult(frame->GetSize(&decoded_width, &decoded_height),
                      "Windows Imaging Component returned an invalid GIF frame");
      ComPtr<IWICMetadataQueryReader> metadata;
      require_hresult(
          frame->GetMetadataQueryReader(&metadata),
          "Windows Imaging Component could not read GIF frame metadata");
      auto &frame_info = frames[index];
      frame_info.left = metadata_uint(metadata.Get(), L"/imgdesc/Left").value_or(0);
      frame_info.top = metadata_uint(metadata.Get(), L"/imgdesc/Top").value_or(0);
      frame_info.width = metadata_uint(metadata.Get(), L"/imgdesc/Width")
                             .value_or(decoded_width);
      frame_info.height = metadata_uint(metadata.Get(), L"/imgdesc/Height")
                              .value_or(decoded_height);
      frame_info.disposal =
          metadata_uint(metadata.Get(), L"/grctlext/Disposal").value_or(0);
      frame_info.delay_centiseconds =
          metadata_uint(metadata.Get(), L"/grctlext/Delay").value_or(0);
      if (frame_info.width == 0 || frame_info.height == 0 ||
          frame_info.width != decoded_width ||
          frame_info.height != decoded_height || frame_info.left > width ||
          frame_info.top > height || frame_info.width > width - frame_info.left ||
          frame_info.height > height - frame_info.top)
        throw std::runtime_error(
            "GIF frame rectangle is outside its logical canvas");
      info.frame_starts_ns[index] = duration_ns;
      constexpr std::int64_t kCentisecondNs = 10'000'000;
      const auto remaining =
          std::numeric_limits<std::int64_t>::max() - duration_ns;
      const auto increment =
          frame_info.delay_centiseconds >
                  static_cast<std::uint64_t>(remaining / kCentisecondNs)
              ? remaining
              : static_cast<std::int64_t>(frame_info.delay_centiseconds) *
                    kCentisecondNs;
      duration_ns += increment;
    }
    info.duration_ms = duration_ns / 1'000'000;
    background = logical_background(factory.Get(), decoder.Get(),
                                    decoder_metadata.Get());
#endif
  }

  void decode(std::chrono::steady_clock::time_point deadline,
              const std::function<bool(const WicGifFrameView &)> &consumer) {
#ifndef _WIN32
    (void)deadline;
    (void)consumer;
#else
    check_deadline(deadline);
    std::vector<std::uint8_t> canvas(
        static_cast<std::size_t>(info.width) * info.height * 4);
    fill_canvas(canvas, background);
    std::vector<std::uint8_t> restore_snapshot;
    for (std::size_t index = 0; index < frames.size(); ++index) {
      check_deadline(deadline);
      if (index > 0) {
        const auto &previous = frames[index - 1];
        if (previous.disposal == 2)
          fill_rect(canvas, info.width, previous, background);
        else if (previous.disposal == 3 && !restore_snapshot.empty())
          canvas = restore_snapshot;
      }
      const auto &frame_info = frames[index];
      if (frame_info.disposal == 3)
        restore_snapshot = canvas;
      else
        restore_snapshot.clear();
      ComPtr<IWICBitmapFrameDecode> frame;
      require_hresult(decoder->GetFrame(static_cast<UINT>(index), &frame),
                      "Windows Imaging Component could not decode GIF frame");
      ComPtr<IWICFormatConverter> converter;
      require_hresult(factory->CreateFormatConverter(&converter),
                      "Windows Imaging Component format conversion failed");
      require_hresult(converter->Initialize(
                          frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeCustom),
                      "Windows Imaging Component could not convert GIF frame");
      const std::uint64_t frame_pixels =
          static_cast<std::uint64_t>(frame_info.width) * frame_info.height;
      if (frame_pixels > kMaxFramePixels ||
          frame_pixels * 4 > std::numeric_limits<UINT>::max())
        throw std::runtime_error("GIF frame exceeds gdupe's safety limit");
      std::vector<std::uint8_t> pixels(
          static_cast<std::size_t>(frame_pixels) * 4);
      const UINT stride = frame_info.width * 4;
      require_hresult(converter->CopyPixels(
                          nullptr, stride, static_cast<UINT>(pixels.size()),
                          pixels.data()),
                      "Windows Imaging Component could not copy GIF pixels");
      composite_frame(canvas, info.width, frame_info, pixels);
      const WicGifFrameView view{
          info.width,
          info.height,
          index,
          info.frame_starts_ns[index],
          static_cast<std::uint64_t>(frame_info.delay_centiseconds) * 10U,
          std::span<const std::uint8_t>(canvas),
      };
      if (!consumer(view))
        break;
    }
#endif
  }
};

WicGifDecoder::WicGifDecoder(const std::filesystem::path &path)
    : impl_(std::make_unique<Impl>(path)) {}
WicGifDecoder::~WicGifDecoder() = default;
WicGifDecoder::WicGifDecoder(WicGifDecoder &&) noexcept = default;
WicGifDecoder &WicGifDecoder::operator=(WicGifDecoder &&) noexcept = default;

const WicGifInfo &WicGifDecoder::info() const noexcept { return impl_->info; }

void WicGifDecoder::decode(
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool(const WicGifFrameView &)> &consumer) {
  impl_->decode(deadline, consumer);
}

} // namespace gdupe
