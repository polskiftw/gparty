#include "main_window.hpp"

#include "image_decode.hpp"
#include "preview_color.hpp"
#include "video_preview.hpp"
#include "wic_gif.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gdupe {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"gdupe.native.window.v1";
constexpr UINT kUiTaskMessage = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 25;
constexpr int kProcessAll = 1001;
constexpr int kDeleteLeft = 1002;
constexpr int kKeepBoth = 1003;
constexpr int kDeleteRight = 1004;
constexpr int kRetry = 1005;

constexpr std::uint32_t kBackground = 0x0b0c0f;
constexpr std::uint32_t kPanel = 0x15161b;
constexpr std::uint32_t kForeground = 0xf5f5f7;
constexpr std::uint32_t kMuted = 0x9a9aa2;
constexpr std::uint32_t kButton = 0x24262e;
constexpr std::uint32_t kDanger = 0x3a171b;
constexpr std::uint32_t kPrimary = 0xe9e9ec;

void require_hresult(HRESULT result, const char *message) {
  if (FAILED(result))
    throw std::runtime_error(message);
}

std::wstring utf8_to_wide(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(),
                                        static_cast<int>(text.size()), nullptr,
                                        0);
  if (count <= 0)
    return L"Text could not be displayed";
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), count);
  return result;
}

std::wstring details(const InventoryObject &item) {
  if (!item.fingerprint)
    return utf8_to_wide(item.remote.key);
  const auto &fingerprint = *item.fingerprint;
  const wchar_t *type = fingerprint.kind == MediaKind::StaticImage
                            ? L"image"
                        : fingerprint.kind == MediaKind::AnimatedImage
                            ? L"animated GIF"
                            : L"video";
  std::wostringstream text;
  text << utf8_to_wide(item.remote.key) << L'\n' << fingerprint.width << L" x "
       << fingerprint.height << L"  -  " << type << L"  -  " << std::fixed
       << std::setprecision(1) << item.remote.size / (1024.0 * 1024.0)
       << L" MiB";
  if (fingerprint.duration_ms > 0)
    text << L"  -  " << fingerprint.duration_ms / 1000.0 << L" s";
  return text.str();
}

float color_component(std::uint32_t value, unsigned shift) {
  return static_cast<float>((value >> shift) & 0xffU) / 255.0F;
}

D2D1_COLOR_F color(std::uint32_t value, float alpha = 1.0F) {
  return D2D1::ColorF(color_component(value, 16), color_component(value, 8),
                      color_component(value, 0), alpha);
}

struct Layout {
  float width{};
  float height{};
  D2D1_RECT_F process_all{};
  D2D1_RECT_F left_media{};
  D2D1_RECT_F right_media{};
  D2D1_RECT_F delete_left{};
  D2D1_RECT_F keep_both{};
  D2D1_RECT_F delete_right{};
  D2D1_RECT_F retry{};
};

Layout make_layout(float width, float height) {
  constexpr float margin = 28.0F;
  constexpr float gap = 18.0F;
  const float center = width / 2.0F;
  const float pane_width = (width - margin * 2.0F - gap) / 2.0F;
  const float pane_height = std::max(260.0F, height - 300.0F);
  const float action_y = height - 66.0F;
  return {
      width,
      height,
      D2D1::RectF(width - margin - 140.0F, 20.0F, width - margin, 62.0F),
      D2D1::RectF(margin, 110.0F, margin + pane_width, 110.0F + pane_height),
      D2D1::RectF(margin + pane_width + gap, 110.0F, width - margin,
                  110.0F + pane_height),
      D2D1::RectF(margin, action_y, margin + 140.0F, action_y + 42.0F),
      D2D1::RectF(center - 70.0F, action_y, center + 70.0F, action_y + 42.0F),
      D2D1::RectF(width - margin - 140.0F, action_y, width - margin,
                  action_y + 42.0F),
      D2D1::RectF(center - 100.0F, height / 2.0F + 60.0F,
                  center + 100.0F, height / 2.0F + 104.0F),
  };
}

RECT to_pixels(D2D1_RECT_F rect, UINT dpi) {
  const auto convert = [dpi](float value) {
    return static_cast<LONG>(std::lround(value * dpi / 96.0F));
  };
  return {convert(rect.left), convert(rect.top), convert(rect.right),
          convert(rect.bottom)};
}

