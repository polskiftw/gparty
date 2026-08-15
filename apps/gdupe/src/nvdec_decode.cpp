#include "nvdec_decode.hpp"
#include "annexb_decoder.hpp"

#include <algorithm>
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

} // namespace

class NvdecPacketDecoder::Impl {
public:
  explicit Impl(NvdecCodec codec) : codec_(codec), api_(std::make_unique<DriverApi>()) {
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
    parse(bytes, 0, 0, nullptr);
  }

  void decode(std::span<const std::uint8_t> bytes, std::int64_t timestamp,
              const NvdecFrameCallback &callback) {
    if (bytes.empty())
      throw std::runtime_error(std::string(codec_name(codec_)) +
                               " compressed packet is empty");
    parse(bytes, timestamp,
          CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE, &callback);
  }

  void flush(const NvdecFrameCallback &callback) {
    parse({}, 0, CUVID_PKT_ENDOFSTREAM, &callback);
  }

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

private:
  void parse(std::span<const std::uint8_t> bytes, std::int64_t timestamp,
             unsigned long flags, const NvdecFrameCallback *callback) {
    ContextScope scope(*api_, context_);
    pending_error_ = nullptr;
    active_callback_ = callback;

    CUVIDSOURCEDATAPACKET packet{};
    packet.flags = flags;
    packet.payload_size = static_cast<tcu_ulong>(bytes.size());
    packet.payload = bytes.empty() ? nullptr : bytes.data();
    packet.timestamp = timestamp;

    const CUresult result = api_->cuvidParseVideoData(parser_, &packet);
    active_callback_ = nullptr;
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
        format.coded_height > caps.nMaxHeight) {
      throw std::runtime_error("Video dimensions exceed NVIDIA NVDEC capability");
    }
    const std::uint64_t macroblocks =
        (static_cast<std::uint64_t>(format.coded_width) *
         static_cast<std::uint64_t>(format.coded_height) + 255ULL) /
        256ULL;
    if (caps.nMaxMBCount != 0 && macroblocks > caps.nMaxMBCount)
      throw std::runtime_error("Video macroblock count exceeds NVIDIA NVDEC capability");

    const auto output_format = choose_output_format(format.chroma_format, bit_depth);
    if ((caps.nOutputFormatMask & (1U << static_cast<unsigned>(output_format))) == 0)
      throw std::runtime_error("NVIDIA NVDEC cannot expose the required luma surface format");

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
    create.ulTargetWidth = static_cast<tcu_ulong>(display_width);
    create.ulTargetHeight = static_cast<tcu_ulong>(display_height);
    create.ulNumOutputSurfaces = 2;
    create.target_rect.left = 0;
    create.target_rect.top = 0;
    create.target_rect.right = static_cast<short>(display_width);
    create.target_rect.bottom = static_cast<short>(display_height);

