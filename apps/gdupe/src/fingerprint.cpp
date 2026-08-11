#include "fingerprint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <openssl/evp.h>

namespace gdupe {
namespace {

cv::Mat grayscale(const cv::Mat &image) {
  if (image.empty())
    throw std::runtime_error("Media decoder returned an empty frame");
  cv::Mat gray;
  if (image.channels() == 1)
    gray = image;
  else if (image.channels() == 3)
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  else if (image.channels() == 4)
    cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
  else
    throw std::runtime_error("Unsupported image channel layout");
  if (gray.depth() != CV_8U) {
    cv::Mat normalized;
    double minimum = 0, maximum = 0;
    cv::minMaxLoc(gray, &minimum, &maximum);
    const double scale = maximum > minimum ? 255.0 / (maximum - minimum) : 1.0;
    gray.convertTo(normalized, CV_8U, scale, -minimum * scale);
    gray = normalized;
  }
  return gray;
}

std::string sha256_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Cannot open staged media for SHA-256");
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context)
    throw std::runtime_error("Cannot allocate SHA-256 context");
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error("Cannot initialize SHA-256");
  }
  std::array<char, 8 * 1024 * 1024> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0 && EVP_DigestUpdate(context, buffer.data(),
                                      static_cast<std::size_t>(count)) != 1) {
      EVP_MD_CTX_free(context);
      throw std::runtime_error("SHA-256 update failed");
    }
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &size) != 1) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error("SHA-256 finalization failed");
  }
  EVP_MD_CTX_free(context);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < size; ++i)
    output << std::setw(2) << static_cast<unsigned int>(digest[i]);
  return output.str();
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

