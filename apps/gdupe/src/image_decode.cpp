#include "image_decode.hpp"
#include "jpeg_decode.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

extern "C" {
#include <webp/decode.h>
}

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxImagePixels = 100'000'000ULL;

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::runtime_error("Cannot open static image");
  const auto end = stream.tellg();
  if (end <= 0)
    throw std::runtime_error("Static image is empty");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream)
    throw std::runtime_error("Cannot read static image");
  return bytes;
}

struct JpegImageGuard {
  GdupeJpegImage image{};
  ~JpegImageGuard() { gdupe_free_jpeg_image(&image); }
};

RgbImage decode_jpeg(const std::vector<std::uint8_t> &bytes) {
  JpegImageGuard decoded;
  if (!gdupe_decode_jpeg_rgb(bytes.data(), bytes.size(), kMaxImagePixels,
                             &decoded.image)) {
    const std::string detail = decoded.image.error[0]
                                   ? decoded.image.error
                                   : "unknown libjpeg failure";
    throw std::runtime_error("JPEG decoder rejected the image: " + detail);
  }
  const std::size_t byte_count =
      static_cast<std::size_t>(decoded.image.width) * decoded.image.height * 3U;
  return {decoded.image.width, decoded.image.height,
          std::vector<std::uint8_t>(decoded.image.pixels,
                                    decoded.image.pixels + byte_count)};
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
      throw std::runtime_error(
          "Windows COM initialization failed for static image decoding");
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

RgbImage decode_png_wic(const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() > std::numeric_limits<DWORD>::max())
    throw std::runtime_error("PNG is too large for Windows Imaging Component");

  ComApartment apartment;
  ComPtr<IWICImagingFactory> factory;
  require_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
                  "Windows Imaging Component factory creation failed");

  ComPtr<IWICStream> stream;
  require_hresult(factory->CreateStream(stream.ReleaseAndGetAddressOf()),
                  "Windows Imaging Component stream creation failed");
  require_hresult(stream->InitializeFromMemory(
                      const_cast<BYTE *>(reinterpret_cast<const BYTE *>(
                          bytes.data())),
                      static_cast<DWORD>(bytes.size())),
                  "Windows Imaging Component could not open PNG data");

  ComPtr<IWICBitmapDecoder> decoder;
  require_hresult(factory->CreateDecoderFromStream(
                      stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                      decoder.ReleaseAndGetAddressOf()),
                  "Windows Imaging Component could not decode PNG");
  GUID container{};
  require_hresult(decoder->GetContainerFormat(&container),
                  "Windows Imaging Component could not identify PNG");
  if (container != GUID_ContainerFormatPng)
    throw std::runtime_error("Windows Imaging Component did not identify a PNG");

  ComPtr<IWICBitmapFrameDecode> frame;
  require_hresult(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()),
                  "Windows Imaging Component could not read PNG frame");
  UINT width = 0;
  UINT height = 0;
  require_hresult(frame->GetSize(&width, &height),
                  "Windows Imaging Component returned invalid PNG dimensions");
  if (width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > kMaxImagePixels ||
      width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
      height > static_cast<UINT>(std::numeric_limits<int>::max()))
    throw std::runtime_error("PNG dimensions exceed gdupe's safety limit");

  ComPtr<IWICFormatConverter> converter;
  require_hresult(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf()),
                  "Windows Imaging Component format converter creation failed");
  require_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom),
                  "Windows Imaging Component could not convert PNG to RGB");

  const std::size_t stride = static_cast<std::size_t>(width) * 3;
  const std::size_t byte_count = stride * height;
  if (stride > std::numeric_limits<UINT>::max() ||
      byte_count > std::numeric_limits<UINT>::max())
    throw std::runtime_error("PNG RGB buffer exceeds Windows Imaging Component limits");

  RgbImage image{static_cast<int>(width), static_cast<int>(height),
                 std::vector<std::uint8_t>(byte_count)};
  require_hresult(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                        static_cast<UINT>(byte_count),
                                        image.pixels.data()),
                  "Windows Imaging Component could not copy PNG pixels");
  return image;
}

#endif

RgbImage decode_webp(const std::vector<std::uint8_t> &bytes) {
  int width = 0, height = 0;
  if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height) || width <= 0 ||
      height <= 0 ||
      static_cast<std::uint64_t>(width) * static_cast<unsigned>(height) >
          kMaxImagePixels)
    throw std::runtime_error("WebP decoder rejected the image");
  RgbImage image{width, height,
                 std::vector<std::uint8_t>(
                     static_cast<std::size_t>(width) * height * 3)};
  if (!WebPDecodeRGBInto(bytes.data(), bytes.size(), image.pixels.data(),
                         image.pixels.size(), width * 3))
    throw std::runtime_error("WebP decoder rejected the image");
  return image;
}

} // namespace

RgbImage decode_static_image(const std::filesystem::path &path,
                             const std::string &extension) {
  const auto bytes = read_file(path);
  if (extension == "jpg" || extension == "jpeg")
    return decode_jpeg(bytes);
  if (extension == "png") {
#ifdef _WIN32
    return decode_png_wic(bytes);
#else
    throw std::runtime_error("PNG decoding requires Windows Imaging Component");
#endif
  }
  if (extension == "webp")
    return decode_webp(bytes);
  throw std::runtime_error("Unsupported static image extension: " + extension);
}

} // namespace gdupe
