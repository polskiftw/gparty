#include "media_decode.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

extern "C" {
#include <dav1d/dav1d.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_image.h>
}

#include <mkvparser/mkvparser.h>

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxFramePixels = 100'000'000ULL;
constexpr std::size_t kMaxCompressedFrameBytes = 256ULL * 1024ULL * 1024ULL;

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("Moving-media decoding exceeded its deadline");
}

void validate_frame_dimensions(int width, int height) {
  if (width <= 0 || height <= 0)
    throw std::runtime_error("Decoder returned invalid frame dimensions");
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > kMaxFramePixels)
    throw std::runtime_error("Decoded frame exceeds gdupe's safety limit");
}

std::vector<std::uint8_t> copy_luma_8(const std::uint8_t *plane, int stride,
                                      int width, int height) {
  if (!plane || stride < width)
    throw std::runtime_error("Decoder returned an invalid 8-bit luma plane");
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y)
    std::memcpy(out.data() + static_cast<std::size_t>(y) * width,
                plane + static_cast<std::ptrdiff_t>(y) * stride,
                static_cast<std::size_t>(width));
  return out;
}

std::vector<std::uint8_t> copy_luma_high(const std::uint8_t *plane, int stride,
                                         int width, int height,
                                         unsigned bit_depth) {
  if (!plane || bit_depth <= 8 || bit_depth > 16 ||
      stride < width * static_cast<int>(sizeof(std::uint16_t)))
    throw std::runtime_error("Decoder returned an invalid high-bit-depth luma plane");
  const std::uint32_t maximum = (std::uint32_t{1} << bit_depth) - 1U;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    const auto *row = reinterpret_cast<const std::uint16_t *>(
        plane + static_cast<std::ptrdiff_t>(y) * stride);
    auto *destination = out.data() + static_cast<std::size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      const std::uint32_t value = std::min<std::uint32_t>(row[x], maximum);
      destination[x] = static_cast<std::uint8_t>(
          (value * 255U + maximum / 2U) / maximum);
    }
  }
  return out;
}

DecodedGrayFrame gray_from_vpx(const vpx_image_t &image,
                               std::int64_t timestamp_ns) {
  const int width = static_cast<int>(image.d_w);
  const int height = static_cast<int>(image.d_h);
  validate_frame_dimensions(width, height);
  DecodedGrayFrame result;
  result.width = width;
  result.height = height;
  result.timestamp_ns = timestamp_ns;
  if ((image.fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0) {
    result.pixels = copy_luma_high(image.planes[VPX_PLANE_Y],
                                   image.stride[VPX_PLANE_Y], width, height,
                                   image.bit_depth);
  } else {
    result.pixels = copy_luma_8(image.planes[VPX_PLANE_Y],
                                image.stride[VPX_PLANE_Y], width, height);
  }
  return result;
}

DecodedGrayFrame gray_from_dav1d(const Dav1dPicture &picture) {
  const int width = picture.p.w;
  const int height = picture.p.h;
  validate_frame_dimensions(width, height);
  if (!picture.data[0])
    throw std::runtime_error("dav1d returned an empty luma plane");
  DecodedGrayFrame result;
  result.width = width;
  result.height = height;
  result.timestamp_ns = picture.m.timestamp == std::numeric_limits<std::int64_t>::min()
                            ? 0
                            : picture.m.timestamp;
  const auto *plane = static_cast<const std::uint8_t *>(picture.data[0]);
  if (picture.p.bpc > 8) {
    result.pixels = copy_luma_high(plane, static_cast<int>(picture.stride[0]),
                                   width, height,
                                   static_cast<unsigned>(picture.p.bpc));
  } else {
    result.pixels = copy_luma_8(plane, static_cast<int>(picture.stride[0]),
                                width, height);
  }
  return result;
}

class FrameSampler {
public:
  FrameSampler(std::size_t maximum, std::int64_t duration_ns)
      : maximum_(maximum), duration_ns_(std::max<std::int64_t>(0, duration_ns)) {
    if (maximum_ == 0)
      throw std::runtime_error("Moving-media sample count must be positive");
    frames_.reserve(maximum_);
  }

  void submit(DecodedGrayFrame frame) {
    ++decoded_count_;
    width_ = std::max(width_, frame.width);
    height_ = std::max(height_, frame.height);
    if (frames_.size() >= maximum_)
      return;

    if (duration_ns_ > 0) {
      const long double fraction = static_cast<long double>(frames_.size()) /
                                   static_cast<long double>(maximum_);
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns_) * fraction);
      if (std::max<std::int64_t>(0, frame.timestamp_ns) < target)
        return;
    }
    frames_.push_back(std::move(frame));
  }

  [[nodiscard]] std::int64_t decoded_count() const noexcept {
    return decoded_count_;
  }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  std::vector<DecodedGrayFrame> take_frames() { return std::move(frames_); }

