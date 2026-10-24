#include "roxopen.h"

#include "mimeapps.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <sys/stat.h>

namespace RoxOpen {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("RoxOpen", text);
}

QString cleanedAbsolute(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

QString unescapeMountField(const QString &field)
{
    // /proc/self/mountinfo and fstab escape space, tab, newline and backslash
    // as \040 \011 \012 \134.
    QString out;
    out.reserve(field.size());
    for (int i = 0; i < field.size(); ++i) {
        if (field.at(i) == QLatin1Char('\\') && i + 3 < field.size() &&
            field.at(i + 1).isDigit() && field.at(i + 2).isDigit() && field.at(i + 3).isDigit()) {
            bool ok = false;
            const int value = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(QChar(value));
                i += 3;
                continue;
            }
        }
        out.append(field.at(i));
    }
    return out;
}

bool isMounted(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty() || canonical == QLatin1String("/"))
        return false;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // readAll(), not QTextStream::atEnd(): procfs files report size 0, so a
    // stream would think there is nothing to read.
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // id parent major:minor root mountpoint options ...
        if (fields.size() < 5)
            continue;
        if (QDir::cleanPath(unescapeMountField(fields.at(4))) == canonical)
            return true;
    }
    return false;
}

bool isFstabMountPoint(const QString &path)
{
    const QString plain = cleanedAbsolute(path);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (plain == QLatin1String("/") || canonical == QLatin1String("/"))
        return false;
    QFile file(QStringLiteral("/etc/fstab"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() < 2)
            continue;
        const QString mountPoint = QDir::cleanPath(unescapeMountField(fields.at(1)));
        if (!mountPoint.startsWith(QLatin1Char('/')) || mountPoint == QLatin1String("/"))
            continue; // swap, none, ...
        if (mountPoint == plain || (!canonical.isEmpty() && mountPoint == canonical))
            return true;
    }
    return false;
}

bool isAppDir(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    struct stat linkInfo {};
    struct stat targetInfo {};
    if (::lstat(encoded.constData(), &linkInfo) != 0 || ::stat(encoded.constData(), &targetInfo) != 0)
        return false;
    if (!S_ISDIR(targetInfo.st_mode))
        return false;
    // Never trust a world-writable directory (/tmp, ...): anyone could drop an
    // AppRun in it and have it run on the next click.
    if (targetInfo.st_mode & S_IWOTH)
        return false;
    // A mount point is a mount point, not an application.
    if (isMounted(path) || isFstabMountPoint(path))
        return false;

    const QByteArray appRun = QFile::encodeName(path + QStringLiteral("/AppRun"));
    struct stat appInfo {};
    if (::lstat(appRun.constData(), &appInfo) != 0)
        return false;
    // AppRun must have the same owner as the directory (for a symlink: the
    // symlink's owner), to prevent abuse of shared directories.
    if (appInfo.st_uid != linkInfo.st_uid)
        return false;
    return (appInfo.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

bool isDesktopFile(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;
    if (fi.suffix().compare(QLatin1String("desktop"), Qt::CaseInsensitive) == 0)
        return true;
    return QMimeDatabase().mimeTypeForFile(path).name() == QLatin1String("application/x-desktop");
}

bool isExecutableFile(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.isFile()) // follows symlinks, like stat() in diritem.c
        return false;
    if (isDesktopFile(path))
        return true; // launchers need no execute bit

    struct stat info {};
    if (::stat(QFile::encodeName(path).constData(), &info) != 0)
        return false;
    if (!(info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        return false;

    const QMimeType mime = QMimeDatabase().mimeTypeForFile(path);
    const QString name = mime.name();
    // diritem.c: unknown/octet-stream with an X bit is an executable binary;
    // a text/plain file with an X bit and no extension is a shell script.
    if (!mime.isValid() || name == QLatin1String("application/octet-stream"))
        return true;
    if (name == QLatin1String("text/plain") && !fi.fileName().contains(QLatin1Char('.')))
        return true;
    return mime.inherits(QStringLiteral("application/x-executable"));
}

bool runAppDir(const QString &path, QString *error)
{
    const QString program = QDir::cleanPath(path) + QStringLiteral("/AppRun");
    if (QProcess::startDetached(program, {}, QDir::homePath()))
        return true;
    if (error)
        *error = tr("Could not run the application:\n%1").arg(program);
    return false;
}

bool runExecutable(const QString &path, const QString &workingDir, QString *error)
{
    const QString dir = workingDir.isEmpty() ? QFileInfo(path).absolutePath() : workingDir;
    if (QProcess::startDetached(QFileInfo(path).absoluteFilePath(), {}, dir))
        return true;
    if (error)
        *error = tr("Could not run the program:\n%1").arg(path);
    return false;
}

bool runDesktopFile(const QString &path, const QString &workingDir, QString *error)
{
    const DesktopApp app = MimeApps::loadDesktopFile(path);
    if (!app.isValid()) {
        if (error)
            *error = tr("The launcher has no command to run:\n%1").arg(path);
        return false;
    }
    const QString dir = workingDir.isEmpty() ? QFileInfo(path).absolutePath() : workingDir;
    return MimeApps::launch(app, {}, dir, error);
}

} // namespace RoxOpen
