#pragma once

/*
 * ROX-Filer2-QT - asynchronous image thumbnails.
 *
 * Uses the Freedesktop thumbnail cache (~/.cache/thumbnails/normal) with the
 * same file-URI hashing as GLib's g_filename_to_uri(), so thumbnails created
 * by the GTK build of Rox-Filer2 are reused here and vice versa.
 *
 * All disk I/O and decoding happens in a private QThreadPool. The GUI thread
 * only does a hash lookup in the in-memory cache; a miss queues a job and the
 * view is refreshed through thumbnailReady().
 */

#include <QCache>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QThreadPool>

class Thumbnailer final : public QObject
{
    Q_OBJECT
public:
    static Thumbnailer *instance();

    /* Returns the thumbnail if it is already in memory. Otherwise queues a
     * job (when the type is supported) and returns a null icon. */
    QIcon thumbnail(const QString &path, qint64 mtimeSecs);

    static bool canThumbnail(const QString &path);
    static QString fileUri(const QString &path);
    static QString cachePathFor(const QString &path);

    /* Synchronous helper for the preview pane (handles SVG as well). */
    static QImage loadImageScaled(const QString &path, const QSize &bound, QSize *originalSize = nullptr);

    static constexpr int ThumbSize = 128;

signals:
    void thumbnailReady(const QString &path);

private:
    explicit Thumbnailer(QObject *parent = nullptr);
    void deliver(const QString &path, qint64 mtime, const QImage &image);

    struct Entry {
        qint64 mtime = 0;
        QIcon icon;
    };

    QCache<QString, Entry> m_cache;
    QHash<QString, qint64> m_pending;
    QHash<QString, qint64> m_failed;
    QThreadPool m_pool;
};
