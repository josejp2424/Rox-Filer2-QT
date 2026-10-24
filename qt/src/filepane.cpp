#include "filepane.h"

#include "filedialogs.h"
#include "filedrop.h"
#include "fileops.h"
#include "hoverpreview.h"
#include "iconprovider.h"
#include "mimeapps.h"
#include "findexpr.h"
#include "roxopen.h"
#include "xattrs.h"
#include "roxoptions.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QPoint>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <functional>
#include <QHeaderView>
#include <QFileDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTextLayout>
#include <QTextOption>
#include <QtMath>
#include <QFontMetrics>
#include <QInputDialog>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollBar>
#include <QStyle>
#include <QScreen>
#include <QGuiApplication>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeView>
#include <QUrl>

#include <unistd.h>
#include <QVBoxLayout>

namespace {

const int kIconSizes[] = {32, 48, 64, 96, 128};

QIcon themeIcon(const char *name)
{
    return QIcon::fromTheme(QString::fromLatin1(name));
}

QDir::Filters baseFilter(bool hidden)
{
    QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (hidden)
        f |= QDir::Hidden;
    return f;
}

} // namespace

// Explicit mouse-driven source dragging, independent of QFileSystemModel's
// ItemIsDragEnabled flags (Qt6 / proxy models vary by platform).  Drops are
// accepted by FilePane's viewport filter before QAbstractItemView can reject
// them on model/selection checks.
template <typename View>
class RoxDragView final : public View
{
public:
    explicit RoxDragView(QWidget *parent) : View(parent) {}
    std::function<QStringList()> selection;
    std::function<QString(const QPoint &)> sourceAt;
    std::function<QString(const QPoint &)> target;
    // ROX click semantics (bind.c): does the item at this point open on a single
    // click, and what to do when it is opened or the background is double-clicked.
    std::function<bool(const QString &)> singleClickFor;
    std::function<void(const QString &path, bool shift, Qt::MouseButton button)> openRequest;
    std::function<void()> backgroundDoubleClick;

protected:
    void toggleIndex(const QModelIndex &index)
    {
        this->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        this->selectionModel()->select(index, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // Important: QAbstractItemView handles right press by mutating the
        // selection *before* customContextMenuRequested is emitted. With a
        // multi-selection that can leave zero selected paths and hide all
        // item operations (including Trash and Delete). We handle the right
        // click explicitly in FilePane::showContextMenu instead, so selected
        // items survive and an unselected clicked item becomes the target.
        if (event->button() == Qt::RightButton) {
            event->accept();
            return;
        }
        m_leftPressed = event->button() == Qt::LeftButton;
        m_pressedButton = event->button();
        m_openOnRelease = false;
        m_ownsPress = false;
        const bool clickButton = event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton;
        const QModelIndex pressedIndex = clickButton ? this->indexAt(event->position().toPoint())
                                                     : QModelIndex();
        m_pressedItem = pressedIndex.isValid() && m_leftPressed;
        m_pressPosition = event->position().toPoint();
        m_pressedPath = pressedIndex.isValid() && sourceAt ? sourceAt(m_pressPosition) : QString();
        // Remembered across the release so a double click can be checked against
        // the item of its FIRST press (GTK3 filer_perform_action does the same).
        m_clickPath = m_pressedPath;
        // Qt changes the selection while handling the press: Ctrl+press on a
        // selected item DESELECTS it and Shift+press re-draws the range, so a
        // drag started with Ctrl/Shift held (the usual way to ask for copy or
        // link) used to carry only the item under the pointer.  Remember what
        // was selected before the press.
        m_selectionBeforePress.clear();
        m_pressedWasSelected = m_pressedItem &&
            this->selectionModel()->rowIntersectsSelection(pressedIndex.row(), pressedIndex.parent());
        if (m_pressedWasSelected) {
            const QModelIndexList selected = this->selectionModel()->selectedRows(0);
            for (const QModelIndex &index : selected)
                m_selectionBeforePress.append(QPersistentModelIndex(index));
        }
        if (pressedIndex.isValid() && clickButton) {
            const bool ctrl = event->modifiers() & Qt::ControlModifier;
            const bool single = singleClickFor && !m_pressedPath.isEmpty() && singleClickFor(m_pressedPath);
            if (single) {
                // ROX single-click mode: a plain press only primes a drag; the
                // item opens when the button is released without having moved.
                // Ctrl+press toggles the item in the selection instead.
                m_ownsPress = true;
                if (ctrl)
                    toggleIndex(pressedIndex);
                else {
                    m_openOnRelease = true;
                    this->selectionModel()->setCurrentIndex(pressedIndex, QItemSelectionModel::NoUpdate);
                }
                event->accept();
                return;
            }
            if (event->button() == Qt::MiddleButton) {
                // "Adjust" in double-click mode toggles the item (bind.c).
                m_ownsPress = true;
                toggleIndex(pressedIndex);
                event->accept();
                return;
            }
        }
        View::mousePressEvent(event);
        if (m_leftPressed)
            FileDrop::log(QStringLiteral("mouse-press view=%1 onItem=%2")
                .arg(this->objectName(), m_pressedItem ? QStringLiteral("yes") : QStringLiteral("no")));
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_leftPressed && m_pressedItem && (event->buttons() & Qt::LeftButton) &&
            (event->position().toPoint() - m_pressPosition).manhattanLength()
                >= QApplication::startDragDistance()) {
            m_leftPressed = false;
            m_pressedItem = false;
            m_openOnRelease = false;
            m_ownsPress = false;
            // Finish Qt's selection/pressed state before entering nested DnD loop.
            this->setState(QAbstractItemView::NoState);
            if (m_pressedWasSelected && !m_selectionBeforePress.isEmpty()) {
                // Dragging an item that was already selected drags the whole
                // selection, whatever modifier keys were held at press time.
                QItemSelection restored;
                for (const QPersistentModelIndex &persistent : std::as_const(m_selectionBeforePress)) {
                    const QModelIndex index(persistent);
                    if (index.isValid())
                        restored.select(index, index);
                }
                this->selectionModel()->select(restored, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            QStringList paths = selection ? selection() : QStringList();
            if (!m_pressedPath.isEmpty() && !paths.contains(m_pressedPath))
                paths = {m_pressedPath};
            FileDrop::log(QStringLiteral("drag-threshold view=%1 selectedFiles=%2")
                .arg(this->objectName()).arg(paths.size()));
            FileDrop::startDrag(this, paths);
            return;
        }
        if (m_ownsPress) {
            event->accept();
            return;
        }
        View::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool owns = m_ownsPress;
        const bool open = m_openOnRelease && event->button() == m_pressedButton;
        const QString pressedPath = m_pressedPath;
        const QPoint pressPosition = m_pressPosition;
        m_leftPressed = false;
        m_pressedItem = false;
        m_pressedWasSelected = false;
        m_ownsPress = false;
        m_openOnRelease = false;
        m_selectionBeforePress.clear();
        m_pressedPath.clear();
        if (owns) {
            const QPoint pos = event->position().toPoint();
            if (open && openRequest && !(event->modifiers() & Qt::ControlModifier) &&
                (pos - pressPosition).manhattanLength() < QApplication::startDragDistance() &&
                sourceAt && sourceAt(pos) == pressedPath) {
                openRequest(pressedPath, event->modifiers() & Qt::ShiftModifier, event->button());
            }
            event->accept();
            return;
        }
        View::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const bool clickButton = event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton;
        const QPoint pos = event->position().toPoint();
        const QModelIndex index = clickButton ? this->indexAt(pos) : QModelIndex();
        if (index.isValid()) {
            const QString path = sourceAt ? sourceAt(pos) : QString();
            // Only the item of the FIRST click may be opened by the double click:
            // a single click may already have navigated and put another item
            // under the pointer.
            if (path.isEmpty() || path != m_clickPath) {
                event->accept();
                return;
            }
            if (singleClickFor && singleClickFor(path)) {
                event->accept(); // already opened by the first click
                return;
            }
            if (openRequest)
                openRequest(path, event->modifiers() & Qt::ShiftModifier, event->button());
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && backgroundDoubleClick) {
            backgroundDoubleClick();
            event->accept();
            return;
        }
        View::mouseDoubleClickEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        FileDrop::allow(event);
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const QString destination = target ? target(event->position().toPoint()) : QString();
        if (destination.isEmpty()) { event->ignore(); return; }
        FileDrop::allow(event);
    }
    void dropEvent(QDropEvent *event) override
    {
        FileDrop::handle(event, this, target ? target(event->position().toPoint()) : QString());
    }
private:
    QPoint m_pressPosition;
    QString m_pressedPath;
    QList<QPersistentModelIndex> m_selectionBeforePress;
    bool m_pressedWasSelected = false;
    QString m_clickPath;
    Qt::MouseButton m_pressedButton = Qt::NoButton;
    bool m_leftPressed = false;
    bool m_pressedItem = false;
    bool m_ownsPress = false;
    bool m_openOnRelease = false;
};

FilePane::FilePane(QWidget *parent)
    : QWidget(parent)
{
    QSettings settings;
    m_showHidden = settings.value(QStringLiteral("display/showHidden"), false).toBool();
    m_iconSize = settings.value(QStringLiteral("display/iconSize"), 48).toInt();
    m_autoSize = settings.value(QStringLiteral("display/autoSize"), false).toBool();
    m_reversed = settings.value(QStringLiteral("display/reversed"), false).toBool();

    // One provider shared by every pane, alive for the whole process: the
    // QFileSystemModel gatherer thread may still query it while a pane is
    // being destroyed, and setIconProvider(nullptr) crashes in Qt 6.
    static RoxIconProvider *sharedProvider = new RoxIconProvider;
    m_iconProvider = sharedProvider;
    m_model = new RoxFileModel(this);
    m_model->setIconProvider(m_iconProvider);
    m_model->setReadOnly(false);
    m_model->setFilter(baseFilter(m_showHidden));
    m_model->setRootPath(QDir::rootPath());
    m_model->setThumbnailsEnabled(settings.value(QStringLiteral("display/thumbnails"), true).toBool());

    m_proxy = new RoxProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterKeyColumn(0);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->setFilterDirectories(settings.value(QStringLiteral("display/filterDirs"), false).toBool());
    m_proxy->setSortMode(static_cast<RoxSort>(settings.value(QStringLiteral("display/sort"), 0).toInt()));

    auto *icons = new RoxDragView<QListView>(this);
    m_icons = icons;
    m_icons->setModel(m_proxy);
    m_icons->setViewMode(QListView::IconMode);
    m_icons->setResizeMode(QListView::Adjust);
    m_icons->setMovement(QListView::Static);
    m_icons->setWordWrap(true);
    m_icons->setTextElideMode(Qt::ElideNone); // Full filenames, not Rox-...
    m_icons->setUniformItemSizes(true);
    m_icons->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_icons->setObjectName(QStringLiteral("sonic-icons"));
    m_icons->setDragEnabled(false); // manual source, avoids QFileSystemModel flags
    m_icons->setAcceptDrops(true);
    m_icons->viewport()->setAcceptDrops(true);
    m_icons->setDropIndicatorShown(true);
    m_icons->setDragDropMode(QAbstractItemView::DropOnly);
    m_icons->setContextMenuPolicy(Qt::CustomContextMenu);
    m_iconDelegate = new RoxIconDelegate(
        [this](const QModelIndex &idx, RoxWith with) { return detailText(idx, with); }, m_icons);
    m_iconDelegate->setWith(static_cast<RoxWith>(settings.value(QStringLiteral("display/with"), 0).toInt()));
    m_icons->setItemDelegate(m_iconDelegate);

    auto *details = new RoxDragView<QTreeView>(this);
    m_details = details;
    m_details->setModel(m_proxy);
    m_details->setRootIsDecorated(false);
    m_details->setItemsExpandable(false);
    m_details->setSortingEnabled(true);
    m_details->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_details->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_details->setObjectName(QStringLiteral("sonic-details"));
    m_details->setDragEnabled(false);
    m_details->setAcceptDrops(true);
    m_details->viewport()->setAcceptDrops(true);
    m_details->setDropIndicatorShown(true);
    m_details->setDragDropMode(QAbstractItemView::DropOnly);
    m_details->setContextMenuPolicy(Qt::CustomContextMenu);
    m_details->setIconSize(QSize(22, 22));
    m_details->setItemDelegate(new RoxItemDelegate(m_details));
    // Full names (and types/dates) instead of "nombre_muy_la...": every column
    // is sized to its content; the last one takes the remaining width.
    m_details->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_details->setTextElideMode(Qt::ElideNone);

    auto dropTarget = [this](QAbstractItemView *view, const QPoint &pos) {
        const QModelIndex idx = view->indexAt(pos);
        if (idx.isValid()) {
            const QString path = m_model->filePath(sourceIndexForViewIndex(idx));
            if (QFileInfo(path).isDir())
                return path;
        }
        return m_currentPath; // empty area or regular file: current folder
    };
    icons->selection = [this] { return selectedPaths(); };
    icons->sourceAt = [this](const QPoint &pos) {
        const QModelIndex idx = m_icons->indexAt(pos);
        return idx.isValid() ? m_model->filePath(sourceIndexForViewIndex(idx)) : QString();
    };
    icons->target = [this, dropTarget](const QPoint &pos) { return dropTarget(m_icons, pos); };
    details->selection = [this] { return selectedPaths(); };
    details->sourceAt = [this](const QPoint &pos) {
        const QModelIndex idx = m_details->indexAt(pos);
        return idx.isValid() ? m_model->filePath(sourceIndexForViewIndex(idx)) : QString();
    };
    details->target = [this, dropTarget](const QPoint &pos) { return dropTarget(m_details, pos); };

    for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(m_icons),
                                    static_cast<QAbstractItemView *>(m_details)}) {
        view->viewport()->setMouseTracking(true);
        view->viewport()->installEventFilter(this);
        view->installEventFilter(this); // Key events go to the view, not the viewport.
    }

