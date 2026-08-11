#pragma once

#include "engine.hpp"

#include <functional>
#include <memory>
#include <vector>

#include <QFuture>
#include <QMainWindow>

class QLabel;
class QPushButton;
class QProgressBar;
class QStackedWidget;
class QCloseEvent;

namespace gdupe {

class MediaPane;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(std::shared_ptr<Engine> engine,
                      QWidget *parent = nullptr);
  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  std::shared_ptr<Engine> engine_;
  QStackedWidget *pages_{};
  QWidget *loading_page_{};
  QWidget *review_page_{};
  QWidget *done_page_{};
  QWidget *error_page_{};
  QLabel *phase_label_{};
  QLabel *progress_label_{};
  QProgressBar *progress_bar_{};
  QLabel *count_label_{};
  QLabel *evidence_label_{};
  QLabel *left_detail_{};
  QLabel *right_detail_{};
  QLabel *error_label_{};
  MediaPane *left_media_{};
  MediaPane *right_media_{};
  QPushButton *delete_left_{};
  QPushButton *keep_both_{};
  QPushButton *delete_right_{};
  QPushButton *process_all_{};
  QPushButton *retry_{};
  std::vector<ReviewPair> queue_;
  QFuture<void> active_;
  bool busy_{};

  void build_ui();
  void start();
  void refresh_queue();
  void load_current_preview();
  void show_error(const QString &message);
  void show_loading(const QString &phase);
  void update_progress(const std::string &phase, std::size_t completed,
                       std::size_t total);
  void launch(std::function<void(Engine::Progress)> work,
              std::function<void()> success);
  void delete_side(bool left);
  void exclude_current();
  void process_all();
};

} // namespace gdupe
