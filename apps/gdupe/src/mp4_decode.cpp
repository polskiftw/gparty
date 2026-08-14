#include "mp4_decode.hpp"
#include "annexb_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>

namespace gdupe {
namespace {

constexpr std::size_t kMaxCompressedSampleBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxAnnexBBytes = 320ULL * 1024ULL * 1024ULL;
constexpr std::array<std::uint8_t, 4> kStartCode{0, 0, 0, 1};

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("MP4 decoding exceeded its deadline");
}

class Mp4FileReader {
public:
  explicit Mp4FileReader(const std::filesystem::path &path)
      : stream_(path, std::ios::binary), size_(std::filesystem::file_size(path)) {
    if (!stream_)
      throw std::runtime_error("Could not open MP4/M4V file");
    if (size_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::runtime_error("MP4 file is too large for minimp4 offsets");
  }

  [[nodiscard]] std::int64_t size() const noexcept {
    return static_cast<std::int64_t>(size_);
  }

  int read(std::int64_t offset, void *buffer, std::size_t bytes) {
    if (offset < 0 || !buffer ||
        static_cast<std::uint64_t>(offset) > size_ ||
        bytes > size_ - static_cast<std::uint64_t>(offset))
      return 1;
    stream_.clear();
    stream_.seekg(offset, std::ios::beg);
    if (!stream_)
      return 1;
    stream_.read(static_cast<char *>(buffer), static_cast<std::streamsize>(bytes));
    return stream_.gcount() == static_cast<std::streamsize>(bytes) ? 0 : 1;
  }

  static int callback(std::int64_t offset, void *buffer, std::size_t bytes,
                      void *token) {
    return static_cast<Mp4FileReader *>(token)->read(offset, buffer, bytes);
  }

private:
  std::ifstream stream_;
  std::uint64_t size_{};
};

class Mp4Demux {
public:
  explicit Mp4Demux(Mp4FileReader &reader) {
    std::memset(&value_, 0, sizeof(value_));
    if (!MP4D_open(&value_, Mp4FileReader::callback, &reader, reader.size()))
      throw std::runtime_error("minimp4 rejected the MP4/M4V container");
    open_ = true;
  }

  ~Mp4Demux() {
    if (open_)
      MP4D_close(&value_);
  }