    m_views = new QStackedWidget(this);
    m_views->addWidget(m_icons);
    m_views->addWidget(m_details);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_views);

    // Opening is driven by RoxDragView (single/double click per the ROX options,
    // button semantics from bind.c), not by QAbstractItemView::doubleClicked.
    icons->singleClickFor = [this](const QString &path) { return singleClickOpens(path); };
    details->singleClickFor = icons->singleClickFor;
    auto openRequest = [this](const QString &path, bool shift, Qt::MouseButton button) {
        openItem(path, shift, button);
    };
    icons->openRequest = openRequest;
    details->openRequest = openRequest;
    auto backgroundDoubleClick = [this] {
        if (RoxOptions::dclickResizes())
            requestResize();
    };
    icons->backgroundDoubleClick = backgroundDoubleClick;
    details->backgroundDoubleClick = backgroundDoubleClick;
    connect(m_icons, &QListView::customContextMenuRequested, this,
            [this](const QPoint &p) { showContextMenu(m_icons, p); });
    connect(m_details, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint &p) { showContextMenu(m_details, p); });
    connect(m_icons->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { emitSelection(); });
    connect(m_details->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { emitSelection(); });
    // Transfers may originate from the other split pane, a tab, or a DnD
    // operation. All visible panes receive the completed destination paths.
    connect(FileOps::events(), &FileTransferEvents::destinationsCreated, this,
            [this](const QStringList &paths) {
        for (const QString &path : paths) {
            if (QDir::cleanPath(QFileInfo(path).absolutePath()) == m_currentPath)
                m_model->markRecent(path);
        }
    });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this,
            [this] { queueIconLayout(); });
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this,
            [this] { queueIconLayout(); });
    connect(m_proxy, &QAbstractItemModel::modelReset, this,
            [this] { queueIconLayout(); });
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &dir) {
        if (QDir::cleanPath(dir) == m_currentPath) {
            autoSizeIfNeeded();
            queueIconLayout();
            trySelectPending();
        }
    });

    createActions();
    applyIconLayout();
    applySort();
    navigate(QDir::homePath());
}

FilePane::~FilePane()
{
    HoverPreview::instance()->stopIfOwner(this);
}

/* ---------------------------------------------------------------- basics */

QAbstractItemView *FilePane::currentView() const
{
    return m_iconMode ? static_cast<QAbstractItemView *>(m_icons)
                      : static_cast<QAbstractItemView *>(m_details);
}

QModelIndex FilePane::sourceIndexForViewIndex(const QModelIndex &index) const
{
    return m_proxy->mapToSource(index);
}

QModelIndex FilePane::proxyRootIndex() const
{
    return m_proxy->mapFromSource(m_model->index(m_currentPath));
}

QStringList FilePane::selectedPaths() const
{
    QStringList out;
    QAbstractItemView *view = currentView();
    if (!view || !view->selectionModel() || m_currentPath.isEmpty())
        return {};
    // Only selected *children of the displayed folder* are copyable. The
    // QFileSystemModel root (e.g. /home/essora) is never a selected file.
    const QModelIndex root = view->rootIndex();
    const QString visibleDirectory = QDir::cleanPath(m_currentPath);
    // QListView may select an individual index without satisfying the
    // all-columns requirement of selectedRows(0). selectedIndexes() is the
    // authoritative selection here; only column 0 carries file names.
    const QModelIndexList rows = view->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : rows) {
        if (!idx.isValid() || idx.column() != 0 || idx.parent() != root)
            continue;
        const QModelIndex source = sourceIndexForViewIndex(idx);
        if (!source.isValid())
            continue;
        const QString p = QDir::cleanPath(m_model->filePath(source));
        const QFileInfo fi(p);
        if (!QDir::isAbsolutePath(p) || p == visibleDirectory ||
            QDir::cleanPath(fi.absolutePath()) != visibleDirectory ||
            fi.fileName().isEmpty() || (!fi.exists() && !fi.isSymLink()))
            continue;
        if (!out.contains(p))
            out << p;
    }
    return out;
}

QString FilePane::selectedPath() const
{
    const QStringList paths = selectedPaths();
    return paths.isEmpty() ? QString() : paths.first();
}

bool FilePane::canGoBack() const { return m_historyPos > 0; }
bool FilePane::canGoForward() const { return m_historyPos >= 0 && m_historyPos + 1 < m_history.size(); }

void FilePane::navigate(const QString &path, bool addHistory)
{
    QFileInfo info(path);
    QString target = info.absoluteFilePath();
    if (info.isFile())
        target = info.absolutePath();
    if (!QFileInfo(target).isDir())
        return;

    HoverPreview::instance()->stopIfOwner(this);
    m_hoverPath.clear();

    target = QDir::cleanPath(target);
    if (addHistory) {
        while (m_history.size() > m_historyPos + 1)
            m_history.removeLast();
        if (m_history.isEmpty() || m_history.last() != target) {
            m_history.append(target);
            m_historyPos = m_history.size() - 1;
        }
    }

    m_currentPath = target;
    m_typeAheadText.clear();
    m_typeAheadClock.invalidate();
    m_proxy->setVisibleRoot(target);
    m_model->setRootPath(target);
    updateRoots();
    emit currentPathChanged(target);
}

void FilePane::goBack()
{
    if (!canGoBack()) return;
    --m_historyPos;
    navigate(m_history.at(m_historyPos), false);
}

void FilePane::goForward()
{
    if (!canGoForward()) return;
    ++m_historyPos;
    navigate(m_history.at(m_historyPos), false);
}

void FilePane::goUp()
{
    QDir dir(m_currentPath);
    if (dir.cdUp()) {
        const QString child = m_currentPath;
        navigate(dir.absolutePath());
        selectPathWhenLoaded(child);
    }
}

void FilePane::refresh()
{
    const QString path = m_currentPath;
    m_model->setRootPath(QString());
    m_model->setRootPath(path);
    updateRoots();
}

void FilePane::setFilterText(const QString &text)
{
    if (text.trimmed().isEmpty())
        m_proxy->setFilterRegularExpression(QRegularExpression());
    else
        m_proxy->setFilterRegularExpression(
            QRegularExpression(QRegularExpression::escape(text.trimmed()),
                               QRegularExpression::CaseInsensitiveOption));
    updateRoots();
}

