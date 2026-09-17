#include "MainWindow.h"
#include "PathNavigator.h"
#include "nlink_ffi.h"

#include <algorithm>
#include <functional>
#include <initializer_list>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPair>
#include <QDateTime>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
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

QString formatItemCount(int n) {
  if (n < 0)
    return {};
  if (n == 1)
    return QStringLiteral("1 item");
  return QStringLiteral("%1 items").arg(n);
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

void notifyQueue(MainWindow *win, const QString &verb, const QString &name, int current, int total) {
  QMetaObject::invokeMethod(win, "showQueueFile", Qt::QueuedConnection, Q_ARG(QString, verb),
                            Q_ARG(QString, name), Q_ARG(int, current), Q_ARG(int, total));
}

bool isOsJunk(const QString &name) {
  return name.compare(QLatin1String("NspireLogs.zip"), Qt::CaseInsensitive) == 0;
}

QString sniffName(const QString &name) {
  QString n = name;
  if (n.endsWith(QLatin1String(".tns"), Qt::CaseInsensitive))
    n.chop(4);
  return n;
}

QString fileExtension(const QString &name) {
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  if (dot <= 0 || dot == name.size() - 1)
    return {};
  return name.mid(dot + 1).toLower();
}

QIcon themeIcon(std::initializer_list<const char *> names) {
  for (const char *name : names) {
    if (QIcon::hasThemeIcon(QLatin1String(name)))
      return QIcon::fromTheme(QLatin1String(name));
  }
  return {};
}

struct RomKind {
  const char *ext;
  const char *icon;
  const char *label;
};

const RomKind kRoms[] = {
    {"nes", "application-x-nes-rom", "NES ROM"},
    {"fds", "application-x-nes-rom", "Famicom Disk ROM"},
    {"smc", "application-x-snes-rom", "SNES ROM"},
    {"sfc", "application-x-snes-rom", "SNES ROM"},
    {"srm", "application-x-snes-rom", "SNES save"},
    {"gb", "application-x-gameboy-rom", "Game Boy ROM"},
    {"gbc", "application-x-gameboy-color-rom", "GBC ROM"},
    {"gba", "application-x-gba-rom", "GBA ROM"},
    {"agb", "application-x-gba-rom", "GBA ROM"},
    {"n64", "application-x-n64-rom", "N64 ROM"},
    {"z64", "application-x-n64-rom", "N64 ROM"},
    {"v64", "application-x-n64-rom", "N64 ROM"},
    {"nds", "application-x-nintendo-ds-rom", "Nintendo DS ROM"},
    {"3ds", "application-x-nintendo-3ds-rom", "3DS ROM"},
    {"cia", "application-x-nintendo-3ds-rom", "3DS CIA"},
    {"sms", "application-x-sms-rom", "Master System ROM"},
    {"gg", "application-x-gamegear-rom", "Game Gear ROM"},
    {"md", "application-x-genesis-rom", "Mega Drive ROM"},
    {"gen", "application-x-genesis-rom", "Genesis ROM"},
    {"smd", "application-x-genesis-rom", "Mega Drive ROM"},
    {"pce", "application-x-pc-engine-rom", "PC Engine ROM"},
    {"ngp", "application-x-neo-geo-pocket-rom", "Neo Geo Pocket ROM"},
    {"iso", "application-x-iso9660-image", "Disc image"},
    {"cue", "application-x-cue", "Cue sheet"},
    {"chd", "application-x-cd-image", "Disc image"},
    {"img", "application-x-raw-disk-image", "Disk image"},
    {"rom", "application-x-rom", "ROM"},
    {"bin", "application-octet-stream", "Binary"},
    {"89u", "application-x-ti89-program", "TI-89 program"},
    {"8xp", "application-x-ti83p-program", "TI-83 program"},
};

const RomKind *romKind(const QString &ext) {
  for (const auto &rom : kRoms) {
    if (ext == QLatin1String(rom.ext))
      return &rom;
  }
  return nullptr;
}

QIcon folderIcon(const QString &name) {
  const QString n = name.trimmed().toLower();
  QIcon icon;
  if (n == QLatin1String("games"))
    icon = themeIcon({"folder-games", "applications-games"});
  else if (n == QLatin1String("images") || n == QLatin1String("photos") ||
           n == QLatin1String("pictures"))
    icon = themeIcon({"folder-pictures", "folder-images"});
  else if (n == QLatin1String("music") || n == QLatin1String("sound") ||
           n == QLatin1String("sounds"))
    icon = themeIcon({"folder-music", "folder-sound"});
  else if (n == QLatin1String("video") || n == QLatin1String("videos"))
    icon = themeIcon({"folder-videos", "folder-video"});
  else if (n == QLatin1String("script") || n == QLatin1String("scripts") ||
           n == QLatin1String("program") || n == QLatin1String("programs"))
    icon = themeIcon({"folder-scripts", "text-x-script"});
  else if (n == QLatin1String("template") || n == QLatin1String("templates"))
    icon = themeIcon({"folder-templates"});
  else if (n == QLatin1String("documents"))
    icon = themeIcon({"folder-documents"});
  else if (n == QLatin1String("downloads"))
    icon = themeIcon({"folder-download", "folder-downloads"});
  if (icon.isNull())
    icon = themeIcon({"folder", "inode-directory"});
  if (icon.isNull())
    icon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
  return icon;
}

QIcon fileIcon(const QString &name, bool isDir) {
  if (isDir)
    return folderIcon(name);

  const QString lower = name.toLower();
  if (lower == QLatin1String("ndless_resources.tns")) {
    QIcon icon = themeIcon({"application-x-addon", "package-x-generic", "application-x-archive"});
    if (!icon.isNull())
      return icon;
  }
  if (lower.startsWith(QLatin1String("ndless_installer"))) {
    QIcon icon = themeIcon({"system-software-install", "application-x-executable"});
    if (!icon.isNull())
      return icon;
  }

  const QString sniffed = sniffName(name);
  const QString ext = fileExtension(sniffed);
  const bool wrappedTns = name.endsWith(QLatin1String(".tns"), Qt::CaseInsensitive);

  if (const RomKind *rom = romKind(ext)) {
    QIcon icon = themeIcon({rom->icon, "application-x-rom", "applications-games", "input-gaming"});
    if (!icon.isNull())
      return icon;
  }

  static QMimeDatabase mimeDb;
  const QMimeType mime = mimeDb.mimeTypeForFile(sniffed, QMimeDatabase::MatchExtension);
  if (mime.isValid() && mime.name() != QLatin1String("application/octet-stream")) {
    QIcon icon = QIcon::fromTheme(mime.iconName());
    if (icon.isNull())
      icon = QIcon::fromTheme(mime.genericIconName());
    if (!icon.isNull())
      return icon;
  }

  if (ext == QLatin1String("tno") || ext == QLatin1String("tnc") || ext == QLatin1String("tcc") ||
      ext == QLatin1String("tco") || ext == QLatin1String("tcc2") || ext == QLatin1String("tco2") ||
      ext == QLatin1String("tct2")) {
    QIcon icon = themeIcon({"application-x-firmware", "system-software-update"});
    if (!icon.isNull())
      return icon;
  }

  if (wrappedTns && ext.isEmpty())
    return QIcon(QStringLiteral(":/icons/icon.png"));

  QIcon fallback = QFileIconProvider().icon(QFileInfo(sniffed));
  if (!fallback.isNull())
    return fallback;
  return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

QString typeLabel(const QString &name, bool isDir) {
  if (isDir)
    return QStringLiteral("Folder");

  const QString lower = name.toLower();
  if (lower == QLatin1String("ndless_resources.tns"))
    return QStringLiteral("Ndless resources");
  if (lower.startsWith(QLatin1String("ndless_installer")))
    return QStringLiteral("Ndless installer");

  const QString sniffed = sniffName(name);
  const QString ext = fileExtension(sniffed);
  const bool wrappedTns = name.endsWith(QLatin1String(".tns"), Qt::CaseInsensitive);

  if (const RomKind *rom = romKind(ext))
    return wrappedTns ? QStringLiteral("%1 (Nspire)").arg(QLatin1String(rom->label))
                      : QLatin1String(rom->label);

  if (ext == QLatin1String("pdf"))
    return wrappedTns ? QStringLiteral("PDF (Nspire)") : QStringLiteral("PDF");
  if (ext == QLatin1String("lua"))
    return QStringLiteral("Lua script");
  if (ext == QLatin1String("py"))
    return QStringLiteral("Python");
  if (ext == QLatin1String("zip") || ext == QLatin1String("tar") || ext == QLatin1String("gz"))
    return QStringLiteral("Archive");
  if (ext == QLatin1String("tno") || ext == QLatin1String("tnc") || ext == QLatin1String("tcc") ||
      ext == QLatin1String("tco") || ext == QLatin1String("tcc2") || ext == QLatin1String("tco2") ||
      ext == QLatin1String("tct2"))
    return QStringLiteral("OS image");

  static QMimeDatabase mimeDb;
  const QMimeType mime = mimeDb.mimeTypeForFile(sniffed, QMimeDatabase::MatchExtension);
  if (mime.isValid() && mime.name() != QLatin1String("application/octet-stream")) {
    QString comment = mime.comment();
    if (!comment.isEmpty()) {
      if (wrappedTns && ext != QLatin1String("tns"))
        comment += QStringLiteral(" (Nspire)");
      return comment;
    }
  }

  if (wrappedTns)
    return QStringLiteral("Nspire document");
  if (!ext.isEmpty())
    return ext.toUpper() + QStringLiteral(" file");
  return QStringLiteral("File");
}

constexpr int kPathRole = Qt::UserRole;
constexpr int kDirRole = Qt::UserRole + 1;
constexpr int kSizeRole = Qt::UserRole + 2;

void applyTranslucentBackground(QWidget *w) {
  if (!w)
    return;
  w->setAutoFillBackground(false);
  w->setAttribute(Qt::WA_TranslucentBackground, true);
  w->setAttribute(Qt::WA_NoSystemBackground, true);
  w->setAttribute(Qt::WA_StyledBackground, false);
  QPalette pal = w->palette();
  pal.setColor(QPalette::Base, Qt::transparent);
  pal.setColor(QPalette::Window, Qt::transparent);
  pal.setColor(QPalette::Button, Qt::transparent);
  QColor alt = pal.color(QPalette::AlternateBase);
  if (alt.alpha() > 80) {
    alt.setAlpha(40);
    pal.setColor(QPalette::AlternateBase, alt);
  }
  w->setPalette(pal);
}

const QString kNlinkMime = QStringLiteral("application/x-nlink-items");

QByteArray encodeNlinkItems(const QVector<CalcFile> &files, bool cut, int bus, int addr) {
  QJsonObject o;
  o.insert(QStringLiteral("cut"), cut);
  o.insert(QStringLiteral("bus"), bus);
  o.insert(QStringLiteral("addr"), addr);
  QJsonArray arr;
  for (const CalcFile &f : files) {
    QJsonObject i;
    i.insert(QStringLiteral("path"), f.path);
    i.insert(QStringLiteral("name"), f.name);
    i.insert(QStringLiteral("isDir"), f.isDir);
    i.insert(QStringLiteral("size"), static_cast<double>(f.size));
    arr.append(i);
  }
  o.insert(QStringLiteral("items"), arr);
  return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool decodeNlinkItems(const QByteArray &bytes, QVector<CalcFile> *files, bool *cut, int *bus,
                      int *addr) {
  const QJsonObject o = QJsonDocument::fromJson(bytes).object();
  if (o.isEmpty())
    return false;
  if (cut)
    *cut = o.value(QStringLiteral("cut")).toBool();
  if (bus)
    *bus = o.value(QStringLiteral("bus")).toInt();
  if (addr)
    *addr = o.value(QStringLiteral("addr")).toInt();
  if (files) {
    files->clear();
    const QJsonArray arr = o.value(QStringLiteral("items")).toArray();
    for (const auto &val : arr) {
      const QJsonObject i = val.toObject();
      CalcFile f;
      f.path = i.value(QStringLiteral("path")).toString();
      f.name = i.value(QStringLiteral("name")).toString();
      f.isDir = i.value(QStringLiteral("isDir")).toBool();
      f.size = static_cast<qint64>(i.value(QStringLiteral("size")).toDouble());
      files->push_back(f);
    }
  }
  return files && !files->isEmpty();
}

class CalcMimeData : public QMimeData {
public:
  CalcMimeData(QVector<CalcFile> items, int bus, int addr)
      : m_items(std::move(items)), m_bus(bus), m_addr(addr) {}

  bool hasFormat(const QString &mime) const override {
    if (mime == QLatin1String("text/uri-list"))
      return true;
    return QMimeData::hasFormat(mime);
  }

  QStringList formats() const override {
    QStringList f = QMimeData::formats();
    if (!f.contains(QLatin1String("text/uri-list")))
      f.prepend(QStringLiteral("text/uri-list"));
    return f;
  }

protected:
  QVariant retrieveData(const QString &mime, QMetaType type) const override {
    if (mime == QLatin1String("text/uri-list") || mime == QLatin1String("text/plain"))
      prepareLocalFiles();
    return QMimeData::retrieveData(mime, type);
  }

private:
  void prepareLocalFiles() const {
    if (m_prepared)
      return;
    m_prepared = true;
    QList<QUrl> urls;
    for (const CalcFile &f : m_items) {
      NLinkString err{};
      int rc;
      if (f.isDir) {
        const QString dest = m_tmp.filePath(f.name);
        rc = nlink_download_dir(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                                f.path.toUtf8().constData(), dest.toUtf8().constData(), nullptr,
                                nullptr, &err);
        if (rc == 0)
          urls.append(QUrl::fromLocalFile(dest));
      } else {
        rc = nlink_download_file(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                                 f.path.toUtf8().constData(), static_cast<uint64_t>(f.size),
                                 m_tmp.path().toUtf8().constData(), nullptr, nullptr, &err);
        if (rc == 0)
          urls.append(QUrl::fromLocalFile(m_tmp.filePath(f.name)));
      }
      takeString(err);
    }
    const_cast<CalcMimeData *>(this)->setUrls(urls);
  }

  QVector<CalcFile> m_items;
  int m_bus = -1;
  int m_addr = -1;
  mutable QTemporaryDir m_tmp;
  mutable bool m_prepared = false;
};

} // namespace

class FileTableView : public QTableView {
public:
  explicit FileTableView(MainWindow *win) : QTableView(win), m_win(win) {}

protected:
  void startDrag(Qt::DropActions acts) override { m_win->startCalcDrag(this, acts); }
  void dragEnterEvent(QDragEnterEvent *e) override { m_win->calcDragEnter(e); }
  void dragMoveEvent(QDragMoveEvent *e) override { m_win->calcDragMove(this, e); }
  void dropEvent(QDropEvent *e) override { m_win->calcDrop(this, e); }
  void contextMenuEvent(QContextMenuEvent *e) override {
    m_win->showFileContextMenu(this, viewport()->mapFromGlobal(e->globalPos()));
    e->accept();
  }

private:
  MainWindow *m_win;
};

class FileIconView : public QListView {
public:
  explicit FileIconView(MainWindow *win) : QListView(win), m_win(win) {}

protected:
  void startDrag(Qt::DropActions acts) override { m_win->startCalcDrag(this, acts); }
  void dragEnterEvent(QDragEnterEvent *e) override { m_win->calcDragEnter(e); }
  void dragMoveEvent(QDragMoveEvent *e) override { m_win->calcDragMove(this, e); }
  void dropEvent(QDropEvent *e) override { m_win->calcDrop(this, e); }
  void contextMenuEvent(QContextMenuEvent *e) override {
    m_win->showFileContextMenu(this, viewport()->mapFromGlobal(e->globalPos()));
    e->accept();
  }

private:
  MainWindow *m_win;
};

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
  m_backup = new QPushButton("Backup");
  m_restore = new QPushButton("Restore");
  m_screenshot = new QPushButton("Screenshot");
  m_exitExam = new QPushButton("Exit exam mode");
  leftLay->addWidget(m_osButton);
  leftLay->addWidget(m_backup);
  leftLay->addWidget(m_restore);
  leftLay->addWidget(m_screenshot);
  leftLay->addWidget(m_exitExam);
  leftLay->addStretch();

  auto *right = new QWidget;
  applyTranslucentBackground(right);
  auto *rightLay = new QVBoxLayout(right);
  m_navigator = new PathNavigator;

  m_detailsBtn = new QToolButton;
  m_detailsBtn->setCheckable(true);
  m_detailsBtn->setChecked(true);
  m_detailsBtn->setAutoRaise(true);
  m_detailsBtn->setToolTip("Details");
  m_detailsBtn->setIcon(QIcon::fromTheme(
      QStringLiteral("view-list-details"),
      style()->standardIcon(QStyle::SP_FileDialogDetailedView)));

  m_iconsBtn = new QToolButton;
  m_iconsBtn->setCheckable(true);
  m_iconsBtn->setAutoRaise(true);
  m_iconsBtn->setToolTip("Icons");
  m_iconsBtn->setIcon(QIcon::fromTheme(
      QStringLiteral("view-list-icons"),
      style()->standardIcon(QStyle::SP_FileDialogListView)));

  auto *viewGroup = new QButtonGroup(this);
  viewGroup->setExclusive(true);
  viewGroup->addButton(m_detailsBtn, 0);
  viewGroup->addButton(m_iconsBtn, 1);

  auto *aboutBtn = new QToolButton;
  aboutBtn->setAutoRaise(true);
  aboutBtn->setToolTip(QStringLiteral("About nlink-ng"));
  aboutBtn->setIcon(QIcon::fromTheme(
      QStringLiteral("help-contextual"),
      QIcon::fromTheme(QStringLiteral("question"),
                       QIcon::fromTheme(QStringLiteral("help-about"),
                                        style()->standardIcon(QStyle::SP_MessageBoxQuestion)))));

  auto *crumbRow = new QHBoxLayout;
  crumbRow->setContentsMargins(0, 0, 0, 0);
  crumbRow->setSpacing(6);
  crumbRow->addWidget(m_navigator, 1);
  crumbRow->addWidget(m_detailsBtn);
  crumbRow->addWidget(m_iconsBtn);
  crumbRow->addWidget(aboutBtn);
  rightLay->addLayout(crumbRow);

  m_files = new QStandardItemModel(this);
  m_files->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"),
                                      QStringLiteral("Type")});

  auto *details = new FileTableView(this);
  m_detailsView = details;
  m_detailsView->setModel(m_files);
  m_detailsView->setShowGrid(false);
  m_detailsView->setAlternatingRowColors(true);
  m_detailsView->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_detailsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_detailsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_detailsView->setSortingEnabled(false);
  m_detailsView->setWordWrap(false);
  m_detailsView->setIconSize(QSize(22, 22));
  m_detailsView->setCornerButtonEnabled(false);
  m_detailsView->verticalHeader()->setVisible(false);
  m_detailsView->verticalHeader()->setDefaultSectionSize(28);
  m_detailsView->horizontalHeader()->setHighlightSections(false);
  m_detailsView->horizontalHeader()->setMinimumSectionSize(48);
  m_detailsView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  m_detailsView->horizontalHeader()->setStretchLastSection(true);
  m_detailsView->setColumnWidth(0, 280);
  m_detailsView->setColumnWidth(1, 120);
  m_detailsView->setColumnWidth(2, 160);
  applyTranslucentBackground(m_detailsView->horizontalHeader());

  auto *icons = new FileIconView(this);
  m_iconsView = icons;
  m_iconsView->setModel(m_files);
  m_iconsView->setViewMode(QListView::IconMode);
  m_iconsView->setResizeMode(QListView::Adjust);
  m_iconsView->setMovement(QListView::Static);
  m_iconsView->setFlow(QListView::LeftToRight);
  m_iconsView->setWrapping(true);
  m_iconsView->setWordWrap(true);
  m_iconsView->setUniformItemSizes(false);
  m_iconsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_iconsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_iconsView->setIconSize(QSize(64, 64));
  m_iconsView->setGridSize(QSize(110, 108));
  m_iconsView->setSpacing(8);
  m_iconsView->setTextElideMode(Qt::ElideNone);
  m_iconsView->setSelectionModel(m_detailsView->selectionModel());

  setupFileView(m_detailsView);
  setupFileView(m_iconsView);

  m_stack = new QStackedWidget;
  applyTranslucentBackground(m_stack);
  m_stack->addWidget(m_detailsView);
  m_stack->addWidget(m_iconsView);
  rightLay->addWidget(m_stack, 1);

  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(1, 1);
  setCentralWidget(splitter);

  auto *status = statusBar();
  m_status = new QLabel("Ready");
  m_queueLabel = new QLabel;
  m_queueLabel->setVisible(false);
  m_transfer = new QProgressBar;
  m_transfer->setMaximumWidth(220);
  m_transfer->setRange(0, 100);
  m_transfer->setValue(0);
  m_transfer->setVisible(false);
  status->addWidget(m_status, 1);
  status->addPermanentWidget(m_queueLabel);
  status->addPermanentWidget(m_transfer);

  connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
  connect(m_devices, &QComboBox::currentIndexChanged, this, &MainWindow::openSelectedDevice);
  connect(m_navigator, &PathNavigator::pathActivated, this, &MainWindow::enterPath);
  connect(m_navigator, &PathNavigator::subdirsRequested, this, &MainWindow::showNavigatorSubdirs);
  connect(viewGroup, &QButtonGroup::idClicked, this, &MainWindow::setViewMode);
  connect(m_detailsView, &QAbstractItemView::activated, this, &MainWindow::onItemActivated);
  connect(m_iconsView, &QAbstractItemView::activated, this, &MainWindow::onItemActivated);
  connect(m_detailsView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &MainWindow::updateActions);
  connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::updateActions);
  connect(m_osButton, &QPushButton::clicked, this, &MainWindow::uploadOs);
  connect(m_backup, &QPushButton::clicked, this, &MainWindow::backupCalculator);
  connect(m_restore, &QPushButton::clicked, this, &MainWindow::restoreCalculator);
  connect(m_screenshot, &QPushButton::clicked, this, &MainWindow::screenshotCalculator);
  connect(m_exitExam, &QPushButton::clicked, this, &MainWindow::exitExamMode);
  connect(aboutBtn, &QToolButton::clicked, this, &MainWindow::showAbout);

  m_openAct = makeAction(QStringLiteral("Open"), QStringLiteral("document-open"),
                         QKeySequence(), &MainWindow::openSelected);
  m_downloadAct = makeAction(QStringLiteral("Download…"), QStringLiteral("document-save"),
                             QKeySequence::Save, &MainWindow::downloadSelected);
  m_uploadAct = makeAction(QStringLiteral("Upload files…"), QStringLiteral("document-import"),
                           QKeySequence(), &MainWindow::uploadFiles);
  m_mkdirAct = makeAction(QStringLiteral("New folder…"), QStringLiteral("folder-new"),
                          QKeySequence(QStringLiteral("Ctrl+Shift+N")), &MainWindow::createFolder);
  m_cutAct = makeAction(QStringLiteral("Cut"), QStringLiteral("edit-cut"), QKeySequence::Cut,
                        &MainWindow::cutSelected);
  m_copyAct = makeAction(QStringLiteral("Copy"), QStringLiteral("edit-copy"), QKeySequence::Copy,
                         &MainWindow::copySelected);
  m_pasteAct = makeAction(QStringLiteral("Paste"), QStringLiteral("edit-paste"), QKeySequence::Paste,
                          &MainWindow::pasteItems);
  m_renameAct = makeAction(QStringLiteral("Rename…"), QStringLiteral("edit-rename"),
                           QKeySequence(Qt::Key_F2), &MainWindow::renameSelected);
  m_deleteAct = makeAction(QStringLiteral("Delete"), QStringLiteral("edit-delete"),
                           QKeySequence::Delete, &MainWindow::deleteSelected);
  m_upAct = makeAction(QStringLiteral("Up"), QStringLiteral("go-up"),
                       QKeySequence(Qt::Key_Backspace), &MainWindow::goUp);
  m_upAct->setShortcuts({QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::ALT | Qt::Key_Up)});
  m_deleteAct->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::SHIFT | Qt::Key_Delete)});

  updateActions();
}

