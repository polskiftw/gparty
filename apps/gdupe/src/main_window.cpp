#include "main_window.hpp"

#include <QApplication>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QFileInfo>
#include <QFrame>
#include <QFuture>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QMovie>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace gdupe {

class MediaPane final : public QFrame {
public:
  explicit MediaPane(QWidget *parent = nullptr) : QFrame(parent) {
    setObjectName("mediaPane");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    stack_ = new QStackedWidget(this);
    placeholder_ = new QLabel("Preparing preview…", stack_);
    placeholder_->setAlignment(Qt::AlignCenter);
    image_ = new QLabel(stack_);
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumSize(300, 300);
    image_->setScaledContents(false);
    video_ = new QVideoWidget(stack_);
    video_->setMinimumSize(300, 300);
    stack_->addWidget(placeholder_);
    stack_->addWidget(image_);
    stack_->addWidget(video_);
    layout->addWidget(stack_);
  }
  ~MediaPane() override { clear(); }
  void clear() {
    if (movie_) {
      movie_->stop();
      delete movie_;
      movie_ = nullptr;
    }
    if (player_) {
      player_->stop();
      delete player_;
      player_ = nullptr;
    }
    if (audio_) {
      delete audio_;
      audio_ = nullptr;
    }
    image_->clear();
    stack_->setCurrentWidget(placeholder_);
    placeholder_->setText("Preparing preview…");
  }
  void show_file(const std::filesystem::path &path, MediaKind kind) {
    clear();
    const QString file = QString::fromStdWString(path.wstring());
    if (kind == MediaKind::AnimatedImage) {
      movie_ = new QMovie(file, QByteArray(), this);
      image_->setMovie(movie_);
      stack_->setCurrentWidget(image_);
      movie_->start();
      return;
    }
    if (kind == MediaKind::Video) {
      player_ = new QMediaPlayer(this);
      audio_ = new QAudioOutput(this);
      audio_->setMuted(true);
      player_->setAudioOutput(audio_);
      player_->setVideoOutput(video_);
      player_->setSource(QUrl::fromLocalFile(file));
      player_->setLoops(QMediaPlayer::Infinite);
      stack_->setCurrentWidget(video_);
      player_->play();
      return;
    }
    pixmap_ = QPixmap(file);
    if (pixmap_.isNull()) {
      placeholder_->setText("Preview unavailable");
      return;
    }
    stack_->setCurrentWidget(image_);
    update_pixmap();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QFrame::resizeEvent(event);
    update_pixmap();
  }

private:
  void update_pixmap() {
    if (!pixmap_.isNull())
      image_->setPixmap(pixmap_.scaled(image_->size(), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
  }
  QStackedWidget *stack_{};
  QLabel *placeholder_{};
  QLabel *image_{};
  QVideoWidget *video_{};
  QMovie *movie_{};
  QMediaPlayer *player_{};
  QAudioOutput *audio_{};
  QPixmap pixmap_;
};

namespace {

QString details(const InventoryObject &item) {
  if (!item.fingerprint)
    return QString::fromStdString(item.remote.key);
  const auto &fp = *item.fingerprint;
  QString type = fp.kind == MediaKind::StaticImage     ? "image"
                 : fp.kind == MediaKind::AnimatedImage ? "animated GIF"
                                                       : "video";
  QString duration =
      fp.duration_ms > 0
          ? QString("  ·  %1 s").arg(fp.duration_ms / 1000.0, 0, 'f', 1)
          : QString{};
  return QString("%1\n%2 × %3  ·  %4  ·  %5 MiB%6")
      .arg(QString::fromStdString(item.remote.key))
      .arg(fp.width)
      .arg(fp.height)
      .arg(type)
      .arg(item.remote.size / (1024.0 * 1024.0), 0, 'f', 1)
      .arg(duration);
}

QPushButton *action_button(const QString &text, QWidget *parent) {
  auto *button = new QPushButton(text, parent);
  button->setMinimumHeight(44);
  return button;
}

} // namespace

MainWindow::MainWindow(std::shared_ptr<Engine> engine, QWidget *parent)
    : QMainWindow(parent), engine_(std::move(engine)) {
  build_ui();
  resize(1320, 820);
  setMinimumSize(980, 650);
  setWindowTitle("gdupe");
  start();
}

MainWindow::~MainWindow() {
  if (active_.isRunning())
    active_.waitForFinished();
}

void MainWindow::build_ui() {
  setStyleSheet(R"QSS(
    QMainWindow,QWidget{background:#0b0c0f;color:#f5f5f7;font-family:"Segoe UI";font-size:14px}
    QLabel#title{font-size:30px;font-weight:600} QLabel#muted{color:#9a9aa2}
    QFrame#mediaPane{background:#15161b;border:1px solid #262830;border-radius:18px}
    QPushButton{background:#24262e;border:1px solid #343741;border-radius:11px;padding:10px 18px;font-weight:600}
    QPushButton:hover{background:#30333d} QPushButton:pressed{background:#1d1f25} QPushButton:disabled{color:#666;background:#18191d}
    QPushButton#danger{background:#3a171b;border-color:#6c282f} QPushButton#danger:hover{background:#522027}
    QPushButton#primary{background:#e9e9ec;color:#111216;border:0} QPushButton#primary:hover{background:white}
    QProgressBar{border:0;background:#22242b;border-radius:3px;height:6px} QProgressBar::chunk{background:#dadbe1;border-radius:3px}
  )QSS");
  pages_ = new QStackedWidget(this);
  setCentralWidget(pages_);

  loading_page_ = new QWidget(pages_);
  auto *loading = new QVBoxLayout(loading_page_);
  loading->setAlignment(Qt::AlignCenter);
  loading->setSpacing(18);
  auto *brand = new QLabel("gdupe", loading_page_);
  brand->setObjectName("title");
  brand->setAlignment(Qt::AlignCenter);
  phase_label_ = new QLabel("Opening the library…", loading_page_);
  phase_label_->setAlignment(Qt::AlignCenter);
  progress_label_ = new QLabel(loading_page_);
  progress_label_->setObjectName("muted");
  progress_label_->setAlignment(Qt::AlignCenter);
  progress_bar_ = new QProgressBar(loading_page_);
  progress_bar_->setFixedWidth(340);
  progress_bar_->setRange(0, 0);
  progress_bar_->setTextVisible(false);
  loading->addStretch();
  loading->addWidget(brand);
  loading->addWidget(phase_label_);
  loading->addWidget(progress_bar_, 0, Qt::AlignCenter);
  loading->addWidget(progress_label_);
  loading->addStretch();

  review_page_ = new QWidget(pages_);
  auto *review = new QVBoxLayout(review_page_);
  review->setContentsMargins(28, 22, 28, 24);
  review->setSpacing(16);
  auto *header = new QHBoxLayout;
  auto *review_title = new QLabel("Review", review_page_);
  review_title->setObjectName("title");
  count_label_ = new QLabel(review_page_);
  count_label_->setObjectName("muted");
  process_all_ = action_button("Process all", review_page_);
  process_all_->setObjectName("primary");
  header->addWidget(review_title);
  header->addWidget(count_label_);
  header->addStretch();
  header->addWidget(process_all_);
  review->addLayout(header);
  evidence_label_ = new QLabel(review_page_);
  evidence_label_->setAlignment(Qt::AlignCenter);
  evidence_label_->setObjectName("muted");
  review->addWidget(evidence_label_);
  auto *media_row = new QHBoxLayout;
  media_row->setSpacing(18);
  left_media_ = new MediaPane(review_page_);
  right_media_ = new MediaPane(review_page_);
  media_row->addWidget(left_media_, 1);
  media_row->addWidget(right_media_, 1);
  review->addLayout(media_row, 1);
  auto *details_row = new QHBoxLayout;
  left_detail_ = new QLabel(review_page_);
  right_detail_ = new QLabel(review_page_);
  left_detail_->setWordWrap(true);
  right_detail_->setWordWrap(true);
  left_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  right_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_row->addWidget(left_detail_, 1);
  details_row->addWidget(right_detail_, 1);
  review->addLayout(details_row);
  auto *actions = new QHBoxLayout;
  delete_left_ = action_button("Delete left", review_page_);
  delete_left_->setObjectName("danger");
  keep_both_ = action_button("Keep both", review_page_);
  delete_right_ = action_button("Delete right", review_page_);
  delete_right_->setObjectName("danger");
  actions->addWidget(delete_left_);
  actions->addStretch();
  actions->addWidget(keep_both_);
  actions->addStretch();
  actions->addWidget(delete_right_);
  review->addLayout(actions);

  done_page_ = new QWidget(pages_);
  auto *done = new QVBoxLayout(done_page_);
  done->setAlignment(Qt::AlignCenter);
  auto *done_title = new QLabel("All clean", done_page_);
  done_title->setObjectName("title");
  auto *done_text = new QLabel(
      "There are no conservative duplicate candidates left to review.",
      done_page_);
  done_text->setObjectName("muted");
  done->addWidget(done_title, 0, Qt::AlignCenter);
  done->addWidget(done_text, 0, Qt::AlignCenter);

  error_page_ = new QWidget(pages_);
  auto *error = new QVBoxLayout(error_page_);
  error->setAlignment(Qt::AlignCenter);
  auto *error_title = new QLabel("gdupe paused safely", error_page_);
  error_title->setObjectName("title");
  error_label_ = new QLabel(error_page_);
  error_label_->setWordWrap(true);
  error_label_->setMaximumWidth(660);
  error_label_->setAlignment(Qt::AlignCenter);
  error_label_->setObjectName("muted");
  retry_ = action_button("Retry synchronization", error_page_);
  error->addWidget(error_title, 0, Qt::AlignCenter);
  error->addWidget(error_label_, 0, Qt::AlignCenter);
  error->addWidget(retry_, 0, Qt::AlignCenter);
  pages_->addWidget(loading_page_);
  pages_->addWidget(review_page_);
  pages_->addWidget(done_page_);
  pages_->addWidget(error_page_);

  connect(delete_left_, &QPushButton::clicked, this,
          [this] { delete_side(true); });
  connect(delete_right_, &QPushButton::clicked, this,
          [this] { delete_side(false); });
  connect(keep_both_, &QPushButton::clicked, this,
          [this] { exclude_current(); });
  connect(process_all_, &QPushButton::clicked, this, [this] { process_all(); });
  connect(retry_, &QPushButton::clicked, this, [this] { start(); });
}

void MainWindow::show_loading(const QString &phase) {
  phase_label_->setText(phase);
  progress_label_->clear();
  progress_bar_->setRange(0, 0);
  pages_->setCurrentWidget(loading_page_);
}

void MainWindow::update_progress(const std::string &phase,
                                 std::size_t completed, std::size_t total) {
  phase_label_->setText(QString::fromStdString(phase));
  if (total > 0) {
    progress_bar_->setRange(0, 1000);
    progress_bar_->setValue(static_cast<int>(1000.0 * completed / total));
    progress_label_->setText(QString("%1 of %2").arg(completed).arg(total));
  } else {
    progress_bar_->setRange(0, 0);
    progress_label_->clear();
  }
}

void MainWindow::launch(std::function<void(Engine::Progress)> work,
                        std::function<void()> success) {
  if (busy_)
    return;
  busy_ = true;
  QPointer<MainWindow> self(this);
  auto engine = engine_;
  active_ = QtConcurrent::run([self, engine, work = std::move(work),
                               success = std::move(success)]() mutable {
    try {
      Engine::Progress progress = [self](const std::string &phase,
                                         std::size_t completed,
                                         std::size_t total) {
        if (self)
          QMetaObject::invokeMethod(
              self,
              [self, phase, completed, total] {
                if (self)
                  self->update_progress(phase, completed, total);
              },
              Qt::QueuedConnection);
      };
      work(progress);
      if (self)
        QMetaObject::invokeMethod(
            self,
            [self, success = std::move(success)]() mutable {
              if (self) {
                self->busy_ = false;
                success();
              }
            },
            Qt::QueuedConnection);
    } catch (const std::exception &problem) {
      const QString message = QString::fromUtf8(problem.what());
      if (self)
        QMetaObject::invokeMethod(
            self,
            [self, message] {
              if (self) {
                self->busy_ = false;
                self->show_error(message);
              }
            },
            Qt::QueuedConnection);
    }
  });
}

void MainWindow::start() {
  show_loading("Synchronizing the library…");
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
    pages_->setCurrentWidget(done_page_);
    return;
  }
  load_current_preview();
}

void MainWindow::load_current_preview() {
  const auto card = queue_.front();
  show_loading("Preparing this comparison…");
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
        left_media_->show_file(paths->first, card.left.fingerprint->kind);
        right_media_->show_file(paths->second, card.right.fingerprint->kind);
        count_label_->setText(QString("%1 comparison%2 remaining")
                                  .arg(queue_.size())
                                  .arg(queue_.size() == 1 ? "" : "s"));
        evidence_label_->setText(
            QString("%1% similar · %2")
                .arg(std::round(card.score * 100.0))
                .arg(QString::fromStdString(card.evidence)));
        left_detail_->setText(details(card.left));
        right_detail_->setText(details(card.right));
        pages_->setCurrentWidget(review_page_);
      });
}

void MainWindow::delete_side(bool left) {
  if (queue_.empty())
    return;
  const auto card = queue_.front();
  const auto &target = left ? card.left : card.right;
  left_media_->clear();
  right_media_->clear();
  show_loading("Deleting and consolidating…");
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
  left_media_->clear();
  right_media_->clear();
  show_loading("Keeping both…");
  launch(
      [engine = engine_, card](Engine::Progress) {
        engine->exclude_pair(card.left.remote.key, card.right.remote.key,
                             card.generation);
      },
      [this] { refresh_queue(); });
}

void MainWindow::process_all() {
  if (queue_.empty())
    return;
  const auto generation = queue_.front().generation;
  left_media_->clear();
  right_media_->clear();
  show_loading("Processing the remaining queue…");
  launch(
      [engine = engine_, generation](Engine::Progress progress) {
        engine->process_all(generation, std::move(progress));
      },
      [this] { refresh_queue(); });
}

void MainWindow::show_error(const QString &message) {
  error_label_->setText(message + "\n\nNo further destructive work will run "
                                  "until synchronization succeeds.");
  pages_->setCurrentWidget(error_page_);
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (busy_) {
    phase_label_->setText("Finishing the current safe checkpoint…");
    event->ignore();
    return;
  }
  event->accept();
}

} // namespace gdupe
