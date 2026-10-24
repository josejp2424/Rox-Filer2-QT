#pragma once

#include <QList>
#include <QString>

// Mounted disk images (loop-backed ISO, SFS, SquashFS, IMG). This is a
// read-only discovery API; it never creates/detaches loop devices.
namespace ImageVolumes {
struct Volume {
    QString device;      // /dev/loopN or /dev/loopNpN
    QString imagePath;   // underlying file, if exposed by sysfs
    QString mountPoint;
    QString fsType;
    bool protectedMount = false; // do NOT offer unmount for OS layer mounts
};

// Paths can be overridden by tests; production uses the kernel's proc/sysfs.
QList<Volume> mounted(const QString &mountinfo = QStringLiteral("/proc/self/mountinfo"),
                     const QString &blockSysfs = QStringLiteral("/sys/class/block"));
QString signature(const QList<Volume> &volumes);
}