void move_control(HWND control, D2D1_RECT_F rect, UINT dpi) {
  const RECT pixels = to_pixels(rect, dpi);
  MoveWindow(control, pixels.left, pixels.top, pixels.right - pixels.left,
             pixels.bottom - pixels.top, TRUE);
}

} // namespace

class MainWindow::MediaPane final {
public:
  ~MediaPane() { clear(); }

  void clear() {
    video_.stop();
    mode_ = Mode::Empty;
    width_ = 0;
    height_ = 0;
    pixels_.clear();
    frames_.clear();
    current_frame_ = 0;
    animation_duration_ms_ = 0;
    bitmap_.Reset();
    bitmap_owner_ = nullptr;
  }

  void show_file(const std::filesystem::path &path,
                 const std::string &extension, MediaKind kind) {
    clear();
    try {
      if (kind == MediaKind::Video) {
        mode_ = Mode::Video;
        video_.start(path, extension);
        return;
      }
      if (kind == MediaKind::AnimatedImage) {
        WicGifDecoder decoder(path);
        const auto &info = decoder.info();
        width_ = info.width;
        height_ = info.height;
        constexpr std::size_t kMaxPreviewBytes = 512ULL * 1024ULL * 1024ULL;
        const std::size_t frame_bytes =
            static_cast<std::size_t>(width_) * height_ * 4;
        decoder.decode(std::chrono::steady_clock::now() +
                           std::chrono::seconds(30),
                       [&](const WicGifFrameView &frame) {
                         if (frame_bytes == 0 ||
                             frames_.size() >= kMaxPreviewBytes / frame_bytes)
                           throw std::runtime_error(
                               "GIF preview exceeds gdupe's memory safety limit");
                         const std::uint64_t delay =
                             frame.delay_ms == 0 ? 100U : frame.delay_ms;
                         frames_.push_back(
                             {std::vector<std::uint8_t>(
                                  frame.premultiplied_bgra.begin(),
                                  frame.premultiplied_bgra.end()),
                              delay});
                         animation_duration_ms_ += delay;
                         return true;
                       });
        if (frames_.empty())
          throw std::runtime_error("GIF preview contains no frames");
        animation_started_ = std::chrono::steady_clock::now();
        mode_ = Mode::Gif;
        return;
      }

      const RgbImage decoded = decode_static_image(path, extension);
      width_ = decoded.width;
      height_ = decoded.height;
      pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4);
      for (std::size_t source = 0, destination = 0;
           source < decoded.pixels.size(); source += 3, destination += 4) {
        pixels_[destination + 0] = decoded.pixels[source + 2];
        pixels_[destination + 1] = decoded.pixels[source + 1];
        pixels_[destination + 2] = decoded.pixels[source + 0];
        pixels_[destination + 3] = 255;
      }
      mode_ = Mode::Static;
    } catch (const std::exception &) {
      clear();
      mode_ = Mode::Unavailable;
    }
  }

  bool advance(std::chrono::steady_clock::time_point now) {
    if (mode_ == Mode::Video) {
      if (auto frame = video_.take_latest()) {
        width_ = frame->width;
        height_ = frame->height;
        pixels_ = std::move(frame->premultiplied_bgra);
        bitmap_.Reset();
        bitmap_owner_ = nullptr;
        return true;
      }
      if (video_.failed()) {
        video_.stop();
        pixels_.clear();
        width_ = 0;
        height_ = 0;
        mode_ = Mode::Unavailable;
        bitmap_.Reset();
        bitmap_owner_ = nullptr;
        return true;
      }
      return false;
    }

    if (mode_ != Mode::Gif || frames_.size() < 2 ||
        animation_duration_ms_ == 0)
      return false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - animation_started_)
                             .count();
    const auto position = static_cast<std::uint64_t>(elapsed) %
                          animation_duration_ms_;
    std::uint64_t boundary = 0;
    std::size_t next = 0;
    for (; next + 1 < frames_.size(); ++next) {
      boundary += frames_[next].delay_ms;
      if (position < boundary)
        break;
    }
    if (next == current_frame_)
      return false;
    current_frame_ = next;
    bitmap_.Reset();
    bitmap_owner_ = nullptr;
    return true;
  }

  void discard_device_resources() {
    bitmap_.Reset();
    bitmap_owner_ = nullptr;
  }

  void paint(ID2D1HwndRenderTarget *target, IDWriteTextFormat *format,
             D2D1_RECT_F rect) {
    ComPtr<ID2D1SolidColorBrush> panel_brush;
    require_hresult(target->CreateSolidColorBrush(color(kPanel), &panel_brush),
                    "Direct2D could not create preview brush");
    target->FillRectangle(rect, panel_brush.Get());

    const auto *data = current_pixels();
    if (data && !data->empty() && width_ > 0 && height_ > 0) {
      if (!bitmap_ || bitmap_owner_ != target) {
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        require_hresult(target->CreateBitmap(
                            D2D1::SizeU(static_cast<UINT32>(width_),
                                       static_cast<UINT32>(height_)),
                            data->data(), static_cast<UINT32>(width_ * 4),
                            properties, &bitmap_),
                        "Direct2D could not create preview bitmap");
        bitmap_owner_ = target;
      }
      const PreviewRect fitted = fit_preview_rect(
          width_, height_,
          {rect.left, rect.top, rect.right, rect.bottom});
      target->DrawBitmap(bitmap_.Get(),
                         D2D1::RectF(fitted.left, fitted.top, fitted.right,
                                     fitted.bottom),
                         1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      return;
    }

    const wchar_t *message = mode_ == Mode::Unavailable
                                 ? L"Preview unavailable"
                                 : L"Preparing preview...";
    ComPtr<ID2D1SolidColorBrush> text_brush;
    require_hresult(target->CreateSolidColorBrush(color(kMuted), &text_brush),
                    "Direct2D could not create preview text brush");
    target->DrawText(message, static_cast<UINT32>(wcslen(message)), format,
                     rect, text_brush.Get());
  }

private:
  enum class Mode { Empty, Static, Gif, Video, Unavailable };
  struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint64_t delay_ms{};
  };

  Mode mode_{Mode::Empty};
  int width_{};
  int height_{};
  std::vector<std::uint8_t> pixels_;
  std::vector<Frame> frames_;
  std::size_t current_frame_{};
  std::uint64_t animation_duration_ms_{};
  std::chrono::steady_clock::time_point animation_started_{};
  VideoPreview video_;
  ComPtr<ID2D1Bitmap> bitmap_;
  ID2D1RenderTarget *bitmap_owner_{};

  const std::vector<std::uint8_t> *current_pixels() const {
    if (mode_ == Mode::Static || mode_ == Mode::Video)
      return &pixels_;
    if (mode_ == Mode::Gif && current_frame_ < frames_.size())
      return &frames_[current_frame_].pixels;
    return nullptr;
  }
};

