#include "imagevolumes.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace {
QString decodeMountField(const QByteArray &escaped)
{
    // mountinfo encodes spaces, tabs, newlines and '\\' as octal sequences.
    QByteArray bytes;
    for (qsizetype i = 0; i < escaped.size(); ++i) {
        if (escaped.at(i) == '\\' && i + 3 < escaped.size()) {
            const char a = escaped.at(i+1), b = escaped.at(i+2), c = escaped.at(i+3);
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                bytes.append(char((a-'0')*64 + (b-'0')*8 + (c-'0')));
                i += 3;
                continue;
            }
        }
        bytes.append(escaped.at(i));
    }
    return QString::fromUtf8(bytes);
}

QString loopNode(const QString &source)
{
    // In mountinfo this may be /dev/loopN, /dev/loopNpM, or just loopN.
    static const QRegularExpression pattern(
        QStringLiteral("^(?:/dev/)?(loop[0-9]+)(?:p[0-9]+)?$"));
    return pattern.match(source).captured(1);
}

bool imageFileName(const QString &source)
{
    static const QRegularExpression suffix(
        QStringLiteral("\\.(?:iso|sfs|squashfs|img|udf|fs)$"),
        QRegularExpression::CaseInsensitiveOption);
    return suffix.match(source).hasMatch();
}

bool unmountIsSafe(const QString &mount)
{
    // Never offer eject for Puppy live-layer SFS, rootfs or arbitrary system
    // mounts.  Classification for *display* must NOT depend on this policy.
    static const QStringList prefixes = {
        QStringLiteral("/mnt/"), QStringLiteral("/media/"),
        QStringLiteral("/run/media/"), QStringLiteral("/home/"),
        QStringLiteral("/tmp/")
    };
    for (const QString &prefix : prefixes)
        if (mount.startsWith(prefix)) return true;
    return false;
}

QHash<QString, QString> sysfsLoopNumbers(const QString &blockSysfs)
{
    QHash<QString, QString> result;
    const QDir sysfs(blockSysfs);
    // Reading the dev attribute works even when mountinfo's source is
    // 'none' or a nonstandard label rather than /dev/loopN.
    const QStringList nodes = sysfs.entryList({QStringLiteral("loop*")},
                                              QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    static const QRegularExpression loopName(QStringLiteral("^loop[0-9]+$"));
    for (const QString &name : nodes) {
        if (!loopName.match(name).hasMatch()) continue;
        QFile dev(sysfs.filePath(name + QStringLiteral("/dev")));
        if (!dev.open(QIODevice::ReadOnly)) continue;
        const QString number = QString::fromLatin1(dev.readAll()).trimmed();
        if (!number.isEmpty()) result.insert(number, name);
    }
    return result;
}


}

QList<ImageVolumes::Volume> ImageVolumes::mounted(const QString &mountinfo,
                                                   const QString &blockSysfs)
{
    QList<Volume> result;
    QFile mounts(mountinfo);
    if (!mounts.open(QIODevice::ReadOnly)) return result;

    const auto sysNumbers = sysfsLoopNumbers(blockSysfs);
    QSet<QString> seen;
    // readAll(): /proc files report size 0, so a readLine loop guarded by atEnd
    // reads nothing at all on a real system (bug in 1.0.4).
    const QList<QByteArray> allLines = mounts.readAll().split('\n');
    for (const QByteArray &rawLine : allLines) {
        const QByteArray line = rawLine.trimmed();
        const qsizetype separator = line.indexOf(" - ");
        if (separator < 0) continue;
        const QList<QByteArray> before = line.left(separator).split(' ');
        const QList<QByteArray> after = line.mid(separator + 3).split(' ');
        if (before.size() < 5 || after.size() < 2) continue;
        const QString mountpoint = decodeMountField(before.at(4));
        const QString fs = QString::fromLatin1(after.at(0));
        const QString source = decodeMountField(after.at(1));
        if (mountpoint.isEmpty() || mountpoint == QStringLiteral("/")) continue;

        QString node = loopNode(source);
        if (node.isEmpty()) node = sysNumbers.value(QString::fromLatin1(before.at(2)));
        QString backingPath;
        if (!node.isEmpty()) {
            QFile backing(QDir(blockSysfs).filePath(node + QStringLiteral("/loop/backing_file")));
            if (backing.open(QIODevice::ReadOnly))
                backingPath = QString::fromUtf8(backing.readAll()).trimmed();
        }

        const bool explicitImageSource = imageFileName(source);
        const bool imageFilesystem = fs == QStringLiteral("squashfs") ||
                                     fs == QStringLiteral("iso9660") ||
                                     fs == QStringLiteral("udf");
        // Prefer loop devices; also handle FUSE-based image mounts whose
        // source is the .iso/.sfs file itself.  Filesystems named squashfs
        // and iso9660 with unusual source strings are still images, but
        // protected unless they have a recognizable loop backing device.
        if (node.isEmpty() && !explicitImageSource && !imageFilesystem) continue;
        // Do not report an optical drive /dev/sr0 as a loop-backed image.
        if (node.isEmpty() && source.startsWith(QStringLiteral("/dev/")) &&
            !explicitImageSource) continue;

        // Avoid duplicate representations of the same mounted image.
        const QString key = mountpoint + QLatin1Char('\n') +
                            (node.isEmpty() ? source : node);
        if (seen.contains(key)) continue;
        seen.insert(key);

        Volume volume;
        volume.device = node.isEmpty() ? source :
            (loopNode(source).isEmpty() ? QStringLiteral("/dev/") + node : source);
        volume.imagePath = backingPath.isEmpty() && explicitImageSource ? source : backingPath;
        volume.mountPoint = mountpoint;
        volume.fsType = fs;
        // FUSE/non-loop image mounts have no safe udisksctl -b target here.
        // Display them but don't offer a destructive unmount action.
        volume.protectedMount = node.isEmpty() || !unmountIsSafe(mountpoint);
        result.append(volume);
    }
    std::sort(result.begin(), result.end(), [](const Volume &a, const Volume &b) {
        const QString al = a.imagePath.isEmpty() ? a.mountPoint : a.imagePath;
        const QString bl = b.imagePath.isEmpty() ? b.mountPoint : b.imagePath;
        const int order = QString::compare(al, bl, Qt::CaseInsensitive);
        return order == 0 ? a.mountPoint < b.mountPoint : order < 0;
    });
    return result;
}

QString ImageVolumes::signature(const QList<Volume> &volumes)
{
    QStringList parts;
    parts.reserve(volumes.size());
    for (const Volume &volume : volumes)
        parts << volume.device + QLatin1Char('|') + volume.mountPoint + QLatin1Char('|')
            + volume.imagePath + QLatin1Char('|') + volume.fsType;
    return parts.join(QLatin1Char('\n'));
}
