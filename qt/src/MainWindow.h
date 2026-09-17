#pragma once

#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QMainWindow>
#include <QPoint>
#include <QVector>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;
class QTableView;
class QListView;
class QStackedWidget;
class QStandardItemModel;
class QToolButton;
class QModelIndex;
class QAction;
class QAbstractItemView;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;
class PathNavigator;
class FileTableView;
class FileIconView;

struct CalcFile {
  QString name;
  QString path;
  bool isDir = false;
  qint64 size = 0;
  int itemCount = -1;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
  friend class FileTableView;
  friend class FileIconView;

public:
  explicit MainWindow(QWidget *parent = nullptr);

public slots:
  void onProgress(qulonglong remaining, qulonglong total);
  void showQueueFile(const QString &verb, const QString &name, int current, int total);

private slots:
  void refreshDevices();
  void openSelectedDevice(int index);
  void reloadListing();
  void enterPath(const QString &path);
  void onItemActivated(const QModelIndex &index);
  void setViewMode(int mode);
  void downloadSelected();
  void uploadFiles();
  void createFolder();
  void deleteSelected();
  void renameSelected();
  void uploadOs();
  void backupCalculator();
  void restoreCalculator();
  void screenshotCalculator();
  void exitExamMode();
  void showNavigatorSubdirs(const QString &path, const QPoint &globalPos);
  void showAbout();
  void openSelected();
  void cutSelected();
  void copySelected();
  void pasteItems();
  void goUp();
  void updateActions();

private:
  void buildUi();
  void fillFiles(const QVector<CalcFile> &files);
  void setBusy(bool busy);
  void hideTransferUi();
  void showError(const QString &message);
  void updateInfoPanel(const QJsonObject &info);
  QVector<CalcFile> selectedFiles() const;
  bool hasDevice() const;
  QString joinCalcPath(const QString &parent, const QString &name) const;
  QString currentDir() const;
  QString dropDestPath(const QModelIndex &index) const;
  bool clipboardHasPaste() const;
  void placeOnClipboard(bool cut);
  void uploadLocalPaths(const QStringList &paths, const QString &destDir);
  void downloadFilesTo(const QVector<CalcFile> &files, const QString &destDir);
  void moveOrCopyItems(const QVector<CalcFile> &files, const QString &destDir, bool copy);
  void startCalcDrag(QAbstractItemView *view, Qt::DropActions acts);
  void calcDragEnter(QDragEnterEvent *event);
  void calcDragMove(QAbstractItemView *view, QDragMoveEvent *event);
  void calcDrop(QAbstractItemView *view, QDropEvent *event);
  bool canAcceptCalcDrop(const QMimeData *mime) const;
  void setupFileView(QAbstractItemView *view);
  void showFileContextMenu(QAbstractItemView *view, const QPoint &viewportPos);
  QAction *makeAction(const QString &text, const QString &icon, const QKeySequence &shortcut,
                      void (MainWindow::*slot)());

  QComboBox *m_devices = nullptr;
  QLabel *m_name = nullptr;
  QLabel *m_os = nullptr;
  QLabel *m_id = nullptr;
  QProgressBar *m_storage = nullptr;
  QProgressBar *m_ram = nullptr;
  PathNavigator *m_navigator = nullptr;
  QStandardItemModel *m_files = nullptr;
  QStackedWidget *m_stack = nullptr;
  QTableView *m_detailsView = nullptr;
  QListView *m_iconsView = nullptr;
  QToolButton *m_detailsBtn = nullptr;
  QToolButton *m_iconsBtn = nullptr;
  QPushButton *m_osButton = nullptr;
  QPushButton *m_backup = nullptr;
  QPushButton *m_restore = nullptr;
  QPushButton *m_screenshot = nullptr;
  QPushButton *m_exitExam = nullptr;
  QProgressBar *m_transfer = nullptr;
  QLabel *m_queueLabel = nullptr;
  QLabel *m_status = nullptr;

  QAction *m_openAct = nullptr;
  QAction *m_downloadAct = nullptr;
  QAction *m_uploadAct = nullptr;
  QAction *m_mkdirAct = nullptr;
  QAction *m_cutAct = nullptr;
  QAction *m_copyAct = nullptr;
  QAction *m_pasteAct = nullptr;
  QAction *m_renameAct = nullptr;
  QAction *m_deleteAct = nullptr;
  QAction *m_upAct = nullptr;

  int m_bus = -1;
  int m_addr = -1;
  QString m_path;
  bool m_busy = false;
  QHash<QString, QStringList> m_dirCache;
};
