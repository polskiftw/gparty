#include "config.hpp"
#include "engine.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

#include <filesystem>
#include <memory>
#include <string>

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName("gdupe");
  application.setOrganizationName("gparty");
  try {
    const QString state_directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(state_directory);
    QLockFile instance_lock(state_directory + "/instance.lock");
    instance_lock.setStaleLockTime(0);
    if (!instance_lock.tryLock(0))
      throw std::runtime_error("gdupe is already open on this computer");
    const auto executable =
        std::filesystem::absolute(std::filesystem::path(argv[0]));
    std::filesystem::path config = gdupe::default_config_path(executable);
    for (int index = 1; index + 1 < argc; ++index)
      if (std::string(argv[index]) == "--config")
        config = argv[index + 1];
    auto engine = std::make_shared<gdupe::Engine>(gdupe::Config::load(config));
    gdupe::MainWindow window(std::move(engine));
    window.show();
    return application.exec();
  } catch (const std::exception &problem) {
    QMessageBox::critical(nullptr, "gdupe could not open",
                          QString::fromUtf8(problem.what()));
    return 1;
  }
}
