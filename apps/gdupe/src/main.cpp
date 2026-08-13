#include "config.hpp"
#include "engine.hpp"
#include "main_window.hpp"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#ifdef _WIN32
#include <mfapi.h>
#include <objbase.h>
#include <windows.h>
#endif

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

#ifdef _WIN32
class WindowsRuntime {
public:
  WindowsRuntime() {
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
#endif

} // namespace

int main(int argc, char *argv[]) {
  try {
#ifdef _WIN32
    WindowsRuntime windows;
#endif
    Fl::scheme("gtk+");
    Fl::lock();
    const auto executable =
        std::filesystem::absolute(std::filesystem::path(argv[0]));
    std::filesystem::path config = gdupe::default_config_path(executable);
    for (int index = 1; index + 1 < argc; ++index)
      if (std::string(argv[index]) == "--config")
        config = argv[index + 1];
    auto engine = std::make_shared<gdupe::Engine>(gdupe::Config::load(config));
    gdupe::MainWindow window(std::move(engine));
    window.show(argc, argv);
    return Fl::run();
  } catch (const std::exception &problem) {
    fl_alert("gdupe could not open:\n\n%s", problem.what());
    return 1;
  }
}
