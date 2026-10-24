#pragma once

/*
 * ROX-Filer2-QT image mounter — a direct port of the GTK3 Rox-Filer2
 * image_mounter.c (same rules, same state files, same messages).
 *
 *  - ISO and SquashFS/SFS images are ALWAYS mounted as the whole loop device,
 *    even when a hybrid ISO also exposes a partition table.  Only raw .img
 *    files descend into loopNpM: no partition table -> whole device, one
 *    partition -> that one, several -> the user chooses.
 *  - Root sessions use losetup + mount -o ro,nosuid,nodev under
 *    /media/rox-filer2-images/<image>-<device>.  Normal users use udisksctl.
 *  - Every mount made by ROX is recorded in a private state file
 *      $XDG_RUNTIME_DIR/rox-filer2/image-mounter/<sha256(path)>.ini
 *    (or $TMPDIR/rox-filer2-<uid>/... without XDG_RUNTIME_DIR), in the same
 *    GKeyFile format the GTK3 build writes, so both builds see each other's
 *    mounts.  The state files are the only authority: the system's loop
 *    devices (Puppy/Essora SFS layers) are never enumerated or touched.
 *  - Privileged programs are run by absolute path from the system
 *    directories only, never through $PATH.
 */

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace ImageMounter {

enum class Method { None = 0, Root = 1, Udisks = 2 };

struct Managed
{
    QString image;
    QString loopdev;
    QString blockdev;
    QString mountpoint;
    Method method = Method::None;
};

// ISO, SFS, SquashFS/SQFS/SQSH and raw IMG (by suffix or MIME type).
bool canHandle(const QString &path);

// Mount point of an active ROX-managed mount of this image, or empty.  A
// state file whose mount is gone is removed, exactly like the GTK3 build.
QString mountedPoint(const QString &image);
QStringList mountPoints(const QString &image); // {mountedPoint} or {}

// Active mounts created by ROX (state files only), sorted by image path.
QList<Managed> managedMounts();
QString signature(const QList<Managed> &mounts);

// Read-only mount, asynchronous, with a progress dialog.  `done` receives the
// mount point on success (also when the image was already mounted).  Errors
// are reported to the user here.
void mount(QWidget *parent, const QString &image, std::function<void(const QString &)> done);

// Asynchronous unmount of a ROX-managed image: umount, detach the loop device,
// remove the empty mount directory (root) and the state file.
void unmount(QWidget *parent, const QString &image, std::function<void(bool ok)> done);

namespace detail {
// Exposed for tests.
constexpr const char *kRootDir = "/media/rox-filer2-images";
QString stateDirectory();
QString statePathForImage(const QString &image);
QString canonicalImagePath(const QString &path);
bool saveState(const Managed &state);
bool loadStateFile(const QString &statePath, Managed *state);
QString keyFileEscape(const QString &value);
QString keyFileUnescape(const QString &value);
bool mountpointIsActive(const QString &mountpoint);
QString findMountpointForDevice(const QString &device);
bool mountpointIsOurs(const QString &mountpoint);
bool blockdevBelongsToLoop(const QString &loopdev, const QString &blockdev);
bool isPartitionableRaw(const QString &image);
QString safeMountName(const QString &image, const QString &blockdev);
QString helperPath(const QString &name);
QString extractLoopDevice(const QString &text);
QString unescapeMountField(const QString &field);
bool unmountStateSync(const Managed &state, QString *error);
} // namespace detail

} // namespace ImageMounter
