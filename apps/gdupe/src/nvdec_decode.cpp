#include "nvdec_decode.hpp"
#include "preview_color.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

extern "C" {
#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_nvcuvid.h>
}

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxFramePixels = 100'000'000ULL;
constexpr int kPreviewMaxWidth = 1920;
constexpr int kPreviewMaxHeight = 1080;

const char *codec_name(NvdecCodec codec) noexcept {
  switch (codec) {
  case NvdecCodec::h264:
    return "H.264";
  case NvdecCodec::hevc:
    return "HEVC";
  case NvdecCodec::vp8:
    return "VP8";
  case NvdecCodec::vp9:
    return "VP9";
  case NvdecCodec::av1:
    return "AV1";
  }
  return "video";
}

cudaVideoCodec cuda_codec(NvdecCodec codec) {
  switch (codec) {
  case NvdecCodec::h264:
    return cudaVideoCodec_H264;
  case NvdecCodec::hevc:
    return cudaVideoCodec_HEVC;
  case NvdecCodec::vp8:
    return cudaVideoCodec_VP8;
  case NvdecCodec::vp9:
    return cudaVideoCodec_VP9;
  case NvdecCodec::av1:
    return cudaVideoCodec_AV1;
  }
  throw std::runtime_error("Unknown NVDEC codec");
}

template <typename Function>
Function load_required(HMODULE module, const char *name) {
  const auto address = GetProcAddress(module, name);
  if (!address)
    throw std::runtime_error(std::string("NVIDIA driver is missing required symbol ") +
                             name);
  return reinterpret_cast<Function>(address);
}

template <typename Function>
Function load_optional(HMODULE module, const char *name) noexcept {
  return reinterpret_cast<Function>(GetProcAddress(module, name));
}

class DriverApi {
public:
  DriverApi() {
    cuda_module_ = LoadLibraryW(L"nvcuda.dll");
    if (!cuda_module_)
      throw std::runtime_error(
          "NVIDIA CUDA driver runtime (nvcuda.dll) is not available");
    try {
      cuvid_module_ = LoadLibraryW(L"nvcuvid.dll");
      if (!cuvid_module_)
        throw std::runtime_error(
            "NVIDIA NVDEC driver runtime (nvcuvid.dll) is not available");

      cuInit = load_required<tcuInit *>(cuda_module_, "cuInit");
      cuDeviceGetCount =
          load_required<tcuDeviceGetCount *>(cuda_module_, "cuDeviceGetCount");
      cuDeviceGet = load_required<tcuDeviceGet *>(cuda_module_, "cuDeviceGet");
      cuCtxCreate =
          load_required<tcuCtxCreate_v2 *>(cuda_module_, "cuCtxCreate_v2");
      cuCtxPushCurrent = load_required<tcuCtxPushCurrent_v2 *>(
          cuda_module_, "cuCtxPushCurrent_v2");
      cuCtxPopCurrent = load_required<tcuCtxPopCurrent_v2 *>(
          cuda_module_, "cuCtxPopCurrent_v2");
      cuCtxDestroy =
          load_required<tcuCtxDestroy_v2 *>(cuda_module_, "cuCtxDestroy_v2");
      cuMemcpy2D =
          load_required<tcuMemcpy2D_v2 *>(cuda_module_, "cuMemcpy2D_v2");
      cuGetErrorName =
          load_optional<tcuGetErrorName *>(cuda_module_, "cuGetErrorName");
      cuGetErrorString =
          load_optional<tcuGetErrorString *>(cuda_module_, "cuGetErrorString");

      cuvidGetDecoderCaps = load_required<tcuvidGetDecoderCaps *>(
          cuvid_module_, "cuvidGetDecoderCaps");
      cuvidCreateDecoder = load_required<tcuvidCreateDecoder *>(
          cuvid_module_, "cuvidCreateDecoder");
      cuvidDestroyDecoder = load_required<tcuvidDestroyDecoder *>(
          cuvid_module_, "cuvidDestroyDecoder");
      cuvidDecodePicture = load_required<tcuvidDecodePicture *>(
          cuvid_module_, "cuvidDecodePicture");
      cuvidGetDecodeStatus = load_optional<tcuvidGetDecodeStatus *>(
          cuvid_module_, "cuvidGetDecodeStatus");
#if defined(__CUVID_DEVPTR64)
      cuvidMapVideoFrame = load_required<tcuvidMapVideoFrame *>(
          cuvid_module_, "cuvidMapVideoFrame64");
      cuvidUnmapVideoFrame = load_required<tcuvidUnmapVideoFrame *>(
          cuvid_module_, "cuvidUnmapVideoFrame64");
#else
      cuvidMapVideoFrame = load_required<tcuvidMapVideoFrame *>(
          cuvid_module_, "cuvidMapVideoFrame");
      cuvidUnmapVideoFrame = load_required<tcuvidUnmapVideoFrame *>(
          cuvid_module_, "cuvidUnmapVideoFrame");
#endif
      cuvidCreateVideoParser = load_required<tcuvidCreateVideoParser *>(
          cuvid_module_, "cuvidCreateVideoParser");
      cuvidParseVideoData = load_required<tcuvidParseVideoData *>(
          cuvid_module_, "cuvidParseVideoData");
      cuvidDestroyVideoParser = load_required<tcuvidDestroyVideoParser *>(
          cuvid_module_, "cuvidDestroyVideoParser");
    } catch (...) {
      reset();
      throw;
    }
  }