MainWindow::MainWindow(HINSTANCE instance, std::shared_ptr<Engine> engine)
    : instance_(instance), engine_(std::move(engine)),
      left_media_(std::make_unique<MediaPane>()),
      right_media_(std::make_unique<MediaPane>()) {
  create_device_independent_resources();
  create_window();
  create_controls();
  SetTimer(window_, kAnimationTimer, kAnimationIntervalMs, nullptr);
  layout();
  set_page(Page::Loading);
  start();
}

MainWindow::~MainWindow() {
  if (active_.joinable())
    active_.join();
  left_media_->clear();
  right_media_->clear();
  discard_render_target();
  if (window_ && IsWindow(window_))
    DestroyWindow(window_);
}

int MainWindow::run(int show_command) {
  ShowWindow(window_, show_command);
  UpdateWindow(window_);
  MSG message{};
  while (true) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == 0)
      return static_cast<int>(message.wParam);
    if (result == -1)
      throw std::runtime_error("Windows message loop failed");
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

void MainWindow::create_device_independent_resources() {
  require_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2d_factory_.ReleaseAndGetAddressOf()),
                  "Direct2D initialization failed");
  require_hresult(DWriteCreateFactory(
                      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown **>(
                          write_factory_.ReleaseAndGetAddressOf())),
                  "DirectWrite initialization failed");

  auto create_format = [&](float size, DWRITE_FONT_WEIGHT weight,
                           DWRITE_TEXT_ALIGNMENT alignment,
                           DWRITE_PARAGRAPH_ALIGNMENT paragraph,
                           IDWriteTextFormat **output) {
    require_hresult(write_factory_->CreateTextFormat(
                        L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", output),
                    "DirectWrite could not create a text format");
    (*output)->SetTextAlignment(alignment);
    (*output)->SetParagraphAlignment(paragraph);
    (*output)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  };
  create_format(32, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                brand_format_.ReleaseAndGetAddressOf());
  create_format(30, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                title_left_format_.ReleaseAndGetAddressOf());
  create_format(30, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                title_center_format_.ReleaseAndGetAddressOf());
  create_format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                body_center_format_.ReleaseAndGetAddressOf());
  create_format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                body_left_format_.ReleaseAndGetAddressOf());
  create_format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                meta_left_format_.ReleaseAndGetAddressOf());
}

