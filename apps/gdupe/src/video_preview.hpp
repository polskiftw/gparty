#pragma once

#include "preview_decode.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace gdupe {

class VideoPreview final {
public:
  VideoPreview() = default;
  ~VideoPreview();

  VideoPreview(const VideoPreview &) = delete;
  VideoPreview &operator=(const VideoPreview &) = delete;

  void start(const std::filesystem::path &path, const std::string &extension);
  void stop();

  std::optional<PreviewDecodedFrame> take_latest();
  [[nodiscard]] bool failed() const;
  [[nodiscard]] std::string failure_message() const;

private:
  void run(std::stop_token stop, std::filesystem::path path,
           std::string extension);

  mutable std::mutex mutex_;
  std::optional<PreviewDecodedFrame> latest_;
  std::string failure_;
  std::jthread worker_;
};

} // namespace gdupe