  DriverApi(const DriverApi &) = delete;
  DriverApi &operator=(const DriverApi &) = delete;
  ~DriverApi() { reset(); }

  std::string error(CUresult result, const char *operation) const {
    std::string message(operation);
    message += " failed";
    const char *name = nullptr;
    const char *description = nullptr;
    if (cuGetErrorName && cuGetErrorName(result, &name) == CUDA_SUCCESS && name) {
      message += " (";
      message += name;
      message += ')';
    } else {
      message += " (CUDA status=";
      message += std::to_string(static_cast<int>(result));
      message += ')';
    }
    if (cuGetErrorString &&
        cuGetErrorString(result, &description) == CUDA_SUCCESS && description) {
      message += ": ";
      message += description;
    }
    return message;
  }

  tcuInit *cuInit{};
  tcuDeviceGetCount *cuDeviceGetCount{};
  tcuDeviceGet *cuDeviceGet{};
  tcuCtxCreate_v2 *cuCtxCreate{};
  tcuCtxPushCurrent_v2 *cuCtxPushCurrent{};
  tcuCtxPopCurrent_v2 *cuCtxPopCurrent{};
  tcuCtxDestroy_v2 *cuCtxDestroy{};
  tcuMemcpy2D_v2 *cuMemcpy2D{};
  tcuGetErrorName *cuGetErrorName{};
  tcuGetErrorString *cuGetErrorString{};

  tcuvidGetDecoderCaps *cuvidGetDecoderCaps{};
  tcuvidCreateDecoder *cuvidCreateDecoder{};
  tcuvidDestroyDecoder *cuvidDestroyDecoder{};
  tcuvidDecodePicture *cuvidDecodePicture{};
  tcuvidGetDecodeStatus *cuvidGetDecodeStatus{};
  tcuvidMapVideoFrame *cuvidMapVideoFrame{};
  tcuvidUnmapVideoFrame *cuvidUnmapVideoFrame{};
  tcuvidCreateVideoParser *cuvidCreateVideoParser{};
  tcuvidParseVideoData *cuvidParseVideoData{};
  tcuvidDestroyVideoParser *cuvidDestroyVideoParser{};

private:
  void reset() noexcept {
    if (cuvid_module_) {
      FreeLibrary(cuvid_module_);
      cuvid_module_ = nullptr;
    }
    if (cuda_module_) {
      FreeLibrary(cuda_module_);
      cuda_module_ = nullptr;
    }
  }

  HMODULE cuda_module_{};
  HMODULE cuvid_module_{};
};

void require_cuda(const DriverApi &api, CUresult result, const char *operation) {
  if (result != CUDA_SUCCESS)
    throw std::runtime_error(api.error(result, operation));
}

class ContextScope {
public:
  ContextScope(DriverApi &api, CUcontext context) : api_(api) {
    require_cuda(api_, api_.cuCtxPushCurrent(context), "cuCtxPushCurrent");
    active_ = true;
  }
  ContextScope(const ContextScope &) = delete;
  ContextScope &operator=(const ContextScope &) = delete;
  ~ContextScope() {
    if (active_) {
      CUcontext popped{};
      api_.cuCtxPopCurrent(&popped);
    }
  }

private:
  DriverApi &api_;
  bool active_{};
};

