#pragma once

#include "config.hpp"
#include "credentials.hpp"

#include <windows.h>

#include <optional>
#include <string>

namespace gparty::fingerprints {

enum class ControlAction { close, save_and_start, live_output };

struct ControlResult {
  ControlAction action{ControlAction::close};
  Config config;
  std::optional<B2Credentials> credentials;
  bool autostart{true};
};

ControlResult show_control_window(HINSTANCE instance, const Config &config,
                                  const std::optional<B2Credentials> &credentials,
                                  bool autostart, const std::string &status);

void show_control_error(const std::string &message);
void show_control_information(const std::string &message);

} // namespace gparty::fingerprints
