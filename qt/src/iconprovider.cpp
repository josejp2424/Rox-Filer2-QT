#include "iconprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStandardPaths>

namespace {

QMutex g_cacheMutex;
QHash<QString, QIcon> g_mimeIconCache;

QIcon themed(const QString &name)
{
    if (name.isEmpty())
        return QIcon();
    return QIcon::fromTheme(name);
}

QIcon bundledMimeIcon(const QString &iconName)
{
    const QString res = QStringLiteral(":/rox-qt/mime/") + iconName + QStringLiteral(".svg");
    return QFile::exists(res) ? QIcon(res) : QIcon();
}

QIcon userMimeIcon(const QString &mimeName)
{
    // Same override location as the GTK build (type.c: MIME-icons).
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                         + QStringLiteral("/rox.sourceforge.net/MIME-icons/");
    QString leaf = mimeName;
    leaf.replace(QLatin1Char('/'), QLatin1Char('_'));
    const QString png = base + leaf + QStringLiteral(".png");
    if (QFileInfo::exists(png))
        return QIcon(png);
    return QIcon();
}

} // namespace

RoxIconProvider::RoxIconProvider()
{
    setOptions(QFileIconProvider::DontUseCustomDirectoryIcons);

    const struct { QStandardPaths::StandardLocation loc; const char *icon; } folders[] = {
        {QStandardPaths::HomeLocation, "user-home"},
        {QStandardPaths::DesktopLocation, "user-desktop"},
        {QStandardPaths::DocumentsLocation, "folder-documents"},
        {QStandardPaths::DownloadLocation, "folder-download"},
        {QStandardPaths::MusicLocation, "folder-music"},
        {QStandardPaths::PicturesLocation, "folder-pictures"},
        {QStandardPaths::MoviesLocation, "folder-videos"},
        {QStandardPaths::TemplatesLocation, "folder-templates"},
        {QStandardPaths::PublicShareLocation, "folder-publicshare"},
    };
    const QString home = QDir::homePath();
    for (const auto &f : folders) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(f.loc));
        // XDG falls back to $HOME for unset dirs; do not paint $HOME as Desktop.
        if (path.isEmpty() || (path == home && f.loc != QStandardPaths::HomeLocation))
            continue;
        m_specialFolders.insert(path, QString::fromLatin1(f.icon));
    }
    m_specialFolders.insert(QDir::cleanPath(home + QStringLiteral("/.local/share/Trash/files")),
                            QStringLiteral("user-trash"));
}

QIcon RoxIconProvider::icon(IconType type) const
{
    QIcon i;
    switch (type) {
    case Computer: i = themed(QStringLiteral("computer")); break;
    case Desktop: i = themed(QStringLiteral("user-desktop")); break;
    case Trashcan: i = themed(QStringLiteral("user-trash")); break;
    case Network: i = themed(QStringLiteral("network-workgroup")); break;
    case Drive: i = themed(QStringLiteral("drive-harddisk")); break;
    case Folder: i = themed(QStringLiteral("folder")); break;
    case File: i = themed(QStringLiteral("text-x-generic")); break;
    }
    return i.isNull() ? QFileIconProvider::icon(type) : i;
}

QIcon RoxIconProvider::specialFolderIcon(const QString &path) const
{
    const QString name = m_specialFolders.value(QDir::cleanPath(path));
    return name.isEmpty() ? QIcon() : themed(name);
}

QMimeType RoxIconProvider::mimeTypeFor(const QFileInfo &info)
{
    static const QMimeDatabase db;
    if (info.isDir())
        return db.mimeTypeForName(QStringLiteral("inode/directory"));
    if (!info.isFile()) {
        // FIFOs, sockets and devices: never read their contents.
        return db.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    }
    QMimeType t = db.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    if (t.isDefault() && info.size() > 0)
        t = db.mimeTypeForFile(info, QMimeDatabase::MatchContent);
    return t;
}

QIcon RoxIconProvider::iconForMime(const QMimeType &mime)
{
    if (!mime.isValid())
        return QIcon();

    {
        QMutexLocker lock(&g_cacheMutex);
        const auto it = g_mimeIconCache.constFind(mime.name());
        if (it != g_mimeIconCache.constEnd())
            return it.value();
    }

    QIcon result = userMimeIcon(mime.name());
    if (result.isNull()) result = themed(mime.iconName());
    if (result.isNull()) result = bundledMimeIcon(mime.iconName());
    if (result.isNull()) result = themed(mime.genericIconName());
    if (result.isNull()) {
        static const QMimeDatabase db;
        for (const QString &parentName : mime.allAncestors()) {
            const QMimeType parent = db.mimeTypeForName(parentName);
            result = themed(parent.iconName());
            if (result.isNull()) result = themed(parent.genericIconName());
            if (!result.isNull()) break;
        }
    }
    if (result.isNull()) {
        if (mime.name().startsWith(QLatin1String("text/")))
            result = themed(QStringLiteral("text-x-generic"));
        else
            result = themed(QStringLiteral("application-x-generic"));
    }

    QMutexLocker lock(&g_cacheMutex);
    g_mimeIconCache.insert(mime.name(), result);
    return result;
}

QIcon RoxIconProvider::icon(const QFileInfo &info) const
{
    if (info.isDir()) {
        const QString dirIcon = info.absoluteFilePath() + QStringLiteral("/.DirIcon");
        if (QFileInfo(dirIcon).isFile())
            return QIcon(dirIcon);
        QIcon special = specialFolderIcon(info.absoluteFilePath());
        if (!special.isNull())
            return special;
        if (info.isRoot()) {
            const QIcon drive = themed(QStringLiteral("drive-harddisk"));
            if (!drive.isNull())
                return drive;
        }
        const QIcon folder = themed(info.isSymLink() ? QStringLiteral("folder-link")
                                                     : QStringLiteral("folder"));
        if (!folder.isNull())
            return folder;
        const QIcon plain = themed(QStringLiteral("folder"));
        return plain.isNull() ? QFileIconProvider::icon(info) : plain;
    }

    const QIcon byMime = iconForMime(mimeTypeFor(info));
    return byMime.isNull() ? QFileIconProvider::icon(info) : byMime;
}

QString RoxIconProvider::type(const QFileInfo &info) const
{
    const QMimeType t = mimeTypeFor(info);
    if (t.isValid() && !t.comment().isEmpty())
        return t.comment();
    return QFileIconProvider::type(info);
}