void MainWindow::showAbout() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("About nlink-ng"));
  dlg.setModal(true);

  auto *lay = new QVBoxLayout(&dlg);
  auto *header = new QHBoxLayout;
  auto *icon = new QLabel;
  icon->setPixmap(windowIcon().pixmap(64, 64));
  icon->setAlignment(Qt::AlignTop);
  header->addWidget(icon);

  auto *titles = new QVBoxLayout;
  auto *name = new QLabel(QStringLiteral("nlink-ng"));
  QFont titleFont = name->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 4);
  name->setFont(titleFont);
  auto *ver = new QLabel(QStringLiteral("Version %1").arg(QApplication::applicationVersion()));
  titles->addWidget(name);
  titles->addWidget(ver);
  titles->addStretch();
  header->addLayout(titles, 1);
  lay->addLayout(header);

  auto *body = new QLabel;
  body->setWordWrap(true);
  body->setTextFormat(Qt::RichText);
  body->setOpenExternalLinks(true);
  body->setTextInteractionFlags(Qt::TextBrowserInteraction);
  body->setText(QStringLiteral(
      "<p>Native linking program for TI-Nspire calculators.</p>"
      "<p>Maintained by Ryan<br>"
      "<a href=\"https://github.com/RyanHakurei/nlink-ng\">github.com/RyanHakurei/nlink-ng</a></p>"
      "<p>Based on N-Link by Ben Schattinger<br>"
      "<a href=\"https://github.com/lights0123/n-link\">github.com/lights0123/n-link</a></p>"
      "<p>Licensed under the GNU General Public License v3.0</p>"));
  lay->addWidget(body);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);

  dlg.adjustSize();
  dlg.exec();
}

