#include "annexb_decoder.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <stdexcept>
#include <string>

extern "C" {
#include "ihevcd_cxa.h"
}

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxFramePixels = 100'000'000ULL;

void *aosp_hevc_aligned_alloc(void *, WORD32 alignment, WORD32 size) {
  if (alignment <= 0 || size <= 0)
    return nullptr;
  return _aligned_malloc(static_cast<std::size_t>(size),
                         static_cast<std::size_t>(alignment));
}

void aosp_hevc_aligned_free(void *, void *pointer) {
  _aligned_free(pointer);
}

class HevcAnnexBDecoder final : public AnnexBDecoder {
public:
  HevcAnnexBDecoder() { create(); }

  ~HevcAnnexBDecoder() override {
    free_output();
    if (codec_) {
      ivd_delete_ip_t input{};
      ivd_delete_op_t output{};
      input.e_cmd = IVD_CMD_DELETE;
      input.u4_size = sizeof(input);
      output.u4_size = sizeof(output);
      (void)ihevcd_cxa_api_function(codec_, &input, &output);
    }
  }

  void initialize(std::span<const std::uint8_t> header) override {
    if (header.empty())
      throw std::runtime_error("HEVC MP4 contains no decoder parameter sets");

    set_mode(IVD_DECODE_HEADER);
    std::size_t offset = 0;
    while (offset < header.size() && (width_ <= 0 || height_ <= 0)) {
      ivd_video_decode_ip_t input{};
      ivd_video_decode_op_t output{};
      input.e_cmd = IVD_CMD_VIDEO_DECODE;
      input.u4_ts = 0;
      input.pv_stream_buffer = const_cast<std::uint8_t *>(header.data() + offset);
      input.u4_num_Bytes = static_cast<UWORD32>(
          std::min<std::size_t>(header.size() - offset, UINT32_MAX));
      input.u4_size = sizeof(input);
      output.u4_size = sizeof(output);

      const auto status = ihevcd_cxa_api_function(codec_, &input, &output);
      const std::size_t consumed = std::min<std::size_t>(
          output.u4_num_bytes_consumed, header.size() - offset);
      if (output.u4_pic_wd > 0 && output.u4_pic_ht > 0) {
        width_ = static_cast<int>(output.u4_pic_wd);
        height_ = static_cast<int>(output.u4_pic_ht);
      }
      if (consumed == 0) {
        if (width_ > 0 && height_ > 0)
          break;
        throw std::runtime_error(
            "AOSP libhevc made no progress while parsing HEVC parameter sets (status=" +
            std::to_string(static_cast<int>(status)) + ", error=" +
            std::to_string(output.u4_error_code) + ")");
      }
      offset += consumed;
    }

    validate_dimensions();
    allocate_output();
    set_mode(IVD_DECODE_FRAME);
  }

  void decode(std::span<const std::uint8_t> access_unit,
              std::uint32_t timestamp_token,
              const AnnexBFrameCallback &callback) override {
    if (access_unit.empty())
      return;
    std::size_t offset = 0;
    while (offset < access_unit.size()) {
      ivd_video_decode_ip_t input{};
      ivd_video_decode_op_t output{};
      input.e_cmd = IVD_CMD_VIDEO_DECODE;
      input.u4_ts = timestamp_token;
      input.pv_stream_buffer =
          const_cast<std::uint8_t *>(access_unit.data() + offset);
      input.u4_num_Bytes = static_cast<UWORD32>(
          std::min<std::size_t>(access_unit.size() - offset, UINT32_MAX));
      input.u4_size = sizeof(input);
      input.s_out_buffer = out_;
      output.u4_size = sizeof(output);

      const auto status = ihevcd_cxa_api_function(codec_, &input, &output);
      if ((output.u4_error_code & 0xFFU) == IVD_RES_CHANGED)
        throw std::runtime_error(
            "HEVC stream changes resolution mid-file; static decoder needs a fresh sequence");

      if (output.u4_pic_wd > 0 && output.u4_pic_ht > 0 &&
          (static_cast<int>(output.u4_pic_wd) != width_ ||
           static_cast<int>(output.u4_pic_ht) != height_))
        throw std::runtime_error(
            "HEVC stream changed display dimensions mid-file");

      if (output.u4_output_present)
        emit_frame(output.u4_ts, callback);

      const std::size_t consumed = std::min<std::size_t>(
          output.u4_num_bytes_consumed, access_unit.size() - offset);
      if (consumed == 0) {
        if (output.u4_output_present)
          continue;
        throw std::runtime_error(
            "AOSP libhevc made no progress decoding a HEVC access unit (status=" +
            std::to_string(static_cast<int>(status)) + ", error=" +
            std::to_string(output.u4_error_code) + ")");
      }
      offset += consumed;
    }
  }

