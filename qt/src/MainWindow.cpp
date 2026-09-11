#include "MainWindow.h"
#include "nlink_ffi.h"

#include <algorithm>

#include <QComboBox>
#include <QFutureWatcher>
#include <QPair>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

QString takeString(NLinkString s) {
  QString out;
  if (s.data && s.len)
    out = QString::fromUtf8(s.data, static_cast<int>(s.len));
  nlink_string_free(s);
  return out;
}

QString formatSize(qint64 bytes) {
  constexpr double k = 1024.0;
  if (bytes < k)
    return QString::number(bytes) + " B";
  if (bytes < k * k)
    return QString::number(bytes / k, 'f', 1) + " KB";
  if (bytes < k * k * k)
    return QString::number(bytes / (k * k), 'f', 1) + " MB";
  return QString::number(bytes / (k * k * k), 'f', 1) + " GB";
}

QString formatVersion(const QJsonObject &v) {
  return QString("%1.%2.%3.%4")
      .arg(v.value("major").toInt())
      .arg(v.value("minor").toInt())
      .arg(v.value("patch").toInt())
      .arg(v.value("build").toInt());
}

extern "C" void nlinkProgressThunk(void *user, uint64_t remaining, uint64_t total) {
  auto *win = static_cast<MainWindow *>(user);
  QMetaObject::invokeMethod(win, "onProgress", Qt::QueuedConnection,
                            Q_ARG(qulonglong, remaining), Q_ARG(qulonglong, total));
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("nlink-ng");
  resize(960, 640);
  buildUi();
  refreshDevices();
}

void MainWindow::buildUi() {
  auto *splitter = new QSplitter(Qt::Horizontal, this);

  auto *left = new QWidget;
  left->setMinimumWidth(240);
  left->setMaximumWidth(320);
  auto *leftLay = new QVBoxLayout(left);

  auto *devRow = new QHBoxLayout;
  m_devices = new QComboBox;
  auto *refresh = new QPushButton("Refresh");
  devRow->addWidget(m_devices, 1);
  devRow->addWidget(refresh);
  leftLay->addLayout(devRow);

  m_name = new QLabel("No calculator");
  m_name->setWordWrap(true);
  QFont title = m_name->font();
  title.setPointSize(title.pointSize() + 2);
  title.setBold(true);
  m_name->setFont(title);
  m_os = new QLabel;
  m_id = new QLabel;
  m_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_id->setWordWrap(true);
  leftLay->addWidget(m_name);
  leftLay->addWidget(m_os);
  leftLay->addWidget(m_id);

  leftLay->addWidget(new QLabel("Storage"));
  m_storage = new QProgressBar;
  m_storage->setRange(0, 100);
  m_storage->setValue(0);
  m_storage->setTextVisible(true);
  leftLay->addWidget(m_storage);

  leftLay->addWidget(new QLabel("RAM"));
  m_ram = new QProgressBar;
  m_ram->setRange(0, 100);
  m_ram->setValue(0);
  leftLay->addWidget(m_ram);

  m_osButton = new QPushButton("Upload OS");
  leftLay->addWidget(m_osButton);
  leftLay->addStretch();

  auto *right = new QWidget;
  auto *rightLay = new QVBoxLayout(right);
  m_crumbBar = new QWidget;
  m_crumbLayout = new QHBoxLayout(m_crumbBar);
  m_crumbLayout->setContentsMargins(0, 0, 0, 0);
  m_crumbLayout->addStretch();
  rightLay->addWidget(m_crumbBar);

  m_table = new QTableWidget(0, 3);
  m_table->setHorizontalHeaderLabels({"Name", "Size", "Type"});
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setAlternatingRowColors(true);
  m_table->verticalHeader()->setVisible(false);
  rightLay->addWidget(m_table, 1);

  auto *actions = new QHBoxLayout;
  m_download = new QPushButton("Download");
  m_upload = new QPushButton("Upload");
  m_mkdir = new QPushButton("New folder");
  m_rename = new QPushButton("Rename");
  m_remove = new QPushButton("Delete");
  actions->addWidget(m_download);
  actions->addWidget(m_upload);
  actions->addWidget(m_mkdir);
  actions->addWidget(m_rename);
  actions->addWidget(m_remove);
  actions->addStretch();
  rightLay->addLayout(actions);

  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(1, 1);
  setCentralWidget(splitter);

  auto *status = statusBar();
  m_status = new QLabel("Ready");
  m_transfer = new QProgressBar;
  m_transfer->setMaximumWidth(220);
  m_transfer->setRange(0, 100);
  m_transfer->setValue(0);
  m_transfer->setVisible(false);
  status->addWidget(m_status, 1);
  status->addPermanentWidget(m_transfer);

  connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
  connect(m_devices, &QComboBox::currentIndexChanged, this, &MainWindow::openSelectedDevice);
  connect(m_table, &QTableWidget::cellDoubleClicked, this, &MainWindow::onCellDoubleClicked);
  connect(m_download, &QPushButton::clicked, this, &MainWindow::downloadSelected);
  connect(m_upload, &QPushButton::clicked, this, &MainWindow::uploadFiles);
  connect(m_mkdir, &QPushButton::clicked, this, &MainWindow::createFolder);
  connect(m_rename, &QPushButton::clicked, this, &MainWindow::renameSelected);
  connect(m_remove, &QPushButton::clicked, this, &MainWindow::deleteSelected);
  connect(m_osButton, &QPushButton::clicked, this, &MainWindow::uploadOs);
}

