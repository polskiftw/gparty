#include "video_demux.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>
#include <webm/mkvparser/mkvparser.h>

namespace gdupe {
namespace {

constexpr std::size_t kMaxCompressedFrameBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxAnnexBBytes = 320ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCodecConfigBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kAv1CodecConfigurationRecordHeaderBytes = 4;
constexpr std::array<std::uint8_t, 4> kStartCode{0, 0, 0, 1};

std::int64_t ticks_to_ns(std::uint64_t ticks, unsigned timescale) {
  if (timescale == 0)
    return 0;
  const long double ns =
      static_cast<long double>(ticks) * 1'000'000'000.0L /
      static_cast<long double>(timescale);
  if (ns >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(ns);
}

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

std::vector<std::uint8_t>
webm_codec_header(const mkvparser::VideoTrack &track, std::string_view codec) {
  if (codec != "V_AV1")
    return {};

  std::size_t private_size = 0;
  const auto *codec_private = track.GetCodecPrivate(private_size);
  if (!codec_private || private_size == 0)
    return {};
  if (private_size < kAv1CodecConfigurationRecordHeaderBytes)
    throw std::runtime_error(
        "WebM AV1 CodecPrivate is shorter than an AV1CodecConfigurationRecord");

  const std::span<const std::uint8_t> private_bytes(
      reinterpret_cast<const std::uint8_t *>(codec_private), private_size);
  const auto marker_and_version = private_bytes.front();
  if ((marker_and_version & 0x80U) == 0 ||
      (marker_and_version & 0x7fU) != 1U)
    throw std::runtime_error(
        "WebM AV1 CodecPrivate has an unsupported configuration-record version");

  if (private_size == kAv1CodecConfigurationRecordHeaderBytes)
    return {};
  const auto config_obus =
      private_bytes.subspan(kAv1CodecConfigurationRecordHeaderBytes);
  return {config_obus.begin(), config_obus.end()};
}

class WebmVideoDemux final : public VideoDemux {
public:
  explicit WebmVideoDemux(const std::filesystem::path &path) : reader_(path) {
    long long position = 0;
    mkvparser::EBMLHeader header;
    if (header.Parse(&reader_, position) < 0)
      throw std::runtime_error("libwebm rejected the EBML header");

    mkvparser::Segment *raw_segment = nullptr;
    if (mkvparser::Segment::CreateInstance(&reader_, position, raw_segment) !=
            0 ||
        !raw_segment)
      throw std::runtime_error("libwebm could not create a WebM segment");
    segment_.reset(raw_segment);
    if (segment_->Load() < 0)
      throw std::runtime_error("libwebm could not parse the WebM segment");

    const auto *tracks = segment_->GetTracks();
    if (!tracks)
      throw std::runtime_error("WebM contains no track table");

    for (unsigned long index = 0; index < tracks->GetTracksCount(); ++index) {
      const auto *track = tracks->GetTrackByIndex(index);
      if (!track || track->GetType() != mkvparser::Track::kVideo)
        continue;
      const char *codec_id = track->GetCodecId();
      if (!codec_id)
        continue;
      const std::string_view candidate(codec_id);
      if (candidate != "V_VP8" && candidate != "V_VP9" &&
          candidate != "V_AV1")
        continue;
      video_track_ = static_cast<const mkvparser::VideoTrack *>(track);
      codec_id_ = std::string(candidate);
      break;
    }
    if (!video_track_)
      throw std::runtime_error(
          "WebM contains no supported VP8/VP9/AV1 video track");

    info_.codec = nvdec_codec_for_webm(codec_id_);
    info_.width = static_cast<int>(video_track_->GetWidth());
    info_.height = static_cast<int>(video_track_->GetHeight());
    info_.duration_ns =
        std::max<std::int64_t>(0, segment_->GetDuration());
    info_.codec_header = webm_codec_header(*video_track_, codec_id_);
  }

  [[nodiscard]] const DemuxedVideoInfo &info() const noexcept override {
    return info_;
  }

  bool visit_packets(const DemuxedVideoPacketCallback &callback) override {
    if (!callback)
      throw std::runtime_error("Video packet callback is empty");

    const mkvparser::BlockEntry *entry = nullptr;
    long status = video_track_->GetFirst(entry);
    if (status < 0)
      throw std::runtime_error("libwebm could not locate the first video block");

    while (entry && !entry->EOS()) {
      const auto *block = entry->GetBlock();
      if (!block)
        throw std::runtime_error("libwebm returned an empty video block");
      const std::int64_t timestamp_ns =
          std::max<std::int64_t>(0, block->GetTime(entry->GetCluster()));
      for (int frame_index = 0; frame_index < block->GetFrameCount();
           ++frame_index) {
        const auto &frame = block->GetFrame(frame_index);
        if (frame.len <= 0 ||
            static_cast<std::size_t>(frame.len) > kMaxCompressedFrameBytes)
          throw std::runtime_error(
              "WebM compressed frame exceeds gdupe's safety limit");
        DemuxedVideoPacket packet;
        packet.bytes.resize(static_cast<std::size_t>(frame.len));
        packet.timestamp_ns = timestamp_ns;
        if (frame.Read(&reader_, packet.bytes.data()) != 0)
          throw std::runtime_error(
              "libwebm could not read compressed video data");
        if (!callback(std::move(packet)))
          return false;
      }

      const mkvparser::BlockEntry *next = nullptr;
      status = video_track_->GetNext(entry, next);
      if (status < 0)
        throw std::runtime_error(
            "libwebm failed while advancing the video track");
      entry = next;
    }
    return true;
  }

private:
  FileMkvReader reader_;
  std::unique_ptr<mkvparser::Segment> segment_;
  const mkvparser::VideoTrack *video_track_{};
  std::string codec_id_;
  DemuxedVideoInfo info_;
};

class Mp4FileReader {
public:
  explicit Mp4FileReader(const std::filesystem::path &path)
      : stream_(path, std::ios::binary), size_(std::filesystem::file_size(path)) {
    if (!stream_)
      throw std::runtime_error("Could not open MP4/M4V file");
    if (size_ > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
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
    stream_.read(static_cast<char *>(buffer),
                 static_cast<std::streamsize>(bytes));
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

std::uint32_t read_be32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_be64(const std::uint8_t *data) {
  return (static_cast<std::uint64_t>(read_be32(data)) << 32U) |
         static_cast<std::uint64_t>(read_be32(data + 4));
}

std::uint16_t read_be16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>(
      (static_cast<unsigned>(data[0]) << 8U) | static_cast<unsigned>(data[1]));
}

struct IsoBox {
  std::uint64_t payload_start{};
  std::uint64_t end{};
  std::array<char, 4> type{};
};

bool box_is(const IsoBox &box, std::string_view type) {
  return type.size() == 4 &&
         std::equal(box.type.begin(), box.type.end(), type.begin());
}

IsoBox read_box(Mp4FileReader &reader, std::uint64_t position,
                std::uint64_t parent_end) {
  if (position > parent_end || parent_end - position < 8)
    throw std::runtime_error("Truncated ISO-BMFF box header");
  std::array<std::uint8_t, 16> header{};
  if (reader.read(static_cast<std::int64_t>(position), header.data(), 8) != 0)
    throw std::runtime_error("Could not read ISO-BMFF box header");

  const std::uint32_t size32 = read_be32(header.data());
  std::uint64_t box_size = size32;
  std::uint64_t header_size = 8;
  if (size32 == 1) {
    if (parent_end - position < 16 ||
        reader.read(static_cast<std::int64_t>(position + 8), header.data() + 8,
                    8) != 0)
      throw std::runtime_error("Truncated extended ISO-BMFF box header");
    box_size = read_be64(header.data() + 8);
    header_size = 16;
  } else if (size32 == 0) {
    box_size = parent_end - position;
  }
  if (box_size < header_size || box_size > parent_end - position)
    throw std::runtime_error("Invalid ISO-BMFF box size");

  IsoBox box;
  box.payload_start = position + header_size;
  box.end = position + box_size;
  std::copy_n(reinterpret_cast<const char *>(header.data() + 4), 4,
              box.type.begin());
  return box;
}

std::optional<IsoBox> find_child(Mp4FileReader &reader, std::uint64_t begin,
                                 std::uint64_t end, std::string_view type) {
  std::uint64_t position = begin;
  while (position < end) {
    if (end - position < 8)
      throw std::runtime_error("Trailing bytes in ISO-BMFF box hierarchy");
    const IsoBox box = read_box(reader, position, end);
    if (box_is(box, type))
      return box;
    position = box.end;
  }
  return std::nullopt;
}

std::vector<std::uint8_t> read_box_payload(Mp4FileReader &reader,
                                           const IsoBox &box) {
  const std::uint64_t bytes64 = box.end - box.payload_start;
  if (bytes64 == 0 || bytes64 > kMaxCodecConfigBytes)
    throw std::runtime_error("MP4 codec configuration exceeds safety limit");
  const auto bytes = static_cast<std::size_t>(bytes64);
  std::vector<std::uint8_t> payload(bytes);
  if (reader.read(static_cast<std::int64_t>(box.payload_start), payload.data(),
                  payload.size()) != 0)
    throw std::runtime_error("Could not read MP4 codec configuration");
  return payload;
}

void append_nal(std::vector<std::uint8_t> &output, const void *data,
                std::size_t size) {
  if (!data || size == 0)
    return;
  if (output.size() > kMaxAnnexBBytes - kStartCode.size() ||
      size > kMaxAnnexBBytes - output.size() - kStartCode.size())
    throw std::runtime_error(
        "MP4 Annex-B conversion exceeds gdupe's safety limit");
  output.insert(output.end(), kStartCode.begin(), kStartCode.end());
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  output.insert(output.end(), bytes, bytes + size);
}

struct Mp4CodecConfig {
  unsigned track_index{};
  NvdecCodec codec{NvdecCodec::h264};
  unsigned nal_length_size{};
  std::vector<std::uint8_t> parameter_sets;
};

Mp4CodecConfig parse_avcc(std::span<const std::uint8_t> config,
                          unsigned track_index) {
  if (config.size() < 7 || config[0] != 1)
    throw std::runtime_error("AVC MP4 has an invalid avcC configuration");
  Mp4CodecConfig result;
  result.track_index = track_index;
  result.codec = NvdecCodec::h264;
  result.nal_length_size = (config[4] & 3U) + 1U;

  std::size_t position = 5;
  const unsigned sps_count = config[position++] & 31U;
  if (sps_count == 0)
    throw std::runtime_error("AVC MP4 contains no SPS");
  for (unsigned index = 0; index < sps_count; ++index) {
    if (position + 2 > config.size())
      throw std::runtime_error("Truncated AVC SPS length");
    const std::size_t bytes = read_be16(config.data() + position);
    position += 2;
    if (bytes == 0 || bytes > config.size() - position)
      throw std::runtime_error("Invalid AVC SPS payload");
    append_nal(result.parameter_sets, config.data() + position, bytes);
    position += bytes;
  }

  if (position >= config.size())
    throw std::runtime_error("AVC MP4 configuration contains no PPS count");
  const unsigned pps_count = config[position++];
  if (pps_count == 0)
    throw std::runtime_error("AVC MP4 contains no PPS");
  for (unsigned index = 0; index < pps_count; ++index) {
    if (position + 2 > config.size())
      throw std::runtime_error("Truncated AVC PPS length");
    const std::size_t bytes = read_be16(config.data() + position);
    position += 2;
    if (bytes == 0 || bytes > config.size() - position)
      throw std::runtime_error("Invalid AVC PPS payload");
    append_nal(result.parameter_sets, config.data() + position, bytes);
    position += bytes;
  }
  return result;
}

Mp4CodecConfig parse_hvcc(std::span<const std::uint8_t> config,
                          unsigned track_index) {
  if (config.size() < 23 || config[0] != 1)
    throw std::runtime_error("HEVC MP4 has an invalid hvcC configuration");
  Mp4CodecConfig result;
  result.track_index = track_index;
  result.codec = NvdecCodec::hevc;
  result.nal_length_size = (config[21] & 3U) + 1U;

  bool saw_vps = false;
  bool saw_sps = false;
  bool saw_pps = false;
  std::size_t position = 22;
  const unsigned array_count = config[position++];
  for (unsigned array_index = 0; array_index < array_count; ++array_index) {
    if (position + 3 > config.size())
      throw std::runtime_error("Truncated HEVC hvcC array header");
    const unsigned nal_type = config[position++] & 0x3fU;
    const unsigned nal_count = read_be16(config.data() + position);
    position += 2;
    for (unsigned nal_index = 0; nal_index < nal_count; ++nal_index) {
      if (position + 2 > config.size())
        throw std::runtime_error("Truncated HEVC hvcC NAL length");
      const std::size_t bytes = read_be16(config.data() + position);
      position += 2;
      if (bytes == 0 || bytes > config.size() - position)
        throw std::runtime_error("Invalid HEVC hvcC NAL payload");
      append_nal(result.parameter_sets, config.data() + position, bytes);
      position += bytes;
      saw_vps = saw_vps || nal_type == 32;
      saw_sps = saw_sps || nal_type == 33;
      saw_pps = saw_pps || nal_type == 34;
    }
  }
  if (!saw_vps || !saw_sps || !saw_pps)
    throw std::runtime_error(
        "HEVC MP4 hvcC is missing VPS/SPS/PPS decoder configuration");
  return result;
}

bool handler_is_video(Mp4FileReader &reader, const IsoBox &mdia) {
  const auto hdlr = find_child(reader, mdia.payload_start, mdia.end, "hdlr");
  if (!hdlr || hdlr->end - hdlr->payload_start < 12)
    return false;
  std::array<std::uint8_t, 12> header{};
  if (reader.read(static_cast<std::int64_t>(hdlr->payload_start), header.data(),
                  header.size()) != 0)
    throw std::runtime_error("Could not read MP4 handler box");
  return header[8] == 'v' && header[9] == 'i' && header[10] == 'd' &&
         header[11] == 'e';
}

std::optional<Mp4CodecConfig>
parse_video_track_config(Mp4FileReader &reader, const IsoBox &trak,
                         unsigned track_index) {
  const auto mdia = find_child(reader, trak.payload_start, trak.end, "mdia");
  if (!mdia || !handler_is_video(reader, *mdia))
    return std::nullopt;
  const auto minf = find_child(reader, mdia->payload_start, mdia->end, "minf");
  if (!minf)
    return std::nullopt;
  const auto stbl = find_child(reader, minf->payload_start, minf->end, "stbl");
  if (!stbl)
    return std::nullopt;
  const auto stsd = find_child(reader, stbl->payload_start, stbl->end, "stsd");
  if (!stsd || stsd->end - stsd->payload_start < 8)
    return std::nullopt;

  std::array<std::uint8_t, 8> stsd_header{};
  if (reader.read(static_cast<std::int64_t>(stsd->payload_start),
                  stsd_header.data(), stsd_header.size()) != 0)
    throw std::runtime_error("Could not read MP4 sample description");
  const unsigned entry_count = read_be32(stsd_header.data() + 4);
  std::uint64_t position = stsd->payload_start + stsd_header.size();
  for (unsigned entry_index = 0;
       entry_index < entry_count && position < stsd->end; ++entry_index) {
    const IsoBox entry = read_box(reader, position, stsd->end);
    const bool avc = box_is(entry, "avc1") || box_is(entry, "avc3");
    const bool hevc = box_is(entry, "hvc1") || box_is(entry, "hev1");
    if (avc || hevc) {
      constexpr std::uint64_t kVisualSampleEntryBytes = 78;
      if (entry.end - entry.payload_start < kVisualSampleEntryBytes)
        throw std::runtime_error("Truncated MP4 visual sample entry");
      const std::uint64_t children =
          entry.payload_start + kVisualSampleEntryBytes;
      const auto codec_box =
          find_child(reader, children, entry.end, hevc ? "hvcC" : "avcC");
      if (!codec_box)
        throw std::runtime_error(
            hevc ? "HEVC MP4 sample entry has no hvcC configuration"
                 : "AVC MP4 sample entry has no avcC configuration");
      const auto payload = read_box_payload(reader, *codec_box);
      return hevc ? std::optional<Mp4CodecConfig>(
                        parse_hvcc(payload, track_index))
                  : std::optional<Mp4CodecConfig>(
                        parse_avcc(payload, track_index));
    }
    position = entry.end;
  }
  return std::nullopt;
}

Mp4CodecConfig read_mp4_codec_config(Mp4FileReader &reader) {
  const auto file_end = static_cast<std::uint64_t>(reader.size());
  std::optional<IsoBox> moov;
  std::uint64_t position = 0;
  while (position < file_end) {
    const IsoBox box = read_box(reader, position, file_end);
    if (box_is(box, "moov")) {
      moov = box;
      break;
    }
    position = box.end;
  }
  if (!moov)
    throw std::runtime_error("MP4/M4V contains no moov box");

  unsigned track_index = 0;
  position = moov->payload_start;
  while (position < moov->end) {
    const IsoBox child = read_box(reader, position, moov->end);
    if (box_is(child, "trak")) {
      if (auto config = parse_video_track_config(reader, child, track_index))
        return std::move(*config);
      ++track_index;
    }
    position = child.end;
  }
  throw std::runtime_error(
      "MP4/M4V contains no supported H.264/HEVC video track");
}

class Mp4DemuxHandle {
public:
  explicit Mp4DemuxHandle(Mp4FileReader &reader) {
    std::memset(&value_, 0, sizeof(value_));
    if (!MP4D_open(&value_, Mp4FileReader::callback, &reader, reader.size()))
      throw std::runtime_error("minimp4 rejected the MP4/M4V container");
  }

  ~Mp4DemuxHandle() { MP4D_close(&value_); }

  MP4D_demux_t &get() noexcept { return value_; }

private:
  MP4D_demux_t value_{};
};

std::uint32_t read_be_length(const std::uint8_t *data, unsigned bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < bytes; ++index)
    value = (value << 8U) | data[index];
  return value;
}

std::vector<std::uint8_t>
sample_to_annexb(std::span<const std::uint8_t> sample, unsigned length_size) {
  if (length_size < 1 || length_size > 4)
    throw std::runtime_error("MP4 uses an invalid NAL length field size");
  std::vector<std::uint8_t> output;
  output.reserve(std::min<std::size_t>(sample.size() + 64, kMaxAnnexBBytes));
  std::size_t position = 0;
  while (position < sample.size()) {
    if (sample.size() - position < length_size)
      throw std::runtime_error("Truncated MP4 NAL length field");
    const std::uint32_t bytes =
        read_be_length(sample.data() + position, length_size);
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

std::uint64_t track_duration_ticks(const MP4D_track_t &track) {
  return (static_cast<std::uint64_t>(track.duration_hi) << 32U) |
         static_cast<std::uint64_t>(track.duration_lo);
}

class Mp4VideoDemux final : public VideoDemux {
public:
  explicit Mp4VideoDemux(const std::filesystem::path &path)
      : reader_(path), codec_(read_mp4_codec_config(reader_)), demux_(reader_) {
    auto &mp4 = demux_.get();
    if (codec_.track_index >= mp4.track_count)
      throw std::runtime_error(
          "MP4 codec track does not match minimp4's sample-table index");
    track_ = &mp4.track[codec_.track_index];
    if (track_->sample_count == 0)
      throw std::runtime_error("MP4 video track contains no samples");
    if (track_->timescale == 0)
      throw std::runtime_error("MP4 video track has an invalid zero timescale");

    info_.codec = codec_.codec;
    info_.duration_ns =
        ticks_to_ns(track_duration_ticks(*track_), track_->timescale);
    info_.frame_count = static_cast<std::int64_t>(track_->sample_count);
    info_.codec_header = codec_.parameter_sets;
  }

  [[nodiscard]] const DemuxedVideoInfo &info() const noexcept override {
    return info_;
  }

  bool visit_packets(const DemuxedVideoPacketCallback &callback) override {
    if (!callback)
      throw std::runtime_error("Video packet callback is empty");

    auto &mp4 = demux_.get();
    const std::uint64_t reader_size = static_cast<std::uint64_t>(reader_.size());
    std::vector<std::uint8_t> compressed;
    for (unsigned sample_index = 0; sample_index < track_->sample_count;
         ++sample_index) {
      unsigned frame_bytes = 0;
      unsigned timestamp = 0;
      unsigned frame_duration = 0;
      const MP4D_file_offset_t offset = MP4D_frame_offset(
          &mp4, codec_.track_index, sample_index, &frame_bytes, &timestamp,
          &frame_duration);
      const std::uint64_t offset64 = static_cast<std::uint64_t>(offset);
      if (frame_bytes == 0 || frame_bytes > kMaxCompressedFrameBytes)
        throw std::runtime_error(
            "MP4 compressed sample exceeds gdupe's safety limit");
      if (offset64 > reader_size ||
          static_cast<std::uint64_t>(frame_bytes) > reader_size - offset64)
        throw std::runtime_error("minimp4 returned an invalid sample offset");

      compressed.resize(frame_bytes);
      if (reader_.read(static_cast<std::int64_t>(offset64), compressed.data(),
                       compressed.size()) != 0)
        throw std::runtime_error("Could not read an MP4 compressed sample");

      DemuxedVideoPacket packet;
      packet.bytes = sample_to_annexb(compressed, codec_.nal_length_size);
      packet.timestamp_ns = ticks_to_ns(timestamp, track_->timescale);
      if (!callback(std::move(packet)))
        return false;
    }
    return true;
  }

private:
  Mp4FileReader reader_;
  Mp4CodecConfig codec_;
  Mp4DemuxHandle demux_;
  const MP4D_track_t *track_{};
  DemuxedVideoInfo info_;
};

} // namespace

std::unique_ptr<VideoDemux>
open_video_demux(const std::filesystem::path &path, std::string_view extension) {
  if (extension == "webm")
    return std::make_unique<WebmVideoDemux>(path);
  if (extension == "mp4" || extension == "m4v")
    return std::make_unique<Mp4VideoDemux>(path);
  throw std::runtime_error("Unsupported moving-media extension: " +
                           std::string(extension));
}

} // namespace gdupe
