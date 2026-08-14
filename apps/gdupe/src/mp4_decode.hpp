#pragma once

#include "media_decode.hpp"

namespace gdupe {

DecodedMovingMedia decode_mp4_static(
    const std::filesystem::path &path, std::size_t sample_count,
    std::chrono::steady_clock::time_point deadline);

} // namespace gdupe
