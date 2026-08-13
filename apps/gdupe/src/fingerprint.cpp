#include "fingerprint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include <QImage>
#include <QString>
#include <openssl/evp.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

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

GrayImage qt_image(const std::filesystem::path &path) {
  const QImage decoded(QString::fromStdWString(path.wstring()));
  if (decoded.isNull())
    throw std::runtime_error("Static image decoder rejected " +
                             path.filename().string());
  const QImage gray = decoded.convertToFormat(QImage::Format_Grayscale8);
  GrayImage result{gray.width(), gray.height(),
                   std::vector<std::uint8_t>(
                       static_cast<std::size_t>(gray.width()) * gray.height())};
  for (int row = 0; row < gray.height(); ++row)
    std::copy_n(gray.constScanLine(row), gray.width(),
                result.pixels.begin() +
                    static_cast<std::size_t>(row) * gray.width());
  return result;
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
  std::vector<char> buffer(1024 * 1024);
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

std::string ffmpeg_error(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0)
    return "FFmpeg error " + std::to_string(code);
  return buffer.data();
}

void require_ffmpeg(int code, const char *operation) {
  if (code < 0)
    throw std::runtime_error(std::string(operation) + ": " +
                             ffmpeg_error(code));
}

std::string utf8_path(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

struct FormatContext {
  AVFormatContext *value{};
  ~FormatContext() {
    if (value)
      avformat_close_input(&value);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext *value) const {
    avcodec_free_context(&value);
  }
};

struct PacketDeleter {
  void operator()(AVPacket *value) const { av_packet_free(&value); }
};

struct FrameDeleter {
  void operator()(AVFrame *value) const { av_frame_free(&value); }
};

struct SwsContextDeleter {
  void operator()(SwsContext *value) const { sws_freeContext(value); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct DecodeDeadline {
  std::chrono::steady_clock::time_point expires;
};

int interrupt_ffmpeg(void *opaque) noexcept {
  const auto &deadline = *static_cast<const DecodeDeadline *>(opaque);
  return std::chrono::steady_clock::now() >= deadline.expires ? 1 : 0;
}

GrayImage gray_frame(const AVFrame &frame, SwsContextPtr &scaler) {
  if (frame.width <= 0 || frame.height <= 0 || frame.format < 0)
    throw std::runtime_error("FFmpeg decoded an invalid video frame");
  SwsContext *updated = sws_getCachedContext(
      scaler.release(), frame.width, frame.height,
      static_cast<AVPixelFormat>(frame.format), frame.width, frame.height,
      AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
  scaler.reset(updated);
  if (!scaler)
    throw std::runtime_error("FFmpeg could not create its pixel converter");
  GrayImage image{frame.width, frame.height,
                  std::vector<std::uint8_t>(
                      static_cast<std::size_t>(frame.width) * frame.height)};
  std::array<std::uint8_t *, 4> destination{image.pixels.data(), nullptr,
                                            nullptr, nullptr};
  std::array<int, 4> destination_stride{image.width, 0, 0, 0};
  const int rows = sws_scale(scaler.get(), frame.data, frame.linesize, 0,
                             frame.height, destination.data(),
                             destination_stride.data());
  if (rows != frame.height)
    throw std::runtime_error("FFmpeg returned an incomplete converted frame");
  return image;
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

Fingerprint
Fingerprinter::static_image(const std::filesystem::path &path) const {
  const GrayImage image = qt_image(path);
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
                                        bool gif) const {
  DecodeDeadline deadline{
      std::chrono::steady_clock::now() + std::chrono::minutes(30)};
  FormatContext format;
  format.value = avformat_alloc_context();
  if (!format.value)
    throw std::runtime_error("FFmpeg could not allocate a media reader");
  format.value->interrupt_callback = {interrupt_ffmpeg, &deadline};
  const std::string input = utf8_path(path);
  require_ffmpeg(avformat_open_input(&format.value, input.c_str(), nullptr,
                                     nullptr),
                 "FFmpeg could not open moving media");
  require_ffmpeg(avformat_find_stream_info(format.value, nullptr),
                 "FFmpeg could not inspect moving media");

  const AVCodec *decoder = nullptr;
  const int video_index = av_find_best_stream(
      format.value, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  require_ffmpeg(video_index, "Moving media contains no supported video stream");
  AVStream *stream = format.value->streams[video_index];
  CodecContextPtr codec(avcodec_alloc_context3(decoder));
  if (!codec)
    throw std::runtime_error("FFmpeg could not allocate a video decoder");
  require_ffmpeg(avcodec_parameters_to_context(codec.get(),
                                               stream->codecpar),
                 "FFmpeg could not initialize the video decoder");
  codec->thread_count = 2;
  codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  require_ffmpeg(avcodec_open2(codec.get(), decoder, nullptr),
                 "FFmpeg could not start the video decoder");

  int width = codec->width;
  int height = codec->height;
  double duration_seconds = 0.0;
  if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
    duration_seconds = stream->duration * av_q2d(stream->time_base);
  else if (format.value->duration != AV_NOPTS_VALUE &&
           format.value->duration > 0)
    duration_seconds = format.value->duration /
                       static_cast<double>(AV_TIME_BASE);
  const AVRational guessed_rate =
      av_guess_frame_rate(format.value, stream, nullptr);
  const double fps = guessed_rate.num > 0 && guessed_rate.den > 0
                         ? av_q2d(guessed_rate)
                         : 0.0;
  std::int64_t total = stream->nb_frames;
  if (total <= 0 && duration_seconds > 0.0 && fps > 0.0)
    total = static_cast<std::int64_t>(std::llround(duration_seconds * fps));
  const int wanted =
      gif ? config_.gif_sample_frames : config_.video_sample_frames;
  const std::size_t sample_count = static_cast<std::size_t>(
      total > 0 ? std::max<std::int64_t>(1, std::min<std::int64_t>(total, wanted))
                : wanted);

  std::vector<std::uint64_t> timeline;
  std::vector<std::array<std::uint8_t, 32>> hashes256;
  timeline.reserve(sample_count);
  hashes256.reserve(sample_count);
  GrayImage representative;
  PacketPtr packet(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  if (!packet || !frame)
    throw std::runtime_error("FFmpeg could not allocate decode buffers");
  SwsContextPtr scaler;
  std::size_t next_sample = 0;
  std::int64_t decoded_frames = 0;
  const double start_seconds =
      stream->start_time != AV_NOPTS_VALUE
          ? stream->start_time * av_q2d(stream->time_base)
          : 0.0;

  const auto sample_frame = [&](const AVFrame &decoded) {
    if (next_sample >= sample_count)
      return;
    bool selected = false;
    if (duration_seconds > 0.0 &&
        decoded.best_effort_timestamp != AV_NOPTS_VALUE) {
      const double seconds =
          decoded.best_effort_timestamp * av_q2d(stream->time_base) -
          start_seconds;
      const double target = duration_seconds * next_sample / sample_count;
      const double tolerance = fps > 0.0 ? 0.5 / fps : 0.0;
      selected = seconds + tolerance >= target;
    } else if (total > 0) {
      const auto target = static_cast<std::int64_t>(
          (next_sample * static_cast<std::uint64_t>(total)) / sample_count);
      selected = decoded_frames >= target;
    } else {
      selected = true;
    }
    ++decoded_frames;
    if (!selected)
      return;

    GrayImage image = gray_frame(decoded, scaler);
    if (representative.empty())
      representative = image;
    width = std::max(width, image.width);
    height = std::max(height, image.height);
    timeline.push_back(perceptual_hash(view(image)));
    hashes256.push_back(perceptual_hash256(view(image)));
    ++next_sample;
  };

  const auto drain_decoder = [&] {
    while (true) {
      if (interrupt_ffmpeg(&deadline) != 0)
        throw std::runtime_error("FFmpeg decoding exceeded 30 minutes");
      const int result = avcodec_receive_frame(codec.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        return;
      require_ffmpeg(result, "FFmpeg failed while decoding a video frame");
      sample_frame(*frame);
      av_frame_unref(frame.get());
    }
  };

  while (next_sample < sample_count) {
    if (interrupt_ffmpeg(&deadline) != 0)
      throw std::runtime_error("FFmpeg decoding exceeded 30 minutes");
    const int result = av_read_frame(format.value, packet.get());
    if (result == AVERROR_EOF)
      break;
    require_ffmpeg(result, "FFmpeg failed while reading moving media");
    if (packet->stream_index == video_index) {
      require_ffmpeg(avcodec_send_packet(codec.get(), packet.get()),
                     "FFmpeg rejected a video packet");
      drain_decoder();
    }
    av_packet_unref(packet.get());
  }
  if (next_sample < sample_count) {
    require_ffmpeg(avcodec_send_packet(codec.get(), nullptr),
                   "FFmpeg could not finish the video stream");
    drain_decoder();
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
  value.crop_hashes = crop_hashes(view(representative));
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
      extension == "webp") {
    return static_image(path);
  }
  throw std::runtime_error("Unsupported media extension: " + extension);
}

} // namespace gdupe