private:
  std::size_t maximum_{};
  std::int64_t duration_ns_{};
  std::int64_t decoded_count_{};
  int width_{};
  int height_{};
  std::vector<DecodedGrayFrame> frames_;
};

class FileMkvReader final : public mkvparser::IMkvReader {
public:
  explicit FileMkvReader(const std::filesystem::path &path)
      : stream_(path, std::ios::binary),
        length_(static_cast<long long>(std::filesystem::file_size(path))) {
    if (!stream_)
      throw std::runtime_error("Could not open WebM file");
  }

  int Read(long long position, long length, unsigned char *buffer) override {
    if (position < 0 || length < 0 || !buffer ||
        position > length_ || length > length_ - position)
      return -1;
    stream_.clear();
    stream_.seekg(position, std::ios::beg);
    if (!stream_)
      return -1;
    stream_.read(reinterpret_cast<char *>(buffer), length);
    return stream_.gcount() == length ? 0 : -1;
  }

  int Length(long long *total, long long *available) override {
    if (!total || !available)
      return -1;
    *total = length_;
    *available = length_;
    return 0;
  }

private:
  std::ifstream stream_;
  long long length_{};
};

class VpxDecoder {
public:
  explicit VpxDecoder(bool vp9) {
    vpx_codec_dec_cfg_t config{};
    config.threads = 2;
    const auto status = vpx_codec_dec_init(
        &context_, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &config, 0);
    if (status != VPX_CODEC_OK)
      throw std::runtime_error("Could not initialize static libvpx decoder");
    initialized_ = true;
  }

  VpxDecoder(const VpxDecoder &) = delete;
  VpxDecoder &operator=(const VpxDecoder &) = delete;

  ~VpxDecoder() {
    if (initialized_)
      vpx_codec_destroy(&context_);
  }

  template <typename Callback>
  void decode(const std::uint8_t *data, std::size_t size,
              std::int64_t timestamp_ns, Callback &&callback) {
    timestamps_.push_back(timestamp_ns);
    const auto token = reinterpret_cast<void *>(
        static_cast<std::uintptr_t>(timestamps_.size()));
    const auto status = vpx_codec_decode(&context_, data,
                                         static_cast<unsigned int>(size),
                                         token, 0);
    if (status != VPX_CODEC_OK)
      throw std::runtime_error(std::string("libvpx decode failed: ") +
                               vpx_codec_error(&context_));
    drain(std::forward<Callback>(callback), timestamp_ns);
  }

