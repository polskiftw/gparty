#include "gif_decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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
    if (SUCCEEDED(result)) {
      owns_initialization_ = true;
    } else if (result != RPC_E_CHANGED_MODE) {
      throw std::runtime_error("Windows COM initialization failed for GIF decoding");
    }
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
  const auto index = metadata_uint(decoder_metadata,
                                   L"/logscrdesc/BackgroundColorIndex");
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
  for (std::size_t offset = 0; offset < canvas.size(); offset += 4) {
    canvas[offset + 0] = color[0];
    canvas[offset + 1] = color[1];
    canvas[offset + 2] = color[2];
    canvas[offset + 3] = color[3];
  }
}

void fill_rect(std::vector<std::uint8_t> &canvas, int canvas_width,
               const FrameInfo &frame,
               const std::array<std::uint8_t, 4> &color) {
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    auto *row = canvas.data() +
                (static_cast<std::size_t>(frame.top + y) * canvas_width +
                 frame.left) *
                    4;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      row[x * 4 + 0] = color[0];
      row[x * 4 + 1] = color[1];
      row[x * 4 + 2] = color[2];
      row[x * 4 + 3] = color[3];
    }
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
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        continue;
      }
      const unsigned inverse = 255U - alpha;
      dst[0] = static_cast<std::uint8_t>(
          std::min(255U, static_cast<unsigned>(src[0]) +
                             (static_cast<unsigned>(dst[0]) * inverse + 127U) /
                                 255U));
      dst[1] = static_cast<std::uint8_t>(
          std::min(255U, static_cast<unsigned>(src[1]) +
                             (static_cast<unsigned>(dst[1]) * inverse + 127U) /
                                 255U));
      dst[2] = static_cast<std::uint8_t>(
          std::min(255U, static_cast<unsigned>(src[2]) +
                             (static_cast<unsigned>(dst[2]) * inverse + 127U) /
                                 255U));
      dst[3] = static_cast<std::uint8_t>(
          std::min(255U, alpha +
                             (static_cast<unsigned>(dst[3]) * inverse + 127U) /
                                 255U));
    }
  }
}

DecodedGrayFrame gray_frame(const std::vector<std::uint8_t> &canvas, int width,
                            int height, std::int64_t timestamp_ns) {
  DecodedGrayFrame result;
  result.width = width;
  result.height = height;
  result.timestamp_ns = timestamp_ns;
  result.pixels.resize(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    const auto *source = canvas.data() +
                         static_cast<std::size_t>(y) * width * 4;
    auto *destination = result.pixels.data() +
                        static_cast<std::size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      const auto *pixel = source + static_cast<std::size_t>(x) * 4;
      destination[x] = static_cast<std::uint8_t>(
          (29U * pixel[0] + 150U * pixel[1] + 77U * pixel[2] + 128U) >> 8U);
    }
  }
  return result;
}

std::vector<std::size_t>
choose_samples(const std::vector<std::int64_t> &starts,
               std::int64_t duration_ns, std::size_t sample_count) {
  const std::size_t frame_count = starts.size();
  const std::size_t wanted = std::min(sample_count, frame_count);
  std::vector<std::size_t> indices;
  indices.reserve(wanted);
  std::size_t previous = std::numeric_limits<std::size_t>::max();

  for (std::size_t sample = 0; sample < wanted; ++sample) {
    std::size_t index = 0;
    if (duration_ns > 0) {
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns) * sample / wanted);
      const auto upper = std::upper_bound(starts.begin(), starts.end(), target);
      index = upper == starts.begin()
                  ? 0
                  : static_cast<std::size_t>((upper - starts.begin()) - 1);
    } else {
      index = (sample * frame_count) / wanted;
    }
    index = std::min(index, frame_count - 1);
    if (index == previous && index + 1 < frame_count)
      ++index;
    indices.push_back(index);
    previous = index;
  }
  return indices;
}

#endif

} // namespace