void FilePane::setIconMode(bool icons)
{
    m_typeAheadText.clear();
    m_typeAheadClock.invalidate();
    m_iconMode = icons;
    m_views->setCurrentWidget(icons ? static_cast<QWidget *>(m_icons)
                                    : static_cast<QWidget *>(m_details));
    updateRoots();
    emit viewSettingsChanged();
}

void FilePane::focusFiles()
{
    currentView()->setFocus(Qt::OtherFocusReason);
}

void FilePane::selectAllFiles()
{
    // QListView and QTreeView both honour the proxy's root and filter here.
    // Do not select source-model rows invisible in the active folder.
    currentView()->selectAll();
}

void FilePane::clearFileSelection()
{
    currentView()->clearSelection();
}

void FilePane::setShowHidden(bool show)
{
    m_showHidden = show;
    m_model->setFilter(baseFilter(show));
    if (m_actShowHidden)
        m_actShowHidden->setChecked(show);
    updateRoots();
}

bool FilePane::thumbnails() const
{
    return m_model->thumbnailsEnabled();
}

void FilePane::setThumbnails(bool enabled)
{
    m_model->setThumbnailsEnabled(enabled);
    if (m_actThumbs)
        m_actThumbs->setChecked(enabled);
    m_icons->viewport()->update();
    m_details->viewport()->update();
}

void FilePane::updateRoots()
{
    const QModelIndex root = proxyRootIndex();
    m_icons->setRootIndex(root);
    m_details->setRootIndex(root);
}

void FilePane::emitSelection()
{
    updateActionStates();
    emit selectionPathChanged(selectedPath());
}

void FilePane::status(const QString &text)
{
    emit hostRequest(HostAction::StatusMessage, text);
}

bool FilePane::inTrash() const
{
    return FileOps::isTrashFilesDir(m_currentPath);
}

void FilePane::openIndex(const QModelIndex &index)
{
    openItem(m_model->filePath(sourceIndexForViewIndex(index)), false, Qt::LeftButton);
}

bool FilePane::isMountPoint(const QString &path) const
{
    return RoxOpen::isFstabMountPoint(path) || RoxOpen::isMounted(path);
}

// Mirrors GTK3 filer.c: with "single click on folders only" the items that
// open on one click are folders that are not applications; files and AppDirs
// need a double click. With "single click" everything does; with "double
// click" nothing does.
bool FilePane::singleClickOpens(const QString &path) const
{
    switch (RoxOptions::clickMode()) {
    case RoxOptions::ClickMode::Single:
        return true;
    case RoxOptions::ClickMode::SingleDirs:
        return QFileInfo(path).isDir() && !RoxOpen::isAppDir(path);
    case RoxOptions::ClickMode::Double:
        break;
    }
    return false;
}

void FilePane::openItem(const QString &path, bool shift, Qt::MouseButton button)
{
    if (path.isEmpty())
        return;
    HoverPreview::instance()->stopIfOwner(this);

    const QFileInfo fi(path);
    // ROX: a button other than 1 asks to close the window after opening, but a
    // plain folder never closes it (only an application directory without Shift).
    bool closeAfter = button != Qt::LeftButton;
    QString error;
    bool ok = false;

    if (shift && fi.isSymLink()) {
        shiftOpen(path); // Show Target
        return;
    }

    // ROX GTK3 (image_mounter_mount): opening an image mounts it read-only and
    // opens its contents; if ROX already mounted it, it just opens it.
    if (!shift && fi.isFile() && ImageMounter::canHandle(path)) {
        const QString point = ImageMounter::mountedPoint(path);
        if (!point.isEmpty()) {
            emit requestNewTab(point);
        } else {
            QPointer<FilePane> safe(this);
            ImageMounter::mount(this, path, [safe](const QString &mountPoint) {
                if (safe && !mountPoint.isEmpty()) emit safe->requestNewTab(mountPoint);
            });
        }
        return;
    }

    if (fi.isDir()) {
        const bool appDir = RoxOpen::isAppDir(path);
        if (appDir && !shift) {
            ok = RoxOpen::runAppDir(path, &error);
            if (!ok)
                QMessageBox::warning(this, tr("Run Application"), error);
        } else {
            closeAfter = false;
            if (isMountPoint(path)) {
                openMountPoint(path, shift);
                return;
            }
            if (shift) {
                navigate(path); // Look Inside (application directories included)
                return;
            }
            // Button 1 enters the folder in this pane; the middle button opens
            // it in a new tab. "Button 1 opens a new window" reverses them,
            // exactly like the GTK3 option (bind_new_button_1).
            const bool newButton1 = RoxOptions::newButton1();
            if (button == Qt::LeftButton && newButton1)
                emit hostRequest(HostAction::NewWindow, path);
            else if (button == Qt::LeftButton || newButton1)
                navigate(path);
            else
                emit requestNewTab(path);
            return;
        }
    } else if (!shift && RoxOpen::isDesktopFile(path)) {
        ok = RoxOpen::runDesktopFile(path, m_currentPath, &error);
        if (!ok)
            QMessageBox::warning(this, tr("Run Launcher"), error.isEmpty() ? tr("Could not run the launcher.") : error);
    } else if (!shift && RoxOpen::isExecutableFile(path)) {
        ok = RoxOpen::runExecutable(path, m_currentPath, &error);
        if (!ok)
            QMessageBox::warning(this, tr("Run Program"), error);
    } else if (shift) {
        openAsText(path);
        return;
    } else {
        ok = openFile(path);
    }

    if (ok && closeAfter)
        emit hostRequest(HostAction::CloseAfterOpen, QString());
}

bool FilePane::openFile(const QString &path)
{
    const DesktopApp app = MimeApps::defaultAppForMime(MimeApps::mimeTypeForPath(path));
    QString error;
    if (app.isValid() && MimeApps::launch(app, {path}, QFileInfo(path).absolutePath(), &error))
        return true;
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return true;
    RoxDialogs::chooseApplication(this, {path}, false);
    return false;
}

void FilePane::openAsText(const QString &path)
{
    const DesktopApp editor = MimeApps::defaultAppForMime(QStringLiteral("text/plain"));
    QString error;
    if (editor.isValid() && MimeApps::launch(editor, {path}, QFileInfo(path).absolutePath(), &error))
        return;
    RoxDialogs::chooseApplication(this, {path}, false);
}

// GTK3 run.c open_mountpoint(): opening an unmounted mount point mounts it and
// then enters it; Shift on a mounted one unmounts it. Only /etc/fstab entries
// are mounted/unmounted by path (the same way `mount DIR` works); anything else
// that happens to be mounted (USB sticks mounted by udisks, ...) is just a
// folder here, and is managed from the Places list / drives, which is unchanged.
void FilePane::openMountPoint(const QString &path, bool shift)
{
    const bool mounted = RoxOpen::isMounted(path);
    const bool fstab = RoxOpen::isFstabMountPoint(path);
    if (!fstab || mounted != shift) {
        navigate(path);
        return;
    }
    const QString command = mounted ? QStringLiteral("umount") : QStringLiteral("mount");
    FileJob::run(this, mounted ? tr("Unmounting…") : tr("Mounting…"),
                 {QStringList{command, path}},
                 [this, path, mounted](bool ok, const QString &errors) {
        if (!ok) {
            QMessageBox::warning(this, mounted ? tr("Unmount") : tr("Mount"),
                                 errors.trimmed().isEmpty() ? tr("The operation failed.") : errors.trimmed());
            return;
        }
        if (!mounted)
            navigate(path);
        else
            refresh();
    });
}

/* ------------------------------------------------------ type-ahead search */

bool FilePane::searchByTyping(const QString &text, bool backspace)
{
    // ROX-style type-ahead SELECTS the matching item; it does not hide files
    // and must not redirect keystrokes into the toolbar's filter field.
    QAbstractItemView *view = currentView();
    if (!view || !view->model()) return false;
    const bool expired = !m_typeAheadClock.isValid() || m_typeAheadClock.hasExpired(1600);
    if (expired) m_typeAheadText.clear();
    if (backspace) {
        if (m_typeAheadText.isEmpty()) return false;
        m_typeAheadText.chop(1);
    } else {
        m_typeAheadText += text;
    }
    m_typeAheadClock.start();
    if (m_typeAheadText.isEmpty()) return true;

    const QModelIndex root = view->rootIndex();
    const int count = view->model()->rowCount(root);
    if (count <= 0) return true;
    const QModelIndex current = view->currentIndex();
    const int previous = current.isValid() && current.parent() == root ? current.row() : -1;

    auto findPrefix = [&](const QString &prefix, int start) -> QModelIndex {
        for (int n = 0; n < count; ++n) {
            const QModelIndex idx = view->model()->index((start + n) % count, 0, root);
            if (idx.data(Qt::DisplayRole).toString().startsWith(prefix, Qt::CaseInsensitive))
                return idx;
        }
        return QModelIndex();
    };
    QModelIndex match = findPrefix(m_typeAheadText, previous >= 0 ? previous : 0);
    // Typing the same initial repeatedly cycles between matching items.
    if (!match.isValid() && !backspace && m_typeAheadText.size() > 1) {
        const QChar first = m_typeAheadText.front();
        bool repeated = true;
        for (QChar character : std::as_const(m_typeAheadText))
            if (character.toCaseFolded() != first.toCaseFolded()) repeated = false;
        if (repeated) {
            m_typeAheadText = QString(first);
            match = findPrefix(m_typeAheadText, previous < 0 ? 0 : (previous + 1) % count);
        }
    }
    if (match.isValid()) {
        view->selectionModel()->setCurrentIndex(match, QItemSelectionModel::ClearAndSelect);
        view->scrollTo(match, QAbstractItemView::EnsureVisible);
    }
    return true;
}

/* ----------------------------------------------------------- hover/media */

