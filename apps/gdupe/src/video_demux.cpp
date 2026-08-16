#include "video_demux.hpp"
#include "video_demux_internal.hpp"

#include <stdexcept>
#include <string>

namespace gdupe {

std::unique_ptr<VideoDemux>
open_video_demux(const std::filesystem::path &path, std::string_view extension) {
  if (extension == "webm")
    return open_webm_video_demux(path);
  if (extension == "mp4" || extension == "m4v")
    return open_mp4_video_demux(path);
  throw std::runtime_error("Unsupported moving-media extension: " +
                           std::string(extension));
}

} // namespace gdupe