  template <typename Callback> void flush(Callback &&callback) {
    const auto status = vpx_codec_decode(&context_, nullptr, 0, nullptr, 0);
    if (status != VPX_CODEC_OK)
      throw std::runtime_error(std::string("libvpx flush failed: ") +
                               vpx_codec_error(&context_));
    drain(std::forward<Callback>(callback),
          timestamps_.empty() ? 0 : timestamps_.back());
  }

private:
  template <typename Callback>
  void drain(Callback &&callback, std::int64_t fallback_timestamp) {
    vpx_codec_iter_t iterator = nullptr;
    while (vpx_image_t *image = vpx_codec_get_frame(&context_, &iterator)) {
      auto timestamp = fallback_timestamp;
      if (image->user_priv) {
        const auto encoded = reinterpret_cast<std::uintptr_t>(image->user_priv);
        if (encoded > 0 && encoded <= timestamps_.size())
          timestamp = timestamps_[encoded - 1];
      }
      callback(gray_from_vpx(*image, timestamp));
    }
  }

  vpx_codec_ctx_t context_{};
  bool initialized_{};
  std::vector<std::int64_t> timestamps_;
};

class Dav1dDecoder {
public:
  Dav1dDecoder() {
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 2;
    settings.max_frame_delay = 1;
    // Film grain is codec-synthesized presentation detail. gdupe fingerprints
    // the decoded source luma deterministically instead.
    settings.apply_grain = 0;
    if (dav1d_open(&context_, &settings) < 0)
      throw std::runtime_error("Could not initialize static dav1d decoder");
  }

  Dav1dDecoder(const Dav1dDecoder &) = delete;
  Dav1dDecoder &operator=(const Dav1dDecoder &) = delete;

  ~Dav1dDecoder() {
    if (context_)
      dav1d_close(&context_);
  }

  template <typename Callback>
  void decode(const std::uint8_t *bytes, std::size_t size,
              std::int64_t timestamp_ns, Callback &&callback) {
    Dav1dData data{};
    auto *destination = dav1d_data_create(&data, size);
    if (!destination)
      throw std::runtime_error("dav1d could not allocate compressed-frame data");
    std::memcpy(destination, bytes, size);
    data.m.timestamp = timestamp_ns;
    data.m.duration = 0;
    data.m.offset = -1;

    while (data.sz > 0) {
      const int status = dav1d_send_data(context_, &data);
      if (status == DAV1D_ERR(EAGAIN)) {
        drain(std::forward<Callback>(callback));
        continue;
      }
      if (status < 0) {
        dav1d_data_unref(&data);
        throw std::runtime_error("dav1d rejected AV1 frame data: " +
                                 std::to_string(status));
      }
      drain(std::forward<Callback>(callback));
    }
    dav1d_data_unref(&data);
  }

  template <typename Callback> void flush(Callback &&callback) {
    drain(std::forward<Callback>(callback));
  }

private:
  template <typename Callback> void drain(Callback &&callback) {
    while (true) {
      Dav1dPicture picture{};
      const int status = dav1d_get_picture(context_, &picture);
      if (status == DAV1D_ERR(EAGAIN))
        return;
      if (status < 0)
        throw std::runtime_error("dav1d failed while producing a frame: " +
                                 std::to_string(status));
      try {
        callback(gray_from_dav1d(picture));
      } catch (...) {
        dav1d_picture_unref(&picture);
        throw;
      }
      dav1d_picture_unref(&picture);
    }
  }

  Dav1dContext *context_{};
};

