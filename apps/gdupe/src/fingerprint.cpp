#include "fingerprint.hpp"
#include "crypto_hash.hpp"
#include "gif_decode.hpp"
#include "image_decode.hpp"
#include "media_decode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gdupe {
namespace {

struct GrayImage {
  int width{};
  int height{};
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept {
    return width <= 0 || height <= 0 || pixels.empty();
  }
};

struct GrayView {
  const std::uint8_t *pixels{};
  int width{};
  int height{};
  int stride{};
};

GrayView view(const GrayImage &image) {
  if (image.empty())
    throw std::runtime_error("Media decoder returned an empty frame");
  return {image.pixels.data(), image.width, image.height, image.width};
}

GrayView view(const DecodedGrayFrame &frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty() ||
      frame.pixels.size() != static_cast<std::size_t>(frame.width) * frame.height)
    throw std::runtime_error("Static media decoder returned an invalid grayscale frame");
  return {frame.pixels.data(), frame.width, frame.height, frame.width};
}

GrayView crop(GrayView image, int x, int y, int width, int height) {
  return {image.pixels + y * image.stride + x, width, height, image.stride};
}

std::vector<double> resize_gray(GrayView image, int output_width,
                                int output_height) {
  std::vector<double> result(static_cast<std::size_t>(output_width) *
                             output_height);
  for (int y = 0; y < output_height; ++y) {
    const double source_y = std::clamp(
        (y + 0.5) * image.height / output_height - 0.5, 0.0,
        static_cast<double>(image.height - 1));
    const int y0 = static_cast<int>(std::floor(source_y));
    const int y1 = std::min(y0 + 1, image.height - 1);
    const double fy = source_y - y0;
    for (int x = 0; x < output_width; ++x) {
      const double source_x = std::clamp(
          (x + 0.5) * image.width / output_width - 0.5, 0.0,
          static_cast<double>(image.width - 1));
      const int x0 = static_cast<int>(std::floor(source_x));
      const int x1 = std::min(x0 + 1, image.width - 1);
      const double fx = source_x - x0;
      const double top = image.pixels[y0 * image.stride + x0] * (1.0 - fx) +
                         image.pixels[y0 * image.stride + x1] * fx;
      const double bottom =
          image.pixels[y1 * image.stride + x0] * (1.0 - fx) +
          image.pixels[y1 * image.stride + x1] * fx;
      result[static_cast<std::size_t>(y) * output_width + x] =
          top * (1.0 - fy) + bottom * fy;
    }
  }
  return result;
}

std::vector<double> low_frequency_dct(GrayView image, int size,
                                      int frequencies) {
  const auto resized = resize_gray(image, size, size);
  std::vector<double> cosine(static_cast<std::size_t>(frequencies) * size);
  const double pi = std::acos(-1.0);
  for (int frequency = 0; frequency < frequencies; ++frequency)
    for (int position = 0; position < size; ++position)
      cosine[static_cast<std::size_t>(frequency) * size + position] =
          std::cos(pi * (2 * position + 1) * frequency / (2 * size));

  std::vector<double> horizontal(static_cast<std::size_t>(size) * frequencies);
  for (int y = 0; y < size; ++y)
    for (int u = 0; u < frequencies; ++u) {
      double sum = 0.0;
      for (int x = 0; x < size; ++x)
        sum += resized[static_cast<std::size_t>(y) * size + x] *
               cosine[static_cast<std::size_t>(u) * size + x];
      horizontal[static_cast<std::size_t>(y) * frequencies + u] = sum;
    }

  std::vector<double> result(static_cast<std::size_t>(frequencies) *
                             frequencies);
  for (int v = 0; v < frequencies; ++v)
    for (int u = 0; u < frequencies; ++u) {
      double sum = 0.0;
      for (int y = 0; y < size; ++y)
        sum += horizontal[static_cast<std::size_t>(y) * frequencies + u] *
               cosine[static_cast<std::size_t>(v) * size + y];
      const double alpha_u =
          u == 0 ? std::sqrt(1.0 / size) : std::sqrt(2.0 / size);
      const double alpha_v =
          v == 0 ? std::sqrt(1.0 / size) : std::sqrt(2.0 / size);
      result[static_cast<std::size_t>(v) * frequencies + u] =
          alpha_u * alpha_v * sum;
    }
  return result;
}

