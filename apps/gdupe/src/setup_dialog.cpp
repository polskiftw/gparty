#include "setup_dialog.hpp"

#include <windows.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace gdupe {
namespace {

constexpr wchar_t kSetupClass[] = L"gdupe.setup.window.v1";
constexpr int kKeyId = 2001;
constexpr int kApplicationKey = 2002;
constexpr int kSave = 2003;
constexpr int kCancel = 2004;

struct SetupState {
  HWND window{};
  HWND key_id{};
  HWND application_key{};
  HWND save{};
  HWND cancel{};
  bool done{};
  std::optional<B2Credentials> result;
};

std::string wide_to_utf8(std::wstring_view text) {
  if (text.empty())
    return {};
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    throw std::runtime_error("B2 credentials contain invalid text");
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count,
                      nullptr, nullptr);
  return result;
}

std::wstring control_text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length < 0)
    throw std::runtime_error("Windows could not read the setup form");
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  if (length > 0 &&
      GetWindowTextW(control, text.data(), length + 1) != length) {
    throw std::runtime_error("Windows could not read the setup form");
  }
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
  if (control == nullptr)
    throw std::runtime_error("Windows could not create the B2 setup form");
  set_font(control, font);
  return control;
}

void close_setup(SetupState &state) {
  state.done = true;
  if (state.window != nullptr)
    DestroyWindow(state.window);
}

LRESULT CALLBACK setup_window_proc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  auto *state = reinterpret_cast<SetupState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    state = static_cast<SetupState *>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
    return TRUE;
  }
  if (state == nullptr)
    return DefWindowProcW(window, message, wparam, lparam);

  try {
    switch (message) {
    case WM_CREATE: {
      HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      create_control(0, L"STATIC", L"Connect gdupe to Backblaze B2", SS_LEFT,
                     24, 20, 450, 22, window, 0, font);
      create_control(
          0, L"STATIC",
          L"Paste your B2 key once. gdupe will store it securely in Windows Credential Manager.",
          SS_LEFT, 24, 48, 470, 38, window, 0, font);
      create_control(0, L"STATIC", L"Key ID", SS_LEFT, 24, 96, 150, 20,
                     window, 0, font);
      state->key_id = create_control(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          ES_AUTOHSCROLL | WS_TABSTOP, 24, 118, 470, 26, window, kKeyId,
          font);
      create_control(0, L"STATIC", L"Application key", SS_LEFT, 24, 154,
                     150, 20, window, 0, font);
      state->application_key = create_control(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 24, 176, 470, 26, window,
          kApplicationKey, font);
      state->save = create_control(0, L"BUTTON", L"Save and continue",
                                   BS_DEFPUSHBUTTON | WS_TABSTOP, 318, 218, 176,
                                   32, window, kSave, font);
      state->cancel = create_control(0, L"BUTTON", L"Cancel",
                                     BS_PUSHBUTTON | WS_TABSTOP, 230, 218, 78,
                                     32, window, kCancel, font);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == kSave) {
        const std::string key_id = wide_to_utf8(control_text(state->key_id));
        const std::string application_key =
            wide_to_utf8(control_text(state->application_key));
        if (key_id.empty() || application_key.empty()) {
          MessageBoxW(window, L"Paste both the Key ID and Application key.",
                      L"gdupe setup", MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        state->result = B2Credentials{key_id, application_key};
        close_setup(*state);
        return 0;
      }
      if (id == kCancel) {
        close_setup(*state);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      close_setup(*state);
      return 0;
    case WM_DESTROY:
      state->done = true;
      state->window = nullptr;
      return 0;
    default:
      break;
    }
  } catch (const std::exception &) {
    MessageBoxW(window, L"Windows could not read the B2 setup form.",
                L"gdupe setup", MB_OK | MB_ICONERROR);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void center_window(HWND window) {
  RECT bounds{};
  if (!GetWindowRect(window, &bounds))
    return;
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
    return;
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  SetWindowPos(window, nullptr, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

std::optional<B2Credentials> show_b2_setup(HINSTANCE instance) {
  WNDCLASSW definition{};
  definition.lpfnWndProc = setup_window_proc;
  definition.hInstance = instance;
  definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  definition.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  definition.lpszClassName = kSetupClass;
  if (RegisterClassW(&definition) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw std::runtime_error("Windows could not register the B2 setup window");
  }

  RECT size{0, 0, 520, 270};
  const DWORD style = WS_CAPTION | WS_SYSMENU;
  const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  if (!AdjustWindowRectEx(&size, style, FALSE, ex_style))
    throw std::runtime_error("Windows could not size the B2 setup window");

  SetupState state;
  HWND window = CreateWindowExW(
      ex_style, kSetupClass, L"gdupe setup", style, CW_USEDEFAULT,
      CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, nullptr,
      nullptr, instance, &state);
  if (window == nullptr)
    throw std::runtime_error("Windows could not create the B2 setup window");

  center_window(window);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  SetFocus(state.key_id);

  MSG message{};
  while (!state.done) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == -1)
      throw std::runtime_error("Windows setup message loop failed");
    if (result == 0) {
      state.done = true;
      break;
    }
    if (message.message == WM_KEYDOWN && state.window != nullptr) {
      if (message.wParam == VK_TAB) {
        const BOOL backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        HWND next = GetNextDlgTabItem(state.window, GetFocus(), backwards);
        if (next != nullptr)
          SetFocus(next);
        continue;
      }
      if (message.wParam == VK_RETURN) {
        SendMessageW(state.window, WM_COMMAND,
                     MAKEWPARAM(kSave, BN_CLICKED), 0);
        continue;
      }
      if (message.wParam == VK_ESCAPE) {
        SendMessageW(state.window, WM_CLOSE, 0, 0);
        continue;
      }
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return state.result;
}

} // namespace gdupe
