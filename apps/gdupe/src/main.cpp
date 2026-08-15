#include "config.hpp"
#include "engine.hpp"
#include "main_window.hpp"

#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class WindowsRuntime final {
public:
  WindowsRuntime() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    instance_ = CreateMutexW(nullptr, FALSE, L"Local\\gdupe-single-instance-v1");
    if (!instance_)
      throw std::runtime_error("Windows could not create the instance lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
      throw std::runtime_error("gdupe is already open on this computer");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
      throw std::runtime_error("Windows COM initialization failed");
    com_initialized_ = true;
    const HRESULT media = MFStartup(MF_VERSION);
    if (FAILED(media))
      throw std::runtime_error("Windows Media Foundation initialization failed");
    media_initialized_ = true;
  }

  ~WindowsRuntime() {
    if (media_initialized_)
      MFShutdown();
    if (com_initialized_)
      CoUninitialize();
    if (instance_)
      CloseHandle(instance_);
  }

private:
  HANDLE instance_{};
  bool com_initialized_{};
  bool media_initialized_{};
};

std::wstring utf8_to_wide(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr,
                                        0);
  if (count <= 0)
    return L"gdupe could not open.";
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), count);
  return wide;
}

void show_startup_error(const std::string &problem) {
  const std::wstring message =
      L"gdupe could not open:\n\n" + utf8_to_wide(problem);
  MessageBoxW(nullptr, message.c_str(), L"gdupe",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

std::filesystem::path executable_path() {
  std::vector<wchar_t> buffer(1024);
  while (true) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0)
      throw std::runtime_error("Windows could not locate gdupe.exe");
    if (length < buffer.size() - 1)
      return std::filesystem::path(std::wstring(buffer.data(), length));
    if (buffer.size() >= 32768)
      throw std::runtime_error("The gdupe.exe path is too long");
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path command_line_config(
    const std::filesystem::path &default_path) {
  int count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments)
    throw std::runtime_error("Windows could not parse the command line");
  std::filesystem::path result = default_path;
  for (int index = 1; index + 1 < count; ++index) {
    if (std::wstring_view(arguments[index]) == L"--config") {
      result = arguments[index + 1];
      break;
    }
  }
  LocalFree(arguments);
  return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  try {
    WindowsRuntime windows;
    const auto executable = executable_path();
    const auto config =
        command_line_config(gdupe::default_config_path(executable));
    auto engine = std::make_shared<gdupe::Engine>(gdupe::Config::load(config));
    gdupe::MainWindow window(instance, std::move(engine));
    return window.run(show_command);
  } catch (const std::exception &problem) {
    show_startup_error(problem.what());
    return 1;
  }
}
