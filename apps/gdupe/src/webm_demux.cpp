#include "video_demux_internal.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <webm/mkvparser/mkvparser.h>

namespace gdupe {
namespace {

constexpr std::size_t kMaxCompressedFrameBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCodecConfigBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kAv1CodecConfigurationRecordHeaderBytes = 4;
constexpr std::uint64_t kDefaultTimecodeScaleNs = 1'000'000ULL;

constexpr std::uint64_t kEbmlSegment = 0x18538067ULL;
constexpr std::uint64_t kEbmlInfo = 0x1549A966ULL;
constexpr std::uint64_t kEbmlTimecodeScale = 0x2AD7B1ULL;
constexpr std::uint64_t kEbmlDuration = 0x4489ULL;
constexpr std::uint64_t kEbmlTracks = 0x1654AE6BULL;
constexpr std::uint64_t kEbmlTrackEntry = 0xAEULL;
constexpr std::uint64_t kEbmlTrackNumber = 0xD7ULL;
constexpr std::uint64_t kEbmlTrackType = 0x83ULL;
constexpr std::uint64_t kEbmlCodecId = 0x86ULL;
constexpr std::uint64_t kEbmlCodecPrivate = 0x63A2ULL;
constexpr std::uint64_t kEbmlVideo = 0xE0ULL;
constexpr std::uint64_t kEbmlPixelWidth = 0xB0ULL;
constexpr std::uint64_t kEbmlPixelHeight = 0xBAULL;
constexpr std::uint64_t kEbmlCluster = 0x1F43B675ULL;
constexpr std::uint64_t kEbmlVideoTrackType = 1ULL;

class FileMkvReader final : public mkvparser::IMkvReader {
public:
  explicit FileMkvReader(const std::filesystem::path &path)
      : stream_(path, std::ios::binary) {
    if (!stream_)
      throw std::runtime_error("Could not open WebM file");
    const auto size = std::filesystem::file_size(path);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<long long>::max()))
      throw std::runtime_error("WebM file is too large for libwebm offsets");
    length_ = static_cast<long long>(size);
  }

