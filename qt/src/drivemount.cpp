#include "drivemount.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>
#include <memory>
#include <utility>
#include <unistd.h>

namespace {
QString tr(const char *text) { return QCoreApplication::translate("SonicDriveMount", text); }
constexpr const char *kHelper = "/usr/lib/rox-filer2-qt/rox-qt-mount-helper";

struct Task {
    QPointer<QObject> owner;
    QPointer<QWidget> window;
    SonicDriveMount::Action action;
    QString device;
    SonicDriveMount::Completion completed;
};

// UDisks2 object names encode each non-alphanumeric UTF-8 byte as _xx.
// Resolve /dev/disk/by-uuid and /dev/mapper aliases to their real block node.
QString udisksObjectPath(const QString &device)
{
    const QString real = QFileInfo(device).canonicalFilePath();
    if (!real.startsWith(QStringLiteral("/dev/"))) return {};
    const QByteArray bytes = QFileInfo(real).fileName().toUtf8();
    if (bytes.isEmpty()) return {};
    QString encoded;
    for (const unsigned char c : bytes) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            encoded.append(QChar(c));
        else
            encoded += QStringLiteral("_%1").arg(uint(c), 2, 16, QLatin1Char('0'));
    }
    return QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + encoded;
}

void runPkexecHelper(const std::shared_ptr<Task> &task)
{
    // pkexec's own authentication dialog is the user's consent; privileges are
    // restricted to the packaged mount helper.
    const QString helper = QString::fromLatin1(kHelper);
    auto *proc = new QProcess(task->owner);
    QObject::connect(proc, &QProcess::finished, task->owner,
        [task, proc](int code, QProcess::ExitStatus status) {
            const QString error = QString::fromUtf8(proc->readAllStandardError()).trimmed();
            const QString output = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
            proc->deleteLater();
            if (!task->owner) return;
            // 126/127: pkexec authentication dismissed or not authorized.
            const bool success = status == QProcess::NormalExit && code == 0;
            if (!success && code != 126 && code != 127)
                QMessageBox::warning(task->window, tr("Storage operation"),
                    error.isEmpty() ? tr("The storage operation failed.") : error);
            task->completed(success, success ? output.section(QLatin1Char('\n'), -1) : QString());
        });
    QObject::connect(proc, &QProcess::errorOccurred, task->owner,
        [task, proc](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            const QString reason = proc->errorString();
            proc->deleteLater();
            if (!task->owner) return;
            QMessageBox::warning(task->window, tr("Storage operation"), reason);
            task->completed(false, {});
        });
    const QStringList args = {helper,
        task->action == SonicDriveMount::Action::Mount
            ? QStringLiteral("mount-device") : QStringLiteral("unmount-device"),
        task->device};
    proc->start(QStringLiteral("pkexec"), args);
}

// udisks2 could not do it as this user.  When the reason is authorization (or
// udisks2 is not reachable at all), go straight to pkexec so its password
// dialog appears, with no intermediate warning.  Any other error (busy, wrong
// filesystem, ...) would fail as root too: show it.
void failOrRetry(const std::shared_ptr<Task> &task, const QString &message, bool escalate)
{
    if (!task->owner) return;
    if (escalate) {
        if (!QFileInfo(QString::fromLatin1(kHelper)).isExecutable()) {
            QMessageBox::warning(task->window, tr("Storage operation"),
                tr("The Sonic mounting helper is missing. Please reinstall ROX-Filer2-QT."));
            task->completed(false, {});
            return;
        }
        if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
            QMessageBox::warning(task->window, tr("Storage operation"),
                tr("pkexec is not installed. Cannot authorize the fallback operation."));
            task->completed(false, {});
            return;
        }
        runPkexecHelper(task);
        return;
    }
    QMessageBox::warning(task->window, tr("Storage operation"), message);
    task->completed(false, {});
}

