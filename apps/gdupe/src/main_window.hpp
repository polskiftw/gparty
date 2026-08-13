#pragma once

#include "engine.hpp"

#include <FL/Fl_Double_Window.H>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class Fl_Box;
class Fl_Button;
class Fl_Group;
class Fl_Progress;
class Fl_Wizard;

namespace gdupe {

class MediaPane;

class MainWindow final : public Fl_Double_Window {
public:
  explicit MainWindow(std::shared_ptr<Engine> engine);
  ~MainWindow() override;

  int handle(int event) override;
  void resize(int x, int y, int width, int height) override;

private:
  std::shared_ptr<Engine> engine_;
  Fl_Wizard *pages_{};
  Fl_Group *loading_page_{};
  Fl_Group *review_page_{};
  Fl_Group *done_page_{};
  Fl_Group *error_page_{};
  Fl_Box *brand_{};
  Fl_Box *phase_label_{};
  Fl_Box *progress_label_{};
  Fl_Progress *progress_bar_{};
  Fl_Box *review_title_{};
  Fl_Box *count_label_{};
  Fl_Box *evidence_label_{};
  Fl_Box *left_detail_{};
  Fl_Box *right_detail_{};
  Fl_Box *done_title_{};
  Fl_Box *done_text_{};
  Fl_Box *error_title_{};
  Fl_Box *error_label_{};
  MediaPane *left_media_{};
  MediaPane *right_media_{};
  Fl_Button *delete_left_{};
  Fl_Button *keep_both_{};
  Fl_Button *delete_right_{};
  Fl_Button *process_all_{};
  Fl_Button *retry_{};
  std::vector<ReviewPair> queue_;
  std::jthread active_;
  bool busy_{};

  void build_ui();
  void layout();
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