  MP4D_demux_t &get() noexcept { return value_; }
  const MP4D_demux_t &get() const noexcept { return value_; }

private:
  MP4D_demux_t value_{};
  bool open_{};
};

void append_nal(std::vector<std::uint8_t> &output, const void *data,
                std::size_t size) {
  if (!data || size == 0)
    return;
  if (output.size() > kMaxAnnexBBytes - kStartCode.size() ||
      size > kMaxAnnexBBytes - output.size() - kStartCode.size())
    throw std::runtime_error("MP4 Annex-B conversion exceeds gdupe's safety limit");
  output.insert(output.end(), kStartCode.begin(), kStartCode.end());
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  output.insert(output.end(), bytes, bytes + size);
}

std::vector<std::uint8_t> avc_parameter_sets(const MP4D_demux_t &demux,
                                              unsigned track_index) {
  std::vector<std::uint8_t> output;
  for (int index = 0;; ++index) {
    int bytes = 0;
    const void *sps = MP4D_read_sps(&demux, track_index, index, &bytes);
    if (!sps)
      break;
    if (bytes <= 0)
      throw std::runtime_error("minimp4 returned an invalid AVC SPS");
    append_nal(output, sps, static_cast<std::size_t>(bytes));
  }
  for (int index = 0;; ++index) {
    int bytes = 0;
    const void *pps = MP4D_read_pps(&demux, track_index, index, &bytes);
    if (!pps)
      break;
    if (bytes <= 0)
      throw std::runtime_error("minimp4 returned an invalid AVC PPS");
    append_nal(output, pps, static_cast<std::size_t>(bytes));
  }
  if (output.empty())
    throw std::runtime_error("AVC MP4 contains no SPS/PPS decoder configuration");
  return output;
}

std::vector<std::uint8_t> hevc_parameter_sets(const MP4D_track_t &track) {
  if (!track.dsi || track.dsi_bytes < 23)
    throw std::runtime_error("HEVC MP4 has an invalid hvcC decoder configuration");
  const std::span<const std::uint8_t> dsi(track.dsi, track.dsi_bytes);
  std::size_t position = 22;
  const unsigned arrays = dsi[position++];
  std::vector<std::uint8_t> output;
  for (unsigned array_index = 0; array_index < arrays; ++array_index) {
    if (position + 3 > dsi.size())
      throw std::runtime_error("Truncated HEVC hvcC array header");
    ++position;
    const unsigned count = (static_cast<unsigned>(dsi[position]) << 8U) |
                           static_cast<unsigned>(dsi[position + 1]);
    position += 2;
    for (unsigned nal_index = 0; nal_index < count; ++nal_index) {
      if (position + 2 > dsi.size())
        throw std::runtime_error("Truncated HEVC hvcC NAL length");
      const std::size_t bytes =
          (static_cast<std::size_t>(dsi[position]) << 8U) |
          static_cast<std::size_t>(dsi[position + 1]);
      position += 2;
      if (bytes == 0 || bytes > dsi.size() - position)
        throw std::runtime_error("Invalid HEVC hvcC NAL payload");
      append_nal(output, dsi.data() + position, bytes);
      position += bytes;
    }
  }
  if (output.empty())
    throw std::runtime_error("HEVC MP4 contains no VPS/SPS/PPS configuration");
  return output;
}

unsigned nal_length_size(const MP4D_track_t &track) {
  if (!track.dsi)
    throw std::runtime_error("MP4 video track has no decoder configuration");
  if (track.object_type_indication == MP4_OBJECT_TYPE_AVC) {
    if (track.dsi_bytes < 5)
      throw std::runtime_error("AVC decoder configuration is truncated");
    return (track.dsi[4] & 3U) + 1U;
  }
  if (track.object_type_indication == MP4_OBJECT_TYPE_HEVC) {
    if (track.dsi_bytes < 22)
      throw std::runtime_error("HEVC decoder configuration is truncated");
    return (track.dsi[21] & 3U) + 1U;
  }
  throw std::runtime_error("Unsupported MP4 video codec");
}

std::uint32_t read_be_length(const std::uint8_t *data, unsigned bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < bytes; ++index)
    value = (value << 8U) | data[index];
  return value;
}

std::vector<std::uint8_t> sample_to_annexb(
    std::span<const std::uint8_t> sample, unsigned length_size) {
  if (length_size < 1 || length_size > 4)
    throw std::runtime_error("MP4 uses an invalid NAL length field size");
  std::vector<std::uint8_t> output;
  output.reserve(std::min<std::size_t>(sample.size() + 64, kMaxAnnexBBytes));
  std::size_t position = 0;
  while (position < sample.size()) {
    if (sample.size() - position < length_size)
      throw std::runtime_error("Truncated MP4 NAL length field");
    const std::uint32_t bytes = read_be_length(sample.data() + position, length_size);
    position += length_size;
    if (bytes == 0 || bytes > sample.size() - position)
      throw std::runtime_error("Invalid length-prefixed NAL in MP4 sample");
    append_nal(output, sample.data() + position, bytes);
    position += bytes;
  }
  if (output.empty())
    throw std::runtime_error("MP4 sample contained no NAL units");
  return output;
}

class Mp4FrameSampler {
public:
  Mp4FrameSampler(std::size_t wanted, std::int64_t duration_ns,
                  const std::vector<std::int64_t> &timestamps)
      : wanted_(wanted), duration_ns_(std::max<std::int64_t>(0, duration_ns)),
        timestamps_(timestamps) {
    if (wanted_ == 0)
      throw std::runtime_error("MP4 sample count must be positive");
    frames_.reserve(wanted_);
  }

  void submit(AnnexBGrayFrame frame) {
    if (frames_.size() >= wanted_)
      return;
    std::int64_t timestamp_ns = last_timestamp_;
    if (frame.timestamp_token > 0 && frame.timestamp_token < timestamps_.size())
      timestamp_ns = timestamps_[frame.timestamp_token];
    timestamp_ns = std::max<std::int64_t>(0, timestamp_ns);
    last_timestamp_ = std::max(last_timestamp_, timestamp_ns);

    if (duration_ns_ > 0) {
      const long double fraction = static_cast<long double>(frames_.size()) /
                                   static_cast<long double>(wanted_);
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns_) * fraction);
      if (timestamp_ns < target)
        return;
    }

    DecodedGrayFrame converted;
    converted.width = frame.width;
    converted.height = frame.height;
    converted.timestamp_ns = timestamp_ns;
    converted.pixels = std::move(frame.pixels);
    width_ = std::max(width_, converted.width);
    height_ = std::max(height_, converted.height);
    frames_.push_back(std::move(converted));
  }

  [[nodiscard]] bool full() const noexcept { return frames_.size() >= wanted_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  std::vector<DecodedGrayFrame> take() { return std::move(frames_); }

private:
  std::size_t wanted_{};
  std::int64_t duration_ns_{};
  const std::vector<std::int64_t> &timestamps_;
  std::int64_t last_timestamp_{};
  int width_{};
  int height_{};
  std::vector<DecodedGrayFrame> frames_;
};

