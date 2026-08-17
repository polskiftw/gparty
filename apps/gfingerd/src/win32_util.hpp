#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gparty::fingerprints::win32 {

inline std::wstring utf8_to_wide(std::string_view text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(),
                                        static_cast<int>(text.size()), nullptr,
                                        0);
  if (count <= 0)
    throw std::runtime_error("Text is not valid UTF-8");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), count) !=
      count)
    throw std::runtime_error("Windows could not convert UTF-8 text");
  return result;
}

inline std::string wide_to_utf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    throw std::runtime_error("Windows text is not valid Unicode");
  std::string result(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), count,
                          nullptr, nullptr) != count)
    throw std::runtime_error("Windows could not convert Unicode text");
  return result;
}

inline std::wstring quote_argument(const std::wstring &value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos)
    return value;
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

inline std::wstring command_line(const std::vector<std::wstring> &arguments) {
  std::wstring command;
  for (const auto &argument : arguments) {
    if (!command.empty())
      command.push_back(L' ');
    command += quote_argument(argument);
  }
  return command;
}

} // namespace gparty::fingerprints::win32
