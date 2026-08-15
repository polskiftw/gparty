#include "media_decode.hpp"
#include "mp4_decode.hpp"
#include "nvdec_decode.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <webm/mkvparser/mkvparser.h>

namespace gdupe {
namespace {

constexpr std::size_t kMaxCompressedFrameBytes = 256ULL * 1024ULL * 1024ULL;

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("Moving-media decoding exceeded its deadline");
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
    if (position < 0 || length < 0 || !buffer || position > length_ ||
        length > length_ - position)
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

NvdecCodec nvdec_codec_for_webm(std::string_view codec) {
  if (codec == "V_VP8")
    return NvdecCodec::vp8;
  if (codec == "V_VP9")
    return NvdecCodec::vp9;
  if (codec == "V_AV1")
    return NvdecCodec::av1;
  throw std::runtime_error("WebM uses an unsupported video codec");
}

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

  const std::int64_t duration_ns =
      std::max<std::int64_t>(0, segment->GetDuration());
  FrameSampler sampler(sample_count, duration_ns);
  NvdecPacketDecoder decoder(nvdec_codec_for_webm(codec));
  const NvdecFrameCallback on_nvdec_frame = [&](NvdecGrayFrame frame) {
    check_deadline(deadline);
    DecodedGrayFrame converted;
    converted.width = frame.width;
    converted.height = frame.height;
    converted.timestamp_ns = frame.timestamp;
    converted.pixels = std::move(frame.pixels);
    sampler.submit(std::move(converted));
  };

  const mkvparser::BlockEntry *entry = nullptr;
  long status = video_track->GetFirst(entry);
  if (status < 0)
    throw std::runtime_error("libwebm could not locate the first video block");

  while (entry && !entry->EOS()) {
    check_deadline(deadline);
    const auto *block = entry->GetBlock();
    if (!block)
      throw std::runtime_error("libwebm returned an empty video block");
    const std::int64_t timestamp_ns =
        std::max<std::int64_t>(0, block->GetTime(entry->GetCluster()));
    for (int frame_index = 0; frame_index < block->GetFrameCount(); ++frame_index) {
      const auto &frame = block->GetFrame(frame_index);
      if (frame.len <= 0 ||
          static_cast<std::size_t>(frame.len) > kMaxCompressedFrameBytes)
        throw std::runtime_error(
            "WebM compressed frame exceeds gdupe's safety limit");
      std::vector<std::uint8_t> packet(static_cast<std::size_t>(frame.len));
      if (frame.Read(&reader, packet.data()) != 0)
        throw std::runtime_error("libwebm could not read compressed video data");
      decoder.decode(packet, timestamp_ns, on_nvdec_frame);
    }

    const mkvparser::BlockEntry *next = nullptr;
    status = video_track->GetNext(entry, next);
    if (status < 0)
      throw std::runtime_error("libwebm failed while advancing the video track");
    entry = next;
  }

  decoder.flush(on_nvdec_frame);

  DecodedMovingMedia result;
  result.width = std::max<int>(static_cast<int>(video_track->GetWidth()),
                               std::max(decoder.width(), sampler.width()));
  result.height = std::max<int>(static_cast<int>(video_track->GetHeight()),
                                std::max(decoder.height(), sampler.height()));
  result.duration_ms = duration_ns / 1'000'000;
  result.frame_count = sampler.decoded_count();
  result.sampled_frames = sampler.take_frames();
  if (result.sampled_frames.empty())
    throw std::runtime_error("WebM contained no NVDEC-decodable video frames");
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
    return decode_mp4_static(path, sample_count, deadline);
  throw std::runtime_error("Unsupported moving-media extension: " + extension);
}

} // namespace gdupe
