#pragma once

#include <QString>
#include <functional>
class QObject;
class QWidget;

namespace SonicDriveMount {
enum class Action { Mount, Unmount };
using Completion = std::function<void(bool, const QString &)>;
// Use udisks2 as a session user first. Never silently elevate the file manager.
// When udisks2 refuses for lack of authorization (or is not reachable), the
// operation goes straight to the narrowly scoped pkexec helper: pkexec's own
// password dialog is the user's consent.  Other errors are only reported.
void run(QObject *owner, QWidget *dialogParent, Action action,
         const QString &device, Completion completed);

// Exposed for tests.
bool isAuthorizationError(const QString &errorName, const QString &message);
bool isServiceUnavailable(const QString &errorName);
}