void MainWindow::setBusy(bool busy) {
  m_busy = busy;
  const bool on = hasDevice() && !busy;
  m_table->setEnabled(!busy);
  m_download->setEnabled(on);
  m_upload->setEnabled(on);
  m_mkdir->setEnabled(on);
  m_rename->setEnabled(on);
  m_remove->setEnabled(on);
  m_osButton->setEnabled(on);
  m_devices->setEnabled(!busy);
}

bool MainWindow::hasDevice() const { return m_bus >= 0 && m_addr >= 0; }

void MainWindow::showError(const QString &message) {
  m_status->setText(message);
  if (!message.isEmpty())
    QMessageBox::warning(this, "nlink-ng", message);
}

void MainWindow::onProgress(qulonglong remaining, qulonglong total) {
  m_transfer->setVisible(true);
  if (total == 0) {
    m_transfer->setRange(0, 0);
    return;
  }
  m_transfer->setRange(0, 1000);
  const int pct = static_cast<int>(((total - remaining) * 1000) / total);
  m_transfer->setValue(pct);
}

void MainWindow::updateInfoPanel(const QJsonObject &info) {
  m_name->setText(info.value("name").toString("TI-Nspire"));
  m_os->setText("OS " + formatVersion(info.value("version").toObject()));
  m_id->setText(info.value("id").toString());
  const double storageTotal = info.value("total_storage").toDouble();
  const double storageFree = info.value("free_storage").toDouble();
  const double ramTotal = info.value("total_ram").toDouble();
  const double ramFree = info.value("free_ram").toDouble();
  if (storageTotal > 0) {
    const int used = static_cast<int>(((storageTotal - storageFree) / storageTotal) * 100);
    m_storage->setValue(used);
    m_storage->setFormat(formatSize(static_cast<qint64>(storageTotal - storageFree)) + " / " +
                         formatSize(static_cast<qint64>(storageTotal)));
  }
  if (ramTotal > 0) {
    const int used = static_cast<int>(((ramTotal - ramFree) / ramTotal) * 100);
    m_ram->setValue(used);
    m_ram->setFormat(formatSize(static_cast<qint64>(ramTotal - ramFree)) + " / " +
                     formatSize(static_cast<qint64>(ramTotal)));
  }
}