QAction *MainWindow::makeAction(const QString &text, const QString &icon, const QKeySequence &shortcut,
                               void (MainWindow::*slot)()) {
  auto *act = new QAction(text, this);
  if (!icon.isEmpty())
    act->setIcon(QIcon::fromTheme(icon));
  if (!shortcut.isEmpty())
    act->setShortcut(shortcut);
  act->setShortcutContext(Qt::WidgetShortcut);
  connect(act, &QAction::triggered, this, slot);
  m_detailsView->addAction(act);
  m_iconsView->addAction(act);
  return act;
}

void MainWindow::setupFileView(QAbstractItemView *view) {
  view->setDragEnabled(true);
  view->setAcceptDrops(true);
  view->viewport()->setAcceptDrops(true);
  view->setDropIndicatorShown(true);
  view->setDragDropMode(QAbstractItemView::DragDrop);
  view->setDefaultDropAction(Qt::MoveAction);
  view->setDragDropOverwriteMode(true);
  view->setContextMenuPolicy(Qt::DefaultContextMenu);
  view->setFrameShape(QFrame::NoFrame);
  applyTranslucentBackground(view);
  applyTranslucentBackground(view->viewport());
}

void MainWindow::setBusy(bool busy) {
  m_busy = busy;
  const bool on = hasDevice() && !busy;
  m_navigator->setEnabled(on);
  m_detailsView->setEnabled(!busy);
  m_iconsView->setEnabled(!busy);
  m_osButton->setEnabled(on);
  m_backup->setEnabled(on);
  m_restore->setEnabled(on);
  m_screenshot->setEnabled(on);
  m_exitExam->setEnabled(on);
  m_devices->setEnabled(!busy);
  updateActions();
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

void MainWindow::showQueueFile(const QString &verb, const QString &name, int current, int total) {
  m_transfer->setVisible(true);
  m_transfer->setRange(0, 1000);
  m_transfer->setValue(0);
  m_queueLabel->setVisible(total > 1);
  if (total > 1)
    m_queueLabel->setText(QStringLiteral("%1/%2").arg(current).arg(total));
  m_status->setText(QStringLiteral("%1 %2").arg(verb, name));
}

void MainWindow::hideTransferUi() {
  m_transfer->setVisible(false);
  m_transfer->setValue(0);
  m_queueLabel->clear();
  m_queueLabel->setVisible(false);
}

void MainWindow::updateInfoPanel(const QJsonObject &info) {
  m_name->setText(info.value("name").toString("TI-Nspire"));
  QString os = QStringLiteral("OS ") + formatVersion(info.value("version").toObject());
  if (info.contains(QStringLiteral("ndless"))) {
    const QString ndless = info.value(QStringLiteral("ndless")).toString();
    if (ndless.isEmpty())
      os += QStringLiteral(" (Ndless)");
    else
      os += QStringLiteral(" (Ndless %1)").arg(ndless);
  }
  m_os->setText(os);
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
    m_path.clear();
    m_dirCache.clear();
    m_name->setText("No calculator");
    m_os->clear();
    m_id->clear();
    m_files->removeRows(0, m_files->rowCount());
    m_navigator->setPath(QString());
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
    m_dirCache.clear();
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

QString MainWindow::currentDir() const {
  return m_path.isEmpty() ? QStringLiteral("/") : m_path;
}

QString MainWindow::dropDestPath(const QModelIndex &index) const {
  const QModelIndex name = index.isValid() ? index.sibling(index.row(), 0) : QModelIndex();
  if (name.isValid() && name.data(kDirRole).toBool())
    return name.data(kPathRole).toString();
  return currentDir();
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
  m_navigator->setPath(m_path);
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
      f.itemCount = o.value("itemCount").toInt(-1);
      f.path = joinCalcPath(m_path, f.name);
      if (!f.isDir && isOsJunk(f.name))
        continue;
      files.push_back(f);
    }
    std::sort(files.begin(), files.end(), [](const CalcFile &a, const CalcFile &b) {
      if (a.isDir != b.isDir)
        return a.isDir;
      return a.name.toLower() < b.name.toLower();
    });
    fillFiles(files);
    m_status->setText(QString("%1 items").arg(files.size()));
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, path] {
    auto join = [](const QString &parent, const QString &name) {
      if (parent.isEmpty() || parent == QLatin1String("/"))
        return QStringLiteral("/") + name;
      if (parent.endsWith(QLatin1Char('/')))
        return parent + name;
      return parent + QLatin1Char('/') + name;
    };
    NLinkString json{}, err{};
    const QByteArray p = path.toUtf8();
    QString error;
    QString body;
    if (nlink_list_dir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), p.constData(), &json,
                       &err) != 0) {
      error = takeString(err);
      takeString(json);
      return qMakePair(body, error);
    }
    takeString(err);
    QJsonArray arr = QJsonDocument::fromJson(takeString(json).toUtf8()).array();
    for (int i = 0; i < arr.size(); ++i) {
      QJsonObject o = arr.at(i).toObject();
      if (!o.value(QStringLiteral("isDir")).toBool())
        continue;
      const QString child = join(path, o.value(QStringLiteral("path")).toString());
      const QByteArray childUtf = child.toUtf8();
      NLinkString childJson{}, childErr{};
      if (nlink_list_dir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), childUtf.constData(),
                         &childJson, &childErr) != 0) {
        takeString(childJson);
        takeString(childErr);
        continue;
      }
      takeString(childErr);
      const QJsonArray kids = QJsonDocument::fromJson(takeString(childJson).toUtf8()).array();
      int n = 0;
      for (const auto &kid : kids) {
        const QString name = kid.toObject().value(QStringLiteral("path")).toString();
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
          continue;
        ++n;
      }
      o.insert(QStringLiteral("itemCount"), n);
      arr[i] = o;
    }
    body = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return qMakePair(body, error);
  }));
}

