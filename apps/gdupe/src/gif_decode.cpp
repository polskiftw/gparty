#include "gif_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <FL/Fl_Anim_GIF_Image.H>
#include <FL/Fl_Image.H>

namespace gdupe {
namespace {

constexpr std::uint64_t kMaxFramePixels = 100'000'000ULL;

void check_deadline(std::chrono::steady_clock::time_point deadline) {
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("GIF decoding exceeded its deadline");
}

std::string utf8_path(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

DecodedGrayFrame gray_frame(const Fl_Image &image, std::int64_t timestamp_ns) {
  const int width = image.data_w();
  const int height = image.data_h();
  const int depth = image.d();
  if (width <= 0 || height <= 0 || depth < 3 || depth > 4 ||
      image.count() != 1 || !image.data() || !image.data()[0])
    throw std::runtime_error("FLTK returned an invalid composed GIF frame");
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > kMaxFramePixels)
    throw std::runtime_error("GIF frame exceeds gdupe's safety limit");

  const int stride = image.ld() > 0 ? image.ld() : width * depth;
  if (stride < width * depth)
    throw std::runtime_error("FLTK returned an invalid GIF row stride");

  const auto *source = reinterpret_cast<const std::uint8_t *>(image.data()[0]);
  DecodedGrayFrame result;
  result.width = width;
  result.height = height;
  result.timestamp_ns = timestamp_ns;
  result.pixels.resize(static_cast<std::size_t>(width) * height);

  for (int y = 0; y < height; ++y) {
    const auto *row = source + static_cast<std::ptrdiff_t>(y) * stride;
    auto *destination = result.pixels.data() + static_cast<std::size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      const auto *pixel = row + static_cast<std::ptrdiff_t>(x) * depth;
      // Canonical gdupe grayscale: integer BT.601, matching static-image input.
      // FLTK supplies already composed full-canvas RGB(A) animation frames.
      destination[x] = static_cast<std::uint8_t>(
          (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2] + 128U) >> 8U);
    }
  }
  return result;
}

} // namespace

DecodedMovingMedia decode_gif_static(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline) {
  if (sample_count == 0)
    throw std::runtime_error("GIF sample count must be positive");
  check_deadline(deadline);

  const std::string filename = utf8_path(path);
  Fl_Anim_GIF_Image animation(
      filename.c_str(), nullptr,
      Fl_Anim_GIF_Image::DONT_START |
          Fl_Anim_GIF_Image::DONT_RESIZE_CANVAS |
          Fl_Anim_GIF_Image::DONT_SET_AS_IMAGE);
  if (!animation.valid())
    throw std::runtime_error("FLTK could not decode animated GIF");

  const int frame_count = animation.frames();
  const int width = animation.canvas_w();
  const int height = animation.canvas_h();
  if (frame_count <= 0 || width <= 0 || height <= 0)
    throw std::runtime_error("GIF contains no decodable frames");
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > kMaxFramePixels)
    throw std::runtime_error("GIF canvas exceeds gdupe's safety limit");

  std::vector<std::int64_t> starts(static_cast<std::size_t>(frame_count));
  std::int64_t duration_ns = 0;
  for (int frame = 0; frame < frame_count; ++frame) {
    starts[static_cast<std::size_t>(frame)] = duration_ns;
    const double delay_seconds = animation.delay(frame);
    if (std::isfinite(delay_seconds) && delay_seconds > 0.0) {
      const long double ns = static_cast<long double>(delay_seconds) *
                             1'000'000'000.0L;
      const auto increment = ns >= static_cast<long double>(
                                      std::numeric_limits<std::int64_t>::max() -
                                      duration_ns)
                                 ? std::numeric_limits<std::int64_t>::max() -
                                       duration_ns
                                 : static_cast<std::int64_t>(std::llround(ns));
      duration_ns += std::max<std::int64_t>(0, increment);
    }
  }

  const std::size_t wanted = std::min<std::size_t>(
      sample_count, static_cast<std::size_t>(frame_count));
  DecodedMovingMedia result;
  result.width = width;
  result.height = height;
  result.duration_ms = duration_ns / 1'000'000;
  result.frame_count = frame_count;
  result.sampled_frames.reserve(wanted);

  int previous_index = -1;
  for (std::size_t sample = 0; sample < wanted; ++sample) {
    check_deadline(deadline);
    int frame_index = 0;
    if (duration_ns > 0) {
      const auto target = static_cast<std::int64_t>(
          static_cast<long double>(duration_ns) * sample / wanted);
      const auto upper = std::upper_bound(starts.begin(), starts.end(), target);
      frame_index = static_cast<int>(
          std::max<std::ptrdiff_t>(0, (upper - starts.begin()) - 1));
    } else {
      frame_index = static_cast<int>(
          (sample * static_cast<std::size_t>(frame_count)) / wanted);
    }
    frame_index = std::clamp(frame_index, 0, frame_count - 1);
    if (frame_index == previous_index && frame_index + 1 < frame_count)
      ++frame_index;
    previous_index = frame_index;

    const Fl_Image *image = animation.image(frame_index);
    if (!image)
      throw std::runtime_error("FLTK returned an empty GIF frame");
    result.sampled_frames.push_back(
        gray_frame(*image, starts[static_cast<std::size_t>(frame_index)]));
  }

  if (result.sampled_frames.empty())
    throw std::runtime_error("GIF contained no sampled frames");
  return result;
}

} // namespace gdupe
