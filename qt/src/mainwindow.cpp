#include "mainwindow.h"
#include "roxoptions.h"
#include "browserpage.h"
#include "filepane.h"
#include "filedrop.h"
#include "fileops.h"
#include "hoverpreview.h"
#include "thumbnailer.h"
#include "pathbar.h"
#include "drivemount.h"
#include "imagemounter.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QAbstractSpinBox>
#include <QClipboard>
#include <QCursor>
#include <QMimeDatabase>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QGroupBox>
#include <QStackedWidget>
#include <QSpinBox>
#include <QScrollArea>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QScreen>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTabWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtAlgorithms>
#include <algorithm>
#include <functional>
#include <unistd.h>

namespace {
enum PlaceRoles {
    PathRole = Qt::UserRole,
    DeviceRole,
    MountPointRole,
    DeviceRowRole
};

QIcon icon(const QString &name, QStyle::StandardPixmap fallback)
{
    QIcon themed = QIcon::fromTheme(name);
    return themed.isNull() ? qApp->style()->standardIcon(fallback) : themed;
}

QListWidgetItem *addHeader(QListWidget *list, const QString &text)
{
    auto *item = new QListWidgetItem(text, list);
    item->setFlags(Qt::NoItemFlags);
    QFont f = item->font();
    f.setBold(true);
    item->setFont(f);
    item->setSizeHint(QSize(0, 27));
    return item;
}

void addPlace(QListWidget *list, const QString &text, const QString &iconName,
              const QString &path)
{
    if (path.isEmpty())
        return;
    auto *item = new QListWidgetItem(QIcon::fromTheme(iconName), text, list);
    item->setData(PathRole, QDir::cleanPath(path));
    item->setToolTip(path);
    item->setSizeHint(QSize(0, 31));
}

QString polkitErrorMessage(const QString &error)
{
    if (error.contains(QStringLiteral("NotAuthorizedCanObtain"), Qt::CaseInsensitive) ||
        error.contains(QStringLiteral("authentication agent"), Qt::CaseInsensitive) ||
        error.contains(QStringLiteral("/dev/tty"), Qt::CaseInsensitive)) {
        return QCoreApplication::translate("MainWindow",
                "No graphical Polkit authentication agent is running in SonicDE. Start a Polkit agent in your session and try again.")
                + (error.isEmpty() ? QString() : QStringLiteral("\n\n") + error);
    }
    return error.isEmpty()
       ? QCoreApplication::translate("MainWindow", "The operation could not be completed.")
       : error;
}

QString humanSize(qint64 size)
{
    return size > 0 ? QLocale().formattedDataSize(size) : QString();
}

bool copyPathRecursive(const QString &source, const QString &destination, QString *error)
{
    QFileInfo info(source);
    if (!info.exists()) {
        if (error) *error = QCoreApplication::translate("MainWindow", "Source does not exist: %1").arg(source);
        return false;
    }

    if (info.isDir() && !info.isSymLink()) {
        QDir destDir(destination);
        if (!destDir.exists() && !QDir().mkpath(destination)) {
            if (error) *error = QCoreApplication::translate("MainWindow", "Could not create: %1").arg(destination);
            return false;
        }
        QDir srcDir(source);
        const QFileInfoList entries = srcDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo &entry : entries) {
            const QString childDest = QDir(destination).filePath(entry.fileName());
            if (!copyPathRecursive(entry.absoluteFilePath(), childDest, error))
                return false;
        }
        return true;
    }

    if (QFileInfo::exists(destination)) {
        if (error) *error = QCoreApplication::translate("MainWindow", "Destination already exists: %1").arg(destination);
        return false;
    }
    if (!QFile::copy(source, destination)) {
        if (error) *error = QCoreApplication::translate("MainWindow", "Could not copy %1").arg(info.fileName());
        return false;
    }
    return true;
}

bool removePathRecursive(const QString &path)
{
    QFileInfo info(path);
    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}
}

MainWindow::MainWindow(const QStringList &initialPaths, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(geteuid() == 0 ? tr("ROX-Filer2-QT (ROOT)") : tr("ROX-Filer2-QT"));
    setWindowIcon(qApp->windowIcon());
    resize(1040, 680);
    setMinimumSize(760, 480);

    buildActions();
    buildUi();
    // A root Qt process may pick the fallback light QStyle even though its
    // inherited, validated palette is dark. Restrict the palette-backed CSS
    // to file-manager chrome; do NOT load the caller's QSS/plugins as root.
    if (geteuid() == 0 && !qEnvironmentVariable("ROX_QT_THEME_PALETTE").isEmpty()) {
        const QPalette p = qApp->palette();
        const QString window = p.color(QPalette::Window).name(QColor::HexRgb);
        const QString foreground = p.color(QPalette::WindowText).name(QColor::HexRgb);
        const QString input = p.color(QPalette::Base).name(QColor::HexRgb);
        const QString inputText = p.color(QPalette::Text).name(QColor::HexRgb);
        const QString button = p.color(QPalette::Button).name(QColor::HexRgb);
        const QString highlight = p.color(QPalette::Highlight).name(QColor::HexRgb);
        const QString highlighted = p.color(QPalette::HighlightedText).name(QColor::HexRgb);
        const QString separator = p.color(QPalette::Mid).name(QColor::HexRgb);
        setStyleSheet(QStringLiteral(
            "QMenuBar, QToolBar { background-color: %1; color: %2; border: 0px; }"
            "QMenuBar::item { background: transparent; color: %2; }"
            "QMenuBar::item:selected, QMenuBar::item:pressed { background: %6; color: %7; }"
            "QMenu { background-color: %1; color: %2; border: 1px solid %8; }"
            "QMenu::item:selected { background-color: %6; color: %7; }"
            "QToolBar QToolButton { background: transparent; color: %2; border: 0px; }"
            "QToolBar QToolButton:hover, QToolBar QToolButton:checked { background: %5; }"
            "QToolBar QLabel { color: %2; background: transparent; }"
            "QToolBar QLineEdit { background-color: %3; color: %4; selection-background-color: %6; selection-color: %7; }"
        ).arg(window, foreground, input, inputText, button, highlight, highlighted, separator));
    }
    buildPlaces();
    // Images mounted by ROX (this window, another ROX window, the GTK3 build or
    // the desktop icons) are recorded in the Image Mounter state files, like
    // GTK3 drives.c.  Poll them and rebuild Places only when they change.
    m_imageSnapshot = ImageMounter::signature(ImageMounter::managedMounts());
    auto *imageRefresh = new QTimer(this);
    imageRefresh->setInterval(3000);
    connect(imageRefresh, &QTimer::timeout, this, [this] {
        const QString current = ImageMounter::signature(ImageMounter::managedMounts());
        if (current != m_imageSnapshot) {
            m_imageSnapshot = current;
            buildPlaces();
        }
    });
    imageRefresh->start();

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    const QString saved = settings.value(QStringLiteral("session/lastPath"), QDir::homePath()).toString();

    if (initialPaths.isEmpty()) {
        addTab(saved);
    } else {
        for (const QString &path : initialPaths)
            addTab(path, false);
        m_tabs->setCurrentIndex(0);
    }

    m_sidebarToggle->setChecked(settings.value(QStringLiteral("ui/sidebar"), true).toBool());
    m_places->setVisible(m_sidebarToggle->isChecked());
    m_previewToggle->setChecked(settings.value(QStringLiteral("ui/preview"), false).toBool());
    m_preview->setVisible(m_previewToggle->isChecked());
    m_pathBar->setBreadcrumbMode(settings.value(QStringLiteral("ui/breadcrumbs"), true).toBool());

    syncUiFromCurrentPage();
    // The file view, not the toolbar or Places list, receives normal typing.
    QTimer::singleShot(0, this, [this] {
        if (BrowserPage *page = currentPage()) page->activePane()->focusFiles();
    });
}

MainWindow::~MainWindow()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/sidebar"), m_sidebarToggle && m_sidebarToggle->isChecked());
    settings.setValue(QStringLiteral("ui/preview"), m_previewToggle && m_previewToggle->isChecked());
    settings.setValue(QStringLiteral("ui/breadcrumbs"), m_pathBar && m_pathBar->breadcrumbMode());
    if (currentPage())
        settings.setValue(QStringLiteral("session/lastPath"), currentPage()->currentPath());
}

