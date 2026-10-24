#include "imagevolumes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

static bool writeFile(const QString &path, const QByteArray &data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    const QString sys = temp.path() + QStringLiteral("/sys");
    const QString mountinfo = temp.path() + QStringLiteral("/mountinfo");
    const QByteArray fixture =
        "41 1 7:0 / /mnt/my\\040image ro,nosuid - squashfs /dev/loop7 ro\n"
        "42 1 7:1 / /run/media/essora/Essora ro - iso9660 /dev/loop8 ro\n"
        "43 1 7:2 / /initrd/pup_ro2 ro - squashfs /dev/loop9 ro\n"
        "44 1 8:1 / /mnt/not-image rw - ext4 /dev/sda1 rw\n"
        "45 1 7:1 / /mnt/image-part ro - ext4 /dev/loop8p1 ro\n"
        // Some mount implementations supply 'none' instead of /dev/loopN.
        "46 1 7:25 / /media/manual ro - iso9660 none ro\n"
        // FUSE mount with the image path as the mount source.
        "47 1 0:42 / /mnt/other-image ro - fuse.fuseiso /home/essora/My\\040ISO.iso ro\n"
        // System squashfs with no exposed loop device: must be VISIBLE,
        // while still protected from accidental unmount.
        "48 1 0:43 / /initrd/layers/sfs ro - squashfs none ro\n"
        // Physical optical media must not masquerade as a disk image.
        "49 1 11:0 / /media/cdrom0 ro - iso9660 /dev/sr0 ro\n"
        // Additional obscure mount source, loop identified by major:minor.
        "50 1 7:26 / /mnt/odd ro - squashfs strange ro\n";
    if (!writeFile(sys + QStringLiteral("/loop7/loop/backing_file"), "/home/essora/my image.sfs\n") ||
        !writeFile(sys + QStringLiteral("/loop8/loop/backing_file"), "/home/essora/Essora.iso\n") ||
        !writeFile(sys + QStringLiteral("/loop9/loop/backing_file"), "/opt/system/root.sfs\n") ||
        !writeFile(sys + QStringLiteral("/loop25/loop/backing_file"), "/home/essora/manual.iso\n") ||
        !writeFile(sys + QStringLiteral("/loop25/dev"), "7:25\n") ||
        !writeFile(sys + QStringLiteral("/loop26/loop/backing_file"), "/home/essora/strange.sfs\n") ||
        !writeFile(sys + QStringLiteral("/loop26/dev"), "7:26\n") ||
        !writeFile(mountinfo, fixture)) return 2;

    const auto images = ImageVolumes::mounted(mountinfo, sys);
    if (images.size() != 8) {
        fprintf(stderr, "expected 8 mounted images, got %lld\n", static_cast<long long>(images.size()));
        return 3;
    }
    bool sfs = false, iso = false, system = false, partition = false;
    bool unnamedLoop = false, fuse = false, anonymousSquashfs = false, oddLoop = false;
    for (const auto &item : images) {
        if (item.device == QStringLiteral("/dev/loop7"))
            sfs = item.mountPoint == QStringLiteral("/mnt/my image") &&
                  item.imagePath.endsWith(QStringLiteral("my image.sfs")) && !item.protectedMount;
        if (item.device == QStringLiteral("/dev/loop8"))
            iso = item.fsType == QStringLiteral("iso9660") && !item.protectedMount;
        if (item.device == QStringLiteral("/dev/loop9"))
            system = item.protectedMount;
        if (item.device == QStringLiteral("/dev/loop8p1"))
            partition = item.imagePath.endsWith(QStringLiteral("Essora.iso")) && !item.protectedMount;
        if (item.device == QStringLiteral("/dev/loop25"))
            unnamedLoop = item.imagePath.endsWith(QStringLiteral("manual.iso")) && !item.protectedMount;
        if (item.mountPoint == QStringLiteral("/mnt/other-image"))
            fuse = item.imagePath == QStringLiteral("/home/essora/My ISO.iso") && item.protectedMount;
        if (item.mountPoint == QStringLiteral("/initrd/layers/sfs"))
            anonymousSquashfs = item.protectedMount;
        if (item.device == QStringLiteral("/dev/loop26"))
            oddLoop = item.imagePath.endsWith(QStringLiteral("strange.sfs"));
        if (item.device == QStringLiteral("/dev/sr0")) return 4;
    }
    if (!sfs || !iso || !system || !partition || !unnamedLoop || !fuse ||
        !anonymousSquashfs || !oddLoop) return 5;
    const QString before = ImageVolumes::signature(images);
    if (!writeFile(mountinfo, "41 1 7:0 / /mnt/my\\040image ro - squashfs /dev/loop7 ro\n")) return 6;
    if (before == ImageVolumes::signature(ImageVolumes::mounted(mountinfo, sys))) return 7;
    puts("PASS: ISO/SFS/IMG detection, hidden loop source, FUSE images, system protection, mount changes");
    return 0;
}