void MainWindow::fillFiles(const QVector<CalcFile> &files) {
  QStringList dirs;
  for (const auto &f : files) {
    if (f.isDir)
      dirs.push_back(f.name);
  }
  m_dirCache.insert(m_path.isEmpty() ? QStringLiteral("/") : m_path, dirs);

  m_files->removeRows(0, m_files->rowCount());
  for (const auto &f : files) {
    auto *name = new QStandardItem(f.name);
    name->setIcon(fileIcon(f.name, f.isDir));
    name->setEditable(false);
    name->setData(f.path, kPathRole);
    name->setData(f.isDir, kDirRole);
    name->setData(QVariant::fromValue(f.size), kSizeRole);
    auto *size = new QStandardItem(f.isDir ? formatItemCount(f.itemCount) : formatSize(f.size));
    size->setEditable(false);
    auto *type = new QStandardItem(typeLabel(f.name, f.isDir));
    type->setEditable(false);
    m_files->appendRow({name, size, type});
  }
  updateActions();
}

void MainWindow::showNavigatorSubdirs(const QString &path, const QPoint &globalPos) {
  if (!hasDevice() || m_busy)
    return;

  const QString key = (path.isEmpty() || path == QLatin1String("/")) ? QStringLiteral("/") : path;
  auto popup = [this, path, key, globalPos](QStringList dirs) {
    dirs.removeAll(QString());
    dirs.removeDuplicates();
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
      return a.toLower() < b.toLower();
    });
    if (dirs.isEmpty())
      return;

    QString currentChild;
    const QString full = m_path.isEmpty() ? QStringLiteral("/") : m_path;
    if (key == QLatin1String("/")) {
      currentChild = full.section(QLatin1Char('/'), 1, 1);
    } else if (full.startsWith(key)) {
      QString rest = full.mid(key.size());
      if (rest.startsWith(QLatin1Char('/')))
        rest.remove(0, 1);
      currentChild = rest.section(QLatin1Char('/'), 0, 0);
    }

    QMenu menu(this);
    for (const QString &dir : dirs) {
      QAction *action = menu.addAction(dir);
      if (dir == currentChild) {
        QFont font = action->font();
        font.setBold(true);
        action->setFont(font);
      }
    }
    QAction *picked = menu.exec(globalPos);
    if (!picked)
      return;
    enterPath(joinCalcPath(path, picked->text()));
  };

  if (m_dirCache.contains(key)) {
    popup(m_dirCache.value(key));
    return;
  }

  setBusy(true);
  m_status->setText("Listing…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QPair<QStringList, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<QStringList, QString>>::finished, this,
          [this, watcher, key, popup] {
            const auto result = watcher->result();
            watcher->deleteLater();
            setBusy(false);
            if (!result.second.isEmpty()) {
              showError(result.second);
              return;
            }
            m_dirCache.insert(key, result.first);
            m_status->setText("Ready");
            popup(result.first);
          });
  watcher->setFuture(QtConcurrent::run([bus, addr, key] {
    NLinkString json{}, err{};
    QStringList dirs;
    QString error;
    if (nlink_list_dir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), key.toUtf8().constData(),
                       &json, &err) != 0) {
      error = takeString(err);
      takeString(json);
      return qMakePair(dirs, error);
    }
    takeString(err);
    const QJsonArray arr = QJsonDocument::fromJson(takeString(json).toUtf8()).array();
    for (const auto &val : arr) {
      const QJsonObject o = val.toObject();
      if (!o.value(QStringLiteral("isDir")).toBool())
        continue;
      const QString name = o.value(QStringLiteral("path")).toString();
      if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        continue;
      dirs.push_back(name);
    }
    return qMakePair(dirs, error);
  }));
}