std::uint64_t perceptual_hash(GrayView image) {
  const auto transformed = low_frequency_dct(image, 32, 8);
  std::array<double, 63> values{};
  std::copy(transformed.begin() + 1, transformed.end(), values.begin());
  auto ordered = values;
  std::nth_element(ordered.begin(), ordered.begin() + ordered.size() / 2,
                   ordered.end());
  const double median = ordered[ordered.size() / 2];
  std::uint64_t result = 0;
  for (std::size_t bit = 0; bit < values.size(); ++bit)
    if (values[bit] > median)
      result |= std::uint64_t{1} << bit;
  return result;
}

std::array<std::uint8_t, 32> perceptual_hash256(GrayView image) {
  const auto transformed = low_frequency_dct(image, 64, 16);
  std::array<double, 255> coefficients{};
  std::copy(transformed.begin() + 1, transformed.end(), coefficients.begin());
  auto ordered = coefficients;
  std::nth_element(ordered.begin(), ordered.begin() + ordered.size() / 2,
                   ordered.end());
  const double median = ordered[ordered.size() / 2];
  std::array<std::uint8_t, 32> result{};
  for (std::size_t bit = 0; bit < coefficients.size(); ++bit)
    if (coefficients[bit] > median)
      result[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
  return result;
}

std::vector<std::uint64_t> crop_hashes(GrayView image) {
  std::vector<GrayView> regions;
  for (double ratio : {0.90, 0.75, 0.60}) {
    const int width =
        std::min(image.width, std::max(1, static_cast<int>(image.width * ratio)));
    const int height = std::min(
        image.height, std::max(1, static_cast<int>(image.height * ratio)));
    regions.push_back(crop(image, (image.width - width) / 2,
                           (image.height - height) / 2, width, height));
  }
  const int width = std::min(
      image.width, std::max(1, static_cast<int>(image.width * 0.78)));
  const int height = std::min(
      image.height, std::max(1, static_cast<int>(image.height * 0.78)));
  regions.push_back(crop(image, 0, 0, width, height));
  regions.push_back(crop(image, image.width - width, 0, width, height));
  regions.push_back(crop(image, 0, image.height - height, width, height));
  regions.push_back(crop(image, image.width - width, image.height - height,
                         width, height));
  std::vector<std::uint64_t> hashes;
  hashes.reserve(regions.size());
  for (const auto region : regions)
    hashes.push_back(perceptual_hash(region));
  return hashes;
}

GrayImage grayscale_image(const std::filesystem::path &path,
                          const std::string &extension) {
  const RgbImage decoded = decode_static_image(path, extension);
  if (decoded.empty())
    throw std::runtime_error("Static image decoder returned an empty image");
  GrayImage result{
      decoded.width, decoded.height,
      std::vector<std::uint8_t>(static_cast<std::size_t>(decoded.width) *
                                decoded.height)};
  for (std::size_t pixel = 0; pixel < result.pixels.size(); ++pixel) {
    const std::size_t source = pixel * 3;
    result.pixels[pixel] = static_cast<std::uint8_t>(
        (77U * decoded.pixels[source] + 150U * decoded.pixels[source + 1] +
         29U * decoded.pixels[source + 2] + 128U) >> 8U);
  }
  return result;
}

std::uint64_t majority_hash(const std::vector<std::uint64_t> &values) {
  std::uint64_t result = 0;
  for (unsigned int bit = 0; bit < 64; ++bit) {
    const std::size_t ones =
        std::count_if(values.begin(), values.end(), [bit](std::uint64_t value) {
          return ((value >> bit) & 1U) != 0;
        });
    if (ones * 2 >= values.size())
      result |= (std::uint64_t{1} << bit);
  }
  return result;
}

std::array<std::uint8_t, 32> majority_perceptual256(
    const std::vector<std::array<std::uint8_t, 32>> &values) {
  std::array<std::uint8_t, 32> result{};
  for (std::size_t byte = 0; byte < result.size(); ++byte) {
    for (unsigned int bit = 0; bit < 8; ++bit) {
      const std::size_t ones = std::count_if(
          values.begin(), values.end(), [byte, bit](const auto &value) {
            return ((value[byte] >> bit) & 1U) != 0;
          });
      if (ones * 2 >= values.size())
        result[byte] |= static_cast<std::uint8_t>(1U << bit);
    }
  }
  return result;
}

bool is_video_extension(const std::string &extension) {
  return extension == "mp4" || extension == "m4v" || extension == "webm";
}

} // namespace