  void flush(const AnnexBFrameCallback &callback) override {
    ivd_ctl_flush_ip_t flush_input{};
    ivd_ctl_flush_op_t flush_output{};
    flush_input.e_cmd = IVD_CMD_VIDEO_CTL;
    flush_input.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    flush_input.u4_size = sizeof(flush_input);
    flush_output.u4_size = sizeof(flush_output);
    (void)ihevcd_cxa_api_function(codec_, &flush_input, &flush_output);

    for (int attempt = 0; attempt < 64; ++attempt) {
      ivd_video_decode_ip_t input{};
      ivd_video_decode_op_t output{};
      input.e_cmd = IVD_CMD_VIDEO_DECODE;
      input.u4_ts = 0;
      input.pv_stream_buffer = nullptr;
      input.u4_num_Bytes = 0;
      input.u4_size = sizeof(input);
      input.s_out_buffer = out_;
      output.u4_size = sizeof(output);
      const auto status = ihevcd_cxa_api_function(codec_, &input, &output);
      if (output.u4_output_present) {
        emit_frame(output.u4_ts, callback);
        continue;
      }
      if (status != IV_SUCCESS || output.u4_num_bytes_consumed == 0)
        break;
    }
  }

  [[nodiscard]] int width() const noexcept override { return width_; }
  [[nodiscard]] int height() const noexcept override { return height_; }

private:
  void create() {
    ihevcd_cxa_create_ip_t input{};
    ihevcd_cxa_create_op_t output{};
    input.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    input.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    input.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    input.s_ivd_create_ip_t.pf_aligned_alloc = aosp_hevc_aligned_alloc;
    input.s_ivd_create_ip_t.pf_aligned_free = aosp_hevc_aligned_free;
    input.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    input.s_ivd_create_ip_t.u4_size = sizeof(input);
    input.u4_enable_frame_info = 0;
    input.u4_keep_threads_active = 0;
    output.s_ivd_create_op_t.u4_size = sizeof(output);

    if (ihevcd_cxa_api_function(nullptr, &input, &output) != IV_SUCCESS ||
        !output.s_ivd_create_op_t.pv_handle)
      throw std::runtime_error("Could not create static AOSP HEVC decoder");
    codec_ = static_cast<iv_obj_t *>(output.s_ivd_create_op_t.pv_handle);
    codec_->pv_fxns = reinterpret_cast<void *>(&ihevcd_cxa_api_function);
    codec_->u4_size = sizeof(iv_obj_t);
  }

  void set_mode(IVD_VIDEO_DECODE_MODE_T mode) {
    ivd_ctl_set_config_ip_t input{};
    ivd_ctl_set_config_op_t output{};
    input.u4_disp_wd = 0;
    input.e_frm_skip_mode = IVD_SKIP_NONE;
    input.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    input.e_vid_dec_mode = mode;
    input.e_cmd = IVD_CMD_VIDEO_CTL;
    input.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    input.u4_size = sizeof(input);
    output.u4_size = sizeof(output);
    if (ihevcd_cxa_api_function(codec_, &input, &output) != IV_SUCCESS)
      throw std::runtime_error("Could not configure static AOSP HEVC decoder");
  }

  void validate_dimensions() const {
    if (width_ <= 0 || height_ <= 0)
      throw std::runtime_error("AOSP libhevc did not discover HEVC dimensions");
    const auto pixels = static_cast<std::uint64_t>(width_) *
                        static_cast<std::uint64_t>(height_);
    if (pixels > kMaxFramePixels)
      throw std::runtime_error("HEVC frame exceeds gdupe's safety limit");
  }

  void allocate_output() {
    free_output();
    validate_dimensions();
    const std::size_t y = static_cast<std::size_t>(width_) * height_;
    const std::size_t chroma_width = (static_cast<std::size_t>(width_) + 1) / 2;
    const std::size_t chroma_height = (static_cast<std::size_t>(height_) + 1) / 2;
    const std::size_t c = chroma_width * chroma_height;
    const std::size_t sizes[3]{y, c, c};
    out_.u4_num_bufs = 3;
    for (int index = 0; index < 3; ++index) {
      if (sizes[index] > UINT32_MAX)
        throw std::runtime_error("HEVC output buffer exceeds decoder limits");
      out_.u4_min_out_buf_size[index] = static_cast<UWORD32>(sizes[index]);
      out_.pu1_bufs[index] = static_cast<UWORD8 *>(
          _aligned_malloc(sizes[index], 16));
      if (!out_.pu1_bufs[index]) {
        free_output();
        throw std::bad_alloc();
      }
      std::memset(out_.pu1_bufs[index], 0, sizes[index]);
    }
  }

  void free_output() noexcept {
    for (auto &pointer : out_.pu1_bufs) {
      if (pointer)
        _aligned_free(pointer);
      pointer = nullptr;
    }
    out_.u4_num_bufs = 0;
  }

  void emit_frame(std::uint32_t token,
                  const AnnexBFrameCallback &callback) const {
    const std::size_t size = static_cast<std::size_t>(width_) * height_;
    if (!out_.pu1_bufs[0] || out_.u4_min_out_buf_size[0] < size)
      throw std::runtime_error("AOSP libhevc returned an invalid luma buffer");
    AnnexBGrayFrame frame;
    frame.width = width_;
    frame.height = height_;
    frame.timestamp_token = token;
    frame.pixels.assign(out_.pu1_bufs[0], out_.pu1_bufs[0] + size);
    callback(std::move(frame));
  }

  iv_obj_t *codec_{};
  int width_{};
  int height_{};
  ivd_out_bufdesc_t out_{};
};

} // namespace

std::unique_ptr<AnnexBDecoder> make_hevc_annexb_decoder() {
  return std::make_unique<HevcAnnexBDecoder>();
}

} // namespace gdupe
