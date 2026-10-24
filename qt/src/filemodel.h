#pragma once

#include <QCollator>
#include <QFileSystemModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

#include <functional>

/* Custom role: true while an item is "new" (just copied/moved/linked/created). */
constexpr int RoxRecentRole = Qt::UserRole + 40;

/* QFileSystemModel that returns image thumbnails as DecorationRole. */
class RoxFileModel final : public QFileSystemModel
{
    Q_OBJECT
public:
    explicit RoxFileModel(QObject *parent = nullptr);

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setThumbnailsEnabled(bool enabled);
    bool thumbnailsEnabled() const { return m_thumbnails; }
    void markRecent(const QString &path);

private:
    void onThumbnailReady(const QString &path);
    bool m_thumbnails = true;
    QSet<QString> m_recent;
};

/* Classic ROX sort keys. Directories are always listed first. */
enum class RoxSort { Name = 0, Type, ATime, CTime, MTime, Size, Owner, Group };

class RoxProxyModel final : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit RoxProxyModel(QObject *parent = nullptr);

    void setVisibleRoot(const QString &path);
    void setFilterDirectories(bool enabled);
    bool filterDirectories() const { return m_filterDirs; }
    void setSortMode(RoxSort mode);
    RoxSort sortMode() const { return m_sortMode; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QString m_visibleRoot;
    bool m_filterDirs = false;
    RoxSort m_sortMode = RoxSort::Name;
    QCollator m_collator;
};

/* "Icons, With..." (Sizes / Permissions / Types / Times) for the icon view. */
enum class RoxWith { None = 0, Sizes, Permissions, Types, Times };

/* Shared by both views: draws "new" items in bold.  The bold font is derived
 * from the view's own font and its style name is cleared: KDE/Plasma fonts
 * carry a style name such as "Regular", and with a style name set Qt ignores
 * setBold(true), so the text would silently stay normal. */
class RoxItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static QFont boldVariant(const QFont &font);

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;
};

class RoxIconDelegate final : public RoxItemDelegate
{
    Q_OBJECT
public:
    using DetailFn = std::function<QString(const QModelIndex &, RoxWith)>;
    RoxIconDelegate(DetailFn detail, QObject *parent = nullptr);

    void setWith(RoxWith with) { m_with = with; }
    RoxWith with() const { return m_with; }

    /* In icon mode every cell is as wide as the view's grid, so the complete
     * file name is laid out (wrapped) instead of being clipped to the icon. */
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    /* Retained for compatibility: wrapping is now done by QTextLayout without
     * adding invisible Unicode characters to displayed filenames. */
    static QString breakable(const QString &text, const QFontMetrics &metrics, int width);

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;

private:
    DetailFn m_detail;
    RoxWith m_with = RoxWith::None;
};