int Fingerprinter::hamming(std::uint64_t first, std::uint64_t second) {
  return std::popcount(first ^ second);
}

int Fingerprinter::hamming(const std::array<std::uint8_t, 32> &first,
                           const std::array<std::uint8_t, 32> &second) {
  int distance = 0;
  for (std::size_t i = 0; i < first.size(); ++i)
    distance += std::popcount(static_cast<unsigned int>(first[i] ^ second[i]));
  return distance;
}

Fingerprint Fingerprinter::static_image(const std::filesystem::path &path,
                                        const std::string &extension) const {
  const GrayImage image = grayscale_image(path, extension);
  Fingerprint value;
  value.version = config_.fingerprint_version;
  value.kind = MediaKind::StaticImage;
  value.sha256 = sha256_file(path);
  value.width = image.width;
  value.height = image.height;
  value.frame_count = 1;
  value.phash = perceptual_hash(view(image));
  value.perceptual256 = perceptual_hash256(view(image));
  value.crop_hashes = crop_hashes(view(image));
  return value;
}

Fingerprint Fingerprinter::moving_media(const std::filesystem::path &path,
                                         const std::string &extension) const {
  const bool gif = extension == "gif";
  const std::size_t wanted = static_cast<std::size_t>(
      std::max(1, gif ? config_.gif_sample_frames : config_.video_sample_frames));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(30);
  DecodedMovingMedia decoded = gif
      ? decode_gif_static(path, wanted, deadline)
      : decode_moving_media_static(path, extension, wanted, deadline);
  if (decoded.sampled_frames.empty())
    throw std::runtime_error("Moving-media decoder returned no sampled frames");

  std::vector<std::uint64_t> timeline;
  std::vector<std::array<std::uint8_t, 32>> hashes256;
  timeline.reserve(decoded.sampled_frames.size());
  hashes256.reserve(decoded.sampled_frames.size());
  for (const auto &frame : decoded.sampled_frames) {
    const GrayView frame_view = view(frame);
    timeline.push_back(perceptual_hash(frame_view));
    hashes256.push_back(perceptual_hash256(frame_view));
  }

  const auto &representative = decoded.sampled_frames.front();
  Fingerprint value;
  value.version = config_.fingerprint_version;
  value.kind = gif ? MediaKind::AnimatedImage : MediaKind::Video;
  value.sha256 = sha256_file(path);
  value.width = decoded.width > 0 ? decoded.width : representative.width;
  value.height = decoded.height > 0 ? decoded.height : representative.height;
  value.frame_count = decoded.frame_count > 0
                          ? decoded.frame_count
                          : static_cast<std::int64_t>(decoded.sampled_frames.size());
  value.duration_ms = std::max<std::int64_t>(0, decoded.duration_ms);
  value.phash = majority_hash(timeline);
  value.perceptual256 = majority_perceptual256(hashes256);
  value.crop_hashes = crop_hashes(view(representative));
  value.timeline = std::move(timeline);
  return value;
}

Fingerprint Fingerprinter::compute(const std::filesystem::path &path,
                                   const std::string &extension) const {
  if (extension == "gif" || is_video_extension(extension))
    return moving_media(path, extension);
  if (extension == "jpg" || extension == "jpeg" || extension == "png" ||
      extension == "webp")
    return static_image(path, extension);
  throw std::runtime_error("Unsupported media extension: " + extension);
}

} // namespace gdupe