DecodedMovingMedia decode_webm(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  FileMkvReader reader(path);
  long long position = 0;
  mkvparser::EBMLHeader header;
  if (header.Parse(&reader, position) < 0)
    throw std::runtime_error("libwebm rejected the EBML header");

  mkvparser::Segment *raw_segment = nullptr;
  if (mkvparser::Segment::CreateInstance(&reader, position, raw_segment) != 0 ||
      !raw_segment)
    throw std::runtime_error("libwebm could not create a WebM segment");
  std::unique_ptr<mkvparser::Segment> segment(raw_segment);
  if (segment->Load() < 0)
    throw std::runtime_error("libwebm could not parse the WebM segment");

  const auto *tracks = segment->GetTracks();
  if (!tracks)
    throw std::runtime_error("WebM contains no track table");

  const mkvparser::VideoTrack *video_track = nullptr;
  std::string_view codec;
  for (unsigned long index = 0; index < tracks->GetTracksCount(); ++index) {
    const auto *track = tracks->GetTrackByIndex(index);
    if (!track || track->GetType() != mkvparser::Track::kVideo)
      continue;
    const char *codec_id = track->GetCodecId();
    if (!codec_id)
      continue;
    const std::string_view candidate(codec_id);
    if (candidate == "V_VP8" || candidate == "V_VP9" ||
        candidate == "V_AV1") {
      video_track = static_cast<const mkvparser::VideoTrack *>(track);
      codec = candidate;
      break;
    }
  }
  if (!video_track)
    throw std::runtime_error("WebM contains no supported VP8/VP9/AV1 video track");

  const std::int64_t duration_ns = std::max<std::int64_t>(0, segment->GetDuration());
  FrameSampler sampler(sample_count, duration_ns);
  const auto on_frame = [&](DecodedGrayFrame frame) {
    check_deadline(deadline);
    sampler.submit(std::move(frame));
  };

  std::unique_ptr<VpxDecoder> vpx;
  std::unique_ptr<Dav1dDecoder> av1;
  if (codec == "V_VP8" || codec == "V_VP9")
    vpx = std::make_unique<VpxDecoder>(codec == "V_VP9");
  else
    av1 = std::make_unique<Dav1dDecoder>();

  const mkvparser::BlockEntry *entry = nullptr;
  long status = video_track->GetFirst(entry);
  if (status < 0)
    throw std::runtime_error("libwebm could not locate the first video block");

  while (entry && !entry->EOS()) {
    check_deadline(deadline);
    const auto *block = entry->GetBlock();
    if (!block)
      throw std::runtime_error("libwebm returned an empty video block");
    const std::int64_t timestamp_ns = std::max<std::int64_t>(0, block->GetTime(entry->GetCluster()));
    for (int frame_index = 0; frame_index < block->GetFrameCount(); ++frame_index) {
      const auto &frame = block->GetFrame(frame_index);
      if (frame.len <= 0 || static_cast<std::size_t>(frame.len) > kMaxCompressedFrameBytes)
        throw std::runtime_error("WebM compressed frame exceeds gdupe's safety limit");
      std::vector<std::uint8_t> packet(static_cast<std::size_t>(frame.len));
      if (frame.Read(&reader, packet.data()) != 0)
        throw std::runtime_error("libwebm could not read compressed video data");
      if (vpx)
        vpx->decode(packet.data(), packet.size(), timestamp_ns, on_frame);
      else
        av1->decode(packet.data(), packet.size(), timestamp_ns, on_frame);
    }

    const mkvparser::BlockEntry *next = nullptr;
    status = video_track->GetNext(entry, next);
    if (status < 0)
      throw std::runtime_error("libwebm failed while advancing the video track");
    entry = next;
  }

  if (vpx)
    vpx->flush(on_frame);
  else
    av1->flush(on_frame);

  DecodedMovingMedia result;
  result.width = std::max<int>(static_cast<int>(video_track->GetWidth()), sampler.width());
  result.height = std::max<int>(static_cast<int>(video_track->GetHeight()), sampler.height());
  result.duration_ms = duration_ns / 1'000'000;
  result.frame_count = sampler.decoded_count();
  result.sampled_frames = sampler.take_frames();
  if (result.sampled_frames.empty())
    throw std::runtime_error("WebM contained no decodable video frames");
  return result;
}

} // namespace

DecodedMovingMedia decode_moving_media_static(
    const std::filesystem::path &path, const std::string &extension,
    std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  if (extension == "webm")
    return decode_webm(path, sample_count, deadline);
  if (extension == "mp4" || extension == "m4v")
    throw std::runtime_error("Static MP4 H.264/HEVC decoder is not wired yet");
  throw std::runtime_error("Unsupported static moving-media extension: " + extension);
}

} // namespace gdupe
