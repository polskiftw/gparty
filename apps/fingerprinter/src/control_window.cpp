#include "control_window.hpp"

#include "registry.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace gparty::fingerprints {
namespace {

constexpr wchar_t kWindowClass[] = L"gparty.fingerprinter.control.v1";
constexpr int kKeyId = 3001;
constexpr int kApplicationKey = 3002;
constexpr int kBucket = 3003;
constexpr int kPrefix = 3004;
constexpr int kInterval = 3005;
constexpr int kWorkers = 3006;
constexpr int kAutostart = 3007;
constexpr int kSave = 3008;
constexpr int kLive = 3009;
constexpr int kClose = 3010;
constexpr UINT_PTR kRefreshTimer = 1;

struct WindowState {
  HWND window{};
  HWND status_control{};
  HWND key_id{};
  HWND application_key{};
  HWND bucket{};
  HWND prefix{};
  HWND interval{};
  HWND workers{};
  HWND autostart{};
  Config config;
  std::unique_ptr<Registry> registry;
  std::optional<B2Credentials> credentials;
  bool initial_autostart{true};
  std::wstring status;
  ControlResult result;
  bool done{};
};

std::wstring utf8_to_wide(std::string_view text);

std::string friendly_bytes(double bytes) {
  constexpr const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  std::size_t unit = 0;
  while (bytes >= 1024.0 && unit + 1 < std::size(units)) {
    bytes /= 1024.0;
    ++unit;
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << bytes << ' '
       << units[unit];
  return text.str();
}

std::string friendly_time(std::int64_t unix_ms) {
  if (unix_ms <= 0)
    return "waiting";
  const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
  std::tm local{};
  if (localtime_s(&local, &seconds) != 0)
    return "unknown";
  std::ostringstream text;
  text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
  return text.str();
}

std::string current_status(const Config &config, Registry &registry) {
  const auto library = registry.status();
  std::ostringstream text;
  text << "Library: " << library.fully_fingerprinted << " complete, "
       << library.pending_objects << " pending, " << library.failed
       << " failed, " << library.unsupported << " unsupported";
  if (!library.last_successful_scan.empty())
    text << " | scan " << library.last_successful_scan;
  text << "\r\n";
  try {
    std::ifstream stream(config.log_path.parent_path() /
                         "fingerprinter-status.json");
    nlohmann::json runtime;
    stream >> runtime;
    const auto updated = runtime.value("updated_unix_ms", 0LL);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (updated > 0 && now - updated < 10'000 &&
        runtime.value("state", std::string{}) != "stopped") {
      text << "Worker: " << runtime.value("state", std::string("running"))
           << ", " << runtime.value("active_workers", 0) << '/'
           << runtime.value("configured_workers", config.worker_threads)
           << " active | "
           << friendly_bytes(runtime.value("bytes_per_second", 0.0))
           << "/s | "
           << friendly_bytes(
                  static_cast<double>(runtime.value("session_bytes", 0ULL)))
           << " downloaded | " << runtime.value("completed_session", 0ULL)
           << " done, " << runtime.value("failed_session", 0ULL)
           << " failed this session\r\n";
      if (runtime.value("launch_mode", std::string{}) == "boot") {
        text << "Boot proof: worker started "
             << friendly_time(runtime.value("process_started_unix_ms", 0LL))
             << " | NVDEC "
             << friendly_time(runtime.value("nvdec_ready_unix_ms", 0LL))
             << "\r\n";
      }
      const auto files = runtime.value("current_files",
                                       std::vector<std::string>{});
      if (!files.empty()) {
        text << "Now: ";
        const auto shown = (std::min)(files.size(), std::size_t{3});
        for (std::size_t index = 0; index < shown; ++index) {
          if (index != 0)
            text << "; ";
          const auto &file = files[index];
          text << (file.size() > 45 ? "..." + file.substr(file.size() - 42)
                                   : file);
        }
        if (files.size() > shown)
          text << "; +" << (files.size() - shown) << " more";
      } else {
        text << "Now: idle until the next inventory check";
      }
      return text.str();
    }
  } catch (...) {
  }
  text << "Worker: not currently reporting | configured for "
       << config.worker_threads << " thread(s)";
  return text.str();
}

void refresh_status(WindowState &state) {
  try {
    const auto status =
        utf8_to_wide(current_status(state.config, *state.registry));
    SetWindowTextW(state.status_control, status.c_str());
  } catch (...) {
  }
}

std::wstring utf8_to_wide(std::string_view text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(),
                                        static_cast<int>(text.size()), nullptr,
                                        0);
  if (count <= 0)
    throw std::runtime_error("Configuration contains invalid text");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count);
  return result;
}

std::string wide_to_utf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    throw std::runtime_error("Configuration contains invalid text");
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count,
                      nullptr, nullptr);
  return result;
}

