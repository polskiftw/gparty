#include "fingerprint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
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

QString native_path(const std::filesystem::path &path) {
  return QString::fromStdWString(path.wstring());
}

std::string run_tool(const std::filesystem::path &program,
                     const QStringList &arguments, int timeout_ms,
                     const char *purpose) {
  if (!std::filesystem::is_regular_file(program))
    throw std::runtime_error(std::string(purpose) +
                             " tool is missing: " + program.string());
  QProcess process;
  process.setProgram(native_path(program));
  process.setArguments(arguments);
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(QIODevice::ReadOnly);
  if (!process.waitForStarted(10'000))
    throw std::runtime_error(std::string(purpose) + " could not start");
  if (!process.waitForFinished(timeout_ms)) {
    process.kill();
    process.waitForFinished(5'000);
    throw std::runtime_error(std::string(purpose) +
                             " exceeded its bounded execution time");
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    std::string error = process.readAllStandardError().toStdString();
    if (error.size() > 2'000)
      error.erase(0, error.size() - 2'000);
    throw std::runtime_error(std::string(purpose) + " failed: " + error);
  }
  return process.readAllStandardOutput().toStdString();
}

double json_number(const nlohmann::json &object, const char *name) {
  if (!object.contains(name) || object.at(name).is_null())
    return 0.0;
  const auto &value = object.at(name);
  if (value.is_number())
    return value.get<double>();
  if (!value.is_string())
    return 0.0;
  const std::string text = value.get<std::string>();
  if (text.empty() || text == "N/A")
    return 0.0;
  try {
    return std::stod(text);
  } catch (...) {
    return 0.0;
  }
}

double frame_rate(const nlohmann::json &stream) {
  const std::string value = stream.value("avg_frame_rate", std::string("0/1"));
  const auto slash = value.find('/');
  if (slash == std::string::npos)
    return json_number(stream, "avg_frame_rate");
  try {
    const double numerator = std::stod(value.substr(0, slash));
    const double denominator = std::stod(value.substr(slash + 1));
    return denominator > 0.0 ? numerator / denominator : 0.0;
  } catch (...) {
    return 0.0;
  }
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
Fingerprinter::perceptual_hash256(const cv::Mat &image) {
  cv::Mat resized, floating, transformed;
  cv::resize(grayscale(image), resized, {64, 64}, 0, 0, cv::INTER_AREA);
  resized.convertTo(floating, CV_32F);
  cv::dct(floating, transformed);
  std::array<float, 255> coefficients{};
  std::size_t cursor = 0;
  for (int row = 0; row < 16; ++row)
    for (int column = 0; column < 16; ++column)
      if (row != 0 || column != 0)
        coefficients[cursor++] = transformed.at<float>(row, column);
  auto ordered = coefficients;
  std::nth_element(ordered.begin(), ordered.begin() + ordered.size() / 2,
                   ordered.end());
  const float median = ordered[ordered.size() / 2];
  std::array<std::uint8_t, 32> result{};
  for (std::size_t bit = 0; bit < coefficients.size(); ++bit)
    if (coefficients[bit] > median)
      result[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
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
  value.perceptual256 = perceptual_hash256(image);
  value.crop_hashes = crop_hashes(image);
  return value;
}

Fingerprint Fingerprinter::moving_media(const std::filesystem::path &path,
                                        bool gif) const {
  const std::string probe_output = run_tool(
      config_.ffprobe_path,
      {"-v", "error", "-select_streams", "v:0", "-show_entries",
       "stream=width,height,nb_frames,avg_frame_rate,duration:format=duration",
       "-of", "json", native_path(path)},
      60'000, "ffprobe metadata inspection");
  const auto probe = nlohmann::json::parse(probe_output);
  const auto streams = probe.value("streams", nlohmann::json::array());
  if (streams.empty())
    throw std::runtime_error("Moving media contains no video stream: " +
                             path.filename().string());
  const auto &stream = streams.front();
  int width = stream.value("width", 0);
  int height = stream.value("height", 0);
  double duration_seconds = json_number(stream, "duration");
  if (duration_seconds <= 0.0)
    duration_seconds = json_number(
        probe.value("format", nlohmann::json::object()), "duration");
  const double fps = frame_rate(stream);
  std::int64_t total =
      static_cast<std::int64_t>(std::llround(json_number(stream, "nb_frames")));
  if (total <= 0 && duration_seconds > 0.0 && fps > 0.0)
    total = static_cast<std::int64_t>(std::llround(duration_seconds * fps));
  const int wanted =
      gif ? config_.gif_sample_frames : config_.video_sample_frames;
  const int sample_count =
      total > 0 ? static_cast<int>(std::min<std::int64_t>(total, wanted))
                : wanted;

  QTemporaryDir frames_directory(
      native_path(config_.cache_directory / "ffmpeg-frames-XXXXXX"));
  if (!frames_directory.isValid())
    throw std::runtime_error("Cannot create temporary FFmpeg frame directory");
  std::ostringstream filter;
  filter.imbue(std::locale::classic());
  if (duration_seconds > 0.0) {
    filter << "fps=" << std::fixed << std::setprecision(9)
           << std::max(0.001, sample_count / duration_seconds);
  } else if (total > 0) {
    const auto interval = std::max<std::int64_t>(1, total / sample_count);
    filter << "select=not(mod(n\\," << interval << "))";
  } else {
    filter << "fps=1";
  }
  const auto output_pattern =
      std::filesystem::path(frames_directory.path().toStdWString()) /
      "frame-%05d.png";
  run_tool(config_.ffmpeg_path,
           {"-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "2",
            "-i", native_path(path), "-an", "-sn", "-dn", "-vf",
            QString::fromStdString(filter.str()), "-frames:v",
            QString::number(sample_count), "-fps_mode", "vfr",
            native_path(output_pattern)},
           30 * 60 * 1000, "FFmpeg frame sampling");

  std::vector<std::filesystem::path> frame_paths;
  for (const auto &entry : std::filesystem::directory_iterator(
           std::filesystem::path(frames_directory.path().toStdWString())))
    if (entry.is_regular_file() && entry.path().extension() == ".png")
      frame_paths.push_back(entry.path());
  std::sort(frame_paths.begin(), frame_paths.end());
  std::vector<std::uint64_t> timeline;
  std::vector<std::array<std::uint8_t, 32>> hashes256;
  cv::Mat representative;
  for (const auto &frame_path : frame_paths) {
    const cv::Mat frame = cv::imread(frame_path.string(), cv::IMREAD_UNCHANGED);
    if (frame.empty())
      continue;
    if (representative.empty())
      representative = frame.clone();
    width = std::max(width, frame.cols);
    height = std::max(height, frame.rows);
    timeline.push_back(perceptual_hash(frame));
    hashes256.push_back(perceptual_hash256(frame));
  }
  if (timeline.empty())
    throw std::runtime_error("No decodable frames were found in moving media");
  Fingerprint value;
  value.version = config_.fingerprint_version;
  value.kind = gif ? MediaKind::AnimatedImage : MediaKind::Video;
  value.sha256 = sha256_file(path);
  value.width = width;
  value.height = height;
  value.frame_count = total > 0 ? total : timeline.size();
  value.duration_ms =
      duration_seconds > 0.0
          ? static_cast<std::int64_t>(std::llround(duration_seconds * 1000.0))
          : 0;
  value.phash = majority_hash(timeline);
  value.perceptual256 = majority_perceptual256(hashes256);
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
      extension == "webp" || extension == "bmp") {
    return static_image(path);
  }
  throw std::runtime_error("Unsupported media extension: " + extension);
}

} // namespace gdupe
