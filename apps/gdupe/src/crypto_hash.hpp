#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace gdupe {

std::string sha1(std::string_view value);
std::string sha256(std::string_view value);
std::string sha1_file(const std::filesystem::path &path);
std::string sha256_file(const std::filesystem::path &path);

} // namespace gdupe