void validate_dimensions(int width, int height) {
  if (width <= 0 || height <= 0)
    throw std::runtime_error("NVDEC returned invalid frame dimensions");
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > kMaxFramePixels)
    throw std::runtime_error("NVDEC frame exceeds gdupe's safety limit");
}

cudaVideoSurfaceFormat choose_output_format(cudaVideoChromaFormat chroma,
                                             unsigned bit_depth) {
  const bool high = bit_depth > 8;
  switch (chroma) {
  case cudaVideoChromaFormat_Monochrome:
  case cudaVideoChromaFormat_420:
    return high ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
  case cudaVideoChromaFormat_422:
    return high ? cudaVideoSurfaceFormat_P216 : cudaVideoSurfaceFormat_NV16;
  case cudaVideoChromaFormat_444:
    return high ? cudaVideoSurfaceFormat_YUV444_16Bit
                : cudaVideoSurfaceFormat_YUV444;
  }
  throw std::runtime_error("NVDEC returned an unsupported chroma format");
}

std::vector<std::uint8_t>
normalize_luma(std::span<const std::uint16_t> samples, unsigned bit_depth) {
  if (bit_depth <= 8 || bit_depth > 16)
    throw std::runtime_error("NVDEC returned an invalid high bit depth");
  const unsigned shift = 16U - bit_depth;
  const std::uint32_t maximum = (std::uint32_t{1} << bit_depth) - 1U;
  std::vector<std::uint8_t> output(samples.size());
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const std::uint32_t value =
        std::min<std::uint32_t>(samples[index] >> shift, maximum);
    output[index] = static_cast<std::uint8_t>(
        (value * 255U + maximum / 2U) / maximum);
  }
  return output;
}

PreviewColorMatrix color_matrix(unsigned char matrix_coefficients) noexcept {
  switch (matrix_coefficients) {
  case 5:
  case 6:
    return PreviewColorMatrix::bt601;
  case 9:
  case 10:
    return PreviewColorMatrix::bt2020;
  default:
    return PreviewColorMatrix::bt709;
  }
}

std::pair<int, int> preview_target(int width, int height) {
  validate_dimensions(width, height);
  const double scale = std::min(
      {1.0, static_cast<double>(kPreviewMaxWidth) / width,
       static_cast<double>(kPreviewMaxHeight) / height});
  if (scale >= 1.0)
    return {width, height};
  int target_width = std::max(2, static_cast<int>(std::floor(width * scale)));
  int target_height = std::max(2, static_cast<int>(std::floor(height * scale)));
  target_width -= target_width & 1;
  target_height -= target_height & 1;
  return {std::max(2, target_width), std::max(2, target_height)};
}

} // namespace

class NvdecPacketDecoder::Impl {
public:
  Impl(NvdecCodec codec, NvdecOutput output)
      : codec_(codec), output_(output), api_(std::make_unique<DriverApi>()) {
    require_cuda(*api_, api_->cuInit(0), "cuInit");
    int device_count = 0;
    require_cuda(*api_, api_->cuDeviceGetCount(&device_count), "cuDeviceGetCount");
    if (device_count <= 0)
      throw std::runtime_error("No NVIDIA CUDA device is available for NVDEC");
    CUdevice device{};
    require_cuda(*api_, api_->cuDeviceGet(&device, 0), "cuDeviceGet");
    require_cuda(*api_, api_->cuCtxCreate(&context_, CU_CTX_SCHED_BLOCKING_SYNC,
                                         device),
                 "cuCtxCreate");
    context_created_ = true;

    try {
      CUVIDPARSERPARAMS parser_params{};
      parser_params.CodecType = cuda_codec(codec_);
      parser_params.ulMaxNumDecodeSurfaces = 1;
      parser_params.ulClockRate = 1'000'000'000U;
      parser_params.ulErrorThreshold = 0;
      parser_params.ulMaxDisplayDelay = 0;
      parser_params.pUserData = this;
      parser_params.pfnSequenceCallback = &sequence_thunk;
      parser_params.pfnDecodePicture = &decode_thunk;
      parser_params.pfnDisplayPicture = &display_thunk;
      require_cuda(*api_,
                   api_->cuvidCreateVideoParser(&parser_, &parser_params),
                   "cuvidCreateVideoParser");
      parser_created_ = true;

      CUcontext popped{};
      require_cuda(*api_, api_->cuCtxPopCurrent(&popped), "cuCtxPopCurrent");
    } catch (...) {
      cleanup_current_context();
      throw;
    }
  }