void MainWindow::refreshDevices() {
  NLinkString json{}, err{};
  if (nlink_enumerate(&json, &err) != 0) {
    showError(takeString(err));
    takeString(json);
    return;
  }
  takeString(err);
  const QJsonArray arr = QJsonDocument::fromJson(takeString(json).toUtf8()).array();
  const QSignalBlocker block(m_devices);
  m_devices->clear();
  for (const auto &val : arr) {
    const QJsonObject o = val.toObject();
    const int bus = o.value("busNumber").toInt();
    const int addr = o.value("address").toInt();
    const QString label =
        QString("%1:%2  %3").arg(bus).arg(addr).arg(o.value("name").toString());
    m_devices->addItem(label, QVariant::fromValue(QPoint(bus, addr)));
  }
  if (m_devices->count() == 0) {
    m_bus = m_addr = -1;
    m_name->setText("No calculator");
    m_os->clear();
    m_id->clear();
    m_table->setRowCount(0);
    m_status->setText("No calculators found");
    setBusy(false);
    return;
  }
  m_status->setText(QString("%1 calculator(s)").arg(m_devices->count()));
  openSelectedDevice(m_devices->currentIndex());
}

void MainWindow::openSelectedDevice(int index) {
  if (index < 0) {
    m_bus = m_addr = -1;
    return;
  }
  const QPoint id = m_devices->itemData(index).toPoint();
  if (m_busy)
    return;
  if (m_bus == id.x() && m_addr == id.y()) {
    reloadListing();
    return;
  }
  setBusy(true);
  m_status->setText("Opening calculator…");
  const int bus = id.x();
  const int addr = id.y();
  auto *watcher = new QFutureWatcher<QPair<QString, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<QString, QString>>::finished, this, [this, watcher, bus, addr] {
    const auto result = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    if (!result.second.isEmpty()) {
      showError(result.second);
      return;
    }
    m_bus = bus;
    m_addr = addr;
    m_path.clear();
    updateInfoPanel(QJsonDocument::fromJson(result.first.toUtf8()).object());
    reloadListing();
  });
  watcher->setFuture(QtConcurrent::run([bus, addr] {
    NLinkString json{}, err{};
    QString error;
    QString info;
    if (nlink_open(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), &json, &err) != 0) {
      error = takeString(err);
      takeString(json);
    } else {
      takeString(err);
      info = takeString(json);
    }
    return qMakePair(info, error);
  }));
}

QString MainWindow::joinCalcPath(const QString &parent, const QString &name) const {
  if (parent.isEmpty() || parent == "/")
    return "/" + name;
  if (parent.endsWith('/'))
    return parent + name;
  return parent + "/" + name;
}