DecodedMovingMedia decode_gif_static(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  if (sample_count == 0)
    throw std::runtime_error("GIF sample count must be positive");
  check_deadline(deadline);

#ifndef _WIN32
  (void)path;
  throw std::runtime_error("GIF decoding requires Windows Imaging Component");
#else
  ComApartment apartment;

  ComPtr<IWICImagingFactory> factory;
  require_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)),
                  "Windows Imaging Component factory creation failed");

  ComPtr<IWICBitmapDecoder> decoder;
  require_hresult(factory->CreateDecoderFromFilename(
                      path.c_str(), nullptr, GENERIC_READ,
                      WICDecodeMetadataCacheOnLoad, &decoder),
                  "Windows Imaging Component could not open GIF");

  GUID container{};
  require_hresult(decoder->GetContainerFormat(&container),
                  "Windows Imaging Component could not identify GIF");
  if (container != GUID_ContainerFormatGif)
    throw std::runtime_error("Windows Imaging Component did not identify a GIF");

  UINT frame_count = 0;
  require_hresult(decoder->GetFrameCount(&frame_count),
                  "Windows Imaging Component could not enumerate GIF frames");
  if (frame_count == 0)
    throw std::runtime_error("GIF contains no decodable frames");

  ComPtr<IWICMetadataQueryReader> decoder_metadata;
  require_hresult(decoder->GetMetadataQueryReader(&decoder_metadata),
                  "Windows Imaging Component could not read GIF metadata");
  const std::uint32_t canvas_width = required_metadata_uint(
      decoder_metadata.Get(), L"/logscrdesc/Width",
      "GIF logical width metadata is missing");
  const std::uint32_t canvas_height = required_metadata_uint(
      decoder_metadata.Get(), L"/logscrdesc/Height",
      "GIF logical height metadata is missing");
  if (canvas_width == 0 || canvas_height == 0 ||
      static_cast<std::uint64_t>(canvas_width) * canvas_height >
          kMaxFramePixels ||
      canvas_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      canvas_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    throw std::runtime_error("GIF canvas exceeds gdupe's safety limit");

  std::vector<FrameInfo> frames(frame_count);
  std::vector<std::int64_t> starts(frame_count);
  std::int64_t duration_ns = 0;
  for (UINT index = 0; index < frame_count; ++index) {
    check_deadline(deadline);
    ComPtr<IWICBitmapFrameDecode> frame;
    require_hresult(decoder->GetFrame(index, &frame),
                    "Windows Imaging Component could not read GIF frame");
    UINT decoded_width = 0;
    UINT decoded_height = 0;
    require_hresult(frame->GetSize(&decoded_width, &decoded_height),
                    "Windows Imaging Component returned an invalid GIF frame");

    ComPtr<IWICMetadataQueryReader> metadata;
    require_hresult(frame->GetMetadataQueryReader(&metadata),
                    "Windows Imaging Component could not read GIF frame metadata");
    auto &info = frames[index];
    info.left = metadata_uint(metadata.Get(), L"/imgdesc/Left").value_or(0);
    info.top = metadata_uint(metadata.Get(), L"/imgdesc/Top").value_or(0);
    info.width = metadata_uint(metadata.Get(), L"/imgdesc/Width")
                     .value_or(decoded_width);
    info.height = metadata_uint(metadata.Get(), L"/imgdesc/Height")
                      .value_or(decoded_height);
    info.disposal = metadata_uint(metadata.Get(), L"/grctlext/Disposal")
                        .value_or(0);
    info.delay_centiseconds = metadata_uint(metadata.Get(), L"/grctlext/Delay")
                                  .value_or(0);

    if (info.width == 0 || info.height == 0 || info.width != decoded_width ||
        info.height != decoded_height || info.left > canvas_width ||
        info.top > canvas_height || info.width > canvas_width - info.left ||
        info.height > canvas_height - info.top)
      throw std::runtime_error("GIF frame rectangle is outside its logical canvas");

    starts[index] = duration_ns;
    if (info.delay_centiseconds > 0) {
      constexpr std::int64_t kCentisecondNs = 10'000'000;
      const auto remaining = std::numeric_limits<std::int64_t>::max() -
                             duration_ns;
      const auto increment =
          info.delay_centiseconds >
                  static_cast<std::uint64_t>(remaining / kCentisecondNs)
              ? remaining
              : static_cast<std::int64_t>(info.delay_centiseconds) *
                    kCentisecondNs;
      duration_ns += increment;
    }
  }

  const auto sample_indices = choose_samples(starts, duration_ns, sample_count);
  const auto background =
      logical_background(factory.Get(), decoder.Get(), decoder_metadata.Get());
  std::vector<std::uint8_t> canvas(
      static_cast<std::size_t>(canvas_width) * canvas_height * 4);
  fill_canvas(canvas, background);
  std::vector<std::uint8_t> restore_snapshot;

  DecodedMovingMedia result;
  result.width = static_cast<int>(canvas_width);
  result.height = static_cast<int>(canvas_height);
  result.duration_ms = duration_ns / 1'000'000;
  result.frame_count = frame_count;
  result.sampled_frames.reserve(sample_indices.size());

  std::size_t sample_cursor = 0;
  const std::size_t last_needed = sample_indices.empty() ? 0 : sample_indices.back();
  for (UINT index = 0; index < frame_count && index <= last_needed; ++index) {
    check_deadline(deadline);

    if (index > 0) {
      const auto &previous = frames[index - 1];
      if (previous.disposal == 2) {
        fill_rect(canvas, static_cast<int>(canvas_width), previous, background);
      } else if (previous.disposal == 3 && !restore_snapshot.empty()) {
        canvas = restore_snapshot;
      }
    }

    const auto &info = frames[index];
    if (info.disposal == 3)
      restore_snapshot = canvas;
    else
      restore_snapshot.clear();

    ComPtr<IWICBitmapFrameDecode> frame;
    require_hresult(decoder->GetFrame(index, &frame),
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
        static_cast<std::uint64_t>(info.width) * info.height;
    if (frame_pixels > kMaxFramePixels)
      throw std::runtime_error("GIF frame exceeds gdupe's safety limit");
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(frame_pixels) * 4);
    const UINT stride = info.width * 4;
    require_hresult(converter->CopyPixels(nullptr, stride,
                                          static_cast<UINT>(pixels.size()),
                                          pixels.data()),
                    "Windows Imaging Component could not copy GIF pixels");
    composite_frame(canvas, static_cast<int>(canvas_width), info, pixels);

    while (sample_cursor < sample_indices.size() &&
           sample_indices[sample_cursor] == index) {
      result.sampled_frames.push_back(gray_frame(
          canvas, static_cast<int>(canvas_width), static_cast<int>(canvas_height),
          starts[index]));
      ++sample_cursor;
    }
  }

  if (result.sampled_frames.empty())
    throw std::runtime_error("GIF contained no sampled frames");
  return result;
#endif
}

} // namespace gdupe
