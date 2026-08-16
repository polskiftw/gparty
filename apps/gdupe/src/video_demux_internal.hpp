#pragma once

#include "video_demux.hpp"

#include <filesystem>
#include <memory>

namespace gdupe {

std::unique_ptr<VideoDemux>
open_mp4_video_demux(const std::filesystem::path &path);
std::unique_ptr<VideoDemux>
open_webm_video_demux(const std::filesystem::path &path);

} // namespace gdupe