void MainWindow::buildActions()
{
    m_back = new QAction(icon(QStringLiteral("go-previous"), QStyle::SP_ArrowBack), tr("Back"), this);
    m_forward = new QAction(icon(QStringLiteral("go-next"), QStyle::SP_ArrowForward), tr("Forward"), this);
    m_up = new QAction(icon(QStringLiteral("go-up"), QStyle::SP_ArrowUp), tr("Up"), this);
    m_refresh = new QAction(icon(QStringLiteral("view-refresh"), QStyle::SP_BrowserReload), tr("Refresh"), this);
    m_home = new QAction(QIcon::fromTheme(QStringLiteral("user-home")), tr("Home"), this);
    m_newTab = new QAction(QIcon::fromTheme(QStringLiteral("tab-new")), tr("New Tab"), this);
    m_closeTab = new QAction(QIcon::fromTheme(QStringLiteral("tab-close")), tr("Close Tab"), this);
    m_split = new QAction(QIcon::fromTheme(QStringLiteral("view-split-left-right")), tr("Split View"), this);
    m_split->setCheckable(true);
    m_icons = new QAction(QIcon::fromTheme(QStringLiteral("view-list-icons")), tr("Icons"), this);
    m_icons->setCheckable(true);
    m_details = new QAction(QIcon::fromTheme(QStringLiteral("view-list-details")), tr("Details"), this);
    m_details->setCheckable(true);
    m_previewToggle = new QAction(QIcon::fromTheme(QStringLiteral("document-preview")), tr("Show Preview Pane"), this);
    m_previewToggle->setCheckable(true);
    m_sidebarToggle = new QAction(QIcon::fromTheme(QStringLiteral("view-list-tree")), tr("Show Sidebar"), this);
    m_sidebarToggle->setCheckable(true);
    m_sidebarToggle->setChecked(true);
    m_terminal = new QAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")), tr("Terminal Here"), this);

    m_newTab->setShortcut(QKeySequence::AddTab);
    m_closeTab->setShortcut(QKeySequence::Close);
    m_split->setShortcut(QKeySequence(Qt::Key_F3));
    m_terminal->setShortcut(QKeySequence(Qt::Key_F4));
    m_refresh->setShortcut(QKeySequence::Refresh);

    connect(m_back, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->goBack(); });
    connect(m_forward, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->goForward(); });
    connect(m_up, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->goUp(); });
    connect(m_refresh, &QAction::triggered, this, [this] { if (currentPage()) { currentPage()->refresh(); buildPlaces(); } });
    connect(m_home, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->navigateActive(QDir::homePath()); });
    connect(m_newTab, &QAction::triggered, this, [this] { addTab(currentPage() ? currentPage()->currentPath() : QDir::homePath()); });
    connect(m_closeTab, &QAction::triggered, this, [this] { closeTab(m_tabs ? m_tabs->currentIndex() : -1); });
    connect(m_split, &QAction::triggered, this, [this] { if (currentPage()) { currentPage()->toggleSplit(); syncUiFromCurrentPage(); } });
    connect(m_icons, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->setIconMode(true); syncUiFromCurrentPage(); });
    connect(m_details, &QAction::triggered, this, [this] { if (currentPage()) currentPage()->setIconMode(false); syncUiFromCurrentPage(); });
    connect(m_terminal, &QAction::triggered, this, &MainWindow::launchTerminal);
}

void MainWindow::buildUi()
{
    auto *toolbar = addToolBar(tr("Navigation"));
    toolbar->setObjectName(QStringLiteral("navigationToolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(m_back);
    toolbar->addAction(m_forward);
    toolbar->addAction(m_up);
    toolbar->addAction(m_refresh);
    toolbar->addAction(m_home);
    toolbar->addSeparator();

    m_pathBar = new PathBar(toolbar);
    m_pathBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(m_pathBar);
    toolbar->addSeparator();
    toolbar->addAction(m_newTab);
    toolbar->addAction(m_split);
    toolbar->addAction(m_icons);
    toolbar->addAction(m_details);
    toolbar->addAction(m_previewToggle);

    auto *filterBar = addToolBar(tr("Filter"));
    filterBar->setObjectName(QStringLiteral("filterToolbar"));
    filterBar->setMovable(false);
    auto *filterLabel = new QLabel(tr("Filter:"), filterBar);
    m_filter = new QLineEdit(filterBar);
    m_filter->setPlaceholderText(tr("Filter current folder…"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(300);
    filterBar->addWidget(filterLabel);
    filterBar->addWidget(m_filter);
    filterBar->addSeparator();
    filterBar->addAction(m_terminal);

    m_rootSplitter = new QSplitter(Qt::Horizontal, this);
    m_places = new QListWidget(m_rootSplitter);
    m_places->setMinimumWidth(180);
    m_places->setMaximumWidth(330);
    m_places->setFrameShape(QFrame::NoFrame);
    m_places->setSpacing(0);

    m_tabs = new QTabWidget(m_rootSplitter);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);

    // Direct drops on bookmarks/places and tab headers, without GVfs.
    m_places->setAcceptDrops(true);
    m_places->viewport()->setAcceptDrops(true);
    m_places->viewport()->installEventFilter(this);
    m_tabs->tabBar()->setAcceptDrops(true);
    m_tabs->tabBar()->installEventFilter(this);

    m_preview = new QWidget(m_rootSplitter);
    auto *previewLayout = new QVBoxLayout(m_preview);
    m_previewImage = new QLabel(m_preview);
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewImage->setMinimumSize(180, 160);
    m_previewInfo = new QLabel(m_preview);
    m_previewInfo->setWordWrap(true);
    m_previewInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(m_previewImage);
    previewLayout->addWidget(m_previewInfo);
    previewLayout->addStretch(1);
    m_preview->setMinimumWidth(200);
    m_preview->setMaximumWidth(340);
    m_preview->hide();

    m_rootSplitter->addWidget(m_places);
    m_rootSplitter->addWidget(m_tabs);
    m_rootSplitter->addWidget(m_preview);
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setStretchFactor(2, 0);
    setCentralWidget(m_rootSplitter);

    buildMenus();

    auto *editLocation = new QAction(tr("Edit Location"), this);
    editLocation->setShortcut(QKeySequence(Qt::Key_F6));
    addAction(editLocation);
    connect(editLocation, &QAction::triggered, m_pathBar, &PathBar::beginEdit);

    auto *focusLocation = new QAction(this);
    focusLocation->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    addAction(focusLocation);
    connect(focusLocation, &QAction::triggered, m_pathBar, &PathBar::beginEdit);

    auto *focusFilter = new QAction(this);
    focusFilter->setShortcut(QKeySequence::Find);
    addAction(focusFilter);
    connect(focusFilter, &QAction::triggered, m_filter, [this] {
        m_filter->setFocus(Qt::ShortcutFocusReason);
        m_filter->selectAll();
    });

    connect(m_pathBar, &PathBar::pathActivated, this,
            [this](const QString &path) { if (currentPage()) currentPage()->navigateActive(path); });
    connect(m_filter, &QLineEdit::textChanged, this,
            [this](const QString &text) { if (currentPage()) currentPage()->setFilterText(text); });
    connect(m_previewToggle, &QAction::toggled, m_preview, &QWidget::setVisible);
    connect(m_sidebarToggle, &QAction::toggled, m_places, &QWidget::setVisible);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { m_filter->clear(); syncUiFromCurrentPage(); });
    connect(m_places, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const QString path = item->data(PathRole).toString();
        if (!path.isEmpty() && currentPage())
            currentPage()->navigateActive(path);
    });
    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item->data(DeviceRowRole).toBool())
            return;
        const QString path = item->data(PathRole).toString();
        if (!path.isEmpty() && currentPage())
            currentPage()->navigateActive(path);
    });

    statusBar()->showMessage(tr("Ready"));
    connect(HoverPreview::instance(), &HoverPreview::message, this,
            [this](const QString &text) { statusBar()->showMessage(text, 6000); });
}

