#pragma once

/*
 * ROX-Filer2-QT - MIME icon provider.
 *
 * QFileIconProvider only returns MIME-specific icons when the active Qt
 * platform theme asks for it (KDE/GNOME platform theme plugins). Without such
 * a plugin it falls back to the two generic QStyle icons (file / folder),
 * which is why every file looked the same. This provider resolves icons the
 * same way the GTK frontend does, independently of the platform plugin:
 *
 *   1. .DirIcon inside a directory (ROX application directories)
 *   2. ~/.config/rox.sourceforge.net/MIME-icons/<media>_<subtype>.png
 *   3. XDG special folders (Home, Desktop, Documents...)
 *   4. icon theme: MIME iconName, genericIconName, then parent MIME types
 *   5. bundled ROX MIME icons (squashfs/sfs/pet) and QFileIconProvider
 *
 * icon() runs in QFileSystemModel's gatherer thread, so it must not touch
 * QPixmap. Only QIcon objects are created here.
 */

#include <QFileIconProvider>
#include <QHash>
#include <QMimeDatabase>
#include <QMutex>

class RoxIconProvider final : public QFileIconProvider
{
public:
    RoxIconProvider();

    QIcon icon(IconType type) const override;
    QIcon icon(const QFileInfo &info) const override;
    QString type(const QFileInfo &info) const override;

    static QMimeType mimeTypeFor(const QFileInfo &info);
    static QIcon iconForMime(const QMimeType &mime);

private:
    QIcon specialFolderIcon(const QString &path) const;

    QHash<QString, QString> m_specialFolders;
};
