#include "main_window.hpp"

#include "image_decode.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Anim_GIF_Image.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Wizard.H>
#ifdef _WIN32
#include <FL/x.H>
#include <mfplay.h>
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gdupe {
namespace {

constexpr Fl_Color background = 0x0b0c0f00;
constexpr Fl_Color panel = 0x15161b00;
constexpr Fl_Color foreground = 0xf5f5f700;
constexpr Fl_Color muted = 0x9a9aa200;
constexpr Fl_Color button = 0x24262e00;
constexpr Fl_Color danger = 0x3a171b00;
constexpr Fl_Color primary = 0xe9e9ec00;

std::string utf8_path(const std::filesystem::path &path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::string details(const InventoryObject &item) {
  if (!item.fingerprint)
    return item.remote.key;
  const auto &fingerprint = *item.fingerprint;
  const char *type = fingerprint.kind == MediaKind::StaticImage
                         ? "image"
                     : fingerprint.kind == MediaKind::AnimatedImage
                         ? "animated GIF"
                         : "video";
  std::ostringstream text;
  text << item.remote.key << '\n' << fingerprint.width << " x "
       << fingerprint.height << "  -  " << type << "  -  " << std::fixed
       << std::setprecision(1) << item.remote.size / (1024.0 * 1024.0)
       << " MiB";
  if (fingerprint.duration_ms > 0)
    text << "  -  " << fingerprint.duration_ms / 1000.0 << " s";
  return text.str();
}

void style_label(Fl_Box *label, int size, Fl_Color color,
                 Fl_Align align = FL_ALIGN_CENTER) {
  label->labelsize(size);
  label->labelcolor(color);
  label->align(align | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
}

void style_button(Fl_Button *value, Fl_Color color, Fl_Color text) {
  value->box(FL_FLAT_BOX);
  value->color(color);
  value->selection_color(fl_lighter(color));
  value->labelcolor(text);
  value->labelsize(14);
}

struct UiTask {
  std::function<void()> function;
};

void run_ui_task(void *opaque) {
  std::unique_ptr<UiTask> task(static_cast<UiTask *>(opaque));
  task->function();
}

void post_ui(std::function<void()> function) {
  Fl::awake(run_ui_task, new UiTask{std::move(function)});
}

} // namespace

class MediaPane final : public Fl_Box {
public:
  MediaPane(int x, int y, int width, int height)
      : Fl_Box(x, y, width, height, "Preparing preview...") {
    box(FL_FLAT_BOX);
    color(panel);
    labelcolor(muted);
    labelsize(15);
    align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  }

  ~MediaPane() override { clear(); }

  void clear() {
#ifdef _WIN32
    if (player_) {
      player_->Shutdown();
      player_->Release();
      player_ = nullptr;
    }
    if (video_window_) {
      DestroyWindow(video_window_);
      video_window_ = nullptr;
    }
#endif
    if (gif_)
      gif_->canvas(nullptr);
    image(nullptr);
    scaled_.reset();
    static_image_.reset();
    gif_.reset();
    decoded_ = {};
    copy_label("Preparing preview...");
    redraw();
  }

  void show_file(const std::filesystem::path &path,
                 const std::string &extension, MediaKind kind) {
    clear();
    try {
      if (kind == MediaKind::AnimatedImage) {
        const std::string file = utf8_path(path);
        gif_ = std::make_unique<Fl_Anim_GIF_Image>(
            file.c_str(), this, Fl_Anim_GIF_Image::DONT_RESIZE_CANVAS);
        if (gif_->fail())
          throw std::runtime_error("GIF preview decoder rejected the image");
        copy_label("");
        update_gif();
        return;
      }
      if (kind == MediaKind::Video) {
#ifdef _WIN32
        HWND parent = fl_xid(window());
        video_window_ = CreateWindowExW(
            0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_BLACKRECT, x(),
            y(), w(), h(), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!video_window_)
          throw std::runtime_error("Windows could not create a video surface");
        const HRESULT result = MFPCreateMediaPlayer(
            path.c_str(), TRUE, MFP_OPTION_NONE, nullptr, video_window_,
            &player_);
        if (FAILED(result))
          throw std::runtime_error(
              "Windows Media Foundation cannot preview this video");
        player_->SetVolume(0.0f);
        copy_label("");
        update_native_video();
        return;
#else
        throw std::runtime_error("Video preview is available only on Windows");
#endif
      }

      decoded_ = decode_static_image(path, extension);
      static_image_ = std::make_unique<Fl_RGB_Image>(
          decoded_.pixels.data(), static_cast<int>(decoded_.pixels.size()),
          decoded_.width, decoded_.height, 3, 0);
      if (static_image_->fail())
        throw std::runtime_error("FLTK could not prepare the image preview");
      copy_label("");
      update_static_image();
    } catch (const std::exception &) {
#ifdef _WIN32
      if (player_) {
        player_->Shutdown();
        player_->Release();
        player_ = nullptr;
      }
      if (video_window_) {
        DestroyWindow(video_window_);
        video_window_ = nullptr;
      }
#endif
      if (gif_)
        gif_->canvas(nullptr);
      image(nullptr);
      scaled_.reset();
      static_image_.reset();
      gif_.reset();
      decoded_ = {};
      copy_label("Preview unavailable");
      redraw();
    }
  }

  void resize(int x, int y, int width, int height) override {
    Fl_Box::resize(x, y, width, height);
    update_static_image();
    update_gif();
    update_native_video();
  }

private:
  RgbImage decoded_;
  std::unique_ptr<Fl_RGB_Image> static_image_;
  std::unique_ptr<Fl_Image> scaled_;
  std::unique_ptr<Fl_Anim_GIF_Image> gif_;
#ifdef _WIN32
  HWND video_window_{};
  IMFPMediaPlayer *player_{};
#endif

  void update_static_image() {
    if (!static_image_)
      return;
    const double scale = std::min(
        static_cast<double>(std::max(1, w() - 24)) / static_image_->w(),
        static_cast<double>(std::max(1, h() - 24)) / static_image_->h());
    const int width = std::max(1, static_cast<int>(static_image_->w() * scale));
    const int height =
        std::max(1, static_cast<int>(static_image_->h() * scale));
    scaled_.reset(static_image_->copy(width, height));
    image(scaled_.get());
    redraw();
  }

  void update_native_video() {
#ifdef _WIN32
    if (video_window_) {
      MoveWindow(video_window_, x(), y(), w(), h(), TRUE);
      if (player_)
        player_->UpdateVideo();
    }
#endif
  }

  void update_gif() {
    if (gif_)
      gif_->scale(std::max(1, w() - 24), std::max(1, h() - 24), 1, 0);
  }
};

MainWindow::MainWindow(std::shared_ptr<Engine> engine)
    : Fl_Double_Window(1320, 820, "gdupe"), engine_(std::move(engine)) {
  color(background);
  size_range(980, 650);
  build_ui();
  start();
}

MainWindow::~MainWindow() {
  if (active_.joinable())
    active_.join();
}

void MainWindow::build_ui() {
  begin();
  pages_ = new Fl_Wizard(0, 0, w(), h());
  pages_->box(FL_FLAT_BOX);
  pages_->color(background);
  pages_->begin();

  loading_page_ = new Fl_Group(0, 0, w(), h());
  loading_page_->box(FL_FLAT_BOX);
  loading_page_->color(background);
  loading_page_->begin();
  brand_ = new Fl_Box(0, 0, 1, 1, "gdupe");
  style_label(brand_, 32, foreground);
  phase_label_ = new Fl_Box(0, 0, 1, 1, "Opening the library...");
  style_label(phase_label_, 16, foreground);
  progress_bar_ = new Fl_Progress(0, 0, 1, 1);
  progress_bar_->minimum(0.0);
  progress_bar_->maximum(1.0);
  progress_bar_->value(0.0);
  progress_bar_->color(button);
  progress_bar_->selection_color(foreground);
  progress_label_ = new Fl_Box(0, 0, 1, 1);
  style_label(progress_label_, 13, muted);
  loading_page_->end();

  review_page_ = new Fl_Group(0, 0, w(), h());
  review_page_->box(FL_FLAT_BOX);
  review_page_->color(background);
  review_page_->begin();
  review_title_ = new Fl_Box(0, 0, 1, 1, "Review");
  style_label(review_title_, 30, foreground, FL_ALIGN_LEFT);
  count_label_ = new Fl_Box(0, 0, 1, 1);
  style_label(count_label_, 14, muted, FL_ALIGN_LEFT);
  process_all_ = new Fl_Button(0, 0, 1, 1, "Process all");
  style_button(process_all_, primary, background);
  evidence_label_ = new Fl_Box(0, 0, 1, 1);
  style_label(evidence_label_, 14, muted);
  left_media_ = new MediaPane(0, 0, 1, 1);
  right_media_ = new MediaPane(0, 0, 1, 1);
  left_detail_ = new Fl_Box(0, 0, 1, 1);
  right_detail_ = new Fl_Box(0, 0, 1, 1);
  style_label(left_detail_, 13, foreground,
              FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  style_label(right_detail_, 13, foreground,
              FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  delete_left_ = new Fl_Button(0, 0, 1, 1, "Delete left");
  keep_both_ = new Fl_Button(0, 0, 1, 1, "Keep both");
  delete_right_ = new Fl_Button(0, 0, 1, 1, "Delete right");
  style_button(delete_left_, danger, foreground);
  style_button(keep_both_, button, foreground);
  style_button(delete_right_, danger, foreground);
  review_page_->end();

  done_page_ = new Fl_Group(0, 0, w(), h());
  done_page_->box(FL_FLAT_BOX);
  done_page_->color(background);
  done_page_->begin();
  done_title_ = new Fl_Box(0, 0, 1, 1, "All clean");
  style_label(done_title_, 30, foreground);
  done_text_ = new Fl_Box(
      0, 0, 1, 1,
      "There are no conservative duplicate candidates left to review.");
  style_label(done_text_, 14, muted);
  done_page_->end();

  error_page_ = new Fl_Group(0, 0, w(), h());
  error_page_->box(FL_FLAT_BOX);
  error_page_->color(background);
  error_page_->begin();
  error_title_ = new Fl_Box(0, 0, 1, 1, "gdupe paused safely");
  style_label(error_title_, 30, foreground);
  error_label_ = new Fl_Box(0, 0, 1, 1);
  style_label(error_label_, 14, muted, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  retry_ = new Fl_Button(0, 0, 1, 1, "Retry synchronization");
  style_button(retry_, primary, background);
  error_page_->end();

  pages_->end();
  end();
  resizable(pages_);

  delete_left_->callback(
      [](Fl_Widget *, void *context) {
        static_cast<MainWindow *>(context)->delete_side(true);
      },
      this);
  delete_right_->callback(
      [](Fl_Widget *, void *context) {
        static_cast<MainWindow *>(context)->delete_side(false);
      },
      this);
  keep_both_->callback(
      [](Fl_Widget *, void *context) {
        static_cast<MainWindow *>(context)->exclude_current();
      },
      this);
  process_all_->callback(
      [](Fl_Widget *, void *context) {
        static_cast<MainWindow *>(context)->process_all();
      },
      this);
  retry_->callback(
      [](Fl_Widget *, void *context) {
        static_cast<MainWindow *>(context)->start();
      },
      this);
  layout();
}

void MainWindow::layout() {
  const int width = w();
  const int height = h();
  pages_->resize(0, 0, width, height);
  for (Fl_Group *page :
       {loading_page_, review_page_, done_page_, error_page_})
    page->resize(0, 0, width, height);

  const int center = width / 2;
  brand_->resize(center - 200, height / 2 - 100, 400, 44);
  phase_label_->resize(center - 300, height / 2 - 45, 600, 30);
  progress_bar_->resize(center - 180, height / 2, 360, 8);
  progress_label_->resize(center - 200, height / 2 + 18, 400, 24);

  constexpr int margin = 28;
  constexpr int gap = 18;
  review_title_->resize(margin, 18, 125, 45);
  count_label_->resize(155, 25, 360, 32);
  process_all_->resize(width - margin - 140, 20, 140, 42);
  evidence_label_->resize(margin, 72, width - margin * 2, 28);
  const int pane_y = 110;
  const int pane_width = (width - margin * 2 - gap) / 2;
  const int pane_height = std::max(260, height - 300);
  left_media_->resize(margin, pane_y, pane_width, pane_height);
  right_media_->resize(margin + pane_width + gap, pane_y, pane_width,
                       pane_height);
  const int detail_y = pane_y + pane_height + 10;
  left_detail_->resize(margin, detail_y, pane_width, 72);
  right_detail_->resize(margin + pane_width + gap, detail_y, pane_width, 72);
  const int action_y = height - 66;
  delete_left_->resize(margin, action_y, 140, 42);
  keep_both_->resize(center - 70, action_y, 140, 42);
  delete_right_->resize(width - margin - 140, action_y, 140, 42);

  done_title_->resize(center - 250, height / 2 - 55, 500, 42);
  done_text_->resize(center - 360, height / 2, 720, 32);
  error_title_->resize(center - 300, height / 2 - 120, 600, 45);
  error_label_->resize(center - 340, height / 2 - 60, 680, 100);
  retry_->resize(center - 100, height / 2 + 60, 200, 44);
}

void MainWindow::resize(int x, int y, int width, int height) {
  Fl_Double_Window::resize(x, y, width, height);
  if (pages_)
    layout();
}

int MainWindow::handle(int event) {
  if (event == FL_CLOSE) {
    if (busy_) {
      phase_label_->copy_label("Finishing the current safe checkpoint...");
      return 1;
    }
    hide();
    return 1;
  }
  return Fl_Double_Window::handle(event);
}

void MainWindow::show_loading(const std::string &phase) {
  left_media_->clear();
  right_media_->clear();
  phase_label_->copy_label(phase.c_str());
  progress_label_->copy_label("");
  progress_bar_->value(0.0);
  pages_->value(loading_page_);
  redraw();
}

void MainWindow::update_progress(const std::string &phase,
                                 std::size_t completed, std::size_t total) {
  phase_label_->copy_label(phase.c_str());
  if (total > 0) {
    progress_bar_->value(static_cast<double>(completed) / total);
    const std::string count = std::to_string(completed) + " of " +
                              std::to_string(total);
    progress_label_->copy_label(count.c_str());
  } else {
    progress_bar_->value(0.0);
    progress_label_->copy_label("");
  }
  redraw();
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
    pages_->value(done_page_);
    redraw();
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
        const std::string count = std::to_string(queue_.size()) +
                                  (queue_.size() == 1
                                       ? " comparison remaining"
                                       : " comparisons remaining");
        count_label_->copy_label(count.c_str());
        const std::string evidence =
            std::to_string(static_cast<int>(std::round(card.score * 100.0))) +
            "% similar  -  " + card.evidence;
        evidence_label_->copy_label(evidence.c_str());
        const std::string left = details(card.left);
        const std::string right = details(card.right);
        left_detail_->copy_label(left.c_str());
        right_detail_->copy_label(right.c_str());
        pages_->value(review_page_);
        redraw();
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
  const std::string text =
      message + "\n\nNo further destructive work will run until "
                "synchronization succeeds.";
  error_label_->copy_label(text.c_str());
  pages_->value(error_page_);
  redraw();
}

} // namespace gdupe