bool FilePane::eventFilter(QObject *watched, QEvent *event)
{
    QAbstractItemView *view = nullptr;
    if (watched == m_icons || watched == m_icons->viewport()) view = m_icons;
    else if (watched == m_details || watched == m_details->viewport()) view = m_details;
    if (!view)
        return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::KeyPress && view == currentView()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && !m_typeAheadText.isEmpty()) {
            m_typeAheadText.clear();
            m_typeAheadClock.invalidate();
            return true;
        }
        // ROX (filer.c return_pressed): Return opens the cursor item, Shift+Return
        // is "edit" (look inside / open as text), Alt+Return opens and closes.
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            QModelIndex cursor = view->currentIndex();
            if (!cursor.isValid()) {
                const QModelIndexList selected = view->selectionModel()->selectedRows(0);
                if (!selected.isEmpty()) cursor = selected.first();
            }
            if (cursor.isValid()) {
                m_typeAheadText.clear();
                m_typeAheadClock.invalidate();
                openItem(m_model->filePath(sourceIndexForViewIndex(cursor)),
                         key->modifiers() & Qt::ShiftModifier,
                         (key->modifiers() & Qt::AltModifier) ? Qt::MiddleButton : Qt::LeftButton);
                return true;
            }
        }
        if (key->key() == Qt::Key_Backspace &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            if (searchByTyping(QString(), true)) return true;
            // No type-ahead in progress: Backspace goes to the parent folder (ROX).
            if (!(key->modifiers() & Qt::ShiftModifier)) {
                goUp();
                return true;
            }
        }
        // ROX: "?" opens Select If.
        if (key->text() == QStringLiteral("?") && m_typeAheadText.isEmpty() &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            selectIf();
            return true;
        }
        // ROX: Space toggles the cursor item in the selection.
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier &&
            m_typeAheadText.isEmpty() && view->currentIndex().isValid()) {
            view->selectionModel()->select(view->currentIndex(),
                QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            return true;
        }
        if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
            !key->text().isEmpty() &&
            !(key->text() == QStringLiteral(" ") && m_typeAheadText.isEmpty())) {
            bool printable = true;
            for (const QChar character : key->text())
                if (!character.isPrint()) printable = false;
            if (printable && searchByTyping(key->text())) return true;
        }
    }
    // Only the viewport handles pointer and drop events; on the view itself,
    // let QWidget process everything except the type-ahead keys above.
    if (watched == view) return QWidget::eventFilter(watched, event);

    // Intercept on the viewport itself: QAbstractItemView may reject a drop
    // before its virtual drag handlers are invoked, especially with proxies.
    auto destination = [this, view](const QPoint &pos) -> QString {
        const QModelIndex index = view->indexAt(pos);
        if (index.isValid()) {
            const QString path = m_model->filePath(sourceIndexForViewIndex(index));
            if (QFileInfo(path).isDir()) return path;
        }
        return m_currentPath;
    };
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *e = static_cast<QDragEnterEvent *>(event);
        FileDrop::allow(e);
        FileDrop::log(QStringLiteral("viewport drag-enter view=%1 urls=%2 actions=%3 accepted=%4")
            .arg(view->objectName()).arg(e->mimeData() && e->mimeData()->hasUrls()
                ? e->mimeData()->urls().size() : 0).arg(int(e->possibleActions()))
            .arg(e->isAccepted() ? QStringLiteral("yes") : QStringLiteral("no")));
        return true;
    }
    case QEvent::DragMove: {
        auto *e = static_cast<QDragMoveEvent *>(event);
        const QString dest = destination(e->position().toPoint());
        if (dest.isEmpty() || !QFileInfo(dest).isDir() || FileOps::isTrashFilesDir(dest)) e->ignore();
        else FileDrop::allow(e);
        FileDrop::traceMove(view, dest, e->isAccepted());
        return true;
    }
    case QEvent::Drop: {
        auto *e = static_cast<QDropEvent *>(event);
        FileDrop::handle(e, this, destination(e->position().toPoint()));
        FileDrop::log(QStringLiteral("viewport drop view=%1 accepted=%2")
            .arg(view->objectName(), e->isAccepted() ? QStringLiteral("yes") : QStringLiteral("no")));
        return true;
    }
    case QEvent::DragLeave:
        FileDrop::log(QStringLiteral("viewport drag-leave view=%1").arg(view->objectName()));
        break;
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->buttons() != Qt::NoButton)
            break; // dragging or rubber-band selection
        const QModelIndex idx = view->indexAt(me->position().toPoint());
        const QString path = idx.isValid() ? m_model->filePath(sourceIndexForViewIndex(idx)) : QString();
        if (path != m_hoverPath) {
            m_hoverPath = path;
            if (path.isEmpty())
                HoverPreview::instance()->stopIfOwner(this);
            else
                HoverPreview::instance()->consider(this, path, me->globalPosition().toPoint());
        }
        break;
    }
    case QEvent::Leave:
    case QEvent::Wheel:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        m_hoverPath.clear();
        HoverPreview::instance()->stopIfOwner(this);
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void FilePane::hideEvent(QHideEvent *event)
{
    m_hoverPath.clear();
    HoverPreview::instance()->stopIfOwner(this);
    QWidget::hideEvent(event);
}

/* --------------------------------------------------------------- display */

void FilePane::applyIconLayout()
{
    const int size = m_iconSize;
    m_icons->setIconSize(QSize(size, size));
    // Minimum comfortable label width. The row height is computed using
    // real names instead of hard-coding two lines for every file.
    const int width = qMax(184, size + 104);
    const int labelWidth = width - 16;
    // New items are shown in bold, which is wider: size the cells for it.
    const QFont layoutFont = RoxItemDelegate::boldVariant(m_icons->font());
    QFontMetrics metrics(layoutFont);
    int maxHeight = metrics.lineSpacing();
    const QModelIndex root = proxyRootIndex();
    for (int row = 0; row < m_proxy->rowCount(root); ++row) {
        const QModelIndex idx = m_proxy->index(row, 0, root);
        QString name = idx.data(Qt::DisplayRole).toString();
        if (m_iconDelegate->with() != RoxWith::None) {
            const QString details = detailText(idx, m_iconDelegate->with());
            if (!details.isEmpty()) name += QLatin1Char('\n') + details;
        }
        // Same transformation the delegate applies while painting, so the cell
        // height always fits every line of the complete name.
        name = RoxIconDelegate::breakable(name, metrics, labelWidth);
        QTextLayout textLayout(name, layoutFont);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); // what the delegate uses
        textLayout.setTextOption(wrap);
        textLayout.beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = textLayout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(labelWidth);
            height += line.height();
        }
        textLayout.endLayout();
        maxHeight = qMax(maxHeight, int(qCeil(height)));
    }
    const int height = size + maxHeight + 20;
    const QSize grid(width, height);
    if (m_icons->gridSize() != grid)
        m_icons->setGridSize(grid);
    if (m_actBigger) m_actBigger->setEnabled(size < kIconSizes[4]);
    if (m_actSmaller) m_actSmaller->setEnabled(size > kIconSizes[0]);
}

void FilePane::queueIconLayout()
{
    if (m_iconLayoutQueued) return;
    m_iconLayoutQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_iconLayoutQueued = false;
        applyIconLayout();
    });
}

void FilePane::changeIconSize(int step)
{
    int idx = 0;
    for (int i = 0; i < 5; ++i)
        if (kIconSizes[i] <= m_iconSize) idx = i;
    idx = qBound(0, idx + step, 4);
    m_iconSize = kIconSizes[idx];
    m_autoSize = false;
    if (m_actAutoSize) m_actAutoSize->setChecked(false);
    applyIconLayout();
}

void FilePane::autoSizeIfNeeded()
{
    if (!m_autoSize)
        return;
    const int count = m_proxy->rowCount(proxyRootIndex());
    m_iconSize = count < 40 ? 96 : count < 150 ? 64 : count < 400 ? 48 : 32;
    applyIconLayout();
}

