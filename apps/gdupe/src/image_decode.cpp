#include "image_decode.hpp"

#include <array>
#include <csetjmp>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

extern "C" {
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
}

namespace gdupe {
namespace {

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

struct JpegError {
  jpeg_error_mgr manager{};
  std::jmp_buf jump{};
  std::array<char, JMSG_LENGTH_MAX> message{};
};

void jpeg_failure(j_common_ptr context) {
  auto &error = *reinterpret_cast<JpegError *>(context->err);
  context->err->format_message(context, error.message.data());
  std::longjmp(error.jump, 1);
}

RgbImage decode_jpeg(const std::vector<std::uint8_t> &bytes) {
  jpeg_decompress_struct decoder{};
  JpegError error;
  decoder.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = jpeg_failure;
  if (setjmp(error.jump) != 0) {
    jpeg_destroy_decompress(&decoder);
    throw std::runtime_error("JPEG decoder rejected the image: " +
                             std::string(error.message.data()));
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, bytes.data(),
               static_cast<unsigned long>(bytes.size()));
  jpeg_read_header(&decoder, TRUE);
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  RgbImage image{
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

RgbImage decode_png(const std::vector<std::uint8_t> &bytes) {
  png_image decoder{};
  decoder.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&decoder, bytes.data(), bytes.size()))
    throw std::runtime_error("PNG decoder rejected the image");
  decoder.format = PNG_FORMAT_RGB;
  RgbImage image{static_cast<int>(decoder.width),
                 static_cast<int>(decoder.height),
                 std::vector<std::uint8_t>(PNG_IMAGE_SIZE(decoder))};
  if (!png_image_finish_read(&decoder, nullptr, image.pixels.data(), 0,
                             nullptr)) {
    const std::string message = decoder.message;
    png_image_free(&decoder);
    throw std::runtime_error("PNG decoder rejected the image: " + message);
  }
  png_image_free(&decoder);
  return image;
}

RgbImage decode_webp(const std::vector<std::uint8_t> &bytes) {
  int width = 0, height = 0;
  if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height) || width <= 0 ||
      height <= 0)
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
  if (extension == "png")
    return decode_png(bytes);
  if (extension == "webp")
    return decode_webp(bytes);
  throw std::runtime_error("Unsupported static image extension: " + extension);
}

} // namespace gdupe