void launchRootHelper(const std::shared_ptr<Task> &task)
{
    if (!task->owner) return;
    const QString helper = QString::fromLatin1(kHelper);
    if (!QFileInfo(helper).isExecutable()) {
        QMessageBox::warning(task->window, tr("Mount"),
            tr("The Sonic mounting helper is missing. Please reinstall ROX-Filer2-QT."));
        task->completed(false, {});
        return;
    }
    auto *proc = new QProcess(task->owner);
    QObject::connect(proc, &QProcess::finished, task->owner,
        [task, proc](int code, QProcess::ExitStatus status) {
            const QString error = QString::fromUtf8(proc->readAllStandardError()).trimmed();
            const QString output = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
            proc->deleteLater();
            if (!task->owner) return;
            const bool success = status == QProcess::NormalExit && code == 0;
            if (!success) QMessageBox::warning(task->window, tr("Storage operation"),
                error.isEmpty() ? tr("The storage operation failed.") : error);
            task->completed(success, success ? output.section(QLatin1Char('\n'), -1) : QString());
        });
    QObject::connect(proc, &QProcess::errorOccurred, task->owner,
        [task, proc](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            const QString reason = proc->errorString();
            proc->deleteLater();
            if (!task->owner) return;
            QMessageBox::warning(task->window, tr("Storage operation"), reason);
            task->completed(false, {});
        });
    proc->start(helper, {task->action == SonicDriveMount::Action::Mount
        ? QStringLiteral("mount-device") : QStringLiteral("unmount-device"), task->device});
}

void attempt(const std::shared_ptr<Task> &task)
{
    if (!task->owner) return;
    if (geteuid() == 0) {
        launchRootHelper(task);
        return;
    }
    // Unlike launching udisksctl in a GUI process with no /dev/tty, a direct
    // D-Bus call never attempts to create a textual Polkit authentication agent.
    // UDisks2's normal session policy still governs this operation.
    const QString path = udisksObjectPath(task->device);
    if (path.isEmpty()) {
        failOrRetry(task, tr("The storage operation failed."), true);
        return;
    }
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        failOrRetry(task, tr("Could not connect to the system D-Bus. Check dbus and udisks2."), true);
        return;
    }
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.UDisks2"), path,
        QStringLiteral("org.freedesktop.UDisks2.Filesystem"),
        task->action == SonicDriveMount::Action::Mount ? QStringLiteral("Mount") : QStringLiteral("Unmount"));
    message << QVariantMap{}; // a{sv}; Polkit may use the active graphical agent
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 120000), task->owner);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, task->owner,
        [task](QDBusPendingCallWatcher *watcher) {
            const QDBusMessage reply = watcher->reply();
            watcher->deleteLater();
            if (!task->owner) return;
            if (reply.type() == QDBusMessage::ErrorMessage) {
                const QString error = reply.errorMessage();
                const bool escalate = SonicDriveMount::isAuthorizationError(reply.errorName(), error) ||
                                      SonicDriveMount::isServiceUnavailable(reply.errorName());
                failOrRetry(task, error.isEmpty() ? tr("The storage operation failed.") : error, escalate);
                return;
            }
            const QString mountPath = task->action == SonicDriveMount::Action::Mount
                ? reply.arguments().value(0).toString() : QString();
            task->completed(true, mountPath);
        });
}
} // namespace

void SonicDriveMount::run(QObject *owner, QWidget *dialogParent, Action action,
                          const QString &device, Completion completed)
{
    if (!owner || !device.startsWith(QStringLiteral("/dev/"))) {
        if (completed) completed(false, {});
        return;
    }
    auto task = std::make_shared<Task>();
    task->owner = owner;
    task->window = dialogParent;
    task->action = action;
    task->device = device;
    task->completed = std::move(completed);
    attempt(task);
}

bool SonicDriveMount::isAuthorizationError(const QString &errorName, const QString &message)
{
    // The user dismissed a polkit dialog: respect it, do not ask again.
    if (errorName.endsWith(QStringLiteral("NotAuthorizedDismissed")))
        return false;
    if (errorName.contains(QStringLiteral("NotAuthorized")) ||
        errorName == QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"))
        return true;
    return errorName.isEmpty() && message.contains(QStringLiteral("Not authorized"), Qt::CaseInsensitive);
}

bool SonicDriveMount::isServiceUnavailable(const QString &errorName)
{
    return errorName == QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown") ||
           errorName == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner") ||
           errorName == QStringLiteral("org.freedesktop.DBus.Error.Disconnected") ||
           errorName == QStringLiteral("org.freedesktop.DBus.Error.UnknownObject");
}