std::wstring control_text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length < 0)
    throw std::runtime_error("Windows could not read the configuration form");
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  if (length > 0 &&
      GetWindowTextW(control, text.data(), length + 1) != length)
    throw std::runtime_error("Windows could not read the configuration form");
  text.resize(static_cast<std::size_t>(length));
  return text;
}

void set_font(HWND control, HFONT font) {
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND create_control(DWORD ex_style, const wchar_t *class_name,
                    const wchar_t *text, DWORD style, int x, int y, int width,
                    int height, HWND parent, int id, HFONT font) {
  HWND control = CreateWindowExW(
      ex_style, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, width,
      height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (!control)
    throw std::runtime_error("Windows could not create the configuration form");
  set_font(control, font);
  return control;
}

void close_window(WindowState &state) {
  state.done = true;
  if (state.window)
    DestroyWindow(state.window);
}

void collect_settings(WindowState &state) {
  const std::string key_id = wide_to_utf8(control_text(state.key_id));
  const std::string application_key =
      wide_to_utf8(control_text(state.application_key));
  if (key_id.empty() || application_key.empty())
    throw std::runtime_error("Paste both the B2 Key ID and Application Key.");

  Config config = state.config;
  config.bucket_name = wide_to_utf8(control_text(state.bucket));
  config.canonical_prefix = wide_to_utf8(control_text(state.prefix));
  const std::string minutes_text = wide_to_utf8(control_text(state.interval));
  int minutes = 0;
  const auto parsed = std::from_chars(minutes_text.data(),
                                      minutes_text.data() + minutes_text.size(),
                                      minutes);
  if (parsed.ec != std::errc{} || parsed.ptr != minutes_text.data() + minutes_text.size() ||
      minutes < 1 || minutes > 1440)
    throw std::runtime_error("Scan interval must be 1 to 1440 minutes.");
  config.polling_seconds = minutes * 60;
  const std::string workers_text = wide_to_utf8(control_text(state.workers));
  int workers = 0;
  const auto workers_parsed = std::from_chars(
      workers_text.data(), workers_text.data() + workers_text.size(), workers);
  if (workers_parsed.ec != std::errc{} ||
      workers_parsed.ptr != workers_text.data() + workers_text.size() ||
      workers < 1 || workers > 16)
    throw std::runtime_error("Worker threads must be 1 to 16.");
  config.worker_threads = workers;
  config.validate();

  state.result.config = std::move(config);
  state.result.credentials = B2Credentials{key_id, application_key};
  state.result.autostart =
      SendMessageW(state.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  auto *state = reinterpret_cast<WindowState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    state = static_cast<WindowState *>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    return TRUE;
  }
  if (!state)
    return DefWindowProcW(window, message, wparam, lparam);

  try {
    switch (message) {
    case WM_CREATE: {
      HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      create_control(0, L"STATIC", L"GParty Background Fingerprinter",
                     SS_LEFT, 24, 18, 530, 24, window, 0, font);
      state->status_control = create_control(
          0, L"STATIC", state->status.c_str(), SS_LEFT, 24, 48, 530, 92,
          window, 0, font);
      create_control(0, L"STATIC", L"B2 Key ID", SS_LEFT, 24, 146, 250, 18,
                     window, 0, font);
      state->key_id = create_control(
          WS_EX_CLIENTEDGE, L"EDIT",
          state->credentials ? utf8_to_wide(state->credentials->key_id).c_str()
                             : L"",
          ES_AUTOHSCROLL | WS_TABSTOP, 24, 166, 530, 25, window, kKeyId, font);
      create_control(0, L"STATIC", L"B2 Application Key", SS_LEFT, 24, 200,
                     250, 18, window, 0, font);
      state->application_key = create_control(
          WS_EX_CLIENTEDGE, L"EDIT",
          state->credentials
              ? utf8_to_wide(state->credentials->application_key).c_str()
              : L"",
          ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 24, 220, 530, 25,
          window, kApplicationKey, font);
      create_control(0, L"STATIC", L"B2 bucket", SS_LEFT, 24, 254, 250, 18,
                     window, 0, font);
      state->bucket = create_control(
          WS_EX_CLIENTEDGE, L"EDIT", utf8_to_wide(state->config.bucket_name).c_str(),
          ES_AUTOHSCROLL | WS_TABSTOP, 24, 274, 258, 25, window, kBucket, font);
      create_control(0, L"STATIC", L"Canonical prefix", SS_LEFT, 296, 254,
                     250, 18, window, 0, font);
      state->prefix = create_control(
          WS_EX_CLIENTEDGE, L"EDIT",
          utf8_to_wide(state->config.canonical_prefix).c_str(),
          ES_AUTOHSCROLL | WS_TABSTOP, 296, 274, 258, 25, window, kPrefix,
          font);
      create_control(0, L"STATIC", L"Check for new media every (minutes)",
                     SS_LEFT, 24, 312, 260, 18, window, 0, font);
      const auto minutes = std::to_wstring(state->config.polling_seconds / 60);
      state->interval = create_control(
          WS_EX_CLIENTEDGE, L"EDIT", minutes.c_str(),
          ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, 24, 332, 120, 25, window,
          kInterval, font);
      create_control(0, L"STATIC", L"Worker threads (1-16)", SS_LEFT, 170,
                     312, 200, 18, window, 0, font);
      const auto workers = std::to_wstring(state->config.worker_threads);
      state->workers = create_control(
          WS_EX_CLIENTEDGE, L"EDIT", workers.c_str(),
          ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, 170, 332, 120, 25, window,
          kWorkers, font);
      state->autostart = create_control(
          0, L"BUTTON", L"Start when the PC boots (before Windows sign-in)",
          BS_AUTOCHECKBOX | WS_TABSTOP, 24, 372, 420, 24, window, kAutostart,
          font);
      SendMessageW(state->autostart, BM_SETCHECK,
                   state->initial_autostart ? BST_CHECKED : BST_UNCHECKED, 0);
      create_control(0, L"STATIC",
                     L"Save & Start asks once for permission to install boot startup.",
                     SS_LEFT, 24, 406, 530, 20, window, 0, font);
      create_control(0, L"BUTTON", L"Live CMD Output",
                     BS_PUSHBUTTON | WS_TABSTOP, 24, 448, 150, 32, window,
                     kLive, font);
      create_control(0, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP,
                     352, 448, 88, 32, window, kClose, font);
      create_control(0, L"BUTTON", L"Save && Start",
                     BS_DEFPUSHBUTTON | WS_TABSTOP, 450, 448, 104, 32, window,
                     kSave, font);
      SetTimer(window, kRefreshTimer, 1000, nullptr);
      refresh_status(*state);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == kSave) {
        collect_settings(*state);
        state->result.action = ControlAction::save_and_start;
        close_window(*state);
        return 0;
      }
      if (id == kLive) {
        state->result.action = ControlAction::live_output;
        close_window(*state);
        return 0;
      }
      if (id == kClose) {
        close_window(*state);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      close_window(*state);
      return 0;
    case WM_TIMER:
      if (wparam == kRefreshTimer)
        refresh_status(*state);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kRefreshTimer);
      state->done = true;
      state->window = nullptr;
      return 0;
    default:
      break;
    }
  } catch (const std::exception &problem) {
    show_control_error(problem.what());
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void center_window(HWND window) {
  RECT bounds{};
  RECT work{};
  if (!GetWindowRect(window, &bounds) ||
      !SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
    return;
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  SetWindowPos(window, nullptr,
               work.left + ((work.right - work.left) - width) / 2,
               work.top + ((work.bottom - work.top) - height) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

void show_control_error(const std::string &message) {
  const std::wstring wide = utf8_to_wide(message);
  MessageBoxW(nullptr, wide.c_str(), L"GParty Fingerprinter",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

void show_control_information(const std::string &message) {
  const std::wstring wide = utf8_to_wide(message);
  MessageBoxW(nullptr, wide.c_str(), L"GParty Fingerprinter",
              MB_OK | MB_ICONINFORMATION | MB_TASKMODAL);
}

ControlResult show_control_window(HINSTANCE instance, const Config &config,
                                  const std::optional<B2Credentials> &credentials,
                                  bool autostart, const std::string &status) {
  WNDCLASSW definition{};
  definition.lpfnWndProc = window_proc;
  definition.hInstance = instance;
  definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  definition.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  definition.lpszClassName = kWindowClass;
  if (RegisterClassW(&definition) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    throw std::runtime_error("Windows could not register the control window");

  RECT size{0, 0, 580, 504};
  const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  const DWORD ex_style = WS_EX_CONTROLPARENT;
  if (!AdjustWindowRectEx(&size, style, FALSE, ex_style))
    throw std::runtime_error("Windows could not size the control window");

  WindowState state;
  state.config = config;
  state.registry = std::make_unique<Registry>(config.database_path);
  state.credentials = credentials;
  state.initial_autostart = autostart;
  state.status = utf8_to_wide(status);
  state.result.config = config;
  state.result.credentials = credentials;
  state.result.autostart = autostart;
  HWND window = CreateWindowExW(
      ex_style, kWindowClass, L"GParty Fingerprinter", style, CW_USEDEFAULT,
      CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, nullptr,
      nullptr, instance, &state);
  if (!window)
    throw std::runtime_error("Windows could not create the control window");
  center_window(window);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  SetFocus(credentials ? state.bucket : state.key_id);

  MSG message{};
  while (!state.done) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == -1)
      throw std::runtime_error("Windows control window message loop failed");
    if (result == 0)
      break;
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return state.result;
}

} // namespace gparty::fingerprints
