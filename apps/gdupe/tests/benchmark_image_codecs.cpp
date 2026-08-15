#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

using Microsoft::WRL::ComPtr;

namespace {

struct Image {
  int width{};
  int height{};
  std::vector<std::uint8_t> pixels;
};

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

void check_hr(HRESULT result, const char *what) {
  if (FAILED(result))
    fail(std::string(what) + " failed (HRESULT " +
         std::to_string(static_cast<unsigned long>(result)) + ")");
}

class ComApartment {
public:
  ComApartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE)
      check_hr(result, "CoInitializeEx");
    owns_ = SUCCEEDED(result);
  }
  ~ComApartment() {
    if (owns_)
      CoUninitialize();
  }

private:
  bool owns_{};
};

ComPtr<IWICImagingFactory> make_wic_factory() {
  ComPtr<IWICImagingFactory> factory;
  check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                            CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
           "CoCreateInstance(CLSID_WICImagingFactory)");
  return factory;
}

Image decode_wic(const std::vector<std::uint8_t> &bytes,
                 IWICImagingFactory *factory) {
  if (bytes.empty() || bytes.size() > MAXDWORD)
    fail("WIC benchmark input size is invalid");

  ComPtr<IWICStream> stream;
  check_hr(factory->CreateStream(stream.ReleaseAndGetAddressOf()),
           "IWICImagingFactory::CreateStream");
  check_hr(stream->InitializeFromMemory(
               const_cast<BYTE *>(reinterpret_cast<const BYTE *>(bytes.data())),
               static_cast<DWORD>(bytes.size())),
           "IWICStream::InitializeFromMemory");

  ComPtr<IWICBitmapDecoder> decoder;
  check_hr(factory->CreateDecoderFromStream(
               stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
               decoder.ReleaseAndGetAddressOf()),
           "IWICImagingFactory::CreateDecoderFromStream");

  ComPtr<IWICBitmapFrameDecode> frame;
  check_hr(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()),
           "IWICBitmapDecoder::GetFrame");

  UINT width = 0;
  UINT height = 0;
  check_hr(frame->GetSize(&width, &height), "IWICBitmapFrameDecode::GetSize");
  if (width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > 100000000ULL)
    fail("WIC benchmark decoded unreasonable image dimensions");

  ComPtr<IWICFormatConverter> converter;
  check_hr(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf()),
           "IWICImagingFactory::CreateFormatConverter");
  check_hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom),
           "IWICFormatConverter::Initialize(24bppRGB)");

  const std::size_t stride = static_cast<std::size_t>(width) * 3;
  const std::size_t byte_count = stride * height;
  if (stride > MAXUINT || byte_count > MAXUINT)
    fail("WIC benchmark RGB buffer is too large");

  Image image{static_cast<int>(width), static_cast<int>(height),
              std::vector<std::uint8_t>(byte_count)};
  check_hr(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                 static_cast<UINT>(byte_count),
                                 image.pixels.data()),
           "IWICBitmapSource::CopyPixels");
  return image;
}

Image decode_jpeg(const std::vector<std::uint8_t> &bytes) {
  jpeg_decompress_struct decoder{};
  jpeg_error_mgr errors{};
  decoder.err = jpeg_std_error(&errors);
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, bytes.data(),
               static_cast<unsigned long>(bytes.size()));
  jpeg_read_header(&decoder, TRUE);
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);

  Image image{
      static_cast<int>(decoder.output_width),
      static_cast<int>(decoder.output_height),
      std::vector<std::uint8_t>(static_cast<std::size_t>(decoder.output_width) *
                                decoder.output_height * 3)};
  while (decoder.output_scanline < decoder.output_height) {
    JSAMPROW row = image.pixels.data() +
                   static_cast<std::size_t>(decoder.output_scanline) *
                       decoder.output_width * 3;
    jpeg_read_scanlines(&decoder, &row, 1);
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  return image;
}

Image decode_png(const std::vector<std::uint8_t> &bytes) {
  png_image decoder{};
  decoder.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&decoder, bytes.data(), bytes.size()))
    fail("libpng benchmark decoder rejected generated PNG");
  decoder.format = PNG_FORMAT_RGB;
  Image image{static_cast<int>(decoder.width),
              static_cast<int>(decoder.height),
              std::vector<std::uint8_t>(PNG_IMAGE_SIZE(decoder))};
  if (!png_image_finish_read(&decoder, nullptr, image.pixels.data(), 0,
                             nullptr)) {
    const std::string message = decoder.message;
    png_image_free(&decoder);
    fail("libpng benchmark decoder failed: " + message);
  }
  png_image_free(&decoder);
  return image;
}

std::vector<std::uint8_t> make_rgb(int width, int height) {
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
  std::uint32_t state = 0x7f4a7c15U;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      state ^= state << 13U;
      state ^= state >> 17U;
      state ^= state << 5U;
      const std::size_t p = (static_cast<std::size_t>(y) * width + x) * 3;
      rgb[p + 0] = static_cast<std::uint8_t>((x * 3 + y + (state & 31U)) & 255);
      rgb[p + 1] = static_cast<std::uint8_t>((x + y * 5 + ((state >> 5U) & 31U)) & 255);
      rgb[p + 2] = static_cast<std::uint8_t>((x * 2 + y * 2 + ((state >> 10U) & 31U)) & 255);
    }
  }
  return rgb;
}

