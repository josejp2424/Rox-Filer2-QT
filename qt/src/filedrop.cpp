#include "filedrop.h"
#include "fileops.h"

#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace FileDrop {

QString logPath()
{
    // XDG State Directory, never a system log requiring elevated privileges.
    const QString custom = qEnvironmentVariable("XDG_STATE_HOME");
    const QString base = QDir::isAbsolutePath(custom) ? custom
        : QDir::homePath() + QStringLiteral("/.local/state");
    return QDir(base).filePath(QStringLiteral("rox-filer2-qt/dragdrop.log"));
}

void log(const QString &entry)
{
    static QMutex lock;
    const QMutexLocker guard(&lock);
    const QString path = logPath();
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return;
    // Avoid unlimited growth when the mouse generates many drag-move events.
    QFile file(path);
    const bool rotate = info.exists() && info.size() >= 1024 * 1024;
    if (!file.open(QIODevice::WriteOnly | (rotate ? QIODevice::Truncate : QIODevice::Append))) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (rotate) file.write("--- ROX-Filer2-QT: DnD log rotated (1 MiB) ---\n");
    QString cleaned = entry;
    cleaned.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cleaned.replace(QLatin1Char('\r'), QLatin1Char(' '));
    const QString record = QStringLiteral("[%1] pid=%2 %3\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
        .arg(QCoreApplication::applicationPid())
        .arg(cleaned);
    file.write(record.toUtf8());
}

void traceMove(QWidget *receiver, const QString &destination, bool accepted)
{
    static qint64 lastLogMs = 0;
    static QWidget *lastReceiver = nullptr;
    static QString lastDestination;
    static bool lastAccepted = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (receiver == lastReceiver && destination == lastDestination &&
        accepted == lastAccepted && now - lastLogMs < 1500) return;
    lastReceiver = receiver;
    lastDestination = destination;
    lastAccepted = accepted;
    lastLogMs = now;
    log(QStringLiteral("drag-move receiver=%1 dest='%2' accepted=%3")
        .arg(qulonglong(reinterpret_cast<quintptr>(receiver)), 0, 16)
        .arg(destination, accepted ? QStringLiteral("yes") : QStringLiteral("no")));
}

bool accepts(const QMimeData *mime)
{
    return !FileOps::pathsFromMime(mime).isEmpty();
}

void startDrag(QWidget *source, const QStringList &paths)
{
    if (paths.isEmpty()) {
        log(QStringLiteral("drag-start rejected: selection empty"));
        return;
    }
    log(QStringLiteral("drag-start source='%1' files=%2 first='%3'")
        .arg(source ? source->objectName() : QStringLiteral("<null>"))
        .arg(paths.size()).arg(paths.first()));
    auto *drag = new QDrag(source);
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &path : paths)
        urls << QUrl::fromLocalFile(path);
    mime->setUrls(urls); // interoperable with ROX, Dolphin, PCManFM and desktop
    drag->setMimeData(mime);
    const Qt::DropAction result = drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction,
                                              Qt::CopyAction);
    log(QStringLiteral("drag-end QtResult=%1 (%2)").arg(int(result))
        .arg(result == Qt::IgnoreAction ? QStringLiteral("IGNORED: target declined or drop cancelled")
             : QStringLiteral("drop accepted")));
}

static void allowDrop(QDropEvent *event)
{
    if (!accepts(event->mimeData()) ||
        !event->possibleActions().testFlag(Qt::CopyAction)) {
        event->ignore();
        return;
    }
    // A file move is performed explicitly by FileOps. Advertise CopyAction
    // to prevent an external drag source from deleting a file *again*.
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void allow(QDragEnterEvent *event) { allowDrop(event); }
void allow(QDragMoveEvent *event) { allowDrop(event); }

void handle(QDropEvent *event, QWidget *parent, const QString &destination)
{
    const QStringList paths = FileOps::pathsFromMime(event->mimeData());
    log(QStringLiteral("drop-received sources=%1 dest='%2' actions=%3 modifiers=%4")
        .arg(paths.size()).arg(destination).arg(int(event->possibleActions())).arg(int(event->modifiers())));
    if (paths.isEmpty() || !QFileInfo(destination).isDir() || FileOps::isTrashFilesDir(destination)) {
        log(QStringLiteral("drop-REJECTED: invalid MIME, missing destination or trash directory"));
        event->ignore();
        return;
    }
    if (!event->possibleActions().testFlag(Qt::CopyAction)) {
        // Never claim a move to an outside application that may remove its
        // source after our own transfer already completed.
        log(QStringLiteral("drop-REJECTED: CopyAction not offered (avoid unintended source deletion)"));
        event->ignore();
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    const QPointer<QWidget> guard(parent);
    // Finish the DnD transaction before opening a popup or progress dialog.
    // In X11, doing this inside dropEvent can hang the initiating drag.
    QTimer::singleShot(0, parent, [guard, paths, destination, mods] {
        if (!guard) return;
        enum Operation { Cancel, Copy, Move, AbsoluteLink, RelativeLink } op = Cancel;
        if (mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier))
            op = RelativeLink;
        else if (mods.testFlag(Qt::ControlModifier))
            op = Copy;
        else if (mods.testFlag(Qt::ShiftModifier))
            op = Move;
        else if (mods.testFlag(Qt::AltModifier))
            op = AbsoluteLink;
        else {
            QMenu menu(guard);
            QAction *copy = menu.addAction(QCoreApplication::translate("FileDrop", "Copy here"));
            QAction *move = menu.addAction(QCoreApplication::translate("FileDrop", "Move here"));
            menu.addSeparator();
            QAction *absolute = menu.addAction(QCoreApplication::translate("FileDrop", "Link here (absolute)"));
            QAction *relative = menu.addAction(QCoreApplication::translate("FileDrop", "Link here (relative)"));
            menu.addSeparator();
            QAction *selected = menu.exec(QCursor::pos());
            if (selected == copy) op = Copy;
            else if (selected == move) op = Move;
            else if (selected == absolute) op = AbsoluteLink;
            else if (selected == relative) op = RelativeLink;
        }
        if (op == Cancel) {
            log(QStringLiteral("drop-operation cancelled by user"));
        } else if (op == Copy || op == Move) {
            log(QStringLiteral("drop-operation %1 dest='%2' count=%3")
                .arg(op == Move ? QStringLiteral("MOVE") : QStringLiteral("COPY"))
                .arg(destination).arg(paths.size()));
            FileOps::transfer(guard, paths, destination, op == Move,
                [destination, op](bool ok) {
                    log(QStringLiteral("drop-transfer result=%1 operation=%2 dest='%3'")
                        .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED"))
                        .arg(op == Move ? QStringLiteral("MOVE") : QStringLiteral("COPY"))
                        .arg(destination));
                });
        } else if (op == AbsoluteLink || op == RelativeLink) {
            const bool ok = FileOps::createLinksIn(guard, paths, destination, op == RelativeLink);
            log(QStringLiteral("drop-link result=%1 kind=%2 dest='%3'")
                .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED"))
                .arg(op == RelativeLink ? QStringLiteral("RELATIVE") : QStringLiteral("ABSOLUTE"))
                .arg(destination));
        }
    });
    // Always return CopyAction; the source must never perform a second delete.
    event->setDropAction(Qt::CopyAction);
    event->accept();
    log(QStringLiteral("drop-ACCEPTED by Qt, deferred chooser scheduled"));
}

} // namespace FileDrop