void MainWindow::setViewMode(int mode) {
  m_stack->setCurrentIndex(mode);
  m_detailsBtn->setChecked(mode == 0);
  m_iconsBtn->setChecked(mode == 1);
}

QVector<CalcFile> MainWindow::selectedFiles() const {
  QVector<CalcFile> out;
  const auto rows = m_detailsView->selectionModel()->selectedRows(0);
  for (const QModelIndex &idx : rows) {
    CalcFile f;
    f.path = idx.data(kPathRole).toString();
    f.isDir = idx.data(kDirRole).toBool();
    f.size = idx.data(kSizeRole).toLongLong();
    f.name = idx.data(Qt::DisplayRole).toString();
    out.push_back(f);
  }
  return out;
}

void MainWindow::onItemActivated(const QModelIndex &index) {
  if (!index.isValid())
    return;
  const QModelIndex name = index.sibling(index.row(), 0);
  if (name.data(kDirRole).toBool())
    enterPath(name.data(kPathRole).toString());
}

void MainWindow::updateActions() {
  const bool ready = hasDevice() && !m_busy;
  const auto sel = selectedFiles();
  const bool any = ready && !sel.isEmpty();
  const bool one = ready && sel.size() == 1;
  const bool oneDir = one && sel[0].isDir;
  if (m_openAct)
    m_openAct->setEnabled(oneDir);
  if (m_downloadAct)
    m_downloadAct->setEnabled(any);
  if (m_uploadAct)
    m_uploadAct->setEnabled(ready);
  if (m_mkdirAct)
    m_mkdirAct->setEnabled(ready);
  if (m_cutAct)
    m_cutAct->setEnabled(any);
  if (m_copyAct)
    m_copyAct->setEnabled(any);
  if (m_pasteAct)
    m_pasteAct->setEnabled(ready && clipboardHasPaste());
  if (m_renameAct)
    m_renameAct->setEnabled(one);
  if (m_deleteAct)
    m_deleteAct->setEnabled(any);
  if (m_upAct)
    m_upAct->setEnabled(ready && !m_path.isEmpty() && m_path != QLatin1String("/"));
}