  ~Impl() { cleanup(); }

  void feed_header(std::span<const std::uint8_t> bytes) {
    if (bytes.empty())
      return;
    parse(bytes, 0, 0, nullptr, nullptr);
  }

  void decode(std::span<const std::uint8_t> bytes, std::int64_t timestamp,
              const NvdecFrameCallback &callback) {
    if (output_ != NvdecOutput::fingerprint)
      throw std::runtime_error("Fingerprint callback used with NVDEC preview decoder");
    if (bytes.empty())
      throw std::runtime_error(std::string(codec_name(codec_)) +
                               " compressed packet is empty");
    parse(bytes, timestamp, CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE,
          &callback, nullptr);
  }

  void flush(const NvdecFrameCallback &callback) {
    if (output_ != NvdecOutput::fingerprint)
      throw std::runtime_error("Fingerprint flush used with NVDEC preview decoder");
    parse({}, 0, CUVID_PKT_ENDOFSTREAM, &callback, nullptr);
  }

  void decode_bgra(std::span<const std::uint8_t> bytes,
                   std::int64_t timestamp,
                   const NvdecBgraFrameCallback &callback) {
    if (output_ != NvdecOutput::preview)
      throw std::runtime_error("Preview callback used with NVDEC fingerprint decoder");
    if (bytes.empty())
      throw std::runtime_error(std::string(codec_name(codec_)) +
                               " compressed packet is empty");
    parse(bytes, timestamp, CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE,
          nullptr, &callback);
  }

  void flush_bgra(const NvdecBgraFrameCallback &callback) {
    if (output_ != NvdecOutput::preview)
      throw std::runtime_error("Preview flush used with NVDEC fingerprint decoder");
    parse({}, 0, CUVID_PKT_ENDOFSTREAM, nullptr, &callback);
  }

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

private:
  void parse(std::span<const std::uint8_t> bytes, std::int64_t timestamp,
             unsigned long flags, const NvdecFrameCallback *gray_callback,
             const NvdecBgraFrameCallback *bgra_callback) {
    ContextScope scope(*api_, context_);
    pending_error_ = nullptr;
    active_gray_callback_ = gray_callback;
    active_bgra_callback_ = bgra_callback;

    CUVIDSOURCEDATAPACKET packet{};
    packet.flags = flags;
    packet.payload_size = static_cast<tcu_ulong>(bytes.size());
    packet.payload = bytes.empty() ? nullptr : bytes.data();
    packet.timestamp = timestamp;

    const CUresult result = api_->cuvidParseVideoData(parser_, &packet);
    active_gray_callback_ = nullptr;
    active_bgra_callback_ = nullptr;
    if (pending_error_)
      std::rethrow_exception(std::exchange(pending_error_, nullptr));
    require_cuda(*api_, result, "cuvidParseVideoData");
  }

  static int CUDAAPI sequence_thunk(void *opaque, CUVIDEOFORMAT *format) noexcept {
    auto *self = static_cast<Impl *>(opaque);
    try {
      return self->on_sequence(*format);
    } catch (...) {
      self->pending_error_ = std::current_exception();
      return 0;
    }
  }

  static int CUDAAPI decode_thunk(void *opaque, CUVIDPICPARAMS *picture) noexcept {
    auto *self = static_cast<Impl *>(opaque);
    try {
      self->on_decode(*picture);
      return 1;
    } catch (...) {
      self->pending_error_ = std::current_exception();
      return 0;
    }
  }

  static int CUDAAPI display_thunk(void *opaque,
                                   CUVIDPARSERDISPINFO *display) noexcept {
    auto *self = static_cast<Impl *>(opaque);
    try {
      self->on_display(*display);
      return 1;
    } catch (...) {
      self->pending_error_ = std::current_exception();
      return 0;
    }
  }