  int Read(long long position, long length, unsigned char *buffer) override {
    if (position < 0 || length < 0)
      return -1;
    if (length == 0)
      return 0;
    if (!buffer || position >= length_ || length > length_ - position)
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

  [[nodiscard]] long long size() const noexcept { return length_; }

private:
  std::ifstream stream_;
  long long length_{};
};

struct EbmlElement {
  std::uint64_t id{};
  long long start{};
  long long payload_start{};
  long long end{};
  long long payload_size{};
  bool unknown_size{};
};

EbmlElement read_element(FileMkvReader &reader, long long position,
                         long long parent_end) {
  if (position < 0 || position >= parent_end || parent_end > reader.size())
    throw std::runtime_error("Invalid WebM element boundary");

  long id_length = 0;
  const long long id_length_status =
      mkvparser::GetUIntLength(&reader, position, id_length);
  if (id_length_status != 0 || id_length < 1 || id_length > 4 ||
      id_length > parent_end - position)
    throw std::runtime_error("Truncated WebM element ID");
  const long long raw_id = mkvparser::ReadID(&reader, position, id_length);
  if (raw_id <= 0)
    throw std::runtime_error("Invalid WebM element ID");

  const long long size_position = position + id_length;
  if (size_position >= parent_end)
    throw std::runtime_error("Truncated WebM element size");
  long size_length = 0;
  const long long size_length_status =
      mkvparser::GetUIntLength(&reader, size_position, size_length);
  if (size_length_status != 0 || size_length < 1 || size_length > 8 ||
      size_length > parent_end - size_position)
    throw std::runtime_error("Truncated WebM element size");
  const long long raw_size =
      mkvparser::ReadUInt(&reader, size_position, size_length);
  if (raw_size < 0)
    throw std::runtime_error("Invalid WebM element size");

  const long long payload_start = size_position + size_length;
  const std::uint64_t unknown_value =
      (std::uint64_t{1} << (7U * static_cast<unsigned>(size_length))) - 1U;
  const bool unknown = static_cast<std::uint64_t>(raw_size) == unknown_value;
  if (unknown)
    return {static_cast<std::uint64_t>(raw_id), position, payload_start,
            parent_end, parent_end - payload_start, true};
  if (raw_size > parent_end - payload_start)
    throw std::runtime_error("WebM element extends beyond its parent");
  return {static_cast<std::uint64_t>(raw_id), position, payload_start,
          payload_start + raw_size, raw_size, false};
}

long long read_uint(FileMkvReader &reader, const EbmlElement &element,
                    const char *name) {
  if (element.unknown_size || element.payload_size < 1 ||
      element.payload_size > 8)
    throw std::runtime_error(std::string("Invalid WebM ") + name);
  const long long value = mkvparser::UnserializeUInt(
      &reader, element.payload_start, element.payload_size);
  if (value < 0)
    throw std::runtime_error(std::string("Could not read WebM ") + name);
  return value;
}

std::vector<std::uint8_t> read_bytes(FileMkvReader &reader,
                                     const EbmlElement &element,
                                     std::size_t maximum, const char *name) {
  if (element.unknown_size || element.payload_size < 0 ||
      static_cast<std::uint64_t>(element.payload_size) > maximum)
    throw std::runtime_error(std::string("WebM ") + name +
                             " exceeds gdupe's safety limit");
  std::vector<std::uint8_t> bytes(
      static_cast<std::size_t>(element.payload_size));
  if (!bytes.empty() &&
      reader.Read(element.payload_start, static_cast<long>(bytes.size()),
                  bytes.data()) != 0)
    throw std::runtime_error(std::string("Could not read WebM ") + name);
  return bytes;
}

std::string read_string(FileMkvReader &reader, const EbmlElement &element,
                        const char *name) {
  constexpr std::size_t kMaxStringBytes = 1024;
  const auto bytes = read_bytes(reader, element, kMaxStringBytes, name);
  return {bytes.begin(), bytes.end()};
}

NvdecCodec codec_from_id(std::string_view codec_id) {
  if (codec_id == "V_VP8")
    return NvdecCodec::vp8;
  if (codec_id == "V_VP9")
    return NvdecCodec::vp9;
  if (codec_id == "V_AV1")
    return NvdecCodec::av1;
  throw std::runtime_error("WebM uses an unsupported video codec");
}

bool supported_codec(std::string_view codec_id) noexcept {
  return codec_id == "V_VP8" || codec_id == "V_VP9" ||
         codec_id == "V_AV1";
}

std::vector<std::uint8_t>
av1_codec_header(std::span<const std::uint8_t> codec_private) {
  if (codec_private.empty())
    return {};
  if (codec_private.size() < kAv1CodecConfigurationRecordHeaderBytes)
    throw std::runtime_error(
        "WebM AV1 CodecPrivate is shorter than an AV1CodecConfigurationRecord");
  const auto marker_and_version = codec_private.front();
  if ((marker_and_version & 0x80U) == 0 ||
      (marker_and_version & 0x7fU) != 1U)
    throw std::runtime_error(
        "WebM AV1 CodecPrivate has an unsupported configuration-record version");
  if (codec_private.size() == kAv1CodecConfigurationRecordHeaderBytes)
    return {};
  const auto config_obus =
      codec_private.subspan(kAv1CodecConfigurationRecordHeaderBytes);
  return {config_obus.begin(), config_obus.end()};
}

struct WebmTrackMetadata {
  long long number{};
  std::string codec_id;
  int width{};
  int height{};
  std::vector<std::uint8_t> codec_private;
};

void parse_video_settings(FileMkvReader &reader, const EbmlElement &video,
                          WebmTrackMetadata &track) {
  if (video.unknown_size)
    throw std::runtime_error("WebM Video metadata has unknown size");
  for (long long position = video.payload_start; position < video.end;) {
    const auto child = read_element(reader, position, video.end);
    if (child.unknown_size)
      throw std::runtime_error("WebM Video child has unknown size");
    if (child.id == kEbmlPixelWidth) {
      const long long width = read_uint(reader, child, "PixelWidth");
      if (width <= 0 || width > std::numeric_limits<int>::max())
        throw std::runtime_error("WebM PixelWidth is invalid");
      track.width = static_cast<int>(width);
    } else if (child.id == kEbmlPixelHeight) {
      const long long height = read_uint(reader, child, "PixelHeight");
      if (height <= 0 || height > std::numeric_limits<int>::max())
        throw std::runtime_error("WebM PixelHeight is invalid");
      track.height = static_cast<int>(height);
    }
    position = child.end;
  }
}

std::optional<WebmTrackMetadata>
parse_track_entry(FileMkvReader &reader, const EbmlElement &entry) {
  if (entry.unknown_size)
    throw std::runtime_error("WebM TrackEntry has unknown size");

  WebmTrackMetadata track;
  long long track_type = 0;
  for (long long position = entry.payload_start; position < entry.end;) {
    const auto child = read_element(reader, position, entry.end);
    if (child.unknown_size)
      throw std::runtime_error("WebM TrackEntry child has unknown size");
    if (child.id == kEbmlTrackNumber) {
      track.number = read_uint(reader, child, "TrackNumber");
    } else if (child.id == kEbmlTrackType) {
      track_type = read_uint(reader, child, "TrackType");
    } else if (child.id == kEbmlCodecId) {
      track.codec_id = read_string(reader, child, "CodecID");
    } else if (child.id == kEbmlCodecPrivate) {
      track.codec_private =
          read_bytes(reader, child, kMaxCodecConfigBytes, "CodecPrivate");
    } else if (child.id == kEbmlVideo) {
      parse_video_settings(reader, child, track);
    }
    position = child.end;
  }

  if (track_type != static_cast<long long>(kEbmlVideoTrackType) ||
      !supported_codec(track.codec_id))
    return std::nullopt;
  if (track.number <= 0 || track.width <= 0 || track.height <= 0)
    throw std::runtime_error("WebM supported video track has incomplete metadata");
  return track;
}

std::optional<WebmTrackMetadata>
parse_tracks(FileMkvReader &reader, const EbmlElement &tracks) {
  if (tracks.unknown_size)
    throw std::runtime_error("WebM Tracks metadata has unknown size");
  for (long long position = tracks.payload_start; position < tracks.end;) {
    const auto child = read_element(reader, position, tracks.end);
    if (child.unknown_size)
      throw std::runtime_error("WebM Tracks child has unknown size");
    if (child.id == kEbmlTrackEntry) {
      if (auto track = parse_track_entry(reader, child))
        return track;
    }
    position = child.end;
  }
  return std::nullopt;
}

struct WebmInfoMetadata {
  std::uint64_t timecode_scale_ns{kDefaultTimecodeScaleNs};
  std::int64_t duration_ns{};
};

WebmInfoMetadata parse_info(FileMkvReader &reader, const EbmlElement &info) {
  if (info.unknown_size)
    throw std::runtime_error("WebM Info metadata has unknown size");

  WebmInfoMetadata result;
  double duration_ticks = 0.0;
  bool has_duration = false;
  for (long long position = info.payload_start; position < info.end;) {
    const auto child = read_element(reader, position, info.end);
    if (child.unknown_size)
      throw std::runtime_error("WebM Info child has unknown size");
    if (child.id == kEbmlTimecodeScale) {
      const long long scale = read_uint(reader, child, "TimecodeScale");
      if (scale <= 0)
        throw std::runtime_error("WebM TimecodeScale is invalid");
      result.timecode_scale_ns = static_cast<std::uint64_t>(scale);
    } else if (child.id == kEbmlDuration) {
      if (child.payload_size != 4 && child.payload_size != 8)
        throw std::runtime_error("WebM Duration has an invalid float size");
      double value = 0.0;
      if (mkvparser::UnserializeFloat(&reader, child.payload_start,
                                     child.payload_size, value) != 0 ||
          !std::isfinite(value) || value < 0.0)
        throw std::runtime_error("WebM Duration is invalid");
      duration_ticks = value;
      has_duration = true;
    }
    position = child.end;
  }

  if (has_duration) {
    const long double duration_ns =
        static_cast<long double>(duration_ticks) * result.timecode_scale_ns;
    if (duration_ns >=
        static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
      result.duration_ns = std::numeric_limits<std::int64_t>::max();
    else
      result.duration_ns = static_cast<std::int64_t>(duration_ns);
  }
  return result;
}

std::int64_t timestamp_ns(long long timecode,
                          std::uint64_t timecode_scale_ns) {
  if (timecode <= 0)
    return 0;
  const long double value =
      static_cast<long double>(timecode) * timecode_scale_ns;
  if (value >=
      static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value);
}

class WebmVideoDemux final : public VideoDemux {
public:
  explicit WebmVideoDemux(const std::filesystem::path &path) : reader_(path) {
    long long after_ebml = 0;
    mkvparser::EBMLHeader header;
    if (header.Parse(&reader_, after_ebml) < 0)
      throw std::runtime_error("libwebm rejected the EBML header");

    mkvparser::Segment *raw_segment = nullptr;
    if (mkvparser::Segment::CreateInstance(&reader_, after_ebml, raw_segment) !=
            0 ||
        !raw_segment)
      throw std::runtime_error("libwebm could not create a WebM segment");
    segment_.reset(raw_segment);

    const EbmlElement segment_element = find_segment(after_ebml);
    segment_payload_start_ = segment_element.payload_start;
    segment_end_ = segment_element.end;

    WebmInfoMetadata container_info;
    std::optional<WebmTrackMetadata> selected_track;
    for (long long position = segment_payload_start_; position < segment_end_;) {
      const auto element = read_element(reader_, position, segment_end_);
      if (element.id == kEbmlCluster) {
        first_cluster_start_ = element.start;
        break;
      }
      if (element.unknown_size)
        throw std::runtime_error(
            "WebM metadata before the first Cluster has unknown size");
      if (element.id == kEbmlInfo)
        container_info = parse_info(reader_, element);
      else if (element.id == kEbmlTracks && !selected_track)
        selected_track = parse_tracks(reader_, element);
      position = element.end;
    }

    if (!selected_track)
      throw std::runtime_error(
          "WebM contains no supported VP8/VP9/AV1 video track");
    if (first_cluster_start_ < 0)
      throw std::runtime_error("WebM contains no Cluster video data");

    track_number_ = selected_track->number;
    timecode_scale_ns_ = container_info.timecode_scale_ns;
    info_.codec = codec_from_id(selected_track->codec_id);
    info_.width = selected_track->width;
    info_.height = selected_track->height;
    info_.duration_ns = container_info.duration_ns;
    if (info_.codec == NvdecCodec::av1)
      info_.codec_header = av1_codec_header(selected_track->codec_private);
  }

  [[nodiscard]] const DemuxedVideoInfo &info() const noexcept override {
    return info_;
  }

  bool visit_packets(const DemuxedVideoPacketCallback &callback) override {
    if (!callback)
      throw std::runtime_error("Video packet callback is empty");

    long long cluster_start = first_cluster_start_;
    long cluster_index = 0;
    while (cluster_start >= 0 && cluster_start < segment_end_) {
      const long long relative = cluster_start - segment_payload_start_;
      if (relative < 0)
        throw std::runtime_error("WebM Cluster precedes Segment payload");
      std::unique_ptr<mkvparser::Cluster> cluster(
          mkvparser::Cluster::Create(segment_.get(), cluster_index++, relative));
      if (!cluster)
        throw std::runtime_error("libwebm could not create a Cluster parser");

      const mkvparser::BlockEntry *entry = nullptr;
      long status = cluster->GetFirst(entry);
      if (status < 0)
        throw std::runtime_error("libwebm could not read the first WebM block");
      while (entry) {
        const auto *block = entry->GetBlock();
        if (!block)
          throw std::runtime_error("libwebm returned an empty WebM block entry");
        if (block->GetTrackNumber() == track_number_) {
          const long long block_timecode = block->GetTimeCode(cluster.get());
          if (block_timecode < 0)
            throw std::runtime_error(
                "libwebm returned an invalid block timestamp");
          const std::int64_t packet_timestamp =
              timestamp_ns(block_timecode, timecode_scale_ns_);

          for (int frame_index = 0; frame_index < block->GetFrameCount();
               ++frame_index) {
            const auto &frame = block->GetFrame(frame_index);
            if (frame.len <= 0 ||
                static_cast<std::size_t>(frame.len) > kMaxCompressedFrameBytes)
              throw std::runtime_error(
                  "WebM compressed frame exceeds gdupe's safety limit");
            DemuxedVideoPacket packet;
            packet.bytes.resize(static_cast<std::size_t>(frame.len));
            packet.timestamp_ns = packet_timestamp;
            if (frame.Read(&reader_, packet.bytes.data()) != 0)
              throw std::runtime_error(
                  "libwebm could not read compressed WebM video data");
            if (!callback(std::move(packet)))
              return false;
          }
        }

        const mkvparser::BlockEntry *next = nullptr;
        status = cluster->GetNext(entry, next);
        if (status < 0)
          throw std::runtime_error("libwebm could not advance a WebM block");
        entry = next;
      }

      const long long cluster_size = cluster->GetElementSize();
      if (cluster_size <= 0 || cluster_size > segment_end_ - cluster_start)
        throw std::runtime_error("libwebm returned an invalid Cluster size");
      cluster_start = find_next_cluster(cluster_start + cluster_size);
    }
    return true;
  }

private:
  EbmlElement find_segment(long long position) {
    while (position < reader_.size()) {
      const auto element = read_element(reader_, position, reader_.size());
      if (element.id == kEbmlSegment)
        return element;
      if (element.unknown_size)
        throw std::runtime_error(
            "Unknown-size element appears before WebM Segment");
      position = element.end;
    }
    throw std::runtime_error("WebM contains no Segment element");
  }

  long long find_next_cluster(long long position) {
    while (position < segment_end_) {
      const auto element = read_element(reader_, position, segment_end_);
      if (element.id == kEbmlCluster)
        return element.start;
      if (element.unknown_size)
        throw std::runtime_error(
            "Unknown-size non-Cluster element appears in WebM Segment");
      position = element.end;
    }
    return -1;
  }

  FileMkvReader reader_;
  std::unique_ptr<mkvparser::Segment> segment_;
  long long segment_payload_start_{};
  long long segment_end_{};
  long long first_cluster_start_{-1};
  long long track_number_{};
  std::uint64_t timecode_scale_ns_{kDefaultTimecodeScaleNs};
  DemuxedVideoInfo info_;
};

} // namespace

std::unique_ptr<VideoDemux>
open_webm_video_demux(const std::filesystem::path &path) {
  return std::make_unique<WebmVideoDemux>(path);
}

} // namespace gdupe