void MainWindow::buildMenus()
{
    menuBar()->clear();

    auto *fileMenu = menuBar()->addMenu(tr("File"));
    QAction *newWindow = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("window-new")), tr("New Window"));
    fileMenu->addAction(m_newTab);
    QAction *duplicateTab = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Duplicate Tab"));
    fileMenu->addAction(m_closeTab);
    QAction *closeOthers = fileMenu->addAction(tr("Close Other Tabs"));
    QAction *closeRight = fileMenu->addAction(tr("Close Tabs to the Right"));
    fileMenu->addSeparator();
    fileMenu->addAction(m_terminal);
    fileMenu->addSeparator();
    QAction *closeWindow = fileMenu->addAction(tr("Close"));
    closeWindow->setShortcut(QKeySequence::Quit);
    connect(newWindow, &QAction::triggered, this, &MainWindow::openNewWindow);
    connect(duplicateTab, &QAction::triggered, this, &MainWindow::duplicateCurrentTab);
    connect(closeOthers, &QAction::triggered, this, &MainWindow::closeOtherTabs);
    connect(closeRight, &QAction::triggered, this, &MainWindow::closeTabsToRight);
    connect(closeWindow, &QAction::triggered, this, &QWidget::close);

    auto *editMenu = menuBar()->addMenu(tr("Edit"));
    QAction *selectAll = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-select-all")), tr("Select All"));
    selectAll->setShortcut(QKeySequence::SelectAll);
    selectAll->setShortcutContext(Qt::WindowShortcut);
    // A text editor may own Ctrl+A (notably the filter and path bar). If Qt
    // dispatches the menu shortcut first, select the text instead of files.
    connect(selectAll, &QAction::triggered, this, [this] {
        QWidget *focused = QApplication::focusWidget();
        if (auto *line = qobject_cast<QLineEdit *>(focused)) { line->selectAll(); return; }
        if (auto *edit = qobject_cast<QTextEdit *>(focused)) { edit->selectAll(); return; }
        if (auto *edit = qobject_cast<QPlainTextEdit *>(focused)) { edit->selectAll(); return; }
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(focused)) {
            if (auto *line = spin->findChild<QLineEdit *>()) { line->selectAll(); return; }
        }
        if (BrowserPage *page = currentPage()) page->activePane()->selectAllFiles();
    });
    QAction *clearSelection = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), tr("Clear Selection"));
    connect(clearSelection, &QAction::triggered, this, [this] {
        if (BrowserPage *page = currentPage()) page->activePane()->clearFileSelection();
    });
    QAction *invertSelection = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-select-all")), tr("Invert Selection"));
    connect(invertSelection, &QAction::triggered, this, [this] {
        if (BrowserPage *page = currentPage()) page->activePane()->invertSelection();
    });
    QAction *selectByName = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), tr("Select by Name…"));
    connect(selectByName, &QAction::triggered, this, [this] {
        if (BrowserPage *page = currentPage()) page->activePane()->selectByName();
    });
    editMenu->addSeparator();

    // Restore the fundamental file operations in the top-level Edit menu.
    // FilePane owns Del / Shift+Del / Ctrl+C etc. as widget shortcuts; do
    // NOT register a second copy of those shortcuts on MainWindow, since
    // Qt would then report ambiguous shortcuts in split and tab views.
    auto *cutFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-cut")),
        QCoreApplication::translate("FilePane", "Cut"));
    auto *copyFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
        QCoreApplication::translate("FilePane", "Copy"));
    auto *pasteFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-paste")),
        QCoreApplication::translate("FilePane", "Paste"));
    auto *renameFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")),
        QCoreApplication::translate("FilePane", "Rename…"));
    editMenu->addSeparator();
    auto *trashFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("user-trash")),
        QCoreApplication::translate("FilePane", "Move to Trash"));
    auto *deleteFiles = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
        QCoreApplication::translate("FilePane", "Delete Permanently…"));
    auto currentPane = [this]() -> FilePane * {
        BrowserPage *page = currentPage();
        return page ? page->activePane() : nullptr;
    };
    connect(cutFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->copyToClipboard(true);
    });
    connect(copyFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->copyToClipboard(false);
    });
    connect(pasteFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->paste();
    });
    connect(renameFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->renameSelected();
    });
    connect(trashFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->trashSelected();
    });
    connect(deleteFiles, &QAction::triggered, this, [currentPane] {
        if (FilePane *pane = currentPane()) pane->deleteSelected();
    });
    connect(editMenu, &QMenu::aboutToShow, this,
        [currentPane, cutFiles, copyFiles, pasteFiles, renameFiles, trashFiles, deleteFiles] {
        FilePane *pane = currentPane();
        const int count = pane ? pane->selectedPaths().size() : 0;
        const bool inTrash = pane && pane->isTrashDirectory();
        cutFiles->setEnabled(count > 0 && !inTrash);
        copyFiles->setEnabled(count > 0);
        pasteFiles->setEnabled(pane && !inTrash);
        renameFiles->setEnabled(count == 1);
        trashFiles->setEnabled(count > 0 && !inTrash);
        deleteFiles->setEnabled(count > 0);
    });

    editMenu->addSeparator();
    QAction *copyOther = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy to Other Pane (Shift+F5)"));
    QAction *moveOther = editMenu->addAction(QIcon::fromTheme(QStringLiteral("go-jump")), tr("Move to Other Pane (Shift+F6)"));
    copyOther->setShortcut(QKeySequence(QStringLiteral("Shift+F5")));
    moveOther->setShortcut(QKeySequence(QStringLiteral("Shift+F6")));
    editMenu->addSeparator();
    QAction *options = editMenu->addAction(QIcon::fromTheme(QStringLiteral("preferences-system")), tr("Options…"));
    connect(copyOther, &QAction::triggered, this, [this] { copyOrMoveToOtherPane(false); });
    connect(moveOther, &QAction::triggered, this, [this] { copyOrMoveToOtherPane(true); });
    connect(options, &QAction::triggered, this, &MainWindow::showOptions);

    auto *viewMenu = menuBar()->addMenu(tr("View"));
    viewMenu->addAction(m_sidebarToggle);
    viewMenu->addAction(m_previewToggle);
    QAction *breadcrumbs = viewMenu->addAction(tr("Breadcrumb Location Bar"));
    breadcrumbs->setCheckable(true);
    breadcrumbs->setChecked(m_pathBar->breadcrumbMode());
    connect(breadcrumbs, &QAction::toggled, this, [this](bool enabled) {
        m_pathBar->setBreadcrumbMode(enabled);
        QSettings().setValue(QStringLiteral("ui/breadcrumbs"), enabled);
    });
    viewMenu->addAction(m_split);
    QAction *quickFilter = viewMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-find")), tr("Quick Filter (Ctrl+I)"));
    quickFilter->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(quickFilter, &QAction::triggered, m_filter, [this] {
        m_filter->setFocus(Qt::ShortcutFocusReason);
        m_filter->selectAll();
    });
    viewMenu->addSeparator();
    viewMenu->addAction(m_icons);
    viewMenu->addAction(m_details);
    viewMenu->addSeparator();
    viewMenu->addAction(m_refresh);
    QAction *search = viewMenu->addAction(QIcon::fromTheme(QStringLiteral("system-search")), tr("Search"));
    connect(search, &QAction::triggered, m_filter, [this] {
        m_filter->setFocus(Qt::ShortcutFocusReason);
        m_filter->selectAll();
    });

    auto *goMenu = menuBar()->addMenu(tr("Go"));
    goMenu->addAction(m_back);
    goMenu->addAction(m_forward);
    goMenu->addAction(m_up);
    goMenu->addAction(m_home);

    auto *sambaMenu = menuBar()->addMenu(tr("Samba"));
    QAction *connectShare = sambaMenu->addAction(QIcon::fromTheme(QStringLiteral("network-server")), tr("Connect to SMB Share…"));
    QAction *disconnectShare = sambaMenu->addAction(QIcon::fromTheme(QStringLiteral("media-eject")), tr("Disconnect SMB Share…"));
    QAction *sharedFolders = sambaMenu->addAction(QIcon::fromTheme(QStringLiteral("folder-remote")), tr("Shared Folders…"));
    connect(disconnectShare, &QAction::triggered, this, &MainWindow::disconnectSmb);
    connect(connectShare, &QAction::triggered, this, &MainWindow::connectSmb);
    connect(sharedFolders, &QAction::triggered, this, &MainWindow::showSharedFolders);

    m_bookmarksMenu = menuBar()->addMenu(tr("Bookmarks"));
    rebuildBookmarks();

    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    QAction *helpFiles = helpMenu->addAction(QIcon::fromTheme(QStringLiteral("help-contents")), tr("Show Help Files"));
    helpMenu->addSeparator();
    QAction *about = helpMenu->addAction(QIcon::fromTheme(QStringLiteral("help-about")), tr("About"));
    connect(helpFiles, &QAction::triggered, this, &MainWindow::showHelp);
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::buildPlaces()
{
    m_places->clear();
    addHeader(m_places, tr("Places"));
    addPlace(m_places, tr("Home"), QStringLiteral("user-home"), QDir::homePath());
    addPlace(m_places, tr("Desktop"), QStringLiteral("user-desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    addPlace(m_places, tr("Documents"), QStringLiteral("folder-documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    addPlace(m_places, tr("Downloads"), QStringLiteral("folder-download"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    addPlace(m_places, tr("Music"), QStringLiteral("folder-music"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    addPlace(m_places, tr("Pictures"), QStringLiteral("folder-pictures"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    addPlace(m_places, tr("Videos"), QStringLiteral("folder-videos"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    addPlace(m_places, tr("Templates"), QStringLiteral("folder-templates"), QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation));

    const QString trashFiles = QDir::homePath() + QStringLiteral("/.local/share/Trash/files");
    if (QFileInfo::exists(trashFiles))
        addPlace(m_places, tr("Trash"), QStringLiteral("user-trash"), trashFiles);

    QSettings settings;
    const QStringList bookmarks = settings.value(QStringLiteral("bookmarks/paths")).toStringList();
    if (!bookmarks.isEmpty()) {
        addHeader(m_places, tr("Bookmarks"));
        for (const QString &path : bookmarks) {
            if (!QFileInfo(path).isDir())
                continue;
            QString label = QFileInfo(path).fileName();
            if (label.isEmpty()) label = path;
            addPlace(m_places, label, QStringLiteral("folder-bookmark"), path);
        }
    }

    addHeader(m_places, tr("Devices"));
    addPlace(m_places, tr("Filesystem"), QStringLiteral("drive-harddisk"), QDir::rootPath());
    rebuildDevices();
    m_imageSnapshot = ImageMounter::signature(ImageMounter::managedMounts());
}

QList<MainWindow::DeviceInfo> MainWindow::enumerateBlockDevices() const
{
    QList<DeviceInfo> result;
    const QString lsblk = QStandardPaths::findExecutable(QStringLiteral("lsblk"));
    if (!lsblk.isEmpty()) {
        QProcess p;
        p.start(lsblk, {QStringLiteral("-J"), QStringLiteral("-b"),
                        QStringLiteral("-o"),
                        QStringLiteral("PATH,LABEL,TYPE,FSTYPE,MOUNTPOINT,RM,HOTPLUG,SIZE,MODEL")});
        if (p.waitForFinished(2500) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
            const QJsonArray roots = doc.object().value(QStringLiteral("blockdevices")).toArray();
            std::function<void(const QJsonArray &)> walk = [&](const QJsonArray &array) {
                for (const QJsonValue &value : array) {
                    const QJsonObject o = value.toObject();
                    DeviceInfo d;
                    d.device = o.value(QStringLiteral("path")).toString();
                    d.label = o.value(QStringLiteral("label")).toString();
                    d.type = o.value(QStringLiteral("type")).toString();
                    d.fsType = o.value(QStringLiteral("fstype")).toString();
                    d.mountPoint = o.value(QStringLiteral("mountpoint")).toString();
                    d.model = o.value(QStringLiteral("model")).toString().trimmed();
                    d.size = o.value(QStringLiteral("size")).toVariant().toLongLong();
                    d.removable = o.value(QStringLiteral("rm")).toVariant().toBool() ||
                                  o.value(QStringLiteral("hotplug")).toVariant().toBool();

                    const bool usefulType = d.type == QStringLiteral("part") ||
                                            d.type == QStringLiteral("crypt") ||
                                            d.type.startsWith(QStringLiteral("raid")) ||
                                            d.type == QStringLiteral("lvm");
                    const bool filesystemOnDisk = d.type == QStringLiteral("disk") && !d.fsType.isEmpty();
                    if ((usefulType || filesystemOnDisk) && !d.device.isEmpty() &&
                        !QRegularExpression(QStringLiteral("^/dev/loop[0-9]+(?:p[0-9]+)?$")).match(d.device).hasMatch() &&
                        d.fsType != QStringLiteral("swap")) {
                        result.append(d);
                    }
                    walk(o.value(QStringLiteral("children")).toArray());
                }
            };
            walk(roots);
        }
    }

    if (result.isEmpty()) {
        for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
            const QString device = QString::fromUtf8(storage.device());
            if (!storage.isValid() || !storage.isReady() || !device.startsWith(QStringLiteral("/dev/")) ||
                QRegularExpression(QStringLiteral("^/dev/loop[0-9]+(?:p[0-9]+)?$")).match(device).hasMatch())
                continue;
            DeviceInfo d;
            d.device = device;
            d.label = storage.displayName();
            d.mountPoint = storage.rootPath();
            d.fsType = QString::fromUtf8(storage.fileSystemType());
            d.size = storage.bytesTotal();
            result.append(d);
        }
    }

    // Loop devices are excluded by the lsblk partition filter above.  Like
    // GTK3 drives.c (append_managed_image_mounts), list ONLY the images ROX
    // itself mounted: never scan /dev/loop*, which Puppy/Essora use for their
    // own SFS layers.
    for (const ImageMounter::Managed &image : ImageMounter::managedMounts()) {
        DeviceInfo d;
        d.device = image.blockdev;
        d.mountPoint = image.mountpoint;
        d.imagePath = image.image;
        d.label = QFileInfo(image.image).fileName();
        d.fsType = image.image.endsWith(QStringLiteral(".iso"), Qt::CaseInsensitive)
            ? QStringLiteral("iso9660") : QString();
        d.size = QFileInfo(image.image).size();
        d.image = true;
        d.protectedImage = false;
        d.type = QStringLiteral("image");
        result.append(d);
    }
    std::sort(result.begin(), result.end(), [](const DeviceInfo &a, const DeviceInfo &b) {
        // Images follow ordinary drives as a dedicated Places section.
        if (a.image != b.image) return !a.image;
        const QString al = a.label.isEmpty() ? a.device : a.label;
        const QString bl = b.label.isEmpty() ? b.device : b.label;
        return QString::localeAwareCompare(al, bl) < 0;
    });
    return result;
}

void MainWindow::rebuildDevices()
{
    const QList<DeviceInfo> devices = enumerateBlockDevices();
    QSettings deviceSettings;
    const bool showInternal = deviceSettings.value(QStringLiteral("devices/showInternal"), true).toBool();
    const bool showRemovable = deviceSettings.value(QStringLiteral("devices/showRemovable"), true).toBool();
    const bool onlyMounted = deviceSettings.value(QStringLiteral("devices/onlyMounted"), false).toBool();
    const bool showSystem = deviceSettings.value(QStringLiteral("devices/showSystem"), false).toBool();
    const bool showImages = deviceSettings.value(QStringLiteral("devices/showImages"), true).toBool();
    bool imageHeader = false;
    for (const DeviceInfo &d : devices) {
        if (d.image) {
            if (!showImages) continue; // Protected images are VISIBLE; only eject is restricted.
            if (!imageHeader) { addHeader(m_places, tr("Mounted images")); imageHeader = true; }
        } else if ((d.removable && !showRemovable) || (!d.removable && !showInternal)) continue;
        if (onlyMounted && d.mountPoint.isEmpty()) continue;
        if (!showSystem && (d.mountPoint == QStringLiteral("/") ||
                            d.mountPoint == QStringLiteral("/boot") ||
                            d.mountPoint == QStringLiteral("/boot/efi"))) continue;
        auto *item = new QListWidgetItem(m_places);
        item->setData(DeviceRowRole, true);
        item->setData(DeviceRole, d.device);
        item->setData(MountPointRole, d.mountPoint);
        item->setSizeHint(QSize(0, 31));

        auto *row = new QWidget(m_places);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 0, 4, 0);
        layout->setSpacing(4);

        auto *openButton = new QToolButton(row);
        openButton->setAutoRaise(true);
        openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        // An expanding QToolButton centres its icon+text under many Qt styles.
        // Let the button keep its natural size and leave the space on the RIGHT,
        // so every device begins at exactly the same x as the Places entries.
        openButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        openButton->setMaximumWidth(m_places->maximumWidth() - 60);
        const QString iconName = d.image
            ? (d.fsType == QStringLiteral("iso9660") || d.fsType == QStringLiteral("udf")
                ? QStringLiteral("media-optical") : QStringLiteral("drive-harddisk"))
            : (d.removable ? QStringLiteral("drive-removable-media")
                           : QStringLiteral("drive-harddisk"));
        openButton->setIcon(QIcon::fromTheme(iconName));
        QString label = d.label.trimmed();
        if (label.isEmpty() && !d.mountPoint.isEmpty() && d.mountPoint != QStringLiteral("/"))
            label = QFileInfo(d.mountPoint).fileName();
        if (label.isEmpty())
            label = QFileInfo(d.device).fileName();
        openButton->setText(label);

        QStringList tip;
        if (d.image) tip << tr("Disk image") << d.imagePath;
        tip << d.device;
        if (!d.mountPoint.isEmpty()) tip << d.mountPoint;
        if (!d.fsType.isEmpty()) tip << d.fsType;
        if (d.size > 0) tip << humanSize(d.size);
        if (!d.model.isEmpty()) tip << d.model;
        if (d.image && d.protectedImage) tip << tr("System image (protected)");
        openButton->setToolTip(tip.join(QStringLiteral(" · ")));
        connect(openButton, &QToolButton::clicked, this, [this, d] { openDevice(d.device, d.mountPoint); });
        layout->addWidget(openButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
        layout->addStretch(1); // Keep the eject button at the right edge.

        if (!d.mountPoint.isEmpty() && d.mountPoint != QStringLiteral("/") &&
            (!d.image || !d.protectedImage)) {
            auto *unmount = new QToolButton(row);
            unmount->setAutoRaise(true);
            QIcon ejectIcon = QIcon::fromTheme(QStringLiteral("media-eject-symbolic"));
            if (ejectIcon.isNull()) ejectIcon = QIcon::fromTheme(QStringLiteral("media-eject"));
            unmount->setIcon(ejectIcon);
            unmount->setToolTip(d.image ? QCoreApplication::translate("FilePane", "Unmount Image") : tr("Unmount"));
            if (d.image) {
                // Image Mounter: umount + detach the loop + remove the state.
                connect(unmount, &QToolButton::clicked, this, [this, d] {
                    QPointer<MainWindow> self(this);
                    ImageMounter::unmount(this, d.imagePath, [self](bool) { if (self) self->buildPlaces(); });
                });
            } else {
                connect(unmount, &QToolButton::clicked, this, [this, d] { unmountDevice(d.device); });
            }
            layout->addWidget(unmount);
        }

        m_places->setItemWidget(item, row);
    }
}

QString MainWindow::mountPointForDevice(const QString &device) const
{
    for (const DeviceInfo &d : enumerateBlockDevices()) {
        if (d.device == device)
            return d.mountPoint;
    }
    return QString();
}

void MainWindow::openDevice(const QString &device, const QString &mountPoint)
{
    if (!mountPoint.isEmpty()) {
        if (currentPage()) currentPage()->navigateActive(mountPoint);
        return;
    }
    mountDevice(device, true);
}

void MainWindow::mountDevice(const QString &device, bool openAfterMount)
{
    if (!device.startsWith(QStringLiteral("/dev/"))) return;
    statusBar()->showMessage(tr("Mounting %1…").arg(device));
    SonicDriveMount::run(this, this, SonicDriveMount::Action::Mount, device,
        [this, device, openAfterMount](bool ok, const QString &mountPath) {
        if (!ok) { statusBar()->clearMessage(); return; }
        buildPlaces();
        if (openAfterMount) {
            const QString detected = mountPointForDevice(device);
            const QString destination = QFileInfo(mountPath).isDir() ? mountPath : detected;
            if (!destination.isEmpty() && currentPage())
                currentPage()->navigateActive(destination);
        }
        statusBar()->showMessage(tr("Mounted %1").arg(device), 3500);
    });
}

void MainWindow::unmountDevice(const QString &device)
{
    if (!device.startsWith(QStringLiteral("/dev/"))) return;
    statusBar()->showMessage(tr("Unmounting %1…").arg(device));
    SonicDriveMount::run(this, this, SonicDriveMount::Action::Unmount, device,
        [this](bool ok, const QString &) {
        if (!ok) { statusBar()->clearMessage(); return; }
        buildPlaces();
        statusBar()->showMessage(tr("Device unmounted safely."), 3500);
    });
}

BrowserPage *MainWindow::currentPage() const
{
    return m_tabs ? qobject_cast<BrowserPage *>(m_tabs->currentWidget()) : nullptr;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    const bool onPlaces = m_places && watched == m_places->viewport();
    const bool onTabs = m_tabs && watched == m_tabs->tabBar();
    if (!onPlaces && !onTabs)
        return QMainWindow::eventFilter(watched, event);

    auto destination = [this, onPlaces, onTabs](const QPoint &pos) -> QString {
        if (onPlaces) {
            QListWidgetItem *item = m_places->itemAt(pos);
            if (!item) return {};
            QString path = item->data(PathRole).toString();
            if (path.isEmpty() && item->data(DeviceRowRole).toBool())
                path = item->data(MountPointRole).toString();
            return QFileInfo(path).isDir() ? path : QString();
        }
        if (onTabs) {
            const int tab = m_tabs->tabBar()->tabAt(pos);
            auto *page = tab >= 0 ? qobject_cast<BrowserPage *>(m_tabs->widget(tab)) : nullptr;
            return page ? page->currentPath() : QString();
        }
        return {};
    };
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *e = static_cast<QDragEnterEvent *>(event);
        FileDrop::allow(e);
        FileDrop::log(QStringLiteral("sidebar/tab drag-enter source=%1 accepted=%2 actions=%3")
            .arg(onPlaces ? QStringLiteral("places") : QStringLiteral("tabs"),
                 e->isAccepted() ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(int(e->possibleActions())));
        return true;
    }
    case QEvent::DragMove: {
        auto *e = static_cast<QDragMoveEvent *>(event);
        const QString dest = destination(e->position().toPoint());
        if (!dest.isEmpty()) FileDrop::allow(e);
        else e->ignore();
        FileDrop::traceMove(onPlaces ? m_places->viewport() : m_tabs->tabBar(),
                            dest, e->isAccepted());
        return true;
    }
    case QEvent::Drop: {
        auto *e = static_cast<QDropEvent *>(event);
        FileDrop::handle(e, this, destination(e->position().toPoint()));
        FileDrop::log(QStringLiteral("sidebar/tab drop accepted=%1")
            .arg(e->isAccepted() ? QStringLiteral("yes") : QStringLiteral("no")));
        return true;
    }
    default:
        break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::addTab(const QString &path, bool makeCurrent)
{
    QString target = path;
    if (target.startsWith(QStringLiteral("file:")))
        target = QUrl(target).toLocalFile();
    QFileInfo info(target);
    if (!info.exists())
        target = QDir::homePath();
    else if (info.isFile())
        target = info.absolutePath();

    auto *page = new BrowserPage(target, m_tabs);
    QSettings settings;
    page->setIconMode(settings.value(QStringLiteral("ui/iconMode"), true).toBool());
    const int index = m_tabs->addTab(page, QIcon::fromTheme(QStringLiteral("folder")),
                                     QFileInfo(target).fileName().isEmpty() ? target : QFileInfo(target).fileName());
    m_tabs->setTabToolTip(index, target);

    connect(page, &BrowserPage::currentPathChanged, this, [this, page](const QString &newPath) {
        const int tab = m_tabs->indexOf(page);
        if (tab >= 0) {
            const QString label = QFileInfo(newPath).fileName().isEmpty() ? newPath : QFileInfo(newPath).fileName();
            m_tabs->setTabText(tab, label);
            m_tabs->setTabToolTip(tab, newPath);
        }
        if (page == currentPage()) {
            m_pathBar->setPath(newPath);
            syncUiFromCurrentPage();
        }
    });
    connect(page, &BrowserPage::selectionPathChanged, this, [this, page](const QString &selected) {
        if (page == currentPage())
            updatePreview(selected);
    });
    connect(page, &BrowserPage::requestNewTab, this, [this](const QString &newPath) { addTab(newPath); });
    connect(page, &BrowserPage::hostRequest, this, &MainWindow::handleHostRequest);
    connect(page, &BrowserPage::viewSettingsChanged, this, [this, page] {
        if (page == currentPage())
            syncUiFromCurrentPage();
    });

    if (makeCurrent) {
        m_tabs->setCurrentIndex(index);
        page->activePane()->focusFiles();
    }
}

void MainWindow::closeTab(int index)
{
    if (!m_tabs || index < 0)
        return;
    if (m_tabs->count() == 1) {
        close();
        return;
    }
    QWidget *page = m_tabs->widget(index);
    m_tabs->removeTab(index);
    page->deleteLater();
}

void MainWindow::closeOtherTabs()
{
    if (!m_tabs) return;
    QWidget *keep = m_tabs->currentWidget();
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        if (m_tabs->widget(i) == keep) continue;
        QWidget *page = m_tabs->widget(i);
        m_tabs->removeTab(i);
        page->deleteLater();
    }
}

void MainWindow::closeTabsToRight()
{
    if (!m_tabs) return;
    const int current = m_tabs->currentIndex();
    for (int i = m_tabs->count() - 1; i > current; --i) {
        QWidget *page = m_tabs->widget(i);
        m_tabs->removeTab(i);
        page->deleteLater();
    }
}

void MainWindow::duplicateCurrentTab()
{
    if (currentPage()) addTab(currentPage()->currentPath());
}

void MainWindow::openNewWindow()
{
    const QString path = currentPage() ? currentPage()->currentPath() : QDir::homePath();
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {path});
}

void MainWindow::syncUiFromCurrentPage()
{
    BrowserPage *page = currentPage();
    if (!page)
        return;
    m_pathBar->setPath(page->currentPath());
    m_split->setChecked(page->splitEnabled());
    m_icons->setChecked(page->activePane()->iconMode());
    m_details->setChecked(!page->activePane()->iconMode());
    m_back->setEnabled(page->activePane()->canGoBack());
    m_forward->setEnabled(page->activePane()->canGoForward());
    QSettings().setValue(QStringLiteral("ui/iconMode"), page->activePane()->iconMode());
    updateStatusBar(page->currentPath());
    updatePreview(page->selectedPath());
}

void MainWindow::updateStatusBar(const QString &path)
{
    QDir dir(path);
    const int count = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System).size();
    QStorageInfo storage(path);
    QString message = tr("%1 items").arg(count);
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0)
        message += tr("  |  Free space: %1").arg(QLocale().formattedDataSize(storage.bytesAvailable()));
    statusBar()->showMessage(message);
}

void MainWindow::updatePreview(const QString &path)
{
    m_previewImage->clear();
    if (path.isEmpty()) {
        m_previewInfo->setText(tr("No selection"));
        return;
    }

    const QFileInfo info(path);
    QString text = QStringLiteral("<b>%1</b><br>%2<br><br>%3: %4<br>%5: %6")
        .arg(info.fileName().toHtmlEscaped(), info.absoluteFilePath().toHtmlEscaped(),
             tr("Type"), info.isDir() ? tr("Folder") : info.suffix().toHtmlEscaped(),
             tr("Size"), info.isDir() ? QStringLiteral("—") : QLocale().formattedDataSize(info.size()));
    m_previewInfo->setText(text);

    if (!info.isFile())
        return;
    static const QMimeDatabase mimeDb;
    const bool image = mimeDb.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name()
                           .startsWith(QLatin1String("image/")) || QImageReader(path).canRead();
    if (!image)
        return;
    const QImage preview = Thumbnailer::loadImageScaled(path, QSize(260, 200));
    if (!preview.isNull())
        m_previewImage->setPixmap(QPixmap::fromImage(preview));
}

void MainWindow::launchTerminal()
{
    const QString path = currentPage() ? currentPage()->currentPath() : QDir::homePath();
    const QStringList terminals = {
        QStringLiteral("x-terminal-emulator"), QStringLiteral("konsole"),
        QStringLiteral("xterm"), QStringLiteral("urxvt")
    };
    for (const QString &terminal : terminals) {
        if (!QStandardPaths::findExecutable(terminal).isEmpty()) {
            QProcess::startDetached(terminal, {}, path);
            return;
        }
    }
    statusBar()->showMessage(tr("No terminal emulator was found."), 5000);
}

void MainWindow::copyOrMoveToOtherPane(bool move)
{
    BrowserPage *page = currentPage();
    if (!page || !page->splitEnabled() || !page->otherPane()) {
        QMessageBox::information(this, tr("Split View"), tr("Open Split View with F3 before using this action."));
        return;
    }
    const QStringList sources = page->activePane()->selectedPaths();
    if (sources.isEmpty()) {
        QMessageBox::information(this, tr("File operation"), tr("Select a file or folder first."));
        return;
    }
    const QString destination = page->otherPane()->currentPath();
    QPointer<BrowserPage> guard(page);
    FileOps::transfer(this, sources, destination, move, [guard](bool ok) {
        if (ok && guard) {
            guard->refresh();
            if (guard->otherPane()) guard->otherPane()->refresh();
        }
    });
}

void MainWindow::connectSmb()
{
    const QString helper = QStringLiteral("/usr/lib/rox-filer2-qt/rox-qt-smb-helper");
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (!QFileInfo(helper).isExecutable() || (geteuid() != 0 && pkexec.isEmpty())) {
        QMessageBox::warning(this, tr("Samba"),
                             tr("The SonicDE SMB helper or pkexec is missing. Please reinstall ROX-Filer2-QT."));
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("mount.cifs")).isEmpty() &&
        !QFileInfo::exists(QStringLiteral("/usr/sbin/mount.cifs")) &&
        !QFileInfo::exists(QStringLiteral("/sbin/mount.cifs"))) {
        QMessageBox::warning(this, tr("Samba"), tr("Native SMB mounting requires cifs-utils (mount.cifs)."));
        return;
    }
    bool ok = false;
    QString address = QInputDialog::getText(this, tr("Connect to SMB Share"),
                                            tr("Server and share (smb://server/share):"),
                                            QLineEdit::Normal, QStringLiteral("smb://"), &ok).trimmed();
    if (!ok || address.isEmpty()) return;
    if (!address.startsWith(QStringLiteral("smb://"), Qt::CaseInsensitive))
        address.prepend(QStringLiteral("smb://"));
    const QUrl url(address);
    const QString host = url.host();
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    static const QRegularExpression safeName(QStringLiteral("^[a-zA-Z0-9_.-]{1,100}$"));
    if (!url.isValid() || url.scheme().compare(QStringLiteral("smb"), Qt::CaseInsensitive) != 0 ||
        !safeName.match(host).hasMatch() || parts.size() != 1 ||
        !safeName.match(parts.first()).hasMatch() ||
        host == QStringLiteral(".") || host == QStringLiteral("..") ||
        parts.first() == QStringLiteral(".") || parts.first() == QStringLiteral("..") ||
        !url.userName().isEmpty() || !url.password().isEmpty() || url.hasQuery() || url.hasFragment()) {
        QMessageBox::warning(this, tr("Samba"), tr("Use smb://server/share. Folder paths and credentials in the URL are not supported."));
        return;
    }
    const QString username = QInputDialog::getText(this, tr("Connect to SMB Share"),
                                                    tr("SMB username (empty for guest):"),
                                                    QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    if (username.contains(QLatin1Char('\n')) || username.contains(QLatin1Char('\r')))
        return;
    const QString password = QInputDialog::getText(this, tr("Connect to SMB Share"),
                                                    tr("SMB password (not saved):"),
                                                    QLineEdit::Password, QString(), &ok);
    if (!ok) return;
    if (password.contains(QLatin1Char('\n')) || password.contains(QLatin1Char('\r'))) return;

    auto *p = new QProcess(this);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            QMessageBox::warning(this, tr("Samba"), tr("Could not launch the SMB helper."));
            p->deleteLater();
        }
    });
    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, p](int code, QProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        const QString target = QString::fromUtf8(p->readAllStandardOutput()).trimmed().section(QLatin1Char('\n'), -1);
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            QMessageBox::warning(this, tr("Samba"), polkitErrorMessage(err));
            return;
        }
        if (QFileInfo(target).isDir() && currentPage()) {
            currentPage()->navigateActive(target);
            addBookmark(target);
        } else {
            QMessageBox::warning(this, tr("Samba"), tr("The SMB share was mounted but its folder could not be opened."));
        }
    });
    if (geteuid() == 0) p->start(helper, {host, parts.first(), username});
    else p->start(pkexec, {helper, host, parts.first(), username});
    // Send credentials over stdin, never as command-line arguments or in a URL.
    // QProcess queues the bytes until the graphical Polkit authentication finishes.
    QByteArray credentials = password.toUtf8();
    credentials.append('\n');
    p->write(credentials);
    credentials.fill(0);
    p->closeWriteChannel();
    statusBar()->showMessage(tr("Connecting to SMB share…"));
}