void FilePane::applySort()
{
    m_proxy->sort(0, m_reversed ? Qt::DescendingOrder : Qt::AscendingOrder);
    m_details->header()->setSortIndicator(0, m_reversed ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void FilePane::setWith(RoxWith with)
{
    m_iconDelegate->setWith(with);
    applyIconLayout();
    m_icons->viewport()->update();
    setIconMode(true);
}

void FilePane::saveDisplaySettings()
{
    QSettings s;
    s.setValue(QStringLiteral("display/showHidden"), m_showHidden);
    s.setValue(QStringLiteral("display/iconSize"), m_iconSize);
    s.setValue(QStringLiteral("display/autoSize"), m_autoSize);
    s.setValue(QStringLiteral("display/reversed"), m_reversed);
    s.setValue(QStringLiteral("display/thumbnails"), thumbnails());
    s.setValue(QStringLiteral("display/filterDirs"), m_proxy->filterDirectories());
    s.setValue(QStringLiteral("display/sort"), static_cast<int>(m_proxy->sortMode()));
    s.setValue(QStringLiteral("display/with"), static_cast<int>(m_iconDelegate->with()));
    s.setValue(QStringLiteral("ui/iconMode"), m_iconMode);
    status(tr("Display settings saved as the default for new windows."));
}

QString FilePane::detailText(const QModelIndex &proxyIndex, RoxWith with) const
{
    const QModelIndex src = m_proxy->mapToSource(proxyIndex.siblingAtColumn(0));
    const QFileInfo fi = m_model->fileInfo(src);
    switch (with) {
    case RoxWith::Sizes:
        return fi.isDir() ? QString() : FileOps::humanSize(fi.size());
    case RoxWith::Permissions:
        return FileOps::permissionString(fi.absoluteFilePath()).section(QLatin1Char(' '), 0, 0);
    case RoxWith::Types:
        return m_model->type(src);
    case RoxWith::Times:
        return QLocale().toString(fi.lastModified(), QLocale::ShortFormat);
    case RoxWith::None:
        break;
    }
    return QString();
}

/* --------------------------------------------------------------- actions */

void FilePane::createActions()
{
    auto make = [this](const QIcon &icon, const QString &text, const QKeySequence &key,
                       auto slot) {
        auto *a = new QAction(icon, text, this);
        if (!key.isEmpty()) {
            a->setShortcut(key);
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            addAction(a);
        }
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    m_actResize = make(themeIcon("view-fullscreen"), tr("Resize Window"), QKeySequence(QStringLiteral("Ctrl+E")),
                       [this] { requestResize(); });
    m_actCut = make(themeIcon("edit-cut"), tr("Cut"), QKeySequence::Cut, [this] { copyToClipboard(true); });
    m_actCopy = make(themeIcon("edit-copy"), tr("Copy"), QKeySequence::Copy, [this] { copyToClipboard(false); });
    m_actPaste = make(themeIcon("edit-paste"), tr("Paste"), QKeySequence::Paste, [this] { paste(); });
    // GTK3's Duplicate savebox edits the entire absolute destination path,
    // not just the basename.  Reuse the same path-aware copy dialog here.
    m_actDuplicate = make(themeIcon("edit-copy"), tr("Duplicate…"), QKeySequence(QStringLiteral("Ctrl+D")), [this] {
        const QStringList paths = selectedPaths();
        if (paths.size() == 1)
            transferToPath(paths, false, true);
    });
    m_actRename = make(themeIcon("document-edit"), tr("Rename…"), QKeySequence(Qt::Key_F2), [this] { renameSelected(); });
    m_actTrash = make(themeIcon("user-trash"), tr("Move to Trash"), QKeySequence(Qt::Key_Delete), [this] { trashSelected(); });
    m_actDelete = make(themeIcon("edit-delete"), tr("Delete Permanently…"),
                       QKeySequence(QStringLiteral("Shift+Del")), [this] { deleteSelected(); });
    m_actProperties = make(themeIcon("document-properties"), tr("Properties"),
                           QKeySequence(QStringLiteral("Ctrl+P")), [this] {
        const QStringList paths = selectedPaths();
        RoxDialogs::showProperties(this, paths.isEmpty() ? QStringList{m_currentPath} : paths);
    });
    // The window owns Ctrl+A, so split panes never register competing
    // shortcuts (which caused Qt's ambiguous-shortcut behaviour).
    m_actSelectAll = make(themeIcon("edit-select-all"), tr("Select All"), QKeySequence(),
                          [this] { selectAllFiles(); });
    m_actShowHidden = make(themeIcon("view-hidden"), tr("Show Hidden"), QKeySequence(QStringLiteral("Ctrl+H")),
                           [this](bool on) { setShowHidden(on); });
    m_actShowHidden->setCheckable(true);
    m_actShowHidden->setChecked(m_showHidden);
    m_actNewDir = make(themeIcon("folder-new"), tr("Directory…"), QKeySequence(Qt::Key_Insert),
                       [this] { newDirectory(); });
    m_actBigger = make(themeIcon("zoom-in"), tr("Bigger Icons"), QKeySequence(Qt::Key_Equal),
                       [this] { changeIconSize(+1); });
    m_actSmaller = make(themeIcon("zoom-out"), tr("Smaller Icons"), QKeySequence(Qt::Key_Minus),
                        [this] { changeIconSize(-1); });
    m_actHome = make(themeIcon("go-home"), tr("Home Directory"), QKeySequence(QStringLiteral("Ctrl+Home")),
                     [this] { navigate(QDir::homePath()); });

    m_actAutoSize = make(themeIcon("zoom-fit-best"), tr("Automatic"), QKeySequence(), [this](bool on) {
        m_autoSize = on;
        autoSizeIfNeeded();
    });
    m_actAutoSize->setCheckable(true);
    m_actAutoSize->setChecked(m_autoSize);

    m_actReversed = make(themeIcon("view-sort-descending"), tr("Reversed"), QKeySequence(), [this](bool on) {
        m_reversed = on;
        applySort();
    });
    m_actReversed->setCheckable(true);
    m_actReversed->setChecked(m_reversed);

    m_actFilterDirs = make(themeIcon("folder"), tr("Filter Directories With Files"), QKeySequence(),
                           [this](bool on) { m_proxy->setFilterDirectories(on); updateRoots(); });
    m_actFilterDirs->setCheckable(true);
    m_actFilterDirs->setChecked(m_proxy->filterDirectories());

    m_actThumbs = make(themeIcon("image-x-generic"), tr("Show Thumbnails"), QKeySequence(),
                       [this](bool on) { setThumbnails(on); });
    m_actThumbs->setCheckable(true);
    m_actThumbs->setChecked(thumbnails());

    m_sortGroup = new QActionGroup(this);
    const struct { RoxSort mode; const char *text; const char *icon; } sorts[] = {
        {RoxSort::Name, QT_TR_NOOP("Sort by Name"), "view-sort-ascending"},
        {RoxSort::Type, QT_TR_NOOP("Sort by Type"), "view-sort-ascending"},
        {RoxSort::ATime, QT_TR_NOOP("Sort by Date (atime)"), "view-sort-ascending"},
        {RoxSort::CTime, QT_TR_NOOP("Sort by Date (ctime)"), "view-sort-ascending"},
        {RoxSort::MTime, QT_TR_NOOP("Sort by Date (mtime)"), "view-sort-ascending"},
        {RoxSort::Size, QT_TR_NOOP("Sort by Size"), "view-sort-ascending"},
        {RoxSort::Owner, QT_TR_NOOP("Sort by Owner"), "system-users"},
        {RoxSort::Group, QT_TR_NOOP("Sort by Group"), "system-users"},
    };
    for (const auto &s : sorts) {
        auto *a = new QAction(themeIcon(s.icon), tr(s.text), m_sortGroup);
        a->setCheckable(true);
        a->setChecked(m_proxy->sortMode() == s.mode);
        const RoxSort mode = s.mode;
        connect(a, &QAction::triggered, this, [this, mode] {
            m_proxy->setSortMode(mode);
            applySort();
        });
    }

    applyIconLayout();
    updateActionStates();
}

void FilePane::updateActionStates()
{
    const int n = selectedPaths().size();
    const bool trash = inTrash();
    m_actCut->setEnabled(n > 0);
    m_actCopy->setEnabled(n > 0);
    m_actDuplicate->setEnabled(n == 1 && !trash);
    m_actRename->setEnabled(n == 1);
    m_actTrash->setEnabled(n > 0 && !trash);
    m_actDelete->setEnabled(n > 0);
    m_actProperties->setEnabled(true);
    m_actPaste->setEnabled(!trash); // the clipboard itself is checked on paste
}

/* --------------------------------------------------------- context menu */

void FilePane::showContextMenu(QWidget *view, const QPoint &point)
{
    auto *itemView = qobject_cast<QAbstractItemView *>(view);
    if (!itemView)
        return;
    m_hoverPath.clear();
    HoverPreview::instance()->stopIfOwner(this);

    // Classic behaviour: right-clicking an unselected item acts on that item
    // only; a multi-selection is kept when the clicked item belongs to it.
    const QModelIndex index = itemView->indexAt(point);
    QItemSelectionModel *sel = itemView->selectionModel();
    if (index.isValid()) {
        if (!sel->isSelected(index)) {
            sel->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            sel->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        }
    }
    // A right-click on empty space does not silently throw away the three
    // files the user deliberately selected. Offer their item operations.
    // Clicking an *unselected* item above still selects only that item.
    updateActionStates();
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        buildItemMenu(&menu, paths);
    else
        buildBackgroundMenu(&menu);
    menu.exec(itemView->viewport()->mapToGlobal(point));
    updateActionStates();
}

void FilePane::buildItemMenu(QMenu *menu, const QStringList &paths)
{
    const bool single = paths.size() == 1;
    const QString first = paths.first();
    const QFileInfo fi(first);
    const bool isDir = single && fi.isDir();
    const bool trash = inTrash();
    const QString workDir = fi.absolutePath();

    /* 1. MIME applications and matching file actions in the foreground,
     *    as in classic ROX (xdg_apps_add_mime_tools). */
    const QString mime = MimeApps::commonMimeType(paths);
    if (!mime.isEmpty()) {
        const QList<DesktopApp> apps = MimeApps::appsForMime(mime);
        for (const DesktopApp &app : apps) {
            QAction *a = menu->addAction(app.qicon(), tr("Open with %1").arg(app.name));
            a->setToolTip(app.exec);
            connect(a, &QAction::triggered, this, [this, app, paths, workDir] {
                QString error;
                if (!MimeApps::launch(app, paths, workDir, &error))
                    QMessageBox::warning(this, tr("Open With"), error);
            });
        }
    }
    const QList<DesktopApp> actions = MimeApps::customActionsFor(paths);
    for (const DesktopApp &action : actions) {
        QAction *a = menu->addAction(action.qicon(), action.name);
        connect(a, &QAction::triggered, this, [this, action, paths, workDir] {
            QString error;
            if (!MimeApps::launch(action, paths, workDir, &error))
                QMessageBox::warning(this, action.name, error);
        });
    }
    menu->addSeparator();

    /* 2. Folder helpers. */
    if (isDir) {
        menu->addAction(themeIcon("tab-new"), tr("Open in New Tab"), this,
                        [this, first] { emit requestNewTab(first); });
    }
    const QString roxFind = QStandardPaths::findExecutable(QStringLiteral("rox-find"));
    bool allDirs = true;
    for (const QString &p : paths)
        if (!QFileInfo(p).isDir()) allDirs = false;
    if (allDirs && !roxFind.isEmpty() && !trash) {
        menu->addAction(themeIcon("edit-find"),
                        paths.size() > 1 ? tr("Search in Selected Folders…") : tr("Search in This Folder…"),
                        this, [paths, roxFind] { QProcess::startDetached(roxFind, paths); });
    }
    if (isDir && !trash) {
        menu->addAction(themeIcon("bookmark-new"), tr("Add to Bookmarks"), this,
                        [this, first] { emit hostRequest(HostAction::AddBookmark, first); });
        if (!QStandardPaths::findExecutable(QStringLiteral("net")).isEmpty()) {
            menu->addAction(themeIcon("folder-publicshare"), tr("Share Folder…"), this, [this, first] {
                RoxDialogs::shareFolder(this, first, [this](const QString &msg) { status(msg); });
            });
        }
    }
    menu->addSeparator();

    /* 3. Clipboard. */
    menu->addAction(m_actCut);
    menu->addAction(m_actCopy);
    menu->addAction(m_actPaste);
    m_actPaste->setEnabled(FileOps::clipboardHasFiles());
    if (!trash) {
        menu->addAction(themeIcon("edit-copy"), tr("Copy to…"), this,
                        [this, paths] { transferToPath(paths, false); });
        menu->addAction(themeIcon("go-jump"), tr("Move to…"), this,
                        [this, paths] { transferToPath(paths, true); });
    }
    menu->addSeparator();

    /* 4. Item operations. */
    if (!trash)
        menu->addAction(m_actDuplicate);
    menu->addAction(m_actRename);
    if (single && !trash) {
        QMenu *link = menu->addMenu(themeIcon("emblem-symbolic-link"), tr("Link"));
        link->addAction(themeIcon("emblem-symbolic-link"), tr("Link (relative)"), this,
                        [this, first] { FileOps::createLink(this, first, true); });
        link->addAction(themeIcon("emblem-symbolic-link"), tr("Link (absolute)"), this,
                        [this, first] { FileOps::createLink(this, first, false); });
    }
    if (trash) {
        menu->addAction(themeIcon("edit-undo"), single ? tr("Restore") : tr("Restore Selected"), this,
                        [this, paths] { FileOps::restoreFromTrash(this, paths, nullptr); });
    } else {
        menu->addAction(m_actTrash);
    }
    menu->addAction(m_actDelete);
    menu->addSeparator();

    /* 5. Opening. */
    if (single) {
        QString shift;
        if (fi.isDir() && !fi.isSymLink() && RoxOpen::isFstabMountPoint(first))
            shift = RoxOpen::isMounted(first) ? tr("Unmount") : tr("Open unmounted");
        else if (fi.isSymLink()) shift = tr("Show Target");
        else if (fi.isDir()) shift = tr("Look Inside");
        else if (fi.isFile()) shift = tr("Open As Text");
        if (!shift.isEmpty())
            menu->addAction(themeIcon("document-open"), shift, this, [this, first] { shiftOpen(first); });
    }
    menu->addAction(themeIcon("system-run"), tr("Open With…"), this,
                    [this, paths] { RoxDialogs::chooseApplication(this, paths, false); });
    menu->addAction(themeIcon("list-add"), tr("Add File Action…"), this,
                    [this, paths] { RoxDialogs::addFileAction(this, paths); });
    // GTK3 open_as_root_selected(): a normal user, one directory selected. This
    // only calls the existing roxfiler-qt-root wrapper (pkexec + helper); it never
    // elevates this window and does not touch how root windows pick their theme.
    if (single && isDir && geteuid() != 0 && !qEnvironmentVariableIsEmpty("DISPLAY") &&
        !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        const QString rootLauncher = QStandardPaths::findExecutable(QStringLiteral("roxfiler-qt-root"));
        if (!rootLauncher.isEmpty()) {
            menu->addAction(themeIcon("dialog-password"), tr("Open as root"), this, [rootLauncher, first] {
                QProcess::startDetached(rootLauncher, {first});
            });
        }
    }
    if (single && FileOps::canCopyToBackgrounds(first)) {
        menu->addAction(themeIcon("preferences-desktop-wallpaper"), tr("Copy to Backgrounds…"), this,
                        [this, first] { FileOps::copyToBackgrounds(this, first); });
    }
    if (single && ImageMounter::canHandle(first)) {
        const QString mp = ImageMounter::mountedPoint(first);
        if (mp.isEmpty()) {
            menu->addAction(themeIcon("media-mount"), tr("Mount Image"), this, [this, first] {
                QPointer<FilePane> safe(this);
                ImageMounter::mount(this, first, [safe](const QString &mp) {
                    if (safe && !mp.isEmpty()) emit safe->requestNewTab(mp);
                });
            });
        } else {
            menu->addAction(themeIcon("folder-open"), tr("Open Mounted Image"), this,
                            [this, mp] { emit requestNewTab(mp); });
            menu->addAction(themeIcon("media-eject"), tr("Unmount Image"), this,
                            [this, first] { ImageMounter::unmount(this, first, nullptr); });
        }
    }
    menu->addSeparator();

    /* 6. Terminal. */
    if (isDir) {
        menu->addAction(themeIcon("utilities-terminal"), tr("Open Terminal Here"), this, [this, first] {
            if (!FileOps::openTerminalIn(first))
                status(tr("No terminal emulator was found."));
        });
    } else if (single) {
        const QStringList argv = FileOps::runInTerminalArgs(first);
        if (!argv.isEmpty()) {
            menu->addAction(themeIcon("utilities-terminal"), tr("Run in Terminal"), this,
                            [argv, workDir] { QProcess::startDetached(argv.first(), argv.mid(1), workDir); });
        }
    }
    menu->addSeparator();

    /* 7. Information. */
    if (single && !mime.isEmpty()) {
        menu->addAction(themeIcon("system-run"), tr("Set Default Application…"), this,
                        [this, paths] { RoxDialogs::chooseApplication(this, paths, true); });
    }
    if (single && Xattr::supported(first)) {
        menu->addAction(themeIcon("document-properties"), tr("Extended attributes…"), this,
                        [this, first] { RoxDialogs::extendedAttributes(this, first); });
    }
    menu->addAction(m_actProperties);
    menu->addAction(themeIcon("accessories-calculator"), tr("Count"), this,
                    [this, paths] { RoxDialogs::showCount(this, paths); });
    if (!trash) {
        menu->addAction(themeIcon("dialog-password"), tr("Permissions…"), this,
                        [this, paths] { RoxDialogs::changePermissions(this, paths, nullptr); });
    }
}

void FilePane::buildBackgroundMenu(QMenu *menu)
{
    fillDisplayMenu(menu->addMenu(themeIcon("video-display"), tr("Display")));
    fillSelectMenu(menu->addMenu(themeIcon("edit-select-all"), tr("Select")));

    QMenu *samba = menu->addMenu(themeIcon("network-workgroup"), tr("Samba"));
    samba->addAction(themeIcon("network-server"), tr("Connect to SMB Share…"), this,
                     [this] { emit hostRequest(HostAction::ConnectSmb, QString()); });
    samba->addAction(themeIcon("folder-publicshare"), tr("Shared Folders…"), this,
                     [this] { emit hostRequest(HostAction::SharedFolders, QString()); });
    if (!QStandardPaths::findExecutable(QStringLiteral("net")).isEmpty() && !inTrash()) {
        samba->addAction(themeIcon("folder-publicshare"), tr("Share This Folder…"), this, [this] {
            RoxDialogs::shareFolder(this, m_currentPath, [this](const QString &msg) { status(msg); });
        });
    }
    menu->addAction(themeIcon("preferences-system"), tr("Options…"), this,
                    [this] { emit hostRequest(HostAction::Options, QString()); });
    menu->addSeparator();

    menu->addAction(m_actCut);
    menu->addAction(m_actCopy);
    menu->addAction(m_actPaste);
    m_actPaste->setEnabled(FileOps::clipboardHasFiles() && !inTrash());
    menu->addSeparator();

    // The context menu on empty space is always available. Keep these
    // operations visible even if there is no selection (then disabled),
    // instead of giving the impression that ROX has lost its Delete menu.
    // They NEVER operate on m_currentPath as an implicit fallback.
    menu->addAction(m_actTrash);
    menu->addAction(m_actDelete);
    menu->addSeparator();

    QMenu *newMenu = menu->addMenu(themeIcon("list-add"), tr("New"));
    newMenu->setEnabled(!inTrash() && QFileInfo(m_currentPath).isWritable());
    fillNewMenu(newMenu);
    fillWindowMenu(menu->addMenu(themeIcon("window-new"), tr("Window")));
}

void FilePane::fillDisplayMenu(QMenu *m)
{
    QAction *iconsView = m->addAction(themeIcon("view-list-icons"), tr("Icons View"), this, [this] {
        m_iconDelegate->setWith(RoxWith::None);
        applyIconLayout();
        setIconMode(true);
    });
    iconsView->setCheckable(true);
    iconsView->setChecked(m_iconMode && m_iconDelegate->with() == RoxWith::None);

    QMenu *with = m->addMenu(themeIcon("view-list-details"), tr("Icons, With…"));
    const struct { RoxWith w; const char *text; const char *icon; } withs[] = {
        {RoxWith::Sizes, QT_TR_NOOP("Sizes"), "view-list-details"},
        {RoxWith::Permissions, QT_TR_NOOP("Permissions"), "dialog-password"},
        {RoxWith::Types, QT_TR_NOOP("Types"), "text-x-generic"},
        {RoxWith::Times, QT_TR_NOOP("Times"), "appointment-new"},
    };
    for (const auto &w : withs) {
        const RoxWith value = w.w;
        QAction *a = with->addAction(themeIcon(w.icon), tr(w.text), this, [this, value] { setWith(value); });
        a->setCheckable(true);
        a->setChecked(m_iconMode && m_iconDelegate->with() == value);
    }
    QAction *list = m->addAction(themeIcon("view-list-details"), tr("List View"), this,
                                 [this] { setIconMode(false); });
    list->setCheckable(true);
    list->setChecked(!m_iconMode);
    m->addSeparator();
    m->addAction(m_actBigger);
    m->addAction(m_actSmaller);
    m->addAction(m_actAutoSize);
    m->addSeparator();
    m->addActions(m_sortGroup->actions());
    m->addAction(m_actReversed);
    m->addSeparator();
    m->addAction(m_actShowHidden);
    m->addAction(themeIcon("edit-find"), tr("Filter Files…"), this,
                 [this] { emit hostRequest(HostAction::FocusFilter, QString()); });
    m->addAction(m_actFilterDirs);
    m->addAction(m_actThumbs);
    m->addAction(themeIcon("view-refresh"), tr("Refresh"), this, [this] { refresh(); });
    m->addAction(themeIcon("document-save"), tr("Save Current Display Settings…"), this,
                 [this] { saveDisplaySettings(); });
}

void FilePane::fillSelectMenu(QMenu *m)
{
    m->addAction(m_actSelectAll);
    m->addAction(themeIcon("edit-clear"), tr("Clear Selection"), this,
                 [this] { clearFileSelection(); });
    m->addAction(themeIcon("edit-select-all"), tr("Invert Selection"), this, [this] { invertSelection(); });
    m->addAction(themeIcon("edit-find"), tr("Select by Name…"), this, [this] { selectByName(); });
    m->addAction(themeIcon("edit-find"), tr("Select If…"), this, [this] { selectIf(); });
}

void FilePane::fillNewMenu(QMenu *m)
{
    m->addAction(m_actNewDir);
    m->addAction(themeIcon("document-new"), tr("Blank file…"), this, [this] { newBlankFile(); });
    const QStringList templates = FileOps::templateFiles();
    if (!templates.isEmpty())
        m->addSeparator();
    for (const QString &t : templates) {
        const QFileInfo tfi(t);
        m->addAction(m_iconProvider->icon(tfi), tfi.fileName(), this, [this, t] { newFromTemplate(t); });
    }
    m->addSeparator();
    m->addAction(themeIcon("preferences-system"), tr("Customise Menu…"), this, [this] {
        const QString dir = FileOps::userTemplatesDir();
        QDir().mkpath(dir);
        QMessageBox::information(this, tr("Customise Menu"),
            tr("Any files placed in your Templates directory will appear in the New menu.\n\n%1").arg(dir));
        emit requestNewTab(dir);
    });
}

void FilePane::fillWindowMenu(QMenu *m)
{
    m->addAction(themeIcon("go-up"), tr("Parent, New Window"), this, [this] {
        emit hostRequest(HostAction::NewWindow, QFileInfo(m_currentPath).absolutePath());
    });
    m->addAction(themeIcon("go-up"), tr("Parent, Same Window"), this, [this] { goUp(); });
    m->addAction(themeIcon("window-new"), tr("New Window"), this,
                 [this] { emit hostRequest(HostAction::NewWindow, m_currentPath); });
    m->addAction(m_actHome);
    m->addAction(themeIcon("bookmarks"), tr("Show Bookmarks"), this,
                 [this] { emit hostRequest(HostAction::ShowBookmarks, QString()); });
    QAction *follow = m->addAction(themeIcon("emblem-symbolic-link"), tr("Follow Symbolic Links"), this, [this] {
        navigate(QFileInfo(m_currentPath).canonicalFilePath());
    });
    follow->setEnabled(QFileInfo(m_currentPath).canonicalFilePath() != m_currentPath);
    m->addAction(m_actResize);
    m->addAction(themeIcon("view-split-left-right"), tr("Open Paired Windows"), this,
                 [this] { emit hostRequest(HostAction::PairedWindows, QString()); });
    m->addSeparator();
    m->addAction(themeIcon("window-close"), tr("Close Window"), this,
                 [this] { emit hostRequest(HostAction::CloseWindow, QString()); });
    m->addSeparator();
    m->addAction(themeIcon("go-jump"), tr("Enter Path…"), this,
                 [this] { emit hostRequest(HostAction::EditLocation, QString()); });
    m->addAction(themeIcon("utilities-terminal"), tr("Shell Command…"), this,
                 [this] { RoxDialogs::shellCommand(this, m_currentPath, selectedPaths(), nullptr); });
    m->addAction(themeIcon("utilities-terminal"), tr("Open Terminal Here"), this,
                 [this] { emit hostRequest(HostAction::OpenTerminal, m_currentPath); });
    m->addAction(themeIcon("utilities-terminal"), tr("Switch to Terminal"), this, [this] {
        // ROX xterm_here(TRUE): open the terminal here and close this window.
        if (FileOps::openTerminalIn(m_currentPath))
            emit hostRequest(HostAction::CloseAfterOpen, QString());
        else
            status(tr("No terminal emulator was found."));
    });
}

/* -------------------------------------------------------------- commands */

void FilePane::copyToClipboard(bool cut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    FileOps::setClipboard(paths, cut);
    status(cut ? tr("%n item(s) cut to the clipboard.", nullptr, int(paths.size()))
               : tr("%n item(s) copied to the clipboard.", nullptr, int(paths.size())));
}

void FilePane::paste()
{
    bool cut = false;
    const QStringList paths = FileOps::clipboardPaths(&cut);
    if (paths.isEmpty() || inTrash())
        return;
    FileOps::transfer(this, paths, m_currentPath, cut, [cut, paths](bool ok) {
        bool stillCut = false;
        if (cut && ok && FileOps::clipboardPaths(&stillCut) == paths && stillCut)
            QApplication::clipboard()->clear(); // only the completed original cut
    });
}

void FilePane::transferToPath(const QStringList &paths, bool move, bool duplicate)
{
    if (paths.isEmpty() || inTrash()) return;
    const bool single = paths.size() == 1;
    const QString title = duplicate ? tr("Duplicate…") : (move ? tr("Move to…") : tr("Copy to…"));
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(760); // Absolute paths must remain editable and readable.
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    if (single) {
        auto *source = new QLineEdit(QFileInfo(paths.first()).absoluteFilePath(), &dlg);
        source->setReadOnly(true);
        source->setCursorPosition(0);
        source->setToolTip(source->text());
        form->addRow(tr("Full source path:"), source);
    } else {
        auto *sourceList = new QPlainTextEdit(&dlg);
        sourceList->setReadOnly(true);
        sourceList->setObjectName(QStringLiteral("roxSelectedSources"));
        sourceList->setPlainText(paths.join(QLatin1Char('\n')));
        sourceList->setMinimumHeight(110);
        sourceList->setMaximumHeight(180);
        form->addRow(tr("Selected items:"), sourceList);
    }
    QString initial = m_currentPath;
    if (single) {
        const QFileInfo fi(paths.first());
        QString name = fi.fileName();
        if (!move && QDir::cleanPath(fi.absolutePath()) == QDir::cleanPath(m_currentPath))
            name = FileOps::uniqueName(m_currentPath, name);
        initial = QDir(m_currentPath).filePath(name);
    }
    auto *destination = new QLineEdit(initial, &dlg);
    destination->setObjectName(QStringLiteral("roxFullDestinationPath"));
    destination->setClearButtonEnabled(true);
    destination->setToolTip(tr("You can edit the full destination, including the filename."));
    auto *browse = new QPushButton(tr("Browse…"), &dlg);
    auto *destRow = new QHBoxLayout;
    destRow->addWidget(destination, 1);
    destRow->addWidget(browse);
    form->addRow(single ? tr("Full destination path:") : tr("Destination folder:"), destRow);
    auto *help = new QLabel(single
        ? tr("You can edit the full destination, including the filename.")
        : tr("Choose the folder into which all selected items will be placed."), &dlg);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(duplicate ? tr("Duplicate") : (move ? tr("Move") : tr("Copy")));
    layout->addWidget(buttons);
    connect(browse, &QPushButton::clicked, &dlg, [this, destination, single, paths] {
        const QString typed = destination->text();
        const QFileInfo info(typed);
        // Browse for a directory; keep the filename when editing a full path.
        const QString folder = single && !info.isDir() ? info.absolutePath() : typed;
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Destination folder"), folder);
        if (chosen.isEmpty()) return;
        const bool directoryEntry = QFileInfo(typed).isDir() || typed.endsWith(QLatin1Char('/'));
        const QString basename = single
            ? (directoryEntry ? QFileInfo(paths.first()).fileName() : QFileInfo(typed).fileName())
            : QString();
        destination->setText(single && !basename.isEmpty()
            ? QDir(chosen).filePath(basename) : chosen);
    });

    QString resolved;
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        const QString typed = destination->text();
        if (!QDir::isAbsolutePath(typed)) {
            QMessageBox::warning(&dlg, title, tr("The destination must be an absolute path."));
            destination->setFocus();
            return; // Keep the dialog open to correct the path.
        }
        resolved = single ? FileOps::resolveSingleDestination(paths.first(), typed, move)
                          : QDir::cleanPath(typed);
        if (single && (resolved.isEmpty() || resolved == QDir::cleanPath(paths.first()))) {
            QMessageBox::warning(&dlg, title, tr("Source and destination must be different."));
            destination->setFocus();
            return;
        }
        if (single && !QFileInfo(resolved).absoluteDir().exists()) {
            QMessageBox::warning(&dlg, title, tr("The destination folder is not available."));
            destination->setFocus();
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    // Like GTK's savebox, select only the filename, leaving the full absolute
    // path visible and editable.  Rename… also uses this full-path convention.
    if (single) {
        const QString base = QFileInfo(initial).fileName();
        const int pos = initial.length() - base.length();
        const int dot = base.lastIndexOf(QLatin1Char('.'));
        destination->setSelection(pos, dot > 0 ? dot : base.length());
    } else {
        destination->setFocus();
    }
    if (dlg.exec() != QDialog::Accepted) return;
    if (single) {
        FileOps::transfer(this, paths, QFileInfo(resolved).absolutePath(), move, nullptr,
                          QFileInfo(resolved).fileName());
    } else {
        FileOps::transfer(this, paths, resolved, move, nullptr);
    }
}

void FilePane::renameSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.size() != 1)
        return;
    const QString original = paths.first();
    const QFileInfo info(original);
    const QString currentFullPath = QDir::cleanPath(info.absoluteFilePath());

    // ROX GTK3's rename/savebox edits the FULL destination. Keep the entire
    // absolute path visible, but preselect only the filename (not extension).
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Rename"));
    dlg.setMinimumWidth(720);
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *origin = new QLineEdit(currentFullPath, &dlg);
    origin->setReadOnly(true);
    origin->setCursorPosition(0);
    form->addRow(tr("Full source path:"), origin);
    auto *target = new QLineEdit(currentFullPath, &dlg);
    target->setObjectName(QStringLiteral("roxFullRenamePath"));
    target->setClearButtonEnabled(true);
    form->addRow(tr("Full destination path:"), target);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Rename"));
    layout->addWidget(buttons);
    const QString basename = info.fileName();
    const int extension = info.isDir() ? -1 : basename.lastIndexOf(QLatin1Char('.'));
    target->setSelection(currentFullPath.size() - basename.size(),
                         extension > 0 ? extension : basename.size());
    target->setFocus();
    QString requestedPath;
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        const QString input = target->text();
        if (input.isEmpty() || !QDir::isAbsolutePath(input)) {
            QMessageBox::warning(&dlg, tr("Rename"), tr("The destination must be an absolute path."));
            return;
        }
        const QString cleaned = QDir::cleanPath(input);
        if (cleaned == currentFullPath) { dlg.reject(); return; }
        const QFileInfo destInfo(cleaned);
        if (cleaned == QLatin1String("/") || !destInfo.absoluteDir().exists() ||
            destInfo.fileName().isEmpty() || input.endsWith(QLatin1Char('/'))) {
            QMessageBox::warning(&dlg, tr("Rename"), tr("The destination folder is not available."));
            return;
        }
        if (destInfo.exists() || destInfo.isSymLink()) {
            QMessageBox::warning(&dlg, tr("Rename"), tr("“%1” already exists.").arg(cleaned));
            return;
        }
        // Moving a folder into one of its own descendants would invalidate
        // the hierarchy and must not be attempted as an ordinary rename.
        const QString canonicalSource = info.canonicalFilePath();
        const QString canonicalDestinationParent = destInfo.absoluteDir().canonicalPath();
        if (info.isDir() && !info.isSymLink() && !canonicalSource.isEmpty() &&
            (canonicalDestinationParent == canonicalSource ||
             canonicalDestinationParent.startsWith(canonicalSource + QLatin1Char('/')))) {
            QMessageBox::warning(&dlg, tr("Rename"), tr("Source and destination must be different."));
            return;
        }
        requestedPath = cleaned;
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || requestedPath.isEmpty()) return;

    const bool ok = info.isDir() && !info.isSymLink()
        ? QDir().rename(currentFullPath, requestedPath)
        : QFile::rename(currentFullPath, requestedPath);
    if (!ok) {
        QMessageBox::warning(this, tr("Rename"),
                             tr("Could not rename the selected item.") +
                             QLatin1Char('\n') + requestedPath);
    } else {
        m_model->markRecent(requestedPath);
        if (QFileInfo(requestedPath).absolutePath() == m_currentPath)
            selectPathWhenLoaded(requestedPath);
    }
}

void FilePane::trashSelected()
{
    if (inTrash())
        return;
    const QStringList failed = FileOps::moveToTrash(selectedPaths());
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Trash"),
                             tr("Could not move these items to Trash:\n%1").arg(failed.join(QLatin1Char('\n'))));
}

