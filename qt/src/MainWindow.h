#pragma once

#include <QMainWindow>
#include <QJsonObject>
#include <QVector>

class QComboBox;
class QLabel;
class QProgressBar;
class QTableWidget;
class QPushButton;
class QHBoxLayout;
class QStatusBar;
class QWidget;

struct CalcFile {
  QString name;
  QString path;
  bool isDir = false;
  qint64 size = 0;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);

public slots:
  void onProgress(qulonglong remaining, qulonglong total);

private slots:
  void refreshDevices();
  void openSelectedDevice(int index);
  void reloadListing();
  void enterPath(const QString &path);
  void onCellDoubleClicked(int row, int column);
  void downloadSelected();
  void uploadFiles();
  void createFolder();
  void deleteSelected();
  void renameSelected();
  void uploadOs();

private:
  void buildUi();
  void rebuildBreadcrumb();
  void fillTable(const QVector<CalcFile> &files);
  void setBusy(bool busy);
  void showError(const QString &message);
  void updateInfoPanel(const QJsonObject &info);
  QVector<CalcFile> selectedFiles() const;
  bool hasDevice() const;
  QString joinCalcPath(const QString &parent, const QString &name) const;

  QComboBox *m_devices = nullptr;
  QLabel *m_name = nullptr;
  QLabel *m_os = nullptr;
  QLabel *m_id = nullptr;
  QProgressBar *m_storage = nullptr;
  QProgressBar *m_ram = nullptr;
  QWidget *m_crumbBar = nullptr;
  QHBoxLayout *m_crumbLayout = nullptr;
  QTableWidget *m_table = nullptr;
  QPushButton *m_download = nullptr;
  QPushButton *m_upload = nullptr;
  QPushButton *m_mkdir = nullptr;
  QPushButton *m_rename = nullptr;
  QPushButton *m_remove = nullptr;
  QPushButton *m_osButton = nullptr;
  QProgressBar *m_transfer = nullptr;
  QLabel *m_status = nullptr;

  int m_bus = -1;
  int m_addr = -1;
  QString m_path;
  bool m_busy = false;
};
