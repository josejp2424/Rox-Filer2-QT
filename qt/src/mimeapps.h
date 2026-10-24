#pragma once

/*
 * ROX-Filer2-QT - XDG application database.
 *
 * The GTK frontend gets this from GIO (g_app_info_*). The Qt frontend has no
 * GIO, so this module reads the same Freedesktop data directly:
 *   - <data dir>/applications/NAME.desktop  (MimeType=)
 *   - mimeapps.list in XDG config/data dirs (Default/Added/Removed)
 *   - <data dir>/applications/mimeinfo.cache (fast path, may be stale)
 * Custom ROX file actions (~/.local/share/rox-filer/actions) are also read so
 * actions created by the GTK build appear in the Qt build and vice versa.
 */

#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>

struct DesktopApp
{
    QString id;          // desktop-file ID, e.g. org.kde.kate.desktop
    QString path;        // absolute .desktop path
    QString name;        // localised Name
    QString icon;
    QString exec;
    QString tryExec;
    QStringList mimeTypes;
    bool terminal = false;
    bool noDisplay = false;
    bool hidden = false;

    // X-ROX-Filer Action fields (only for custom file actions)
    QString actionMimeTypes;
    QString actionTargets;
    QString actionSelection;
    bool actionEnabled = true;

    bool isValid() const { return !exec.isEmpty(); }
    QIcon qicon() const;
};

namespace MimeApps {

QString mimeTypeForPath(const QString &path);
QString commonMimeType(const QStringList &paths);

DesktopApp loadDesktopFile(const QString &path, const QString &id = QString());

QList<DesktopApp> appsForMime(const QString &mime);
DesktopApp defaultAppForMime(const QString &mime);
QList<DesktopApp> allApps();
bool setDefaultApp(const QString &mime, const QString &desktopId, QString *error);
DesktopApp createCustomApp(const QString &command, const QString &mime, QString *error);

bool launch(const DesktopApp &app, const QStringList &paths,
            const QString &workingDir, QString *error);

QString findTerminal();
QStringList wrapInTerminal(const QStringList &argv);

QString customActionsDir();
QList<DesktopApp> customActionsFor(const QStringList &paths);

void invalidateCache();

} // namespace MimeApps