void MainWindow::disconnectSmb()
{
    // The privileged helper only accepts SMB mounts below its own per-user
    // mount prefix, so it cannot be used to unmount arbitrary system paths.
    const QString base = QStringLiteral("/mnt/rox-filer2-qt-smb/%1").arg(geteuid() == 0 ? 0 : getuid());
    QDir folder(base);
    QStringList shares;
    for (const QString &name : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString path = folder.filePath(name);
        if (QStorageInfo(path).isValid() && QStorageInfo(path).rootPath() == path)
            shares << name;
    }
    if (shares.isEmpty()) {
        QMessageBox::information(this, tr("Samba"), tr("No mounted SonicDE SMB shares were found."));
        return;
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Disconnect SMB Share…"),
                                                   tr("Mounted network share:"), shares, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;
    const QString helper = QStringLiteral("/usr/lib/rox-filer2-qt/rox-qt-smb-helper");
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (!QFileInfo(helper).isExecutable() || (geteuid() != 0 && pkexec.isEmpty())) return;
    // Explicit host length avoids ambiguity if host/share contain underscores.
    const qsizetype first = chosen.indexOf(QLatin1Char('_'));
    bool validLength = false;
    const qsizetype hostLength = chosen.left(first).toLongLong(&validLength);
    if (!validLength || first < 1 || hostLength < 1 || hostLength > 100 ||
        chosen.size() <= first + 1 + hostLength ||
        chosen.at(first + 1 + hostLength) != QLatin1Char('_')) return;
    const QString host = chosen.mid(first + 1, hostLength);
    const QString share = chosen.mid(first + hostLength + 2);
    auto *p = new QProcess(this);
    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, p](int code, QProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
            QMessageBox::warning(this, tr("Samba"), polkitErrorMessage(err));
        else {
            buildPlaces();
            statusBar()->showMessage(tr("SMB share disconnected."), 3500);
        }
    });
    if (geteuid() == 0) p->start(helper, {host, share});
    else p->start(pkexec, {helper, host, share});
}

