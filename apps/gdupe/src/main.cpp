#include "config.hpp"
#include "engine.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QMessageBox>

#include <filesystem>
#include <memory>
#include <string>

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  application.setApplicationName("gdupe");
  application.setOrganizationName("gparty");
  try {
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