std::array<std::uint8_t, 32>
majority_gradient(const std::vector<std::array<std::uint8_t, 32>> &values) {
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

std::vector<int> sample_positions(int total, int wanted) {
  if (total <= 0 || wanted <= 0)
    return {};
  const int count = std::min(total, wanted);
  std::vector<int> result;
  result.reserve(count);
  if (count == 1)
    return {0};
  for (int index = 0; index < count; ++index) {
    result.push_back(static_cast<int>(
        std::llround(static_cast<double>(index) * (total - 1) / (count - 1))));
  }
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool is_video_extension(const std::string &extension) {
  return extension == "mp4" || extension == "m4v" || extension == "webm" ||
         extension == "mov" || extension == "mkv";
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

std::uint64_t Fingerprinter::perceptual_hash(const cv::Mat &image) {
  cv::Mat resized, floating, transformed;
  cv::resize(grayscale(image), resized, {32, 32}, 0, 0, cv::INTER_AREA);
  resized.convertTo(floating, CV_32F);
  cv::dct(floating, transformed);
  std::array<float, 63> values{};
  std::size_t cursor = 0;
  for (int row = 0; row < 8; ++row)
    for (int column = 0; column < 8; ++column) {
      if (row != 0 || column != 0)
        values[cursor++] = transformed.at<float>(row, column);
    }
  auto median_values = values;
  std::nth_element(median_values.begin(),
                   median_values.begin() + median_values.size() / 2,
                   median_values.end());
  const float median = median_values[median_values.size() / 2];
  std::uint64_t hash = 0;
  cursor = 0;
  for (int row = 0; row < 8; ++row)
    for (int column = 0; column < 8; ++column) {
      if (row == 0 && column == 0)
        continue;
      if (values[cursor] > median)
        hash |= (std::uint64_t{1} << cursor);
      ++cursor;
    }
  return hash;
}

std::array<std::uint8_t, 32>
Fingerprinter::gradient_hash(const cv::Mat &image) {
  cv::Mat resized;
  cv::resize(grayscale(image), resized, {17, 16}, 0, 0, cv::INTER_AREA);
  std::array<std::uint8_t, 32> result{};
  int bit = 0;
  for (int row = 0; row < 16; ++row)
    for (int column = 0; column < 16; ++column, ++bit) {
      if (resized.at<std::uint8_t>(row, column) >
          resized.at<std::uint8_t>(row, column + 1)) {
        result[static_cast<std::size_t>(bit / 8)] |=
            static_cast<std::uint8_t>(1U << (bit % 8));
      }
    }
  return result;
}

std::vector<std::uint64_t> Fingerprinter::crop_hashes(const cv::Mat &image) {
  const cv::Mat gray = grayscale(image);
  std::vector<cv::Rect> regions;
  for (double ratio : {0.90, 0.75, 0.60}) {
    const int width =
        std::min(gray.cols, std::max(1, static_cast<int>(gray.cols * ratio)));
    const int height =
        std::min(gray.rows, std::max(1, static_cast<int>(gray.rows * ratio)));
    regions.emplace_back((gray.cols - width) / 2, (gray.rows - height) / 2,
                         width, height);
  }
  const int width =
      std::min(gray.cols, std::max(1, static_cast<int>(gray.cols * 0.78)));
  const int height =
      std::min(gray.rows, std::max(1, static_cast<int>(gray.rows * 0.78)));
  regions.emplace_back(0, 0, width, height);
  regions.emplace_back(gray.cols - width, 0, width, height);
  regions.emplace_back(0, gray.rows - height, width, height);
  regions.emplace_back(gray.cols - width, gray.rows - height, width, height);
  std::vector<std::uint64_t> hashes;
  hashes.reserve(regions.size());
  for (const auto &region : regions)
    hashes.push_back(perceptual_hash(gray(region)));
  return hashes;
}

Fingerprint
Fingerprinter::static_image(const std::filesystem::path &path) const {
  const cv::Mat image = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
  if (image.empty())
    throw std::runtime_error("Static image decoder rejected " +
                             path.filename().string());
  Fingerprint value;
  value.version = config_.fingerprint_version;
  value.kind = MediaKind::StaticImage;
  value.sha256 = sha256_file(path);
  value.width = image.cols;
  value.height = image.rows;
  value.frame_count = 1;
  value.phash = perceptual_hash(image);
  value.gradient256 = gradient_hash(image);
  value.crop_hashes = crop_hashes(image);
  return value;
}

Fingerprint Fingerprinter::moving_media(const std::filesystem::path &path,
                                        bool gif) const {
  std::vector<cv::Mat> fallback_frames;
  if (gif)
    cv::imreadmulti(path.string(), fallback_frames, cv::IMREAD_UNCHANGED);
  cv::VideoCapture capture;
  if (fallback_frames.empty())
    capture.open(path.string(), cv::CAP_FFMPEG);
  if (!capture.isOpened() && fallback_frames.empty())
    throw std::runtime_error("Moving-media decoder rejected " +
                             path.filename().string());
  int total = capture.isOpened() ? static_cast<int>(std::llround(
                                       capture.get(cv::CAP_PROP_FRAME_COUNT)))
                                 : static_cast<int>(fallback_frames.size());
  const int wanted =
      gif ? config_.gif_sample_frames : config_.video_sample_frames;
  if (total <= 0)
    total = wanted;
  const auto positions = sample_positions(total, wanted);
  std::vector<std::uint64_t> timeline;
  std::vector<std::array<std::uint8_t, 32>> gradients;
  cv::Mat representative;
  int width = 0;
  int height = 0;
  for (const int position : positions) {
    cv::Mat frame;
    if (!fallback_frames.empty())
      frame = fallback_frames[static_cast<std::size_t>(
          std::min(position, static_cast<int>(fallback_frames.size() - 1)))];
    else {
      capture.set(cv::CAP_PROP_POS_FRAMES, position);
      if (!capture.read(frame) || frame.empty())
        continue;
    }
    if (representative.empty())
      representative = frame.clone();
    width = std::max(width, frame.cols);
    height = std::max(height, frame.rows);
    timeline.push_back(perceptual_hash(frame));
    gradients.push_back(gradient_hash(frame));
  }
  if (timeline.empty())
    throw std::runtime_error("No decodable frames were found in moving media");
  const double fps = capture.isOpened() ? capture.get(cv::CAP_PROP_FPS) : 0.0;
  Fingerprint value;
  value.version = config_.fingerprint_version;
  value.kind = gif ? MediaKind::AnimatedImage : MediaKind::Video;
  value.sha256 = sha256_file(path);
  value.width = width;
  value.height = height;
  value.frame_count = total;
  value.duration_ms =
      fps > 0.01 ? static_cast<std::int64_t>(std::llround(1000.0 * total / fps))
                 : 0;
  value.phash = majority_hash(timeline);
  value.gradient256 = majority_gradient(gradients);
  value.crop_hashes = crop_hashes(representative);
  value.timeline = std::move(timeline);
  return value;
}

Fingerprint Fingerprinter::compute(const std::filesystem::path &path,
                                   const std::string &extension) const {
  if (extension == "gif")
    return moving_media(path, true);
  if (is_video_extension(extension))
    return moving_media(path, false);
  if (extension == "jpg" || extension == "jpeg" || extension == "png" ||
      extension == "webp" || extension == "bmp" || extension == "tif" ||
      extension == "tiff") {
    return static_image(path);
  }
  throw std::runtime_error("Unsupported media extension: " + extension);
}

} // namespace gdupe