void MainWindow::rebuildBreadcrumb() {
  while (QLayoutItem *item = m_crumbLayout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  QStringList parts = m_path.split('/', Qt::SkipEmptyParts);
  parts.prepend(QString());
  for (int i = 0; i < parts.size(); ++i) {
    auto *btn = new QPushButton(parts[i].isEmpty() ? "Home" : parts[i]);
    btn->setFlat(true);
    btn->setCursor(Qt::PointingHandCursor);
    const QString target = QStringList(parts.mid(0, i + 1)).join('/');
    connect(btn, &QPushButton::clicked, this, [this, target] { enterPath(target); });
    m_crumbLayout->addWidget(btn);
    if (i + 1 < parts.size()) {
      auto *sep = new QLabel("›");
      m_crumbLayout->addWidget(sep);
    }
  }
  m_crumbLayout->addStretch();
}

void MainWindow::enterPath(const QString &path) {
  m_path = path;
  if (m_path == "/")
    m_path.clear();
  reloadListing();
}

void MainWindow::reloadListing() {
  if (!hasDevice())
    return;
  rebuildBreadcrumb();
  setBusy(true);
  m_status->setText("Listing…");
  const int bus = m_bus;
  const int addr = m_addr;
  const QString path = m_path.isEmpty() ? QString("/") : m_path;
  auto *watcher = new QFutureWatcher<QPair<QString, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<QString, QString>>::finished, this, [this, watcher] {
    const auto result = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    if (!result.second.isEmpty()) {
      showError(result.second);
      return;
    }
    QVector<CalcFile> files;
    const QJsonArray arr = QJsonDocument::fromJson(result.first.toUtf8()).array();
    for (const auto &val : arr) {
      const QJsonObject o = val.toObject();
      CalcFile f;
      f.name = o.value("path").toString();
      f.isDir = o.value("isDir").toBool();
      f.size = static_cast<qint64>(o.value("size").toDouble());
      f.path = joinCalcPath(m_path, f.name);
      if (!f.isDir && !f.name.endsWith(".tns"))
        continue;
      files.push_back(f);
    }
    std::sort(files.begin(), files.end(), [](const CalcFile &a, const CalcFile &b) {
      if (a.isDir != b.isDir)
        return a.isDir;
      return a.name.toLower() < b.name.toLower();
    });
    fillTable(files);
    m_status->setText(QString("%1 items").arg(files.size()));
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, path] {
    NLinkString json{}, err{};
    const QByteArray p = path.toUtf8();
    QString error;
    QString body;
    if (nlink_list_dir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), p.constData(), &json,
                       &err) != 0) {
      error = takeString(err);
      takeString(json);
    } else {
      takeString(err);
      body = takeString(json);
    }
    return qMakePair(body, error);
  }));
}

void MainWindow::fillTable(const QVector<CalcFile> &files) {
  m_table->setRowCount(files.size());
  for (int i = 0; i < files.size(); ++i) {
    const auto &f = files[i];
    auto *name = new QTableWidgetItem(f.isDir ? f.name + "/" : f.name);
    name->setData(Qt::UserRole, QVariant::fromValue(f.path));
    name->setData(Qt::UserRole + 1, f.isDir);
    name->setData(Qt::UserRole + 2, QVariant::fromValue(f.size));
    auto *size = new QTableWidgetItem(f.isDir ? QString() : formatSize(f.size));
    auto *type = new QTableWidgetItem(f.isDir ? "Folder" : "File");
    m_table->setItem(i, 0, name);
    m_table->setItem(i, 1, size);
    m_table->setItem(i, 2, type);
  }
}

QVector<CalcFile> MainWindow::selectedFiles() const {
  QVector<CalcFile> out;
  const auto rows = m_table->selectionModel()->selectedRows();
  for (const QModelIndex &idx : rows) {
    QTableWidgetItem *item = m_table->item(idx.row(), 0);
    if (!item)
      continue;
    CalcFile f;
    f.path = item->data(Qt::UserRole).toString();
    f.isDir = item->data(Qt::UserRole + 1).toBool();
    f.size = item->data(Qt::UserRole + 2).toLongLong();
    f.name = item->text();
    if (f.name.endsWith('/'))
      f.name.chop(1);
    out.push_back(f);
  }
  return out;
}

void MainWindow::onCellDoubleClicked(int row, int) {
  QTableWidgetItem *item = m_table->item(row, 0);
  if (!item)
    return;
  if (item->data(Qt::UserRole + 1).toBool())
    enterPath(item->data(Qt::UserRole).toString());
}