std::uint64_t track_duration_ticks(const MP4D_track_t &track) {
  return (static_cast<std::uint64_t>(track.duration_hi) << 32U) |
         static_cast<std::uint64_t>(track.duration_lo);
}

std::int64_t ticks_to_ns(std::uint64_t ticks, unsigned timescale) {
  if (timescale == 0)
    return 0;
  const long double ns = static_cast<long double>(ticks) * 1'000'000'000.0L /
                         static_cast<long double>(timescale);
  if (ns >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(ns);
}

} // namespace

DecodedMovingMedia decode_mp4_static(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  Mp4FileReader reader(path);
  Mp4Demux demux(reader);
  auto &mp4 = demux.get();

  unsigned video_index = mp4.track_count;
  for (unsigned index = 0; index < mp4.track_count; ++index) {
    const auto codec = mp4.track[index].object_type_indication;
    if (codec == MP4_OBJECT_TYPE_AVC || codec == MP4_OBJECT_TYPE_HEVC) {
      video_index = index;
      break;
    }
  }
  if (video_index == mp4.track_count)
    throw std::runtime_error("MP4/M4V contains no supported H.264/HEVC video track");

  const MP4D_track_t &track = mp4.track[video_index];
  if (track.sample_count == 0)
    throw std::runtime_error("MP4 video track contains no samples");
  if (track.timescale == 0)
    throw std::runtime_error("MP4 video track has an invalid zero timescale");

  const bool hevc = track.object_type_indication == MP4_OBJECT_TYPE_HEVC;
  const unsigned length_size = nal_length_size(track);
  std::vector<std::uint8_t> parameter_sets =
      hevc ? hevc_parameter_sets(track) : avc_parameter_sets(mp4, video_index);
  auto decoder = hevc ? make_hevc_annexb_decoder() : make_h264_annexb_decoder();
  decoder->initialize(parameter_sets);

  const std::int64_t duration_ns =
      ticks_to_ns(track_duration_ticks(track), track.timescale);
  std::vector<std::int64_t> timestamps(1, 0);
  timestamps.reserve(static_cast<std::size_t>(track.sample_count) + 1);
  Mp4FrameSampler sampler(sample_count, duration_ns, timestamps);
  const AnnexBFrameCallback on_frame = [&](AnnexBGrayFrame frame) {
    check_deadline(deadline);
    sampler.submit(std::move(frame));
  };

  std::vector<std::uint8_t> compressed;
  for (unsigned sample_index = 0;
       sample_index < track.sample_count && !sampler.full(); ++sample_index) {
    check_deadline(deadline);
    unsigned frame_bytes = 0;
    unsigned timestamp = 0;
    unsigned frame_duration = 0;
    const MP4D_file_offset_t offset = MP4D_frame_offset(
        &mp4, video_index, sample_index, &frame_bytes, &timestamp,
        &frame_duration);
    if (frame_bytes == 0 || frame_bytes > kMaxCompressedSampleBytes)
      throw std::runtime_error("MP4 compressed sample exceeds gdupe's safety limit");
    if (offset < 0 || static_cast<std::uint64_t>(offset) + frame_bytes >
                          static_cast<std::uint64_t>(reader.size()))
      throw std::runtime_error("minimp4 returned an invalid sample offset");

    compressed.resize(frame_bytes);
    if (reader.read(static_cast<std::int64_t>(offset), compressed.data(),
                    compressed.size()) != 0)
      throw std::runtime_error("Could not read an MP4 compressed sample");
    auto annexb = sample_to_annexb(compressed, length_size);

    const std::int64_t timestamp_ns = ticks_to_ns(timestamp, track.timescale);
    timestamps.push_back(timestamp_ns);
    const std::uint32_t token = static_cast<std::uint32_t>(timestamps.size() - 1);
    decoder->decode(annexb, token, on_frame);
  }

  decoder->flush(on_frame);
  DecodedMovingMedia result;
  result.width = std::max(decoder->width(), sampler.width());
  result.height = std::max(decoder->height(), sampler.height());
  result.duration_ms = duration_ns / 1'000'000;
  result.frame_count = track.sample_count;
  result.sampled_frames = sampler.take();
  if (result.sampled_frames.empty())
    throw std::runtime_error("MP4/M4V contained no decodable video frames");
  return result;
}

} // namespace gdupe