void MainWindow::create_window() {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &MainWindow::window_proc;
  window_class.hInstance = instance_;
  window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = kWindowClass;
  window_class.hIconSm = window_class.hIcon;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    throw std::runtime_error("Windows could not register the gdupe window");

  dpi_ = GetDpiForSystem();
  RECT bounds{0, 0, MulDiv(1320, static_cast<int>(dpi_), 96),
              MulDiv(820, static_cast<int>(dpi_), 96)};
  AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
  window_ = CreateWindowExW(
      0, kWindowClass, L"gdupe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
      nullptr, nullptr, instance_, this);
  if (!window_)
    throw std::runtime_error("Windows could not create the gdupe window");
  dpi_ = GetDpiForWindow(window_);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
}

void MainWindow::create_controls() {
  auto create_button = [&](int id, const wchar_t *label) {
    HWND control = CreateWindowExW(
        0, L"BUTTON", label,
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY, 0, 0, 1, 1, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (!control)
      throw std::runtime_error("Windows could not create a gdupe button");
    return control;
  };
  process_all_ = create_button(kProcessAll, L"Process all");
  delete_left_ = create_button(kDeleteLeft, L"Delete left");
  keep_both_ = create_button(kKeepBoth, L"Keep both");
  delete_right_ = create_button(kDeleteRight, L"Delete right");
  retry_ = create_button(kRetry, L"Retry synchronization");
}

LRESULT CALLBACK MainWindow::window_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  MainWindow *self =
      reinterpret_cast<MainWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    self = static_cast<MainWindow *>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->handle_message(message, wparam, lparam)
              : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT MainWindow::handle_message(UINT message, WPARAM wparam,
                                   LPARAM lparam) {
  switch (message) {
  case WM_GETMINMAXINFO: {
    auto *minimum = reinterpret_cast<MINMAXINFO *>(lparam);
    RECT bounds{0, 0, MulDiv(980, static_cast<int>(dpi_), 96),
                MulDiv(650, static_cast<int>(dpi_), 96)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
    minimum->ptMinTrackSize.x = bounds.right - bounds.left;
    minimum->ptMinTrackSize.y = bounds.bottom - bounds.top;
    return 0;
  }
  case WM_DPICHANGED: {
    dpi_ = HIWORD(wparam);
    const auto *suggested = reinterpret_cast<RECT *>(lparam);
    SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    if (render_target_)
      render_target_->SetDpi(static_cast<float>(dpi_),
                             static_cast<float>(dpi_));
    layout();
    invalidate();
    return 0;
  }
  case WM_SIZE:
    if (render_target_)
      render_target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
    layout();
    invalidate();
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT paint_state{};
    BeginPaint(window_, &paint_state);
    try {
      paint();
    } catch (const std::exception &problem) {
      show_error(problem.what());
    }
    EndPaint(window_, &paint_state);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_DRAWITEM:
    draw_button(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam));
    return TRUE;
  case WM_COMMAND:
    if (HIWORD(wparam) == BN_CLICKED) {
      switch (LOWORD(wparam)) {
      case kProcessAll:
        process_all();
        break;
      case kDeleteLeft:
        delete_side(true);
        break;
      case kKeepBoth:
        exclude_current();
        break;
      case kDeleteRight:
        delete_side(false);
        break;
      case kRetry:
        start();
        break;
      default:
        break;
      }
    }
    return 0;
  case WM_TIMER:
    if (wparam == kAnimationTimer && page_ == Page::Review) {
      const auto now = std::chrono::steady_clock::now();
      const bool left_changed = left_media_->advance(now);
      const bool right_changed = right_media_->advance(now);
      if (left_changed || right_changed)
        invalidate();
    }
    return 0;
  case WM_CLOSE:
    if (busy_) {
      phase_text_ = L"Finishing the current safe checkpoint...";
      invalidate();
      return 0;
    }
    DestroyWindow(window_);
    return 0;
  case WM_DESTROY:
    closing_ = true;
    KillTimer(window_, kAnimationTimer);
    left_media_->clear();
    right_media_->clear();
    PostQuitMessage(0);
    return 0;
  case kUiTaskMessage: {
    std::unique_ptr<std::function<void()>> task(
        reinterpret_cast<std::function<void()> *>(lparam));
    (*task)();
    return 0;
  }
  default:
    return DefWindowProcW(window_, message, wparam, lparam);
  }
}

void MainWindow::ensure_render_target() {
  if (render_target_)
    return;
  RECT client{};
  GetClientRect(window_, &client);
  require_hresult(
      d2d_factory_->CreateHwndRenderTarget(
          D2D1::RenderTargetProperties(),
          D2D1::HwndRenderTargetProperties(
              window_, D2D1::SizeU(client.right - client.left,
                                   client.bottom - client.top)),
          render_target_.ReleaseAndGetAddressOf()),
      "Direct2D could not create the window render target");
  render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
}

void MainWindow::discard_render_target() {
  left_media_->discard_device_resources();
  right_media_->discard_device_resources();
  render_target_.Reset();
  control_target_.Reset();
}

void MainWindow::layout() {
  if (!window_)
    return;
  RECT client{};
  GetClientRect(window_, &client);
  const float width = (client.right - client.left) * 96.0F / dpi_;
  const float height = (client.bottom - client.top) * 96.0F / dpi_;
  const Layout positions = make_layout(width, height);
  move_control(process_all_, positions.process_all, dpi_);
  move_control(delete_left_, positions.delete_left, dpi_);
  move_control(keep_both_, positions.keep_both, dpi_);
  move_control(delete_right_, positions.delete_right, dpi_);
  move_control(retry_, positions.retry, dpi_);
}

void MainWindow::paint() {
  ensure_render_target();
  const D2D1_SIZE_F size = render_target_->GetSize();
  const Layout positions = make_layout(size.width, size.height);
  ComPtr<ID2D1SolidColorBrush> foreground;
  ComPtr<ID2D1SolidColorBrush> muted;
  ComPtr<ID2D1SolidColorBrush> button;
  require_hresult(render_target_->CreateSolidColorBrush(color(kForeground),
                                                        &foreground),
                  "Direct2D could not create foreground brush");
  require_hresult(render_target_->CreateSolidColorBrush(color(kMuted), &muted),
                  "Direct2D could not create muted brush");
  require_hresult(render_target_->CreateSolidColorBrush(color(kButton), &button),
                  "Direct2D could not create progress brush");

  auto text = [&](const std::wstring &value, IDWriteTextFormat *format,
                  D2D1_RECT_F rect, ID2D1Brush *brush) {
    render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()),
                             format, rect, brush,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
  };

  render_target_->BeginDraw();
  render_target_->Clear(color(kBackground));
  if (page_ == Page::Loading) {
    const float center = size.width / 2.0F;
    const float middle = size.height / 2.0F;
    text(L"gdupe", brand_format_.Get(),
         D2D1::RectF(center - 200, middle - 100, center + 200, middle - 56),
         foreground.Get());
    text(phase_text_, body_center_format_.Get(),
         D2D1::RectF(center - 300, middle - 45, center + 300, middle - 15),
         foreground.Get());
    const auto bar = D2D1::RectF(center - 180, middle, center + 180, middle + 8);
    render_target_->FillRectangle(bar, button.Get());
    if (progress_ > 0) {
      auto filled = bar;
      filled.right = filled.left +
                     (filled.right - filled.left) *
                         static_cast<float>(std::clamp(progress_, 0.0, 1.0));
      render_target_->FillRectangle(filled, foreground.Get());
    }
    text(progress_text_, body_center_format_.Get(),
         D2D1::RectF(center - 200, middle + 18, center + 200, middle + 42),
         muted.Get());
  } else if (page_ == Page::Review) {
    text(L"Review", title_left_format_.Get(), D2D1::RectF(28, 18, 153, 63),
         foreground.Get());
    text(count_text_, body_left_format_.Get(),
         D2D1::RectF(155, 25, 515, 57), muted.Get());
    text(evidence_text_, body_center_format_.Get(),
         D2D1::RectF(28, 72, size.width - 28, 100), muted.Get());
    left_media_->paint(render_target_.Get(), body_center_format_.Get(),
                       positions.left_media);
    right_media_->paint(render_target_.Get(), body_center_format_.Get(),
                        positions.right_media);
    const float detail_y = positions.left_media.bottom + 10;
    text(left_detail_text_, meta_left_format_.Get(),
         D2D1::RectF(positions.left_media.left, detail_y,
                     positions.left_media.right, detail_y + 72),
         foreground.Get());
    text(right_detail_text_, meta_left_format_.Get(),
         D2D1::RectF(positions.right_media.left, detail_y,
                     positions.right_media.right, detail_y + 72),
         foreground.Get());
  } else if (page_ == Page::Done) {
    const float center = size.width / 2.0F;
    const float middle = size.height / 2.0F;
    text(L"All clean", title_center_format_.Get(),
         D2D1::RectF(center - 250, middle - 55, center + 250, middle - 13),
         foreground.Get());
    text(L"There are no conservative duplicate candidates left to review.",
         body_center_format_.Get(),
         D2D1::RectF(center - 360, middle, center + 360, middle + 32),
         muted.Get());
  } else {
    const float center = size.width / 2.0F;
    const float middle = size.height / 2.0F;
    text(L"gdupe paused safely", title_center_format_.Get(),
         D2D1::RectF(center - 300, middle - 120, center + 300, middle - 75),
         foreground.Get());
    text(error_text_, body_center_format_.Get(),
         D2D1::RectF(center - 340, middle - 60, center + 340, middle + 40),
         muted.Get());
  }
  const HRESULT result = render_target_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET)
    discard_render_target();
  else
    require_hresult(result, "Direct2D could not draw the gdupe window");
}

