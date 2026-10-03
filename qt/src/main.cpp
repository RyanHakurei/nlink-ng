#include "MainWindow.h"
#include "nlink_ffi.h"

#include <QApplication>
#include <QIcon>

#ifdef _WIN32
#include <cstdio>
#include <iostream>
#include <windows.h>

namespace {
void attach_parent_console() {
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
  freopen("CONIN$", "r", stdin);
  std::cout.clear();
  std::cerr.clear();
}
}  // namespace
#endif

int main(int argc, char **argv) {
  if (argc >= 2) {
#ifdef _WIN32
    attach_parent_console();
#endif
    const int rc = nlink_cli_run();
    return rc < 0 ? 0 : rc;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("nlink-ng");
  QApplication::setOrganizationName("nlink-ng");
  QApplication::setApplicationVersion("1.0.0");
  QApplication::setWindowIcon(QIcon(":/icons/icon.png"));

  MainWindow window;
  window.show();
  return app.exec();
}