void MainWindow::showSharedFolders()
{
    const QString net = QStandardPaths::findExecutable(QStringLiteral("net"));
    if (net.isEmpty()) {
        QMessageBox::information(this, tr("Shared Folders"), tr("Samba 'net' is not installed, so usershares cannot be listed."));
        return;
    }
    QProcess p;
    p.start(net, {QStringLiteral("usershare"), QStringLiteral("info")});
    p.waitForFinished(3000);
    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    if (out.isEmpty()) out = err.isEmpty() ? tr("No shared folders were found.") : err;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Shared Folders"));
    dialog.resize(620, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QTextEdit(&dialog);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(out);
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::rebuildBookmarks()
{
    if (!m_bookmarksMenu) return;
    m_bookmarksMenu->clear();
    QAction *add = m_bookmarksMenu->addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")), tr("Add Current Folder"));
    QAction *edit = m_bookmarksMenu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit Bookmarks…"));
    connect(add, &QAction::triggered, this, &MainWindow::addCurrentBookmark);
    connect(edit, &QAction::triggered, this, &MainWindow::editBookmarks);

    QSettings settings;
    const QStringList bookmarks = settings.value(QStringLiteral("bookmarks/paths")).toStringList();
    if (!bookmarks.isEmpty()) m_bookmarksMenu->addSeparator();
    for (const QString &path : bookmarks) {
        if (!QFileInfo(path).isDir()) continue;
        QString label = QFileInfo(path).fileName();
        if (label.isEmpty()) label = path;
        QAction *a = m_bookmarksMenu->addAction(QIcon::fromTheme(QStringLiteral("folder-bookmark")), label);
        a->setToolTip(path);
        connect(a, &QAction::triggered, this, [this, path] { if (currentPage()) currentPage()->navigateActive(path); });
    }
}

void MainWindow::addCurrentBookmark()
{
    if (currentPage())
        addBookmark(currentPage()->currentPath());
}

void MainWindow::addBookmark(const QString &path)
{
    if (path.isEmpty())
        return;
    QSettings settings;
    QStringList bookmarks = settings.value(QStringLiteral("bookmarks/paths")).toStringList();
    if (!bookmarks.contains(path))
        bookmarks.append(path);
    settings.setValue(QStringLiteral("bookmarks/paths"), bookmarks);
    rebuildBookmarks();
    buildPlaces();
    statusBar()->showMessage(tr("Bookmark added: %1").arg(path), 4000);
}

void MainWindow::handleHostRequest(FilePane::HostAction action, const QString &argument)
{
    switch (action) {
    case FilePane::HostAction::NewWindow:
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                {argument.isEmpty() && currentPage() ? currentPage()->currentPath() : argument});
        break;
    case FilePane::HostAction::EditLocation:
        m_pathBar->beginEdit();
        break;
    case FilePane::HostAction::FocusFilter:
        m_filter->setFocus(Qt::ShortcutFocusReason);
        m_filter->selectAll();
        break;
    case FilePane::HostAction::Options:
        showOptions();
        break;
    case FilePane::HostAction::OpenTerminal:
        launchTerminal();
        break;
    case FilePane::HostAction::AddBookmark:
        addBookmark(argument);
        break;
    case FilePane::HostAction::ShowBookmarks:
        m_bookmarksMenu->popup(QCursor::pos());
        break;
    case FilePane::HostAction::ConnectSmb:
        connectSmb();
        break;
    case FilePane::HostAction::SharedFolders:
        showSharedFolders();
        break;
    case FilePane::HostAction::CloseWindow:
        close();
        break;
    case FilePane::HostAction::ResizeToFit: {
        FilePane *pane = currentPage() ? currentPage()->activePane() : nullptr;
        const QStringList parts = argument.split(QLatin1Char('x'));
        if (!pane || parts.size() != 2 || isMaximized() || isFullScreen())
            break;
        const QSize wanted(parts.at(0).toInt(), parts.at(1).toInt());
        const QSize delta = wanted - pane->currentViewSize();
        QSize target = size() + delta;
        const QScreen *screenForWindow = screen() ? screen() : QGuiApplication::primaryScreen();
        if (screenForWindow)
            target = target.boundedTo(screenForWindow->availableGeometry().size());
        resize(target.expandedTo(minimumSizeHint()));
        break;
    }
    case FilePane::HostAction::PairedWindows:
        if (currentPage() && !currentPage()->splitEnabled())
            m_split->trigger();
        break;
    case FilePane::HostAction::CloseAfterOpen:
        // Deferred: this is raised from inside a mouse/key handler of the pane
        // that is about to be destroyed.
        QTimer::singleShot(0, this, [this] { closeTab(m_tabs ? m_tabs->currentIndex() : -1); });
        break;
    case FilePane::HostAction::StatusMessage:
        statusBar()->showMessage(argument, 5000);
        break;
    }
}

void MainWindow::editBookmarks()
{
    QSettings settings;
    QStringList bookmarks = settings.value(QStringLiteral("bookmarks/paths")).toStringList();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Edit Bookmarks"));
    dialog.resize(520, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->addItems(bookmarks);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(list);
    auto *row = new QHBoxLayout;
    auto *add = new QToolButton(&dialog);
    add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    add->setToolTip(tr("Add Current Folder"));
    auto *remove = new QToolButton(&dialog);
    remove->setIcon(QIcon::fromTheme(QStringLiteral("list-remove")));
    remove->setToolTip(tr("Remove"));
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    layout->addLayout(row);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(add, &QToolButton::clicked, this, [this, list] {
        if (!currentPage()) return;
        const QString path = currentPage()->currentPath();
        if (list->findItems(path, Qt::MatchExactly).isEmpty()) list->addItem(path);
    });
    connect(remove, &QToolButton::clicked, list, [list] { qDeleteAll(list->selectedItems()); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    bookmarks.clear();
    for (int i = 0; i < list->count(); ++i) bookmarks << list->item(i)->text();
    settings.setValue(QStringLiteral("bookmarks/paths"), bookmarks);
    rebuildBookmarks();
    buildPlaces();
}

void MainWindow::showHelp()
{
    const QString lang = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    const QString base = QStringLiteral("/usr/share/rox-filer2-qt/help/");
    const QString localBase = QCoreApplication::applicationDirPath() + QStringLiteral("/../share/rox-filer2-qt/help/");
    const QStringList candidates = {
        base + QStringLiteral("Manual-") + lang + QStringLiteral(".html"),
        base + QStringLiteral("Manual.html"),
        localBase + QStringLiteral("Manual-") + lang + QStringLiteral(".html"),
        localBase + QStringLiteral("Manual.html")
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
    }
    QMessageBox::information(this, tr("Help"), tr("The ROX-Filer2 help files are not installed."));
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About ROX-Filer2-QT"));
    box.setIconPixmap(qApp->windowIcon().pixmap(96, 96));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<h2>ROX-Filer2-QT %1</h2>").arg(QCoreApplication::applicationVersion()) +
                tr("<p>Qt6 file manager for <b>Essora SonicDE</b>.</p>") +
                tr("<p>Built with Essora Qt %1, exclusive to SonicDE.<br>Icon theme: essora-dark-teal.</p>").arg(QString::fromLatin1(qVersion())) +
                tr("<p>Based on ROX-Filer2 and distributed under its free software license.</p>"));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void MainWindow::showOptions()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("ROX-Filer2-QT Preferences"));
    dialog.setWindowIcon(qApp->windowIcon());
    dialog.resize(940, 610);
    dialog.setMinimumSize(780, 500);
    auto *outer = new QVBoxLayout(&dialog);
    auto *columns = new QHBoxLayout;
    auto *categories = new QListWidget(&dialog);
    categories->setFixedWidth(205);
    categories->setIconSize(QSize(22, 22));
    auto *pages = new QStackedWidget(&dialog);
    columns->addWidget(categories);
    columns->addWidget(pages, 1);
    outer->addLayout(columns, 1);

    auto addPage = [&](const QString &name, const QString &iconName) -> QVBoxLayout * {
        auto *item = new QListWidgetItem(QIcon::fromTheme(iconName), name, categories);
        item->setSizeHint(QSize(0, 46));
        auto *scroll = new QScrollArea(pages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *inner = new QWidget(scroll);
        auto *layout = new QVBoxLayout(inner);
        layout->setContentsMargins(16, 10, 16, 10);
        layout->setSpacing(12);
        scroll->setWidget(inner);
        pages->addWidget(scroll);
        return layout;
    };
    auto addCheck = [&](QVBoxLayout *page, const QString &label, bool checked) -> QCheckBox * {
        auto *check = new QCheckBox(label, &dialog);
        check->setChecked(checked);
        page->addWidget(check);
        return check;
    };
    auto note = [&](QVBoxLayout *page, const QString &message) {
        auto *text = new QLabel(message, &dialog);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        page->addWidget(text);
    };

    QSettings settings;
    auto *general = addPage(tr("General"), QStringLiteral("preferences-system"));
    auto *sidebar = addCheck(general, tr("Show sidebar"), m_sidebarToggle->isChecked());
    auto *preview = addCheck(general, tr("Show preview pane"), m_previewToggle->isChecked());
    note(general, tr("These settings affect the file manager window only."));
    general->addStretch();

    auto *navigation = addPage(tr("Navigation"), QStringLiteral("go-jump"));
    auto *breadcrumbs = addCheck(navigation, tr("Use breadcrumb location bar"), m_pathBar->breadcrumbMode());
    note(navigation, tr("You can also edit a path directly in the location bar."));
    navigation->addStretch();

    // ROX behaviour options (GTK3 "bind_*"): the initial values come from the
    // GTK3 build's Options file when it exists, so both filers start out alike.
    auto *mouse = addPage(tr("Mouse and opening"), QStringLiteral("input-mouse"));
    auto *clickRow = new QHBoxLayout;
    auto *clickLabel = new QLabel(tr("Open items with:"), &dialog);
    auto *clickMode = new QComboBox(&dialog);
    clickMode->addItem(tr("A single click"), int(RoxOptions::ClickMode::Single));
    clickMode->addItem(tr("A double click"), int(RoxOptions::ClickMode::Double));
    clickMode->addItem(tr("A single click on folders, a double click on files"),
                       int(RoxOptions::ClickMode::SingleDirs));
    const RoxOptions::ClickMode initialClickMode = RoxOptions::clickMode();
    clickMode->setCurrentIndex(clickMode->findData(int(initialClickMode)));
    clickRow->addWidget(clickLabel);
    clickRow->addWidget(clickMode, 1);
    mouse->addLayout(clickRow);
    const bool initialNewButton1 = RoxOptions::newButton1();
    auto *newButton1 = addCheck(mouse, tr("A left click on a folder opens it in a new window"), initialNewButton1);
    const bool initialDclick = RoxOptions::dclickResizes();
    auto *dclickResizes = addCheck(mouse, tr("Double click on empty space resizes the window to fit its items"),
                                   initialDclick);
    note(mouse, tr("Middle click on a folder opens it in a new tab; on a file or program it opens it and closes the "
                   "tab. Ctrl+click toggles the selection, Shift+click (or Shift+Enter) opens a folder to look "
                   "inside, a file as text, or unmounts a mount point. Application directories (with an AppRun) "
                   "and executable files run when opened."));
    mouse->addStretch();

    auto *appearance = addPage(tr("Appearance"), QStringLiteral("preferences-desktop-theme"));
    auto *icons = addCheck(appearance, tr("Use icon view by default"),
                           currentPage() ? currentPage()->activePane()->iconMode() : true);
    auto *thumbs = addCheck(appearance, tr("Show thumbnails for images"),
                           currentPage() ? currentPage()->activePane()->thumbnails()
                           : settings.value(QStringLiteral("display/thumbnails"), true).toBool());
    auto *hidden = addCheck(appearance, tr("Show hidden files by default"),
                            currentPage() ? currentPage()->activePane()->showHidden()
                            : settings.value(QStringLiteral("display/showHidden"), false).toBool());
    appearance->addStretch();

    auto *volumes = addPage(tr("Drives and partitions"), QStringLiteral("drive-harddisk"));
    auto *internal = addCheck(volumes, tr("Show internal partitions"),
                              settings.value(QStringLiteral("devices/showInternal"), true).toBool());
    auto *removable = addCheck(volumes, tr("Show USB and removable devices"),
                               settings.value(QStringLiteral("devices/showRemovable"), true).toBool());
    auto *onlyMounted = addCheck(volumes, tr("Show only mounted volumes"),
                                 settings.value(QStringLiteral("devices/onlyMounted"), false).toBool());
    auto *showSystem = addCheck(volumes, tr("Show system partitions (/, /boot, EFI)"),
                                settings.value(QStringLiteral("devices/showSystem"), false).toBool());
    auto *showImages = addCheck(volumes, tr("Show mounted images (ISO, SFS, IMG)"),
                                settings.value(QStringLiteral("devices/showImages"), true).toBool());
    note(volumes, tr("Mount and unmount actions use the existing Polkit helper, without GVfs."));
    volumes->addStretch();

    auto *desktop = addPage(tr("Desktop drive icons"), QStringLiteral("user-desktop"));
    auto *deskEnabled = addCheck(desktop, tr("Show drives on the SonicDE desktop"),
                                 settings.value(QStringLiteral("desktop/drivesEnabled"), false).toBool());
    auto *deskInternal = addCheck(desktop, tr("Include internal partitions"),
                                  settings.value(QStringLiteral("desktop/showInternal"), false).toBool());
    auto *deskRemovable = addCheck(desktop, tr("Include removable devices"),
                                   settings.value(QStringLiteral("desktop/showRemovable"), true).toBool());
    auto *deskMounted = addCheck(desktop, tr("Show only mounted volumes on the desktop"),
                                 settings.value(QStringLiteral("desktop/onlyMounted"), false).toBool());
    auto *deskSystem = addCheck(desktop, tr("Show system partitions on the desktop"),
                                settings.value(QStringLiteral("desktop/showSystem"), false).toBool());
    auto *deskImages = addCheck(desktop, tr("Show mounted image icons (ISO, SFS, IMG)"),
                                settings.value(QStringLiteral("desktop/showImages"), true).toBool());
    auto *deskAutoMount = addCheck(desktop, tr("Try mounting removable devices automatically at login (no password prompt)"),
                                   settings.value(QStringLiteral("desktop/autoMountRemovable"), false).toBool());
    auto *placement = new QHBoxLayout;
    auto *placementLabel = new QLabel(tr("Icon position:"), &dialog);
    auto *edge = new QComboBox(&dialog);
    edge->addItem(tr("Right"), QStringLiteral("right"));
    edge->addItem(tr("Left"), QStringLiteral("left"));
    edge->addItem(tr("Top"), QStringLiteral("top"));
    edge->addItem(tr("Bottom"), QStringLiteral("bottom"));
    const int idx = edge->findData(settings.value(QStringLiteral("desktop/edge"), QStringLiteral("right")).toString());
    edge->setCurrentIndex(idx < 0 ? 0 : idx);
    placement->addWidget(placementLabel);
    placement->addWidget(edge);
    desktop->addLayout(placement);
    auto *layoutRow = new QHBoxLayout;
    auto *layoutLabel = new QLabel(tr("Icon layout:"), &dialog);
    auto *layout = new QComboBox(&dialog);
    layout->addItem(tr("Vertical"), QStringLiteral("vertical"));
    layout->addItem(tr("Horizontal"), QStringLiteral("horizontal"));
    const int layoutIndex = layout->findData(settings.value(QStringLiteral("desktop/layout"),
                                              QStringLiteral("vertical")).toString());
    layout->setCurrentIndex(layoutIndex < 0 ? 0 : layoutIndex);
    layoutRow->addWidget(layoutLabel);
    layoutRow->addWidget(layout);
    desktop->addLayout(layoutRow);
    auto *showBackground = addCheck(desktop, tr("Show icon background"),
                                    settings.value(QStringLiteral("desktop/showBackground"), false).toBool());
    auto *marginRow = new QHBoxLayout;
    auto *marginLabel = new QLabel(tr("Distance from top:"), &dialog);
    auto *topMargin = new QSpinBox(&dialog);
    topMargin->setRange(0, 2000);
    topMargin->setSingleStep(8);
    topMargin->setSuffix(QStringLiteral(" px"));
    topMargin->setValue(settings.value(QStringLiteral("desktop/topMargin"), 96).toInt());
    marginRow->addWidget(marginLabel);
    marginRow->addWidget(topMargin);
    desktop->addLayout(marginRow);
    auto *edgeMarginRow = new QHBoxLayout;
    auto *edgeMarginLabel = new QLabel(tr("Edge distance:"), &dialog);
    auto *edgeMargin = new QSpinBox(&dialog);
    edgeMargin->setRange(0, 400);
    edgeMargin->setSingleStep(4);
    edgeMargin->setSuffix(QStringLiteral(" px"));
    edgeMargin->setValue(settings.value(QStringLiteral("desktop/edgeMargin"), 18).toInt());
    edgeMarginRow->addWidget(edgeMarginLabel);
    edgeMarginRow->addWidget(edgeMargin);
    desktop->addLayout(edgeMarginRow);
    auto *horizontalOffsetRow = new QHBoxLayout;
    auto *horizontalOffsetLabel = new QLabel(tr("Horizontal start offset:"), &dialog);
    auto *horizontalOffset = new QSpinBox(&dialog);
    horizontalOffset->setRange(0, 2000);
    horizontalOffset->setSingleStep(8);
    horizontalOffset->setSuffix(QStringLiteral(" px"));
    horizontalOffset->setValue(settings.value(QStringLiteral("desktop/horizontalOffset"), 18).toInt());
    horizontalOffsetRow->addWidget(horizontalOffsetLabel);
    horizontalOffsetRow->addWidget(horizontalOffset);
    desktop->addLayout(horizontalOffsetRow);
    auto *sizeRow = new QHBoxLayout;
    auto *sizeLabel = new QLabel(tr("Icon size:"), &dialog);
    auto *size = new QSpinBox(&dialog);
    size->setRange(32, 96);
    size->setSingleStep(8);
    size->setSuffix(QStringLiteral(" px"));
    size->setValue(settings.value(QStringLiteral("desktop/iconSize"), 64).toInt());
    sizeRow->addWidget(sizeLabel);
    sizeRow->addWidget(size);
    desktop->addLayout(sizeRow);
    note(desktop, tr("Desktop drive icons run as a separate user process. They do not replace SonicDE's wallpaper or require rox --desktop. X11/XLibre only."));
    if (geteuid() == 0) {
        for (QCheckBox *control : {deskEnabled, deskInternal, deskRemovable,
                                   deskMounted, deskSystem, deskImages, deskAutoMount})
            control->setEnabled(false);
        edge->setEnabled(false);
        layout->setEnabled(false);
        showBackground->setEnabled(false);
        topMargin->setEnabled(false);
        edgeMargin->setEnabled(false);
        horizontalOffset->setEnabled(false);
        size->setEnabled(false);
        note(desktop, tr("Configure desktop icons from your normal user session, not from ROX as administrator."));
    }
    desktop->addStretch();

    auto *operations = addPage(tr("File operations"), QStringLiteral("edit-copy"));
    note(operations, tr("Drag and drop: choose Copy, Move, Absolute Link or Relative Link. Shortcuts: Ctrl=Copy, Shift=Move, Alt=Absolute Link, Ctrl+Shift=Relative Link."));
    note(operations, tr("The working drag-and-drop engine from 2.14.0-8 is preserved."));
    operations->addStretch();

    auto *media = addPage(tr("Preview and media"), QStringLiteral("image-x-generic"));
    auto *hover = addCheck(media, tr("Play audio and video when the pointer rests on a file (requires mpv)"),
                           HoverPreview::instance()->isEnabled());
    media->addStretch();

    QObject::connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    categories->setCurrentRow(0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    m_sidebarToggle->setChecked(sidebar->isChecked());
    m_previewToggle->setChecked(preview->isChecked());
    m_pathBar->setBreadcrumbMode(breadcrumbs->isChecked());
    if (currentPage()) currentPage()->setIconMode(icons->isChecked());
    settings.setValue(QStringLiteral("ui/sidebar"), sidebar->isChecked());
    settings.setValue(QStringLiteral("ui/preview"), preview->isChecked());
    settings.setValue(QStringLiteral("ui/breadcrumbs"), breadcrumbs->isChecked());
    settings.setValue(QStringLiteral("ui/iconMode"), icons->isChecked());
    // Only values the user actually changed are stored: an untouched option
    // keeps following the GTK3 Options file.
    if (RoxOptions::ClickMode(clickMode->currentData().toInt()) != initialClickMode)
        RoxOptions::setClickMode(RoxOptions::ClickMode(clickMode->currentData().toInt()));
    if (newButton1->isChecked() != initialNewButton1)
        RoxOptions::setNewButton1(newButton1->isChecked());
    if (dclickResizes->isChecked() != initialDclick)
        RoxOptions::setDclickResizes(dclickResizes->isChecked());
    settings.setValue(QStringLiteral("display/thumbnails"), thumbs->isChecked());
    settings.setValue(QStringLiteral("display/showHidden"), hidden->isChecked());
    settings.setValue(QStringLiteral("devices/showInternal"), internal->isChecked());
    settings.setValue(QStringLiteral("devices/showRemovable"), removable->isChecked());
    settings.setValue(QStringLiteral("devices/onlyMounted"), onlyMounted->isChecked());
    settings.setValue(QStringLiteral("devices/showSystem"), showSystem->isChecked());
    settings.setValue(QStringLiteral("devices/showImages"), showImages->isChecked());
    if (geteuid() != 0) {
        settings.setValue(QStringLiteral("desktop/drivesEnabled"), deskEnabled->isChecked());
        settings.setValue(QStringLiteral("desktop/showInternal"), deskInternal->isChecked());
        settings.setValue(QStringLiteral("desktop/showRemovable"), deskRemovable->isChecked());
        settings.setValue(QStringLiteral("desktop/onlyMounted"), deskMounted->isChecked());
        settings.setValue(QStringLiteral("desktop/showSystem"), deskSystem->isChecked());
        settings.setValue(QStringLiteral("desktop/showImages"), deskImages->isChecked());
        settings.setValue(QStringLiteral("desktop/autoMountRemovable"), deskAutoMount->isChecked());
        settings.setValue(QStringLiteral("desktop/edge"), edge->currentData());
        settings.setValue(QStringLiteral("desktop/layout"), layout->currentData());
        settings.setValue(QStringLiteral("desktop/showBackground"), showBackground->isChecked());
        settings.setValue(QStringLiteral("desktop/topMargin"), topMargin->value());
        settings.setValue(QStringLiteral("desktop/edgeMargin"), edgeMargin->value());
        settings.setValue(QStringLiteral("desktop/horizontalOffset"), horizontalOffset->value());
        settings.setValue(QStringLiteral("desktop/iconSize"), size->value());
    }
    settings.sync();
    buildPlaces();
    HoverPreview::instance()->setEnabled(hover->isChecked());
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *page = qobject_cast<BrowserPage *>(m_tabs->widget(i))) {
            page->activePane()->setThumbnails(thumbs->isChecked());
            page->activePane()->setShowHidden(hidden->isChecked());
            if (FilePane *other = page->otherPane()) {
                other->setThumbnails(thumbs->isChecked());
                other->setShowHidden(hidden->isChecked());
            }
        }
    }
    buildPlaces();
    syncUiFromCurrentPage();
    if (geteuid() != 0 && deskEnabled->isChecked()) {
        const QString executable = QStringLiteral("/usr/bin/rox-qt-drives");
        if (QFileInfo(executable).isExecutable())
            QProcess::startDetached(executable, {});
        else
            QMessageBox::warning(this, tr("Desktop drive icons"),
                                 tr("The drive-icon companion is not installed. Reinstall the Sonic package."));
    }
}
