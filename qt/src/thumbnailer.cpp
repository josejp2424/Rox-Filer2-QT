#include "thumbnailer.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeDatabase>
#include <QPainter>
#include <QPixmap>
#include <QRunnable>
#include <QStandardPaths>
#include <QThread>

#ifdef ROX_QT_HAVE_QTSVG
#include <QSvgRenderer>
#endif

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

QString cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/thumbnails/normal");
}

bool isSvgMime(const QString &name)
{
    return name == QLatin1String("image/svg+xml") ||
           name == QLatin1String("image/svg+xml-compressed");
}

QString mimeByExtension(const QString &path)
{
    static const QMimeDatabase db;
    return db.mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
}

QImage readCachedThumbnail(const QString &cachePath, qint64 mtime)
{
    if (!QFileInfo::exists(cachePath))
        return QImage();
    QImageReader reader(cachePath, "png");
    // Thumb::MTime lives in a tEXt chunk; read() makes it available via text().
    QImage img = reader.read();
    if (img.isNull())
        return QImage();
    if (img.text(QStringLiteral("Thumb::MTime")).toLongLong() != mtime)
        return QImage();
    return img;
}

bool saveThumbnail(const QString &path, qint64 mtime, const QImage &thumb, const QSize &original)
{
    const QString dir = cacheDir();
    if (!QDir().mkpath(dir))
        return false;
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QImage img = thumb;
    img.setText(QStringLiteral("Thumb::URI"), Thumbnailer::fileUri(path));
    img.setText(QStringLiteral("Thumb::MTime"), QString::number(mtime));
    img.setText(QStringLiteral("Thumb::Size"), QString::number(QFileInfo(path).size()));
    if (original.isValid()) {
        img.setText(QStringLiteral("Thumb::Image::Width"), QString::number(original.width()));
        img.setText(QStringLiteral("Thumb::Image::Height"), QString::number(original.height()));
    }
    img.setText(QStringLiteral("Software"), QStringLiteral("ROX-Filer2-QT"));

    const QString final = Thumbnailer::cachePathFor(path);
    const QString tmp = final + QStringLiteral(".rox-qt-%1-%2")
                                    .arg(::getpid())
                                    .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    QImageWriter writer(tmp, "png");
    if (!writer.write(img)) {
        QFile::remove(tmp);
        return false;
    }
    QFile::setPermissions(tmp, QFile::ReadOwner | QFile::WriteOwner);
    // rename(2) is atomic and replaces an older thumbnail.
    if (std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(final).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

QImage produceThumbnail(const QString &path, qint64 mtime)
{
    const QString mime = mimeByExtension(path);
    const QString cached = Thumbnailer::cachePathFor(path);

    // Files that already live in the thumbnail cache are tiny: show them as-is.
    if (QFileInfo(path).absolutePath() == QFileInfo(cacheDir()).absoluteFilePath())
        return Thumbnailer::loadImageScaled(path, QSize(Thumbnailer::ThumbSize, Thumbnailer::ThumbSize));

    QImage img = readCachedThumbnail(cached, mtime);
    if (!img.isNull())
        return img;

    // Video thumbnails are only read from the shared cache (the GTK build can
    // create them with ffmpeg); this frontend does not generate them.
    if (mime.startsWith(QLatin1String("video/")))
        return QImage();

    const QFileInfo fi(path);
    if (!fi.isFile() || fi.size() <= 0 || fi.size() > qint64(256) * 1024 * 1024)
        return QImage();

    QSize original;
    img = Thumbnailer::loadImageScaled(path, QSize(Thumbnailer::ThumbSize, Thumbnailer::ThumbSize), &original);
    if (img.isNull())
        return QImage();

    // Small images are cheap to decode; do not fill the cache with copies.
    if (original.isValid() && original.width() <= Thumbnailer::ThumbSize &&
        original.height() <= Thumbnailer::ThumbSize)
        return img;

    saveThumbnail(path, mtime, img, original);
    return img;
}

} // namespace

Thumbnailer *Thumbnailer::instance()
{
    static Thumbnailer *self = nullptr;
    if (!self)
        self = new Thumbnailer(qApp);
    return self;
}

Thumbnailer::Thumbnailer(QObject *parent)
    : QObject(parent)
{
    m_cache.setMaxCost(96 * 1024); // KiB
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        m_pool.clear();
        m_pool.waitForDone();
    });
}