void MainWindow::downloadSelected() {
  auto files = selectedFiles();
  if (files.isEmpty()) {
    showError("Select a file or folder to download.");
    return;
  }
  const QString dest = QFileDialog::getExistingDirectory(this, "Download to");
  if (dest.isEmpty())
    return;
  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Downloading…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    m_transfer->setVisible(false);
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Download complete");
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, files, dest, this] {
    for (const CalcFile &f : files) {
      NLinkString err{};
      int rc;
      const QByteArray remote = f.path.toUtf8();
      if (f.isDir) {
        const QString local = dest + "/" + f.name;
        const QByteArray localUtf = local.toUtf8();
        rc = nlink_download_dir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                remote.constData(), localUtf.constData(), nlinkProgressThunk,
                                const_cast<MainWindow *>(this), &err);
      } else {
        const QByteArray destUtf = dest.toUtf8();
        rc = nlink_download_file(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                 remote.constData(), static_cast<uint64_t>(f.size),
                                 destUtf.constData(), nlinkProgressThunk,
                                 const_cast<MainWindow *>(this), &err);
      }
      if (rc != 0)
        return takeString(err);
      takeString(err);
    }
    return QString();
  }));
}

void MainWindow::uploadFiles() {
  const QStringList srcs = QFileDialog::getOpenFileNames(this, "Upload files");
  if (srcs.isEmpty())
    return;
  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Uploading…");
  const int bus = m_bus;
  const int addr = m_addr;
  const QString dest = m_path.isEmpty() ? QString("/") : m_path;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    m_transfer->setVisible(false);
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Upload complete");
    reloadListing();
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, srcs, dest, this] {
    const QByteArray destUtf = dest.toUtf8();
    for (const QString &src : srcs) {
      NLinkString err{};
      const QByteArray srcUtf = src.toUtf8();
      const int rc = nlink_upload_file(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                       destUtf.constData(), srcUtf.constData(), nlinkProgressThunk,
                                       const_cast<MainWindow *>(this), &err);
      if (rc != 0)
        return takeString(err);
      takeString(err);
    }
    return QString();
  }));
}

void MainWindow::createFolder() {
  bool ok = false;
  const QString name = QInputDialog::getText(this, "New folder", "Name:", QLineEdit::Normal,
                                             QString(), &ok);
  if (!ok || name.isEmpty() || name.contains('/'))
    return;
  const QString path = joinCalcPath(m_path, name);
  NLinkString err{};
  if (nlink_mkdir(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr), path.toUtf8().constData(),
                  &err) != 0) {
    showError(takeString(err));
    return;
  }
  takeString(err);
  reloadListing();
}

void MainWindow::deleteSelected() {
  auto files = selectedFiles();
  if (files.isEmpty())
    return;
  if (QMessageBox::question(this, "Delete",
                            QString("Delete %1 item(s)?").arg(files.size())) != QMessageBox::Yes)
    return;
  for (const CalcFile &f : files) {
    NLinkString err{};
    const int rc = f.isDir ? nlink_rmdir(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                                         f.path.toUtf8().constData(), &err)
                           : nlink_rm(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                                      f.path.toUtf8().constData(), &err);
    if (rc != 0) {
      showError(takeString(err));
      return;
    }
    takeString(err);
  }
  reloadListing();
}

void MainWindow::renameSelected() {
  auto files = selectedFiles();
  if (files.size() != 1)
    return;
  bool ok = false;
  const QString name = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal,
                                             files[0].name, &ok);
  if (!ok || name.isEmpty() || name.contains('/'))
    return;
  const QString dest = joinCalcPath(m_path, name);
  NLinkString err{};
  if (nlink_move(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                 files[0].path.toUtf8().constData(), dest.toUtf8().constData(), &err) != 0) {
    showError(takeString(err));
    return;
  }
  takeString(err);
  reloadListing();
}

void MainWindow::uploadOs() {
  const QString src = QFileDialog::getOpenFileName(this, "Upload OS");
  if (src.isEmpty())
    return;
  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Uploading OS…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    m_transfer->setVisible(false);
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("OS upload complete");
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, src, this] {
    NLinkString err{};
    const int rc = nlink_upload_os(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                   src.toUtf8().constData(), nlinkProgressThunk,
                                   const_cast<MainWindow *>(this), &err);
    if (rc != 0)
      return takeString(err);
    takeString(err);
    return QString();
  }));
}