void FilePane::deleteSelected()
{
    FileOps::deletePermanently(this, selectedPaths(), nullptr);
}

void FilePane::shiftOpen(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isSymLink()) {
        const QString target = fi.symLinkTarget();
        const QFileInfo tfi(target);
        if (!tfi.exists()) {
            QMessageBox::warning(this, tr("Show Target"), tr("The link target does not exist:\n%1").arg(target));
            return;
        }
        navigate(tfi.absolutePath());
        selectPathWhenLoaded(tfi.absoluteFilePath());
        return;
    }
    if (fi.isDir()) {
        if (isMountPoint(path)) {
            openMountPoint(path, true); // Unmount / Open unmounted
            return;
        }
        navigate(path); // Look Inside (application directories included)
        return;
    }
    openAsText(path);
}

void FilePane::newDirectory()
{
    if (inTrash())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Directory"), tr("Name:"),
        QLineEdit::Normal, FileOps::uniqueName(m_currentPath, tr("New Folder")), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || !QDir(m_currentPath).mkdir(name)) {
        QMessageBox::warning(this, tr("New Directory"), tr("Could not create the folder."));
        return;
    }
    m_model->markRecent(QDir(m_currentPath).filePath(name));
    selectPathWhenLoaded(QDir(m_currentPath).filePath(name));
}

