#include "filemodel.h"
#include "thumbnailer.h"

#include <QDateTime>
#include <QApplication>
#include <QPalette>
#include <QTimer>
#include <QBrush>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QListView>
#include <QPainter>
#include <QStyle>
#include <QTextLayout>
#include <QTextOption>
#include <QRegularExpression>

RoxFileModel::RoxFileModel(QObject *parent)
    : QFileSystemModel(parent)
{
    connect(Thumbnailer::instance(), &Thumbnailer::thumbnailReady,
            this, &RoxFileModel::onThumbnailReady);
}

QVariant RoxFileModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::ToolTipRole && index.column() == 0)
        return filePath(index); // always inspectable, even for narrow windows
    if (!m_recent.isEmpty() && index.column() == 0 &&
        m_recent.contains(QDir::cleanPath(filePath(index)))) {
        if (role == Qt::BackgroundRole) {
            QColor color = QApplication::palette().color(QPalette::Highlight);
            color.setAlpha(90);  // Same Qt/SonicDE theme color, translucent.
            return QBrush(color);
        }
        if (role == RoxRecentRole)
            return true; // bold is applied by the delegates (see RoxItemDelegate)
    }
    if (role == Qt::DecorationRole && index.column() == 0 && m_thumbnails) {
        const QString path = filePath(index);
        if (Thumbnailer::canThumbnail(path)) {
            const QFileInfo fi = fileInfo(index);
            if (fi.isFile()) {
                const QIcon thumb = Thumbnailer::instance()->thumbnail(
                    path, fi.lastModified().toSecsSinceEpoch());
                if (!thumb.isNull())
                    return thumb;
            }
        }
    }
    return QFileSystemModel::data(index, role);
}

void RoxFileModel::markRecent(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    m_recent.insert(clean);
    const QModelIndex idx = index(clean);
    if (idx.isValid()) emit dataChanged(idx, idx, {Qt::BackgroundRole, RoxRecentRole});
    // GTK's wink is transient. Leave a longer, steady indication for Qt.
    QTimer::singleShot(12000, this, [this, clean] {
        if (!m_recent.remove(clean)) return;
        const QModelIndex idx = index(clean);
        if (idx.isValid()) emit dataChanged(idx, idx, {Qt::BackgroundRole, RoxRecentRole});
    });
}

void RoxFileModel::setThumbnailsEnabled(bool enabled)
{
    if (m_thumbnails == enabled)
        return;
    m_thumbnails = enabled;
    // Repaint every loaded row of the visible directory.
    const QModelIndex root = index(rootPath());
    const int rows = rowCount(root);
    if (rows > 0)
        emit dataChanged(index(0, 0, root), index(rows - 1, 0, root), {Qt::DecorationRole});
}

void RoxFileModel::onThumbnailReady(const QString &path)
{
    if (!m_thumbnails)
        return;
    const QModelIndex idx = index(path);
    if (idx.isValid() && filePath(idx) == path)
        emit dataChanged(idx, idx, {Qt::DecorationRole});
}

RoxProxyModel::RoxProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
}

/* Qt 6.11 deprecates invalidateFilter() in favour of begin/endFilterChange();
 * the filter state must change between the two calls. */
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
#define ROX_FILTER_CHANGE(statement) do { beginFilterChange(); statement; endFilterChange(); } while (0)
#else
#define ROX_FILTER_CHANGE(statement) do { statement; invalidateFilter(); } while (0)
#endif

void RoxProxyModel::setVisibleRoot(const QString &path)
{
    ROX_FILTER_CHANGE(m_visibleRoot = QDir::cleanPath(path));
}

void RoxProxyModel::setFilterDirectories(bool enabled)
{
    ROX_FILTER_CHANGE(m_filterDirs = enabled);
}

void RoxProxyModel::setSortMode(RoxSort mode)
{
    m_sortMode = mode;
    invalidate();
}

bool RoxProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    auto *fs = qobject_cast<QFileSystemModel *>(sourceModel());
    if (!fs)
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);

    const QModelIndex idx = fs->index(sourceRow, 0, sourceParent);
    const QString path = QDir::cleanPath(fs->filePath(idx));
    if (m_visibleRoot.isEmpty() || path == m_visibleRoot)
        return true;

    // Ancestors of the visible root must stay so the root index stays mapped.
    QString ancestorPrefix = path;
    if (!ancestorPrefix.endsWith(QLatin1Char('/')))
        ancestorPrefix += QLatin1Char('/');
    if (m_visibleRoot.startsWith(ancestorPrefix))
        return true;

    // Only filter children of the visible directory.
    if (QDir::cleanPath(fs->filePath(sourceParent)) != m_visibleRoot)
        return true;

    const QRegularExpression re = filterRegularExpression();
    if (re.pattern().isEmpty())
        return true;
    // Classic ROX: the filter does not hide directories unless
    // "Filter Directories With Files" is enabled.
    if (!m_filterDirs && fs->isDir(idx))
        return true;
    return re.match(fs->fileName(idx)).hasMatch();
}

