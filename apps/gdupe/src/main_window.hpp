#pragma once

#include "engine.hpp"

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gdupe {

class MainWindow final {
public:
  MainWindow(HINSTANCE instance, std::shared_ptr<Engine> engine);
  ~MainWindow();

  MainWindow(const MainWindow &) = delete;
  MainWindow &operator=(const MainWindow &) = delete;

  int run(int show_command);

private:
  class MediaPane;
  enum class Page { Loading, Review, Done, Error };

  HINSTANCE instance_{};
  HWND window_{};
  HWND delete_left_{};
  HWND keep_both_{};
  HWND delete_right_{};
  HWND process_all_{};
  HWND retry_{};
  UINT dpi_{96};
  Page page_{Page::Loading};
  std::shared_ptr<Engine> engine_;
  std::unique_ptr<MediaPane> left_media_;
  std::unique_ptr<MediaPane> right_media_;
  std::vector<ReviewPair> queue_;
  std::jthread active_;
  bool busy_{};
  bool closing_{};
  bool close_requested_{};

  std::wstring phase_text_{L"Opening the library..."};
  std::wstring progress_text_;
  std::wstring count_text_;
  std::wstring evidence_text_;
  std::wstring left_detail_text_;
  std::wstring right_detail_text_;
  std::wstring error_text_;
  double progress_{};

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> write_factory_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> control_target_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> brand_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> title_left_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> title_center_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> body_center_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> body_left_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> meta_left_format_;

  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

  void create_window();
  void create_controls();
  void create_device_independent_resources();
  void ensure_render_target();
  void discard_render_target();
  void layout();
  void paint();
  void draw_button(const DRAWITEMSTRUCT &item);
  void set_page(Page page);
  void invalidate();
  void post_ui(std::function<void()> function);

  void start();
  void refresh_queue();
  void load_current_preview();
  void show_error(const std::string &message);
  void show_loading(const std::string &phase);
  void update_progress(const std::string &phase, std::size_t completed,
                       std::size_t total);
  void launch(std::function<void(Engine::Progress)> work,
              std::function<void()> success);
  void delete_side(bool left);
  void exclude_current();
  void process_all();
};

} // namespace gdupe
