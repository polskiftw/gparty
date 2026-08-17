#pragma once

#include <array>
#include <string_view>

namespace gparty::fingerprints {

struct ComponentVersion {
  std::string_view name;
  int version;
};

inline constexpr int kCompatibleGdupeFingerprintVersion = 3;
inline constexpr ComponentVersion kMediaInfo{"media_info", 1};
inline constexpr ComponentVersion kSha256{"sha256", 1};
inline constexpr ComponentVersion kPhash64{"phash64", 1};
inline constexpr ComponentVersion kPerceptual256{"perceptual256", 1};
inline constexpr ComponentVersion kCropPhash64{"crop_phash64", 1};
inline constexpr ComponentVersion kTimelinePhash64{"timeline_phash64", 1};

inline constexpr std::array kStaticComponents{
    kMediaInfo, kSha256, kPhash64, kPerceptual256, kCropPhash64};
inline constexpr std::array kMovingComponents{
    kMediaInfo, kSha256, kPhash64, kPerceptual256, kCropPhash64,
    kTimelinePhash64};

} // namespace gparty::fingerprints
