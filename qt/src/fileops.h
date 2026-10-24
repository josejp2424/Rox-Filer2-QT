#pragma once

/*
 * ROX-Filer2-QT - file operations used by the classic context menu.
 *
 * Long operations (copy, move, delete, chmod, mount) run as external
 * processes through FileJob so the window never blocks, mirroring the GTK
 * build which runs action.c in a child process.
 */

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QProgressDialog;
class QTimer;
class TransferProgress;
class QWidget;
class QMimeData;

/* Runs a list of argv commands sequentially with a cancellable progress
 * dialog. Deletes itself when finished. */
class FileJob final : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(bool ok, const QString &errors)>;

    static FileJob *run(QWidget *parent, const QString &title,
                        const QList<QStringList> &commands, Done done,
                        const QString &workingDir = QString());

    // Copy/move with real byte progress: the bar follows what is confirmed on
    // the destination device, and the job ends only after it has been flushed.
    // The job takes ownership of an unstarted tracker.
    static FileJob *runTracked(QWidget *parent, const QString &title,
                               const QList<QStringList> &commands,
                               TransferProgress *tracker, Done done);

private:
    FileJob(QWidget *parent, const QString &title, const QList<QStringList> &commands,
            Done done, const QString &workingDir);
    void attachTracker(TransferProgress *tracker);
    void updateBytes();
    void next();
    void finish();

    QPointer<QWidget> m_parent;
    QList<QStringList> m_commands;
    Done m_done;
    QString m_workingDir;
    QString m_errors;
    QProcess *m_process = nullptr;
    QProgressDialog *m_progress = nullptr;
    int m_index = 0;
    bool m_cancelled = false;
    bool m_finished = false;

    QPointer<TransferProgress> m_tracker;
    QTimer *m_ticker = nullptr;
    QString m_labelBase;
    QElapsedTimer m_clock;
    bool m_commandsStarted = false;
    bool m_flushRequested = false;
};

class FileTransferEvents final : public QObject
{
    Q_OBJECT
public:
    explicit FileTransferEvents(QObject *parent = nullptr) : QObject(parent) {}
signals:
    void destinationsCreated(const QStringList &paths);
};

namespace FileOps {
FileTransferEvents *events();

/* Clipboard: text/uri-list + x-special/gnome-copied-files (GTK Rox-Filer2,
 * Caja, Thunar) + application/x-kde-cutselection (Dolphin) + text/plain. */
void setClipboard(const QStringList &paths, bool cut);
bool clipboardHasFiles();
QStringList pathsFromMime(const QMimeData *mime);
QStringList clipboardPaths(bool *cut);

QString uniqueName(const QString &dir, const QString &leaf);
bool isInside(const QString &path, const QString &dir);

// GTK savebox-style absolute target path.  A target ending in '/' or an
// existing directory places the source inside that directory.  Copying onto
// the exact same source picks a safe unique name instead of overwriting it.
QString resolveSingleDestination(const QString &source, const QString &typed, bool move);

void transfer(QWidget *parent, const QStringList &sources, const QString &destDir,
              bool move, std::function<void(bool)> done,
              const QString &destinationName = QString());
void deletePermanently(QWidget *parent, const QStringList &paths, std::function<void()> done);
QStringList moveToTrash(const QStringList &paths); // returns failures

/* Trash browsing / restore (Freedesktop trash spec). */
bool isTrashFilesDir(const QString &dir);
QString trashOriginalPath(const QString &trashedPath);
void restoreFromTrash(QWidget *parent, const QStringList &paths, std::function<void()> done);

bool createLink(QWidget *parent, const QString &target, bool relative);
bool createLinksIn(QWidget *parent, const QStringList &sources, const QString &destDir, bool relative);
bool duplicate(QWidget *parent, const QString &path, std::function<void()> done);

/* Terminal helpers. */
QStringList runInTerminalArgs(const QString &path); // empty: not runnable
bool openTerminalIn(const QString &dir);

/* Templates for New > ... (same locations as the GTK build). */
QStringList templateFiles();
QString userTemplatesDir();

bool canCopyToBackgrounds(const QString &path);
void copyToBackgrounds(QWidget *parent, const QString &path);

QString humanSize(qint64 size);
QString permissionString(const QString &path);

} // namespace FileOps

/* Disk images: see imagemounter.h (port of the GTK3 image_mounter.c). */
#include "imagemounter.h"