  int on_sequence(const CUVIDEOFORMAT &format) {
    const unsigned bit_depth =
        static_cast<unsigned>(format.bit_depth_luma_minus8) + 8U;
    if (bit_depth < 8 || bit_depth > 16)
      throw std::runtime_error("NVDEC reported an unsupported bit depth");

    const int display_width =
        format.display_area.right > format.display_area.left
            ? format.display_area.right - format.display_area.left
            : static_cast<int>(format.coded_width);
    const int display_height =
        format.display_area.bottom > format.display_area.top
            ? format.display_area.bottom - format.display_area.top
            : static_cast<int>(format.coded_height);
    validate_dimensions(display_width, display_height);

    if (decoder_created_) {
      if (format.coded_width != coded_width_ ||
          format.coded_height != coded_height_ || bit_depth != bit_depth_ ||
          format.chroma_format != chroma_format_) {
        throw std::runtime_error(
            "NVDEC sequence changes that alter resolution, bit depth, or chroma are not supported");
      }
      return std::max<int>(1, format.min_num_decode_surfaces);
    }

    if (output_ == NvdecOutput::preview &&
        format.chroma_format != cudaVideoChromaFormat_420 &&
        format.chroma_format != cudaVideoChromaFormat_Monochrome) {
      throw std::runtime_error(
          "NVDEC preview currently supports 4:2:0 or monochrome video surfaces");
    }

    CUVIDDECODECAPS caps{};
    caps.eCodecType = format.codec;
    caps.eChromaFormat = format.chroma_format;
    caps.nBitDepthMinus8 = format.bit_depth_luma_minus8;
    require_cuda(*api_, api_->cuvidGetDecoderCaps(&caps),
                 "cuvidGetDecoderCaps");
    if (!caps.bIsSupported) {
      throw std::runtime_error(std::string("NVIDIA GPU does not support ") +
                               codec_name(codec_) + " " +
                               std::to_string(bit_depth) + "-bit decode");
    }
    if (format.coded_width < caps.nMinWidth ||
        format.coded_height < caps.nMinHeight ||
        format.coded_width > caps.nMaxWidth ||
        format.coded_height > caps.nMaxHeight)
      throw std::runtime_error("Video dimensions exceed NVIDIA NVDEC capability");

    const std::uint64_t macroblocks =
        (static_cast<std::uint64_t>(format.coded_width) *
             static_cast<std::uint64_t>(format.coded_height) +
         255ULL) /
        256ULL;
    if (caps.nMaxMBCount != 0 && macroblocks > caps.nMaxMBCount)
      throw std::runtime_error("Video macroblock count exceeds NVIDIA NVDEC capability");

    const auto output_format = choose_output_format(format.chroma_format, bit_depth);
    if ((caps.nOutputFormatMask & (1U << static_cast<unsigned>(output_format))) == 0)
      throw std::runtime_error("NVIDIA NVDEC cannot expose the required surface format");

    int target_width = display_width;
    int target_height = display_height;
    if (output_ == NvdecOutput::preview) {
      const auto target = preview_target(display_width, display_height);
      target_width = target.first;
      target_height = target.second;
    }
    validate_dimensions(target_width, target_height);

    CUVIDDECODECREATEINFO create{};
    create.ulWidth = format.coded_width;
    create.ulHeight = format.coded_height;
    create.ulNumDecodeSurfaces =
        std::max<unsigned>(1U, format.min_num_decode_surfaces);
    create.CodecType = format.codec;
    create.ChromaFormat = format.chroma_format;
    create.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    create.bitDepthMinus8 = format.bit_depth_luma_minus8;
    create.ulMaxWidth = format.coded_width;
    create.ulMaxHeight = format.coded_height;
    create.display_area.left = static_cast<short>(format.display_area.left);
    create.display_area.top = static_cast<short>(format.display_area.top);
    create.display_area.right = static_cast<short>(format.display_area.right);
    create.display_area.bottom = static_cast<short>(format.display_area.bottom);
    create.OutputFormat = output_format;
    create.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    create.ulTargetWidth = static_cast<tcu_ulong>(target_width);
    create.ulTargetHeight = static_cast<tcu_ulong>(target_height);
    create.ulNumOutputSurfaces = 2;
    create.target_rect.left = 0;
    create.target_rect.top = 0;
    create.target_rect.right = static_cast<short>(target_width);
    create.target_rect.bottom = static_cast<short>(target_height);

    require_cuda(*api_, api_->cuvidCreateDecoder(&decoder_, &create),
                 "cuvidCreateDecoder");
    decoder_created_ = true;
    width_ = target_width;
    height_ = target_height;
    coded_width_ = format.coded_width;
    coded_height_ = format.coded_height;
    bit_depth_ = bit_depth;
    chroma_format_ = format.chroma_format;
    output_format_ = output_format;
    full_range_ = format.video_signal_description.video_full_range_flag != 0;
    matrix_ = color_matrix(format.video_signal_description.matrix_coefficients);
    return static_cast<int>(create.ulNumDecodeSurfaces);
  }

