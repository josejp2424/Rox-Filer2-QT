// Image Mounter (port of GTK3 image_mounter.c).
//
// Part A always runs: GTK3-compatible state files, path safety, partition
// rule, udisks2 error classification.
// Part B runs only as root with losetup/mount/mksquashfs: it mounts and
// unmounts REAL images through the public API, including the mount ->
// unmount -> mount again cycle that failed in 1.0.4.

#include "drivemount.h"
#include "imagemounter.h"
#include "imagevolumes.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using namespace ImageMounter;

static QStringList dialogs; // texts of every QMessageBox ROX showed

static bool waitFor(const std::function<bool()> &cond, int ms = 20000)
{
    for (int i = 0; i < ms / 20; ++i) {
        if (cond()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return cond();
}

static QString writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }
    return path;
}

static int loopsBackedBy(const QString &image)
{
    int count = 0;
    const QString canonical = QFileInfo(image).canonicalFilePath();
    const QDir sys(QStringLiteral("/sys/block"));
    for (const QString &name : sys.entryList({QStringLiteral("loop*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(sys.filePath(name) + QStringLiteral("/loop/backing_file"));
        if (f.open(QIODevice::ReadOnly) && QString::fromLocal8Bit(f.readAll()).trimmed() == canonical)
            ++count;
    }
    return count;
}

static bool run(const QString &program, const QStringList &args)
{
    QProcess p;
    p.start(program, args);
    return p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

static QString mountAndWait(const QString &image)
{
    QString result;
    bool called = false;
    ImageMounter::mount(nullptr, image, [&](const QString &mp) { result = mp; called = true; });
    waitFor([&] { return called || !dialogs.isEmpty(); });
    waitFor([] { return QApplication::activeModalWidget() == nullptr; }, 2000);
    return result;
}

static bool unmountAndWait(const QString &image)
{
    bool ok = false, called = false;
    ImageMounter::unmount(nullptr, image, [&](bool r) { ok = r; called = true; });
    waitFor([&] { return called; });
    return ok;
}

static void partA(const QString &tmp)
{
    using namespace ImageMounter::detail;

    /* --- GKeyFile escaping, as g_key_file_set_string writes it --- */
    CHECK(keyFileEscape(QStringLiteral(" a\\b\nc\td")) == QStringLiteral("\\sa\\\\b\\nc\\td"));
    CHECK(keyFileUnescape(keyFileEscape(QStringLiteral(" x \\ y\n"))) == QStringLiteral(" x \\ y\n"));
    CHECK(keyFileEscape(QStringLiteral("a b")) == QStringLiteral("a b")); // only a LEADING space is escaped

    /* --- state directory and file, same layout as GTK3 --- */
    const QString dir = stateDirectory();
    CHECK(dir == tmp + QStringLiteral("/runtime/rox-filer2/image-mounter"));
    struct stat st {};
    CHECK(::stat(QFile::encodeName(dir).constData(), &st) == 0 && (st.st_mode & 0777) == 0700);

    const QString image = writeFile(tmp + QStringLiteral("/My Image.iso"), "x");
    const QByteArray expectedHash = QCryptographicHash::hash(QFile::encodeName(image), QCryptographicHash::Sha256).toHex();
    CHECK(statePathForImage(image) == dir + QLatin1Char('/') + QString::fromLatin1(expectedHash) + QStringLiteral(".ini"));
    // g_canonicalize_filename(): ./.. resolved, symlinks NOT followed.
    CHECK(canonicalImagePath(tmp + QStringLiteral("/sub/../My Image.iso")) == image);
    QFile::link(image, tmp + QStringLiteral("/link.iso"));
    CHECK(canonicalImagePath(tmp + QStringLiteral("/link.iso")) == tmp + QStringLiteral("/link.iso"));

    Managed state;
    state.image = image;
    state.loopdev = QStringLiteral("/dev/loop7");
    state.blockdev = QStringLiteral("/dev/loop7");
    state.mountpoint = QStringLiteral("/media/rox-filer2-images/My_Image.iso-loop7");
    state.method = Method::Root;
    CHECK(saveState(state));
    {
        QFile f(statePathForImage(image));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == QByteArray("[ImageMounter]\nimage=") + QFile::encodeName(image)
              + "\nloopdev=/dev/loop7\nblockdev=/dev/loop7\n"
                "mountpoint=/media/rox-filer2-images/My_Image.iso-loop7\nmethod=1\n");
        CHECK((QFileInfo(f.fileName()).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
    }
    Managed loaded;
    CHECK(loadStateFile(statePathForImage(image), &loaded));
    CHECK(loaded.image == image && loaded.loopdev == state.loopdev && loaded.blockdev == state.blockdev &&
          loaded.mountpoint == state.mountpoint && loaded.method == Method::Root);
    // A file exactly as GTK3 writes it, with escapes and spacing.
    const QString gtkFile = writeFile(tmp + QStringLiteral("/gtk.ini"),
        "# comment\n[Other]\nimage=/nope\n\n[ImageMounter]\nimage = /tmp/a\\\\b c.sfs\nloopdev=/dev/loop3\n"
        "blockdev=/dev/loop3p1\nmountpoint=\\s/media/x\nmethod=2\n");
    CHECK(loadStateFile(gtkFile, &loaded));
    CHECK(loaded.image == QStringLiteral("/tmp/a\\b c.sfs") && loaded.blockdev == QStringLiteral("/dev/loop3p1") &&
          loaded.mountpoint == QStringLiteral(" /media/x") && loaded.method == Method::Udisks);
    CHECK(!loadStateFile(writeFile(tmp + QStringLiteral("/bad.ini"), "[ImageMounter]\nimage=/a\nmethod=1\n"), &loaded));

    /* --- an inactive mount is forgotten, like image_mounter_is_mounted() --- */
    CHECK(mountedPoint(image).isEmpty());
    CHECK(!QFileInfo::exists(statePathForImage(image)));

    /* --- managed list: only valid, active state files; stale ones removed --- */
    state.mountpoint = QStringLiteral("/proc"); // certainly mounted
    state.loopdev = QStringLiteral("/dev/loop9");
    state.blockdev = QStringLiteral("/dev/loop9p2");
    CHECK(saveState(state));
    Managed bogus = state;
    bogus.image = writeFile(tmp + QStringLiteral("/other.sfs"), "y");
    bogus.blockdev = QStringLiteral("/dev/sda1"); // does not belong to the loop
    CHECK(saveState(bogus));
    const QList<Managed> mounts = managedMounts();
    CHECK(mounts.size() == 1 && mounts.first().image == image);
    CHECK(!QFileInfo::exists(statePathForImage(bogus.image)));
    // root state pointing outside /media/rox-filer2-images is never unmounted
    QString error;
    CHECK(!unmountStateSync(state, &error));
    CHECK(error.contains(QStringLiteral("Refusing")));
    CHECK(mountpointIsActive(QStringLiteral("/proc")));
    QFile::remove(statePathForImage(image));

    /* --- path rules --- */
    CHECK(mountpointIsOurs(QStringLiteral("/media/rox-filer2-images/a.iso-loop0")));
    CHECK(!mountpointIsOurs(QStringLiteral("/media/rox-filer2-images/")));
    CHECK(!mountpointIsOurs(QStringLiteral("/media/rox-filer2-images/../../etc")));
    CHECK(!mountpointIsOurs(QStringLiteral("/media/rox-filer2-images-evil/x")));
    CHECK(!mountpointIsOurs(QStringLiteral("/tmp/victima")));
    CHECK(blockdevBelongsToLoop(QStringLiteral("/dev/loop1"), QStringLiteral("/dev/loop1")));
    CHECK(blockdevBelongsToLoop(QStringLiteral("/dev/loop1"), QStringLiteral("/dev/loop1p12")));
    CHECK(!blockdevBelongsToLoop(QStringLiteral("/dev/loop1"), QStringLiteral("/dev/loop12")));
    CHECK(!blockdevBelongsToLoop(QStringLiteral("/dev/loop1"), QStringLiteral("/dev/loop1p")));
    CHECK(safeMountName(QStringLiteral("/x/Essora Eos 1.4.iso"), QStringLiteral("/dev/loop0"))
          == QStringLiteral("Essora_Eos_1.4.iso-loop0"));
    CHECK(safeMountName(QStringLiteral("/x/") + QString(100, QLatin1Char('a')) + QStringLiteral(".iso"),
                        QStringLiteral("/dev/loop3p1")) == QString(72, QLatin1Char('a')) + QStringLiteral("-loop3p1"));
    CHECK(safeMountName(QStringLiteral("/x/ñ.iso"), QStringLiteral("/dev/loop2")) == QStringLiteral("__.iso-loop2"));
    CHECK(extractLoopDevice(QStringLiteral("Mapped file /a b.iso as /dev/loop12.")) == QStringLiteral("/dev/loop12"));
    CHECK(extractLoopDevice(QStringLiteral("/dev/loopX nothing")).isEmpty());
    CHECK(helperPath(QStringLiteral("mount")).startsWith(QLatin1Char('/')));
    CHECK(helperPath(QStringLiteral("../bin/sh")).isEmpty());
    CHECK(helperPath(QStringLiteral("/bin/sh")).isEmpty());

    /* --- the partition rule of GTK3: only raw .img descends into loopNpM --- */
    CHECK(!isPartitionableRaw(QStringLiteral("/a/Essora.iso")));   // hybrid ISO -> whole device
    CHECK(!isPartitionableRaw(QStringLiteral("/a/puppy.SFS")));
    CHECK(isPartitionableRaw(QStringLiteral("/a/disk.IMG")));
    for (const char *name : {"a.iso", "b.sfs", "c.squashfs", "d.sqfs", "e.sqsh", "f.img"})
        CHECK(canHandle(writeFile(tmp + QLatin1Char('/') + QLatin1String(name), "z")));
    CHECK(!canHandle(writeFile(tmp + QStringLiteral("/notes.txt"), "z")));
    CHECK(!canHandle(tmp)); // a directory

    /* --- udisks2 errors that go straight to pkexec --- */
    CHECK(SonicDriveMount::isAuthorizationError(QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorized"), {}));
    CHECK(SonicDriveMount::isAuthorizationError(QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain"), {}));
    CHECK(!SonicDriveMount::isAuthorizationError(QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed"), {}));
    CHECK(SonicDriveMount::isAuthorizationError(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"), {}));
    CHECK(!SonicDriveMount::isAuthorizationError(QStringLiteral("org.freedesktop.UDisks2.Error.DeviceBusy"),
                                                 QStringLiteral("Not authorized")));
    CHECK(SonicDriveMount::isAuthorizationError({}, QStringLiteral("Not authorized to perform operation")));
    CHECK(SonicDriveMount::isServiceUnavailable(QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown")));
    CHECK(!SonicDriveMount::isServiceUnavailable(QStringLiteral("org.freedesktop.UDisks2.Error.Failed")));
}

static void partB(const QString &tmp)
{
    using namespace ImageMounter::detail;
    if (::geteuid() != 0 || helperPath(QStringLiteral("losetup")).isEmpty() ||
        helperPath(QStringLiteral("mksquashfs")).isEmpty() || helperPath(QStringLiteral("mkfs.ext4")).isEmpty()) {
        std::printf("imagemounter: real mount tests SKIPPED (need root, losetup, mksquashfs, mkfs.ext4)\n");
        return;
    }
    QDir(tmp).mkpath(QStringLiteral("content"));
    writeFile(tmp + QStringLiteral("/content/leeme.txt"), "hola essora\n");
    const QString sfs = tmp + QStringLiteral("/Essora Eos test.sfs");
    if (!run(helperPath(QStringLiteral("mksquashfs")), {tmp + QStringLiteral("/content"), sfs, QStringLiteral("-quiet"), QStringLiteral("-noappend")})) {
        std::printf("imagemounter: mksquashfs failed, real mount tests SKIPPED\n");
        return;
    }
    const QString expectedPrefix = QStringLiteral("/media/rox-filer2-images/Essora_Eos_test.sfs-loop");

    // 1) mount: whole loop device, ro,nosuid,nodev, state written, contents readable
    QString mp = mountAndWait(sfs);
    CHECK(dialogs.isEmpty());
    CHECK(mp.startsWith(expectedPrefix));
    CHECK(mountpointIsActive(mp));
    CHECK(QFile::exists(mp + QStringLiteral("/leeme.txt")));
    CHECK(mountedPoint(sfs) == mp);
    CHECK(managedMounts().size() == 1 && managedMounts().first().blockdev == managedMounts().first().loopdev);
    CHECK(loopsBackedBy(sfs) == 1);
    // the 1.0.4 /proc bug: the kernel view must now see it too
    bool seen = false;
    for (const auto &v : ImageVolumes::mounted())
        if (v.mountPoint == mp) seen = true;
    CHECK(seen);

    // 2) opening it again does not create a second loop or mount
    CHECK(mountAndWait(sfs) == mp);
    CHECK(loopsBackedBy(sfs) == 1);

    // 3) unmount: umount + loop detached + empty directory and state removed
    CHECK(unmountAndWait(sfs));
    CHECK(dialogs.isEmpty());
    CHECK(!mountpointIsActive(mp));
    CHECK(!QFileInfo::exists(mp));
    CHECK(mountedPoint(sfs).isEmpty() && managedMounts().isEmpty());
    CHECK(loopsBackedBy(sfs) == 0);

    // 4) THE 1.0.4 FAILURE: mount -> unmount -> mount again
    mp = mountAndWait(sfs);
    CHECK(dialogs.isEmpty());
    CHECK(mp.startsWith(expectedPrefix) && mountpointIsActive(mp));
    CHECK(unmountAndWait(sfs));
    CHECK(loopsBackedBy(sfs) == 0);

    // 5) a leftover (empty) directory with the same name does not break it
    QDir().mkpath(expectedPrefix + QStringLiteral("0"));
    mp = mountAndWait(sfs);
    CHECK(dialogs.isEmpty() && mountpointIsActive(mp));
    CHECK(unmountAndWait(sfs));
    QDir().rmdir(expectedPrefix + QStringLiteral("0"));

    // 6) ".iso" (hybrid or not) always mounts the whole loop device
    const QString iso = tmp + QStringLiteral("/fake-hybrid.iso");
    {
        QFile f(iso); f.open(QIODevice::WriteOnly); f.resize(8 * 1024 * 1024); f.close();
        run(helperPath(QStringLiteral("mkfs.ext4")), {QStringLiteral("-q"), QStringLiteral("-F"), iso});
    }
    mp = mountAndWait(iso);
    CHECK(dialogs.isEmpty() && !mp.isEmpty());
    CHECK(!managedMounts().isEmpty() && managedMounts().first().blockdev == managedMounts().first().loopdev);
    CHECK(unmountAndWait(iso));

    // 7) raw .img without a partition table -> whole device (blkid PTTYPE empty)
    const QString img = tmp + QStringLiteral("/plain.img");
    QFile::copy(iso, img);
    mp = mountAndWait(img);
    CHECK(dialogs.isEmpty() && !mp.isEmpty());
    CHECK(!managedMounts().isEmpty() && managedMounts().first().blockdev == managedMounts().first().loopdev);
    CHECK(unmountAndWait(img));

    // 8) a broken image: error shown, nothing left behind (no loop, dir, state)
    const QString broken = writeFile(tmp + QStringLiteral("/broken.sfs"), QByteArray(64 * 1024, '\x5a'));
    dialogs.clear();
    CHECK(mountAndWait(broken).isEmpty());
    CHECK(dialogs.size() == 1 && dialogs.first().contains(QStringLiteral("Unable to mount image")));
    waitFor([&] { return loopsBackedBy(broken) == 0; }, 5000);
    CHECK(loopsBackedBy(broken) == 0);
    CHECK(!QFileInfo::exists(QStringLiteral("/media/rox-filer2-images/broken.sfs-loop0")));
    CHECK(!QFileInfo::exists(statePathForImage(broken)));
    dialogs.clear();

    // 9) unmounting something not mounted by ROX: a clear message, no action
    CHECK(!unmountAndWait(sfs));
    CHECK(dialogs.size() == 1 && dialogs.first().contains(QStringLiteral("no longer available")));
    dialogs.clear();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir tmpDir(QDir::homePath() + QStringLiteral("/.rox-imagemounter-test-XXXXXX"));
    const QString tmp = QFileInfo(tmpDir.path()).canonicalFilePath();
    QDir(tmp).mkpath(QStringLiteral("runtime"));
    qputenv("XDG_RUNTIME_DIR", (tmp + QStringLiteral("/runtime")).toUtf8());

    // Close (and record) every error dialog ROX opens.
    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            dialogs << box->text();
            std::printf("  dialog: %s\n", qPrintable(box->text().replace(QLatin1Char('\n'), QStringLiteral(" | "))));
            box->done(0);
        }
    });
    closer.start(50);

    partA(tmp);
    partB(tmp);

    if (failures == 0) std::printf("imagemounter: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