bool MainWindow::clipboardHasPaste() const {
  const QMimeData *mime = QApplication::clipboard()->mimeData();
  return mime && (mime->hasFormat(kNlinkMime) || mime->hasUrls());
}

void MainWindow::showFileContextMenu(QAbstractItemView *view, const QPoint &viewportPos) {
  if (!view)
    return;
  const QModelIndex idx = view->indexAt(viewportPos);
  if (idx.isValid()) {
    const QModelIndex name = idx.sibling(idx.row(), 0);
    if (!view->selectionModel()->isSelected(name))
      view->selectionModel()->select(name, QItemSelectionModel::ClearAndSelect |
                                               QItemSelectionModel::Rows);
  }
  updateActions();

  QMenu menu(this);
  const auto sel = selectedFiles();
  if (!sel.isEmpty()) {
    if (sel.size() == 1 && sel[0].isDir)
      menu.addAction(m_openAct);
    menu.addAction(m_downloadAct);
    menu.addSeparator();
    menu.addAction(m_cutAct);
    menu.addAction(m_copyAct);
    menu.addAction(m_pasteAct);
    menu.addSeparator();
    menu.addAction(m_renameAct);
    menu.addAction(m_deleteAct);
    menu.addSeparator();
  } else {
    menu.addAction(m_pasteAct);
  }
  menu.addAction(m_mkdirAct);
  menu.addAction(m_uploadAct);
  menu.exec(view->viewport()->mapToGlobal(viewportPos));
}

void MainWindow::openSelected() {
  const auto sel = selectedFiles();
  if (sel.size() == 1 && sel[0].isDir)
    enterPath(sel[0].path);
}

void MainWindow::cutSelected() { placeOnClipboard(true); }

void MainWindow::copySelected() { placeOnClipboard(false); }