void FilePane::newBlankFile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New File"), tr("Name:"),
        QLineEdit::Normal, FileOps::uniqueName(m_currentPath, tr("NewFile")), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    QFile f(QDir(m_currentPath).filePath(name));
    if (name.contains(QLatin1Char('/')) || !f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        QMessageBox::warning(this, tr("New File"), tr("Could not create the file."));
        return;
    }
    f.close();
    selectPathWhenLoaded(f.fileName());
}

void FilePane::newFromTemplate(const QString &templatePath)
{
    const QFileInfo tfi(templatePath);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New"), tr("Name:"), QLineEdit::Normal,
        FileOps::uniqueName(m_currentPath, tfi.fileName()), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    const QString dest = QDir(m_currentPath).filePath(name);
    if (name.contains(QLatin1Char('/')) || QFileInfo::exists(dest)) {
        QMessageBox::warning(this, tr("New"), tr("“%1” already exists.").arg(name));
        return;
    }
    if (tfi.isDir()) {
        FileJob::run(this, tr("Copying…"),
                     {QStringList{QStringLiteral("cp"), QStringLiteral("-a"), QStringLiteral("--"), templatePath, dest}},
                     nullptr);
        return;
    }
    if (!QFile::copy(templatePath, dest)) {
        QMessageBox::warning(this, tr("New"), tr("Could not create the file."));
        return;
    }
    // Templates from the bundled resources are read-only; scripts stay executable.
    QFile::Permissions perms = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
    QFile f(dest);
    if (f.open(QIODevice::ReadOnly) && f.peek(2) == "#!")
        perms |= QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
    f.close();
    QFile::setPermissions(dest, perms);
    selectPathWhenLoaded(dest);
}