void MainWindow::draw_button(const DRAWITEMSTRUCT &item) {
  if (!control_target_) {
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE));
    require_hresult(d2d_factory_->CreateDCRenderTarget(
                        &properties, control_target_.ReleaseAndGetAddressOf()),
                    "Direct2D could not create a button render target");
  }
  control_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  require_hresult(control_target_->BindDC(item.hDC, &item.rcItem),
                  "Direct2D could not bind a button surface");
  const D2D1_SIZE_F size = control_target_->GetSize();
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool danger = item.CtlID == kDeleteLeft || item.CtlID == kDeleteRight;
  const std::uint32_t base = danger ? kDanger
                                    : item.CtlID == kProcessAll ||
                                              item.CtlID == kRetry
                                          ? kPrimary
                                          : kButton;
  const std::uint32_t ink = base == kPrimary ? kBackground : kForeground;
  ComPtr<ID2D1SolidColorBrush> background;
  ComPtr<ID2D1SolidColorBrush> foreground;
  require_hresult(control_target_->CreateSolidColorBrush(
                      color(base, pressed ? 0.78F : 1.0F), &background),
                  "Direct2D could not create button background");
  require_hresult(control_target_->CreateSolidColorBrush(color(ink), &foreground),
                  "Direct2D could not create button foreground");
  wchar_t label[128]{};
  const int length = GetWindowTextW(item.hwndItem, label,
                                    static_cast<int>(std::size(label)));
  control_target_->BeginDraw();
  control_target_->Clear(color(base, pressed ? 0.78F : 1.0F));
  control_target_->FillRectangle(D2D1::RectF(0, 0, size.width, size.height),
                                 background.Get());
  control_target_->DrawText(label, static_cast<UINT32>(std::max(0, length)),
                            body_center_format_.Get(),
                            D2D1::RectF(0, 0, size.width, size.height),
                            foreground.Get());
  if ((item.itemState & ODS_FOCUS) != 0)
    control_target_->DrawRectangle(
        D2D1::RectF(2, 2, size.width - 2, size.height - 2), foreground.Get(),
        1.0F);
  const HRESULT result = control_target_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET)
    control_target_.Reset();
}

