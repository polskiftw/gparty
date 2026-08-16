#pragma once

#include "credentials.hpp"

#include <windows.h>

#include <optional>

namespace gdupe {

std::optional<B2Credentials> show_b2_setup(HINSTANCE instance);

} // namespace gdupe