std::vector<std::uint8_t> encode_jpeg(const std::vector<std::uint8_t> &rgb,
                                      int width, int height) {
  jpeg_compress_struct encoder{};
  jpeg_error_mgr errors{};
  encoder.err = jpeg_std_error(&errors);
  jpeg_create_compress(&encoder);

  unsigned char *memory = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&encoder, &memory, &size);
  encoder.image_width = width;
  encoder.image_height = height;
  encoder.input_components = 3;
  encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  jpeg_set_quality(&encoder, 90, TRUE);
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(
        rgb.data() + static_cast<std::size_t>(encoder.next_scanline) * width * 3);
    jpeg_write_scanlines(&encoder, &row, 1);
  }
  jpeg_finish_compress(&encoder);
  std::vector<std::uint8_t> result(memory, memory + size);
  std::free(memory);
  jpeg_destroy_compress(&encoder);
  return result;
}

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t> &rgb,
                                     int width, int height) {
  png_image encoder{};
  encoder.version = PNG_IMAGE_VERSION;
  encoder.width = static_cast<png_uint_32>(width);
  encoder.height = static_cast<png_uint_32>(height);
  encoder.format = PNG_FORMAT_RGB;

  png_alloc_size_t size = 0;
  if (!png_image_write_to_memory(&encoder, nullptr, &size, 0, rgb.data(), 0,
                                 nullptr))
    fail("libpng benchmark could not size encoded PNG");
  std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
  if (!png_image_write_to_memory(&encoder, result.data(), &size, 0, rgb.data(),
                                 0, nullptr))
    fail("libpng benchmark could not encode PNG");
  result.resize(static_cast<std::size_t>(size));
  png_image_free(&encoder);
  return result;
}

struct Difference {
  double mean_absolute{};
  int maximum{};
  std::size_t changed{};
};

Difference compare_pixels(const Image &a, const Image &b) {
  if (a.width != b.width || a.height != b.height ||
      a.pixels.size() != b.pixels.size())
    fail("Decoder output dimensions differ");
  std::uint64_t total = 0;
  int maximum = 0;
  std::size_t changed = 0;
  for (std::size_t i = 0; i < a.pixels.size(); ++i) {
    const int delta = std::abs(static_cast<int>(a.pixels[i]) -
                               static_cast<int>(b.pixels[i]));
    total += static_cast<unsigned>(delta);
    maximum = std::max(maximum, delta);
    changed += delta != 0;
  }
  return {static_cast<double>(total) / a.pixels.size(), maximum, changed};
}

volatile std::uint64_t sink = 0;

template <typename Decode>
double benchmark(Decode decode, int iterations) {
  for (int i = 0; i < 3; ++i) {
    const Image image = decode();
    sink = sink ^ image.pixels[static_cast<std::size_t>(i) % image.pixels.size()];
  }
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const Image image = decode();
    const std::size_t p = (static_cast<std::size_t>(i) * 7919U) %
                          image.pixels.size();
    sink = sink ^ image.pixels[p];
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count() /
         iterations;
}

void run_case(int width, int height, int jpeg_iterations, int png_iterations,
              IWICImagingFactory *factory) {
  std::cout << "\n" << width << "x" << height << " ("
            << std::fixed << std::setprecision(2)
            << (static_cast<double>(width) * height / 1000000.0) << " MP)\n";
  const auto source = make_rgb(width, height);
  const auto jpeg = encode_jpeg(source, width, height);
  const auto png = encode_png(source, width, height);

  const Image jpeg_lib = decode_jpeg(jpeg);
  const Image jpeg_wic = decode_wic(jpeg, factory);
  const Image png_lib = decode_png(png);
  const Image png_wic = decode_wic(png, factory);
  const Difference jpeg_diff = compare_pixels(jpeg_lib, jpeg_wic);
  const Difference png_diff = compare_pixels(png_lib, png_wic);

  const double jpeg_lib_ms =
      benchmark([&] { return decode_jpeg(jpeg); }, jpeg_iterations);
  const double jpeg_wic_ms = benchmark(
      [&] { return decode_wic(jpeg, factory); }, jpeg_iterations);
  const double png_lib_ms =
      benchmark([&] { return decode_png(png); }, png_iterations);
  const double png_wic_ms =
      benchmark([&] { return decode_wic(png, factory); }, png_iterations);

  auto print = [&](const char *format, double third_party_ms, double wic_ms,
                   const Difference &difference, std::size_t compressed) {
    const double ratio = wic_ms / third_party_ms;
    std::cout << format << ": third-party=" << std::setprecision(3)
              << third_party_ms << " ms, WIC=" << wic_ms
              << " ms, WIC/third-party=" << std::setprecision(2) << ratio
              << "x, compressed=" << (compressed / 1024) << " KiB"
              << ", pixel MAE=" << std::setprecision(4)
              << difference.mean_absolute << ", max=" << difference.maximum
              << ", changed=" << difference.changed << "\n";
  };

  print("JPEG", jpeg_lib_ms, jpeg_wic_ms, jpeg_diff, jpeg.size());
  print("PNG ", png_lib_ms, png_wic_ms, png_diff, png.size());
}

} // namespace

int main() {
  try {
    ComApartment apartment;
    auto factory = make_wic_factory();
    std::cout << "gdupe image decoder benchmark\n"
                 "Exact workload: full decode to 8-bit 24bpp RGB; file I/O excluded.\n"
                 "WIC factory is reused, matching the intended production backend.\n";
    run_case(1920, 1080, 20, 12, factory.Get());
    run_case(3840, 2160, 6, 4, factory.Get());
    std::cout << "sink=" << sink << "\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << "benchmark failed: " << problem.what() << "\n";
    return 1;
  }
}