void MainWindow::set_page(Page page) {
  page_ = page;
  const bool review = page == Page::Review;
  const bool error = page == Page::Error;
  for (HWND button : {delete_left_, keep_both_, delete_right_, process_all_})
    ShowWindow(button, review ? SW_SHOW : SW_HIDE);
  ShowWindow(retry_, error ? SW_SHOW : SW_HIDE);
  if (review)
    layout();
  invalidate();
}

void MainWindow::invalidate() {
  if (window_)
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::post_ui(std::function<void()> function) {
  auto task = std::make_unique<std::function<void()>>(std::move(function));
  if (PostMessageW(window_, kUiTaskMessage, 0,
                   reinterpret_cast<LPARAM>(task.get())))
    task.release();
}

void MainWindow::show_loading(const std::string &phase) {
  left_media_->clear();
  right_media_->clear();
  phase_text_ = utf8_to_wide(phase);
  progress_text_.clear();
  progress_ = 0.0;
  set_page(Page::Loading);
}

void MainWindow::update_progress(const std::string &phase,
                                 std::size_t completed, std::size_t total) {
  phase_text_ = utf8_to_wide(phase);
  if (total > 0) {
    progress_ = static_cast<double>(completed) / total;
    progress_text_ = std::to_wstring(completed) + L" of " +
                     std::to_wstring(total);
  } else {
    progress_ = 0.0;
    progress_text_.clear();
  }
  invalidate();
}

void MainWindow::launch(std::function<void(Engine::Progress)> work,
                        std::function<void()> success) {
  if (busy_)
    return;
  busy_ = true;
  active_ = std::jthread(
      [this, work = std::move(work), success = std::move(success)]() mutable {
        try {
          Engine::Progress progress = [this](const std::string &phase,
                                             std::size_t completed,
                                             std::size_t total) {
            post_ui([this, phase, completed, total] {
              update_progress(phase, completed, total);
            });
          };
          work(std::move(progress));
          post_ui([this, success = std::move(success)]() mutable {
            busy_ = false;
            success();
          });
        } catch (const std::exception &problem) {
          const std::string message = problem.what();
          post_ui([this, message] {
            busy_ = false;
            show_error(message);
          });
        }
      });
}

void MainWindow::start() {
  show_loading("Synchronizing the library...");
  launch(
      [engine = engine_](Engine::Progress progress) {
        engine->startup(std::move(progress));
      },
      [this] { refresh_queue(); });
}

void MainWindow::refresh_queue() {
  queue_ = engine_->queue();
  if (queue_.empty()) {
    left_media_->clear();
    right_media_->clear();
    set_page(Page::Done);
    return;
  }
  load_current_preview();
}

void MainWindow::load_current_preview() {
  const auto card = queue_.front();
  show_loading("Preparing this comparison...");
  auto paths = std::make_shared<
      std::pair<std::filesystem::path, std::filesystem::path>>();
  launch(
      [engine = engine_, card, paths](Engine::Progress) {
        paths->first = engine->materialize(card.left.remote.key);
        paths->second = engine->materialize(card.right.remote.key);
      },
      [this, card, paths] {
        if (queue_.empty() || queue_.front().generation != card.generation)
          return;
        left_media_->show_file(paths->first, card.left.remote.extension,
                               card.left.fingerprint->kind);
        right_media_->show_file(paths->second, card.right.remote.extension,
                                card.right.fingerprint->kind);
        count_text_ = std::to_wstring(queue_.size()) +
                      (queue_.size() == 1 ? L" comparison remaining"
                                         : L" comparisons remaining");
        evidence_text_ = std::to_wstring(
                             static_cast<int>(std::round(card.score * 100.0))) +
                         L"% similar  -  " + utf8_to_wide(card.evidence);
        left_detail_text_ = details(card.left);
        right_detail_text_ = details(card.right);
        set_page(Page::Review);
      });
}

void MainWindow::delete_side(bool left) {
  if (queue_.empty())
    return;
  const auto card = queue_.front();
  const auto &target = left ? card.left : card.right;
  show_loading("Deleting and consolidating...");
  launch(
      [engine = engine_, key = target.remote.key, id = target.remote.file_id,
       generation = card.generation](Engine::Progress progress) {
        engine->delete_object(key, id, generation, std::move(progress));
      },
      [this] { refresh_queue(); });
}

void MainWindow::exclude_current() {
  if (queue_.empty())
    return;
  const auto card = queue_.front();
  show_loading("Keeping both...");
  launch(
      [engine = engine_, card](Engine::Progress progress) {
        engine->exclude_pair(card.left.remote.key, card.right.remote.key,
                             card.generation, std::move(progress));
      },
      [this] { refresh_queue(); });
}

void MainWindow::process_all() {
  if (queue_.empty())
    return;
  const auto generation = queue_.front().generation;
  show_loading("Processing the remaining queue...");
  launch(
      [engine = engine_, generation](Engine::Progress progress) {
        engine->process_all(generation, std::move(progress));
      },
      [this] { refresh_queue(); });
}

void MainWindow::show_error(const std::string &message) {
  error_text_ = utf8_to_wide(
      message + "\n\nNo further destructive work will run until "
                "synchronization succeeds.");
  set_page(Page::Error);
}

} // namespace gdupe