    require_cuda(*api_, api_->cuvidCreateDecoder(&decoder_, &create),
                 "cuvidCreateDecoder");
    decoder_created_ = true;
    width_ = display_width;
    height_ = display_height;
    coded_width_ = format.coded_width;
    coded_height_ = format.coded_height;
    bit_depth_ = bit_depth;
    chroma_format_ = format.chroma_format;
    output_format_ = output_format;
    return static_cast<int>(create.ulNumDecodeSurfaces);
  }

  void on_decode(CUVIDPICPARAMS &picture) {
    if (!decoder_created_)
      throw std::runtime_error("NVDEC received a picture before sequence initialization");
    require_cuda(*api_, api_->cuvidDecodePicture(decoder_, &picture),
                 "cuvidDecodePicture");
  }

  void on_display(const CUVIDPARSERDISPINFO &display) {
    if (!decoder_created_)
      throw std::runtime_error("NVDEC attempted display before decoder initialization");
    if (!active_callback_)
      return;

    if (api_->cuvidGetDecodeStatus) {
      CUVIDGETDECODESTATUS status{};
      const CUresult status_result =
          api_->cuvidGetDecodeStatus(decoder_, display.picture_index, &status);
      if (status_result == CUDA_SUCCESS &&
          (status.decodeStatus == cuvidDecodeStatus_Error ||
           status.decodeStatus == cuvidDecodeStatus_Error_Concealed)) {
        throw std::runtime_error("NVDEC reported a corrupted decoded frame");
      }
    }

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

    NvdecGrayFrame frame;
    try {
      validate_dimensions(width_, height_);
      frame.width = width_;
      frame.height = height_;
      frame.timestamp = display.timestamp;

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
      } else {
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
    } catch (...) {
      api_->cuvidUnmapVideoFrame(decoder_, device_frame);
      throw;
    }

    // Release the decoder-owned surface before invoking application code. If
    // the callback throws, there is no mapped GPU resource left to clean up,
    // so exception handling cannot accidentally unmap the same surface twice.
    require_cuda(*api_, api_->cuvidUnmapVideoFrame(decoder_, device_frame),
                 "cuvidUnmapVideoFrame");
    (*active_callback_)(std::move(frame));
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
  std::unique_ptr<DriverApi> api_;
  CUcontext context_{};
  CUvideoparser parser_{};
  CUvideodecoder decoder_{};
  bool context_created_{};
  bool parser_created_{};
  bool decoder_created_{};
  const NvdecFrameCallback *active_callback_{};
  std::exception_ptr pending_error_;
  int width_{};
  int height_{};
  unsigned coded_width_{};
  unsigned coded_height_{};
  unsigned bit_depth_{8};
  cudaVideoChromaFormat chroma_format_{cudaVideoChromaFormat_420};
  cudaVideoSurfaceFormat output_format_{cudaVideoSurfaceFormat_NV12};
};

NvdecPacketDecoder::NvdecPacketDecoder(NvdecCodec codec)
    : impl_(std::make_unique<Impl>(codec)) {}
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

namespace {

class NvdecAnnexBDecoder final : public AnnexBDecoder {
public:
  explicit NvdecAnnexBDecoder(NvdecCodec codec) : decoder_(codec) {}

  void initialize(std::span<const std::uint8_t> header) override {
    decoder_.feed_header(header);
  }

  void decode(std::span<const std::uint8_t> access_unit,
              std::uint32_t timestamp_token,
              const AnnexBFrameCallback &callback) override {
    decoder_.decode(access_unit, static_cast<std::int64_t>(timestamp_token),
                    [&](NvdecGrayFrame frame) {
                      if (frame.timestamp < 0 ||
                          frame.timestamp >
                              static_cast<std::int64_t>(
                                  std::numeric_limits<std::uint32_t>::max()))
                        throw std::runtime_error(
                            "NVDEC returned an invalid MP4 timestamp token");
                      AnnexBGrayFrame converted;
                      converted.width = frame.width;
                      converted.height = frame.height;
                      converted.timestamp_token =
                          static_cast<std::uint32_t>(frame.timestamp);
                      converted.pixels = std::move(frame.pixels);
                      callback(std::move(converted));
                    });
  }

  void flush(const AnnexBFrameCallback &callback) override {
    decoder_.flush([&](NvdecGrayFrame frame) {
      if (frame.timestamp < 0 ||
          frame.timestamp > static_cast<std::int64_t>(
                                std::numeric_limits<std::uint32_t>::max()))
        throw std::runtime_error("NVDEC returned an invalid MP4 timestamp token");
      AnnexBGrayFrame converted;
      converted.width = frame.width;
      converted.height = frame.height;
      converted.timestamp_token = static_cast<std::uint32_t>(frame.timestamp);
      converted.pixels = std::move(frame.pixels);
      callback(std::move(converted));
    });
  }

  [[nodiscard]] int width() const noexcept override { return decoder_.width(); }
  [[nodiscard]] int height() const noexcept override { return decoder_.height(); }

private:
  NvdecPacketDecoder decoder_;
};

} // namespace

std::unique_ptr<AnnexBDecoder> make_h264_annexb_decoder() {
  return std::make_unique<NvdecAnnexBDecoder>(NvdecCodec::h264);
}

std::unique_ptr<AnnexBDecoder> make_hevc_annexb_decoder() {
  return std::make_unique<NvdecAnnexBDecoder>(NvdecCodec::hevc);
}

} // namespace gdupe
