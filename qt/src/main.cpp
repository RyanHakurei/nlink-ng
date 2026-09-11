#include "MainWindow.h"
#include "nlink_ffi.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char **argv) {
  if (argc >= 2) {
    const int rc = nlink_cli_run();
    return rc < 0 ? 0 : rc;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("nlink-ng");
  QApplication::setOrganizationName("nlink-ng");
  QApplication::setApplicationVersion("0.2.0");
  QApplication::setWindowIcon(QIcon(":/icons/icon.png"));

  MainWindow window;
  window.show();
  return app.exec();
}