QString Thumbnailer::fileUri(const QString &path)
{
    // Byte-for-byte equivalent of GLib's g_filename_to_uri(), which the GTK
    // build hashes. Unreserved: A-Z a-z 0-9 ! $ & ' ( ) * + , - . / : = @ _ ~
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray bytes = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    QByteArray out("file://");
    for (const char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        std::strchr("!$&'()*+,-./:=@_~", c) != nullptr;
        if (ok && c != 0) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return QString::fromLatin1(out);
}

QString Thumbnailer::cachePathFor(const QString &path)
{
    const QByteArray md5 = QCryptographicHash::hash(fileUri(path).toLatin1(),
                                                    QCryptographicHash::Md5).toHex();
    return cacheDir() + QLatin1Char('/') + QString::fromLatin1(md5) + QStringLiteral(".png");
}

bool Thumbnailer::canThumbnail(const QString &path)
{
    static QHash<QString, bool> bySuffix;
    static QSet<QString> readable;
    if (readable.isEmpty()) {
        for (const QByteArray &m : QImageReader::supportedMimeTypes())
            readable.insert(QString::fromLatin1(m));
        readable.insert(QStringLiteral("image/svg+xml"));
        readable.insert(QStringLiteral("image/svg+xml-compressed"));
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return false;
    const auto it = bySuffix.constFind(suffix);
    if (it != bySuffix.constEnd())
        return it.value();
    const QString mime = mimeByExtension(path);
    const bool ok = readable.contains(mime) || mime.startsWith(QLatin1String("video/"));
    bySuffix.insert(suffix, ok);
    return ok;
}

QImage Thumbnailer::loadImageScaled(const QString &path, const QSize &bound, QSize *originalSize)
{
    if (originalSize)
        *originalSize = QSize();
    const QString mime = mimeByExtension(path);

#ifdef ROX_QT_HAVE_QTSVG
    if (isSvgMime(mime)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid())
            return QImage();
        QSize size = renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = bound;
        if (originalSize)
            *originalSize = size;
        const QSize target = size.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        QImage img(target, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter painter(&img);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter);
        painter.end();
        return img;
    }
#else
    Q_UNUSED(isSvgMime);
#endif

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (originalSize)
        *originalSize = size;
    if (size.isValid() && (size.width() > bound.width() || size.height() > bound.height()))
        reader.setScaledSize(size.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
    QImage img = reader.read();
    if (img.isNull())
        return img;
    if (img.width() > bound.width() || img.height() > bound.height())
        img = img.scaled(bound, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

QIcon Thumbnailer::thumbnail(const QString &path, qint64 mtimeSecs)
{
    if (const Entry *e = m_cache.object(path)) {
        if (e->mtime == mtimeSecs)
            return e->icon;
    }
    if (m_failed.value(path, -1) == mtimeSecs)
        return QIcon();
    if (m_pending.value(path, -1) == mtimeSecs)
        return QIcon();
    if (!canThumbnail(path))
        return QIcon();

    m_pending.insert(path, mtimeSecs);
    m_pool.start(QRunnable::create([this, path, mtimeSecs] {
        const QImage img = produceThumbnail(path, mtimeSecs);
        QMetaObject::invokeMethod(this, [this, path, mtimeSecs, img] {
            deliver(path, mtimeSecs, img);
        }, Qt::QueuedConnection);
    }));
    return QIcon();
}

void Thumbnailer::deliver(const QString &path, qint64 mtime, const QImage &image)
{
    if (m_pending.value(path, -1) == mtime)
        m_pending.remove(path);
    if (image.isNull()) {
        m_failed.insert(path, mtime);
        return;
    }
    auto *entry = new Entry;
    entry->mtime = mtime;
    entry->icon = QIcon(QPixmap::fromImage(image));
    const int cost = qMax(1, int(qint64(image.width()) * image.height() * 4 / 1024));
    m_cache.insert(path, entry, cost);
    emit thumbnailReady(path);
}