void FilePane::selectByName()
{
    bool ok = false;
    const QString pattern = QInputDialog::getText(this, tr("Select by Name"),
        tr("Select items whose name matches (wildcards * ? [ ] allowed):"),
        QLineEdit::Normal, QStringLiteral("*"), &ok).trimmed();
    if (!ok || pattern.isEmpty())
        return;
    const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
                                QRegularExpression::CaseInsensitiveOption);
    QAbstractItemView *view = currentView();
    const QModelIndex root = proxyRootIndex();
    QItemSelection selection;
    for (int r = 0; r < m_proxy->rowCount(root); ++r) {
        const QModelIndex idx = m_proxy->index(r, 0, root);
        if (re.match(idx.data(Qt::DisplayRole).toString()).hasMatch())
            selection.select(idx, idx);
    }
    view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void FilePane::selectIf()
{
    bool ok = false;
    const QString expression = QInputDialog::getText(this, tr("Select If"),
        tr("Select items for which this is true.\n"
           "Examples:  size > 1M      'ptr*' and IsDir      mtime after 1 week ago      -fx"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || expression.isEmpty())
        return;
    QString error;
    const std::unique_ptr<FindExpr> condition = FindExpr::parse(expression, &error);
    if (!condition) {
        QMessageBox::warning(this, tr("Select If"), error);
        return;
    }
    QAbstractItemView *view = currentView();
    const QModelIndex root = proxyRootIndex();
    QItemSelection selection;
    for (int r = 0; r < m_proxy->rowCount(root); ++r) {
        const QModelIndex idx = m_proxy->index(r, 0, root);
        if (condition->matches(m_model->filePath(sourceIndexForViewIndex(idx))))
            selection.select(idx, idx);
    }
    view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    status(tr("%n item(s) selected.", nullptr, int(view->selectionModel()->selectedRows(0).size())));
}

QSize FilePane::currentViewSize() const
{
    QAbstractItemView *view = currentView();
    return view ? view->viewport()->size() : QSize();
}

// Mirrors ROX view_autosize(): fit every item, within 80% of the screen.
QSize FilePane::desiredViewSize() const
{
    QAbstractItemView *view = currentView();
    if (!view)
        return QSize();
    const int count = m_proxy->rowCount(proxyRootIndex());
    if (count <= 0)
        return view->viewport()->size();
    const QScreen *screen = window() && window()->screen() ? window()->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const int maxWidth = available.width() * 8 / 10;
    const int maxHeight = available.height() * 8 / 10;
    const int frame = view->frameWidth() * 2;
    const int scroll = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    QSize wanted;
    if (m_iconMode) {
        const QSize grid = m_icons->gridSize().isValid() ? m_icons->gridSize() : QSize(120, 100);
        const int maxColumns = qMax(1, (maxWidth - frame - scroll) / grid.width());
        // Roughly a 4:3 window.
        int columns = int(qCeil(qSqrt(count * (4.0 / 3.0) * grid.height() / double(grid.width()))));
        columns = qBound(1, columns, qMin(count, maxColumns));
        const int rows = (count + columns - 1) / columns;
        wanted = QSize(columns * grid.width() + frame + scroll, rows * grid.height() + frame);
    } else {
        const int rowHeight = qMax(1, m_details->sizeHintForRow(0));
        wanted = QSize(m_details->header()->length() + frame + scroll,
                       (count * rowHeight) + m_details->header()->height() + frame);
    }
    return QSize(qBound(320, wanted.width(), maxWidth), qBound(160, wanted.height(), maxHeight));
}

void FilePane::requestResize()
{
    const QSize wanted = desiredViewSize();
    if (wanted.isValid())
        emit hostRequest(HostAction::ResizeToFit, QStringLiteral("%1x%2").arg(wanted.width()).arg(wanted.height()));
}

void FilePane::invertSelection()
{
    QAbstractItemView *view = currentView();
    const QModelIndex root = proxyRootIndex();
    const int rows = m_proxy->rowCount(root);
    if (rows == 0)
        return;
    const QItemSelection all(m_proxy->index(0, 0, root), m_proxy->index(rows - 1, 0, root));
    view->selectionModel()->select(all, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void FilePane::selectPathWhenLoaded(const QString &path)
{
    queueIconLayout();
    m_pendingSelect = QDir::cleanPath(path);
    QTimer::singleShot(250, this, [this] { trySelectPending(); });
}

void FilePane::trySelectPending()
{
    if (m_pendingSelect.isEmpty())
        return;
    const QModelIndex src = m_model->index(m_pendingSelect);
    if (!src.isValid() || QDir::cleanPath(m_model->filePath(src)) != m_pendingSelect)
        return;
    const QModelIndex idx = m_proxy->mapFromSource(src);
    if (!idx.isValid())
        return;
    QAbstractItemView *view = currentView();
    view->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(idx);
    m_pendingSelect.clear();
}