  void on_decode(CUVIDPICPARAMS &picture) {
    if (!decoder_created_)
      throw std::runtime_error(
          "NVDEC received a picture before sequence initialization");
    require_cuda(*api_, api_->cuvidDecodePicture(decoder_, &picture),
                 "cuvidDecodePicture");
  }

  void validate_decode_status(int picture_index) {
    if (!api_->cuvidGetDecodeStatus)
      return;
    CUVIDGETDECODESTATUS status{};
    const CUresult status_result =
        api_->cuvidGetDecodeStatus(decoder_, picture_index, &status);
    if (status_result == CUDA_SUCCESS &&
        (status.decodeStatus == cuvidDecodeStatus_Error ||
         status.decodeStatus == cuvidDecodeStatus_Error_Concealed))
      throw std::runtime_error("NVDEC reported a corrupted decoded frame");
  }

  void copy_gray(CUdeviceptr device_frame, unsigned int pitch,
                 NvdecGrayFrame &frame) {
    frame.width = width_;
    frame.height = height_;
    if (bit_depth_ <= 8) {
      frame.pixels.resize(static_cast<std::size_t>(width_) * height_);
      CUDA_MEMCPY2D copy{};
      copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.srcDevice = device_frame;
      copy.srcPitch = pitch;
      copy.dstMemoryType = CU_MEMORYTYPE_HOST;
      copy.dstHost = frame.pixels.data();
      copy.dstPitch = static_cast<std::size_t>(width_);
      copy.WidthInBytes = static_cast<std::size_t>(width_);
      copy.Height = static_cast<std::size_t>(height_);
      require_cuda(*api_, api_->cuMemcpy2D(&copy), "cuMemcpy2D luma");
      return;
    }

    std::vector<std::uint16_t> high(
        static_cast<std::size_t>(width_) * height_);
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = device_frame;
    copy.srcPitch = pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = high.data();
    copy.dstPitch = static_cast<std::size_t>(width_) * sizeof(std::uint16_t);
    copy.WidthInBytes =
        static_cast<std::size_t>(width_) * sizeof(std::uint16_t);
    copy.Height = static_cast<std::size_t>(height_);
    require_cuda(*api_, api_->cuMemcpy2D(&copy), "cuMemcpy2D high-bit luma");
    frame.pixels = normalize_luma(high, bit_depth_);
  }