void MainWindow::placeOnClipboard(bool cut) {
  const auto files = selectedFiles();
  if (files.isEmpty() || !hasDevice())
    return;
  auto *mime = new CalcMimeData(files, m_bus, m_addr);
  mime->setData(kNlinkMime, encodeNlinkItems(files, cut, m_bus, m_addr));
  QApplication::clipboard()->setMimeData(mime);
  updateActions();
}

void MainWindow::pasteItems() {
  if (!hasDevice() || m_busy)
    return;
  const QMimeData *mime = QApplication::clipboard()->mimeData();
  if (!mime)
    return;
  if (mime->hasFormat(kNlinkMime)) {
    QVector<CalcFile> files;
    bool cut = false;
    int bus = -1, addr = -1;
    if (!decodeNlinkItems(mime->data(kNlinkMime), &files, &cut, &bus, &addr))
      return;
    if (bus != m_bus || addr != m_addr) {
      showError(QStringLiteral("Those items belong to a different calculator."));
      return;
    }
    moveOrCopyItems(files, currentDir(), !cut);
    if (cut)
      QApplication::clipboard()->clear();
    return;
  }
  if (mime->hasUrls()) {
    QStringList paths;
    for (const QUrl &url : mime->urls()) {
      if (url.isLocalFile())
        paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty())
      uploadLocalPaths(paths, currentDir());
  }
}

void MainWindow::goUp() {
  if (m_path.isEmpty() || m_path == QLatin1String("/"))
    return;
  QString p = m_path;
  if (p.endsWith(QLatin1Char('/')))
    p.chop(1);
  const int slash = p.lastIndexOf(QLatin1Char('/'));
  enterPath(slash <= 0 ? QStringLiteral("/") : p.left(slash));
}

bool MainWindow::canAcceptCalcDrop(const QMimeData *mime) const {
  return mime && (mime->hasFormat(kNlinkMime) || mime->hasUrls());
}

void MainWindow::startCalcDrag(QAbstractItemView *view, Qt::DropActions) {
  const auto files = selectedFiles();
  if (files.isEmpty() || !hasDevice())
    return;
  auto *mime = new CalcMimeData(files, m_bus, m_addr);
  mime->setData(kNlinkMime, encodeNlinkItems(files, false, m_bus, m_addr));
  QDrag drag(view);
  drag.setMimeData(mime);
  const QIcon icon = fileIcon(files[0].name, files[0].isDir);
  drag.setPixmap(icon.pixmap(32, 32));
  drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
}

void MainWindow::calcDragEnter(QDragEnterEvent *event) {
  if (!canAcceptCalcDrop(event->mimeData()) || !hasDevice() || m_busy) {
    event->ignore();
    return;
  }
  event->acceptProposedAction();
}

void MainWindow::calcDragMove(QAbstractItemView *view, QDragMoveEvent *event) {
  Q_UNUSED(view);
  if (!canAcceptCalcDrop(event->mimeData()) || !hasDevice() || m_busy) {
    event->ignore();
    return;
  }
  if (event->mimeData()->hasFormat(kNlinkMime) && !(event->modifiers() & Qt::ControlModifier))
    event->setDropAction(Qt::MoveAction);
  else
    event->setDropAction(Qt::CopyAction);
  event->accept();
}

void MainWindow::calcDrop(QAbstractItemView *view, QDropEvent *event) {
  if (!hasDevice() || m_busy) {
    event->ignore();
    return;
  }
  const QPoint pos = view->viewport()->mapFrom(view, event->position().toPoint());
  const QString dest = dropDestPath(view->indexAt(pos));
  if (event->mimeData()->hasFormat(kNlinkMime)) {
    QVector<CalcFile> files;
    bool cut = false;
    int bus = -1, addr = -1;
    if (!decodeNlinkItems(event->mimeData()->data(kNlinkMime), &files, &cut, &bus, &addr)) {
      event->ignore();
      return;
    }
    if (bus != m_bus || addr != m_addr) {
      event->ignore();
      return;
    }
    const bool copy =
        event->dropAction() == Qt::CopyAction || (event->modifiers() & Qt::ControlModifier);
    moveOrCopyItems(files, dest, copy);
    event->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
    event->accept();
    return;
  }
  if (event->mimeData()->hasUrls()) {
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
      if (url.isLocalFile())
        paths.append(url.toLocalFile());
    }
    if (paths.isEmpty()) {
      event->ignore();
      return;
    }
    uploadLocalPaths(paths, dest);
    event->setDropAction(Qt::CopyAction);
    event->accept();
    return;
  }
  event->ignore();
}

void MainWindow::moveOrCopyItems(const QVector<CalcFile> &files, const QString &destDir, bool copy) {
  if (!hasDevice())
    return;
  for (const CalcFile &f : files) {
    const QString target = joinCalcPath(destDir, f.name);
    if (f.path == target)
      continue;
    if (destDir == f.path || destDir.startsWith(f.path + QLatin1Char('/')))
      continue;
    NLinkString err{};
    const int rc =
        copy ? nlink_copy(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                          f.path.toUtf8().constData(), target.toUtf8().constData(), &err)
             : nlink_move(static_cast<uint8_t>(m_bus), static_cast<uint8_t>(m_addr),
                          f.path.toUtf8().constData(), target.toUtf8().constData(), &err);
    if (rc != 0) {
      showError(takeString(err));
      reloadListing();
      return;
    }
    takeString(err);
  }
  reloadListing();
}

void MainWindow::downloadSelected() {
  auto files = selectedFiles();
  if (files.isEmpty())
    return;
  const QString dest = QFileDialog::getExistingDirectory(this, "Download to");
  if (dest.isEmpty())
    return;
  downloadFilesTo(files, dest);
}