bool RoxProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto *fs = qobject_cast<QFileSystemModel *>(sourceModel());
    if (!fs)
        return QSortFilterProxyModel::lessThan(left, right);

    const QFileInfo a = fs->fileInfo(left);
    const QFileInfo b = fs->fileInfo(right);
    const bool aDir = a.isDir();
    const bool bDir = b.isDir();
    if (aDir != bDir) {
        // Keep directories first even when the order is reversed.
        return sortOrder() == Qt::AscendingOrder ? aDir : bDir;
    }

    RoxSort mode = m_sortMode;
    switch (left.column()) {
    case 1: mode = RoxSort::Size; break;
    case 2: mode = RoxSort::Type; break;
    case 3: mode = RoxSort::MTime; break;
    default: break;
    }

    auto cmp = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
    int c = 0;
    switch (mode) {
    case RoxSort::Name: break;
    case RoxSort::Type:
        c = m_collator.compare(fs->type(left), fs->type(right));
        if (c == 0) c = m_collator.compare(a.suffix(), b.suffix());
        break;
    case RoxSort::ATime: c = cmp(a.lastRead(), b.lastRead()); break;
    case RoxSort::CTime: c = cmp(a.metadataChangeTime(), b.metadataChangeTime()); break;
    case RoxSort::MTime: c = cmp(a.lastModified(), b.lastModified()); break;
    case RoxSort::Size: c = cmp(a.size(), b.size()); break;
    case RoxSort::Owner: c = m_collator.compare(a.owner(), b.owner()); break;
    case RoxSort::Group: c = m_collator.compare(a.group(), b.group()); break;
    }
    if (c == 0)
        c = m_collator.compare(a.fileName(), b.fileName());
    return c < 0;
}

QFont RoxItemDelegate::boldVariant(const QFont &font)
{
    QFont bold(font);
    bold.setStyleName(QString()); // otherwise a "Regular" style name defeats setBold()
    bold.setBold(true);
    return bold;
}

void RoxItemDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    if (index.column() == 0 && index.data(RoxRecentRole).toBool()) {
        option->font = boldVariant(option->font);
        option->fontMetrics = QFontMetrics(option->font);
    }
}

RoxIconDelegate::RoxIconDelegate(DetailFn detail, QObject *parent)
    : RoxItemDelegate(parent), m_detail(std::move(detail))
{
}

void RoxIconDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const
{
    RoxItemDelegate::initStyleOption(option, index);
    option->textElideMode = Qt::ElideNone; // Never hide part of the name.
    if (m_with != RoxWith::None && m_detail) {
        const QString extra = m_detail(index, m_with);
        if (!extra.isEmpty())
            option->text += QLatin1Char('\n') + extra;
    }
    // Same label width FilePane::applyIconLayout() uses to size the cells.
    const int available = option->rect.width() > 0 ? option->rect.width() - 16 : 168;
    option->text = breakable(option->text, option->fontMetrics, available);
}

QString RoxIconDelegate::breakable(const QString &text, const QFontMetrics &, int)
{
    // QTextLayout with WrapAtWordBoundaryOrAnywhere wraps unbroken names without
    // inserting U+200B inside filenames, including composed Unicode sequences.
    return text;
}

QSize RoxIconDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // uniformItemSizes() makes the view reuse the size of the FIRST item for
    // all of them; with the default hint that was only as wide as the icon, and
    // every longer name was cut off.  The grid already holds the width/height
    // computed from the real names (FilePane::applyIconLayout).
    if (auto *list = qobject_cast<const QListView *>(option.widget)) {
        const QSize grid = list->gridSize();
        if (grid.isValid())
            return grid;
    }
    return RoxItemDelegate::sizeHint(option, index);
}

void RoxIconDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    QStyleOptionViewItem item(option);
    initStyleOption(&item, index);
    QStyleOptionViewItem backdrop(item);
    backdrop.text.clear();
    backdrop.icon = QIcon();
    const QStyle *style = item.widget ? item.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &backdrop, painter, item.widget);

    const int iconSize = qMax(16, item.decorationSize.width());
    const QRect iconArea(item.rect.center().x() - iconSize / 2,
                         item.rect.top() + 6, iconSize, iconSize);
    const QIcon::Mode mode = (item.state & QStyle::State_Enabled)
         ? ((item.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal)
         : QIcon::Disabled;
    item.icon.paint(painter, iconArea, Qt::AlignCenter, mode);

    // QStyledItemDelegate's built-in IconMode layout can truncate after its
    // implicit label rectangle. Draw every calculated line ourselves.
    const int labelWidth = qMax(32, item.rect.width() - 16);
    QTextLayout layout(item.text, item.font);
    QTextOption wrap;
    wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(wrap);
    layout.beginLayout();
    qreal y = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(labelWidth);
        line.setPosition(QPointF(qMax(qreal(0),
            (labelWidth - line.naturalTextWidth()) / 2.0), y));
        y += line.height();
    }
    layout.endLayout();
    const QRect labelRect = item.rect.adjusted(8, iconSize + 12, -8, -4);
    painter->save();
    painter->setClipRect(labelRect);
    painter->setFont(item.font);
    const QPalette::ColorRole foreground = (item.state & QStyle::State_Selected)
            ? QPalette::HighlightedText : QPalette::Text;
    painter->setPen(item.palette.color(foreground));
    layout.draw(painter, labelRect.topLeft());
    painter->restore();
}