  void copy_bgra(CUdeviceptr device_frame, unsigned int pitch,
                 NvdecBgraFrame &frame) {
    if (output_format_ != cudaVideoSurfaceFormat_NV12 &&
        output_format_ != cudaVideoSurfaceFormat_P016)
      throw std::runtime_error("NVDEC preview surface is not NV12/P016");

    frame.width = width_;
    frame.height = height_;
    const std::size_t y_samples = static_cast<std::size_t>(width_) * height_;
    const std::size_t uv_stride = static_cast<std::size_t>((width_ + 1) / 2) * 2U;
    const std::size_t uv_rows = static_cast<std::size_t>((height_ + 1) / 2);
    const std::size_t uv_samples = uv_stride * uv_rows;
    const CUdeviceptr chroma_frame =
        device_frame + static_cast<CUdeviceptr>(pitch) *
                           static_cast<CUdeviceptr>(height_);

    if (bit_depth_ <= 8) {
      std::vector<std::uint8_t> y(y_samples);
      std::vector<std::uint8_t> uv(uv_samples, 128U);
      CUDA_MEMCPY2D copy_y{};
      copy_y.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy_y.srcDevice = device_frame;
      copy_y.srcPitch = pitch;
      copy_y.dstMemoryType = CU_MEMORYTYPE_HOST;
      copy_y.dstHost = y.data();
      copy_y.dstPitch = static_cast<std::size_t>(width_);
      copy_y.WidthInBytes = static_cast<std::size_t>(width_);
      copy_y.Height = static_cast<std::size_t>(height_);
      require_cuda(*api_, api_->cuMemcpy2D(&copy_y), "cuMemcpy2D preview luma");
      if (chroma_format_ != cudaVideoChromaFormat_Monochrome) {
        CUDA_MEMCPY2D copy_uv{};
        copy_uv.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy_uv.srcDevice = chroma_frame;
        copy_uv.srcPitch = pitch;
        copy_uv.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy_uv.dstHost = uv.data();
        copy_uv.dstPitch = uv_stride;
        copy_uv.WidthInBytes = uv_stride;
        copy_uv.Height = uv_rows;
        require_cuda(*api_, api_->cuMemcpy2D(&copy_uv),
                     "cuMemcpy2D preview chroma");
      }
      frame.pixels = nv12_to_bgra(y, uv, width_, height_, full_range_, matrix_);
      return;
    }

    std::vector<std::uint16_t> y(y_samples);
    std::vector<std::uint16_t> uv(uv_samples, 0x8000U);
    CUDA_MEMCPY2D copy_y{};
    copy_y.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy_y.srcDevice = device_frame;
    copy_y.srcPitch = pitch;
    copy_y.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy_y.dstHost = y.data();
    copy_y.dstPitch = static_cast<std::size_t>(width_) * sizeof(std::uint16_t);
    copy_y.WidthInBytes = copy_y.dstPitch;
    copy_y.Height = static_cast<std::size_t>(height_);
    require_cuda(*api_, api_->cuMemcpy2D(&copy_y),
                 "cuMemcpy2D preview high-bit luma");

    if (chroma_format_ != cudaVideoChromaFormat_Monochrome) {
      CUDA_MEMCPY2D copy_uv{};
      copy_uv.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy_uv.srcDevice = chroma_frame;
      copy_uv.srcPitch = pitch;
      copy_uv.dstMemoryType = CU_MEMORYTYPE_HOST;
      copy_uv.dstHost = uv.data();
      copy_uv.dstPitch = uv_stride * sizeof(std::uint16_t);
      copy_uv.WidthInBytes = copy_uv.dstPitch;
      copy_uv.Height = uv_rows;
      require_cuda(*api_, api_->cuMemcpy2D(&copy_uv),
                   "cuMemcpy2D preview high-bit chroma");
    }
    frame.pixels = p016_to_bgra(y, uv, width_, height_, bit_depth_, full_range_,
                                matrix_);
  }

  void on_display(const CUVIDPARSERDISPINFO &display) {
    if (!decoder_created_)
      throw std::runtime_error(
          "NVDEC attempted display before decoder initialization");
    if (!active_gray_callback_ && !active_bgra_callback_)
      return;

    validate_decode_status(display.picture_index);

    CUVIDPROCPARAMS processing{};
    processing.progressive_frame = display.progressive_frame;
    processing.second_field = 0;
    processing.top_field_first = display.top_field_first;
    processing.unpaired_field = display.repeat_first_field < 0;

    CUdeviceptr device_frame{};
    unsigned int pitch = 0;
    require_cuda(*api_,
                 api_->cuvidMapVideoFrame(decoder_, display.picture_index,
                                          &device_frame, &pitch, &processing),
                 "cuvidMapVideoFrame");

    NvdecGrayFrame gray;
    NvdecBgraFrame bgra;
    try {
      validate_dimensions(width_, height_);
      if (active_gray_callback_) {
        gray.timestamp = display.timestamp;
        copy_gray(device_frame, pitch, gray);
      } else {
        bgra.timestamp = display.timestamp;
        copy_bgra(device_frame, pitch, bgra);
      }
    } catch (...) {
      api_->cuvidUnmapVideoFrame(decoder_, device_frame);
      throw;
    }

    // Decoder-owned GPU surfaces are never allowed to escape the NVDEC
    // callback. Complete every host copy and unmap before application code is
    // invoked, including the preview path.
    require_cuda(*api_, api_->cuvidUnmapVideoFrame(decoder_, device_frame),
                 "cuvidUnmapVideoFrame");
    if (active_gray_callback_)
      (*active_gray_callback_)(std::move(gray));
    else
      (*active_bgra_callback_)(std::move(bgra));
  }