void MainWindow::downloadFilesTo(const QVector<CalcFile> &files, const QString &dest) {
  if (files.isEmpty() || dest.isEmpty() || !hasDevice())
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
    hideTransferUi();
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Download complete");
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, files, dest, this] {
    const int total = files.size();
    for (int i = 0; i < total; ++i) {
      const CalcFile &f = files[i];
      notifyQueue(const_cast<MainWindow *>(this), QStringLiteral("Downloading"), f.name, i + 1,
                  total);
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
  uploadLocalPaths(srcs, currentDir());
}

void MainWindow::uploadLocalPaths(const QStringList &paths, const QString &destDir) {
  if (paths.isEmpty() || !hasDevice())
    return;
  struct Job {
    QString local;
    QString remoteDir;
    bool mkdir = false;
  };
  QVector<Job> jobs;
  std::function<void(const QString &, const QString &)> walk = [&](const QString &local,
                                                                   const QString &remoteDir) {
    const QFileInfo fi(local);
    if (fi.isDir()) {
      const QString remote = joinCalcPath(remoteDir, fi.fileName());
      Job dirJob;
      dirJob.local = local;
      dirJob.remoteDir = remote;
      dirJob.mkdir = true;
      jobs.push_back(dirJob);
      const QDir dir(local);
      const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::DirsFirst);
      for (const QFileInfo &child : entries)
        walk(child.absoluteFilePath(), remote);
    } else {
      Job fileJob;
      fileJob.local = local;
      fileJob.remoteDir = remoteDir;
      jobs.push_back(fileJob);
    }
  };
  for (const QString &path : paths)
    walk(path, destDir);

  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Uploading…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    hideTransferUi();
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Upload complete");
    reloadListing();
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, jobs, this] {
    const int total = jobs.size();
    int n = 0;
    for (const Job &job : jobs) {
      ++n;
      notifyQueue(const_cast<MainWindow *>(this),
                  job.mkdir ? QStringLiteral("Creating") : QStringLiteral("Uploading"),
                  QFileInfo(job.local).fileName(), n, total);
      NLinkString err{};
      int rc;
      if (job.mkdir) {
        rc = nlink_mkdir(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                         job.remoteDir.toUtf8().constData(), &err);
      } else {
        rc = nlink_upload_file(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                               job.remoteDir.toUtf8().constData(), job.local.toUtf8().constData(),
                               nlinkProgressThunk, const_cast<MainWindow *>(this), &err);
      }
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
    hideTransferUi();
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

void MainWindow::screenshotCalculator() {
  if (!hasDevice())
    return;
  setBusy(true);
  m_status->setText("Capturing screenshot…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QPair<QImage, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<QImage, QString>>::finished, this, [this, watcher] {
    const auto result = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    hideTransferUi();
    if (!result.second.isEmpty()) {
      showError(result.second);
      return;
    }
    if (result.first.isNull()) {
      showError(QStringLiteral("Screenshot was empty"));
      return;
    }
    m_status->setText("Screenshot captured");

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Screenshot"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *preview = new QLabel;
    QPixmap pix = QPixmap::fromImage(result.first);
    if (pix.width() > 0 && pix.width() < 480)
      pix = pix.scaled(pix.size() * 2, Qt::KeepAspectRatio, Qt::FastTransformation);
    preview->setPixmap(pix);
    preview->setAlignment(Qt::AlignCenter);
    lay->addWidget(preview);

    auto *buttons = new QDialogButtonBox;
    auto *saveBtn = buttons->addButton(QStringLiteral("Save…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(saveBtn, &QPushButton::clicked, this, [this, image = result.first, &dlg] {
      const QString suggested =
          QStringLiteral("nlink-ng-screenshot-%1.png")
              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm")));
      const QString path = QFileDialog::getSaveFileName(&dlg, "Save screenshot", suggested,
                                                        "PNG image (*.png)");
      if (path.isEmpty())
        return;
      if (!image.save(path, "PNG"))
        showError(QStringLiteral("Failed to save screenshot"));
      else
        m_status->setText(QStringLiteral("Saved %1").arg(path));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
  });
  watcher->setFuture(QtConcurrent::run([bus, addr] {
    NLinkImage img{};
    NLinkString err{};
    const int rc =
        nlink_screenshot(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), &img, &err);
    if (rc != 0)
      return qMakePair(QImage(), takeString(err));
    takeString(err);
    if (img.rgba == nullptr || img.width <= 0 || img.height <= 0) {
      nlink_image_free(img);
      return qMakePair(QImage(), QStringLiteral("Screenshot was empty"));
    }
    const QImage view(img.rgba, img.width, img.height, img.stride, QImage::Format_RGBA8888);
    const QImage copy = view.copy();
    nlink_image_free(img);
    return qMakePair(copy, QString());
  }));
}

void MainWindow::exitExamMode() {
  if (!hasDevice())
    return;
  const auto reply = QMessageBox::question(
      this, "Exit exam mode",
      "This uploads “Exit Test Mode.tns” into the calculator’s Press-to-Test folder.\n"
      "If the handheld is in exam mode, it will restart and leave Press-to-Test.\n\n"
      "Continue?");
  if (reply != QMessageBox::Yes)
    return;
  setBusy(true);
  m_status->setText("Exiting exam mode…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    hideTransferUi();
    if (!err.isEmpty()) {
      showError(err);
      return;
    }
    m_status->setText("Exam-mode exit sent. The calculator should restart.");
    QTimer::singleShot(1500, this, &MainWindow::refreshDevices);
  });
  watcher->setFuture(QtConcurrent::run([bus, addr] {
    NLinkString err{};
    const int rc =
        nlink_exit_exam_mode(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), &err);
    if (rc != 0)
      return takeString(err);
    takeString(err);
    return QString();
  }));
}

void MainWindow::backupCalculator() {
  const QString suggested =
      QStringLiteral("nlink-ng-backup-%1.tar.gz")
          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm")));
  const QString dest = QFileDialog::getSaveFileName(
      this, "Backup calculator", suggested, "Backup archive (*.tar.gz *.tgz)");
  if (dest.isEmpty())
    return;
  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Backing up…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    hideTransferUi();
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Backup complete");
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, dest, this] {
    NLinkString err{};
    const QByteArray destUtf = dest.toUtf8();
    const int rc = nlink_backup(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                destUtf.constData(), nlinkProgressThunk,
                                const_cast<MainWindow *>(this), &err);
    if (rc != 0)
      return takeString(err);
    takeString(err);
    return QString();
  }));
}

void MainWindow::restoreCalculator() {
  const QString src = QFileDialog::getOpenFileName(
      this, "Restore backup", QString(), "Backup archive (*.tar.gz *.tgz)");
  if (src.isEmpty())
    return;
  const auto reply = QMessageBox::question(
      this, "Restore backup",
      "This will upload files from the archive onto the calculator.\n"
      "Existing files with the same names may be overwritten.\n\nContinue?");
  if (reply != QMessageBox::Yes)
    return;
  setBusy(true);
  m_transfer->setVisible(true);
  m_status->setText("Restoring…");
  const int bus = m_bus;
  const int addr = m_addr;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    const QString err = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    hideTransferUi();
    if (!err.isEmpty())
      showError(err);
    else
      m_status->setText("Restore complete");
    reloadListing();
  });
  watcher->setFuture(QtConcurrent::run([bus, addr, src, this] {
    NLinkString err{};
    const QByteArray srcUtf = src.toUtf8();
    const int rc = nlink_restore(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr),
                                 srcUtf.constData(), nlinkProgressThunk,
                                 const_cast<MainWindow *>(this), &err);
    if (rc != 0)
      return takeString(err);
    takeString(err);
    return QString();
  }));
}