  void cleanup_current_context() noexcept {
    if (!api_)
      return;
    if (parser_created_) {
      api_->cuvidDestroyVideoParser(parser_);
      parser_created_ = false;
      parser_ = nullptr;
    }
    if (decoder_created_) {
      api_->cuvidDestroyDecoder(decoder_);
      decoder_created_ = false;
      decoder_ = nullptr;
    }
    if (context_created_) {
      CUcontext popped{};
      api_->cuCtxPopCurrent(&popped);
      api_->cuCtxDestroy(context_);
      context_created_ = false;
      context_ = nullptr;
    }
  }

  void cleanup() noexcept {
    if (!api_ || !context_created_)
      return;
    if (api_->cuCtxPushCurrent(context_) == CUDA_SUCCESS) {
      if (parser_created_) {
        api_->cuvidDestroyVideoParser(parser_);
        parser_created_ = false;
        parser_ = nullptr;
      }
      if (decoder_created_) {
        api_->cuvidDestroyDecoder(decoder_);
        decoder_created_ = false;
        decoder_ = nullptr;
      }
      CUcontext popped{};
      api_->cuCtxPopCurrent(&popped);
    }
    api_->cuCtxDestroy(context_);
    context_created_ = false;
    context_ = nullptr;
  }

  NvdecCodec codec_;
  NvdecOutput output_;
  std::unique_ptr<DriverApi> api_;
  CUcontext context_{};
  CUvideoparser parser_{};
  CUvideodecoder decoder_{};
  bool context_created_{};
  bool parser_created_{};
  bool decoder_created_{};
  const NvdecFrameCallback *active_gray_callback_{};
  const NvdecBgraFrameCallback *active_bgra_callback_{};
  std::exception_ptr pending_error_;
  int width_{};
  int height_{};
  unsigned coded_width_{};
  unsigned coded_height_{};
  unsigned bit_depth_{8};
  cudaVideoChromaFormat chroma_format_{cudaVideoChromaFormat_420};
  cudaVideoSurfaceFormat output_format_{cudaVideoSurfaceFormat_NV12};
  bool full_range_{};
  PreviewColorMatrix matrix_{PreviewColorMatrix::bt709};
};

NvdecPacketDecoder::NvdecPacketDecoder(NvdecCodec codec, NvdecOutput output)
    : impl_(std::make_unique<Impl>(codec, output)) {}
NvdecPacketDecoder::~NvdecPacketDecoder() = default;
NvdecPacketDecoder::NvdecPacketDecoder(NvdecPacketDecoder &&) noexcept = default;
NvdecPacketDecoder &
NvdecPacketDecoder::operator=(NvdecPacketDecoder &&) noexcept = default;

void NvdecPacketDecoder::feed_header(std::span<const std::uint8_t> bytes) {
  impl_->feed_header(bytes);
}
void NvdecPacketDecoder::decode(std::span<const std::uint8_t> bytes,
                                std::int64_t timestamp,
                                const NvdecFrameCallback &callback) {
  impl_->decode(bytes, timestamp, callback);
}
void NvdecPacketDecoder::flush(const NvdecFrameCallback &callback) {
  impl_->flush(callback);
}
void NvdecPacketDecoder::decode_bgra(
    std::span<const std::uint8_t> bytes, std::int64_t timestamp,
    const NvdecBgraFrameCallback &callback) {
  impl_->decode_bgra(bytes, timestamp, callback);
}
void NvdecPacketDecoder::flush_bgra(const NvdecBgraFrameCallback &callback) {
  impl_->flush_bgra(callback);
}
int NvdecPacketDecoder::width() const noexcept { return impl_->width(); }
int NvdecPacketDecoder::height() const noexcept { return impl_->height(); }

bool nvdec_runtime_available() noexcept {
  try {
    DriverApi api;
    if (api.cuInit(0) != CUDA_SUCCESS)
      return false;
    int count = 0;
    return api.cuDeviceGetCount(&count) == CUDA_SUCCESS && count > 0;
  } catch (...) {
    return false;
  }
}

} // namespace gdupe
