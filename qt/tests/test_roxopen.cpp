// Offscreen checks for the ROX run-item rules (roxopen.cpp) and the ROX
// behaviour options (roxoptions.cpp). No GUI is shown.

#include "roxopen.h"
#include "roxoptions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTextStream>

#include <cstdio>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static void write(const QString &path, const QByteArray &data, mode_t mode)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.close();
    }
    ::chmod(QFile::encodeName(path).constData(), mode);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("rox-qt-test"));
    QCoreApplication::setApplicationName(QStringLiteral("roxopen-test"));

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();

    /* ---- mountinfo / fstab escaping ---- */
    CHECK(RoxOpen::unescapeMountField(QStringLiteral("/mnt/my\\040disk")) == QStringLiteral("/mnt/my disk"));
    CHECK(RoxOpen::unescapeMountField(QStringLiteral("/a\\134b")) == QStringLiteral("/a\\b"));
    CHECK(RoxOpen::unescapeMountField(QStringLiteral("/plain")) == QStringLiteral("/plain"));
    CHECK(RoxOpen::unescapeMountField(QStringLiteral("/trail\\04")) == QStringLiteral("/trail\\04"));

    /* ---- AppDir ---- */
    QDir(root).mkpath(QStringLiteral("App"));
    ::chmod(QFile::encodeName(root + "/App").constData(), 0755);
    write(root + "/App/AppRun", "#!/bin/sh\nexit 0\n", 0755);
    CHECK(RoxOpen::isAppDir(root + "/App"));

    QDir(root).mkpath(QStringLiteral("NoExec"));
    write(root + "/NoExec/AppRun", "#!/bin/sh\n", 0644);
    CHECK(!RoxOpen::isAppDir(root + "/NoExec"));

    QDir(root).mkpath(QStringLiteral("NoAppRun"));
    CHECK(!RoxOpen::isAppDir(root + "/NoAppRun"));

    QDir(root).mkpath(QStringLiteral("Writable"));
    write(root + "/Writable/AppRun", "#!/bin/sh\n", 0755);
    ::chmod(QFile::encodeName(root + "/Writable").constData(), 0777); // world-writable: never trusted
    CHECK(!RoxOpen::isAppDir(root + "/Writable"));

    CHECK(!RoxOpen::isAppDir(root + "/does-not-exist"));
    write(root + "/plainfile", "x", 0644);
    CHECK(!RoxOpen::isAppDir(root + "/plainfile"));

    // Directory symlink to an application is still an application.
    CHECK(QFile::link(root + "/App", root + "/AppLink"));
    CHECK(RoxOpen::isAppDir(root + "/AppLink"));

    /* ---- executable files ---- */
    write(root + "/script", "#!/bin/sh\necho hi\n", 0755);           // no dot, text + X bit
    CHECK(RoxOpen::isExecutableFile(root + "/script"));
    write(root + "/run.sh", "#!/bin/sh\necho hi\n", 0755);
    CHECK(RoxOpen::isExecutableFile(root + "/run.sh"));
    write(root + "/run.py", "#!/usr/bin/env python3\nprint(1)\n", 0755);
    CHECK(RoxOpen::isExecutableFile(root + "/run.py"));
    write(root + "/notx.sh", "#!/bin/sh\n", 0644);                    // no X bit: opens in an editor
    CHECK(!RoxOpen::isExecutableFile(root + "/notx.sh"));
    write(root + "/notes.txt", "hello\n", 0755);                       // FAT-style 755 text file
    CHECK(!RoxOpen::isExecutableFile(root + "/notes.txt"));
    // A real ELF binary with the X bit.
    CHECK(QFile::copy(QStringLiteral("/bin/true"), root + "/elf"));
    ::chmod(QFile::encodeName(root + "/elf").constData(), 0755);
    CHECK(RoxOpen::isExecutableFile(root + "/elf"));
    CHECK(!RoxOpen::isExecutableFile(root + "/App")); // a directory is not a program

    // .desktop launchers need no X bit.
    write(root + "/app.desktop", "[Desktop Entry]\nType=Application\nName=T\nExec=true\n", 0644);
    CHECK(RoxOpen::isDesktopFile(root + "/app.desktop"));
    CHECK(RoxOpen::isExecutableFile(root + "/app.desktop"));
    CHECK(!RoxOpen::isDesktopFile(root + "/script"));

    /* ---- running: AppRun and executables really start, in the right folder ---- */
    const QString marker = root + "/marker";
    QDir(root).mkpath(QStringLiteral("Touch"));
    ::chmod(QFile::encodeName(root + "/Touch").constData(), 0755);
    write(root + "/Touch/AppRun",
          QByteArray("#!/bin/sh\npwd > '") + marker.toUtf8() + "'\n", 0755);
    QString error;
    CHECK(RoxOpen::runAppDir(root + "/Touch", &error));
    for (int i = 0; i < 100 && !QFileInfo::exists(marker); ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50), QThread::msleep(50);
    CHECK(QFileInfo::exists(marker));
    {
        QFile f(marker);
        f.open(QIODevice::ReadOnly);
        // AppRun runs from the home directory, like run.c run_app().
        CHECK(QString::fromUtf8(f.readAll()).trimmed() == QDir::homePath());
    }
    QFile::remove(marker);

    write(root + "/runme", QByteArray("#!/bin/sh\npwd > '") + marker.toUtf8() + "'\n", 0755);
    CHECK(RoxOpen::runExecutable(root + "/runme", root, &error));
    for (int i = 0; i < 100 && !QFileInfo::exists(marker); ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50), QThread::msleep(50);
    CHECK(QFileInfo::exists(marker));
    {
        QFile f(marker);
        f.open(QIODevice::ReadOnly);
        CHECK(QString::fromUtf8(f.readAll()).trimmed() == QFileInfo(root).canonicalFilePath());
    }

    CHECK(!RoxOpen::runExecutable(root + "/does-not-exist", root, &error));
    CHECK(!error.isEmpty());

    /* ---- mount points ---- */
    CHECK(!RoxOpen::isMounted(QStringLiteral("/")));            // "/" is never a mount point here
    CHECK(!RoxOpen::isFstabMountPoint(QStringLiteral("/")));
    CHECK(!RoxOpen::isMounted(root + "/App"));
    CHECK(RoxOpen::isMounted(QStringLiteral("/proc")));          // always mounted on Linux
    CHECK(!RoxOpen::isFstabMountPoint(root + "/App"));
    // A mount point is never an application, even with an AppRun inside.
    // (/proc has no AppRun, so only check the flag order via a fake: skipped.)

    /* ---- GTK3 Options file ---- */
    const QString options = root + "/Options";
    write(options,
          "<?xml version=\"1.0\"?>\n<Options>\n"
          "<Option name=\"bind_single_click\">0</Option>\n"
          "<Option name=\"bind_single_click_dirs\">1</Option>\n"
          "<Option name=\"bind_new_button_1\">1</Option>\n"
          "</Options>\n", 0644);
    CHECK(RoxOptions::gtkOptionValue(QStringLiteral("bind_single_click"), options) == QStringLiteral("0"));
    CHECK(RoxOptions::gtkOptionValue(QStringLiteral("bind_single_click_dirs"), options) == QStringLiteral("1"));
    CHECK(RoxOptions::gtkOptionValue(QStringLiteral("missing"), options).isEmpty());
    CHECK(RoxOptions::gtkOptionValue(QStringLiteral("x"), root + "/no-such-file").isEmpty());

    /* ---- Qt-saved options win over the GTK3 file and the built-in defaults ---- */
    {
        QSettings s;
        s.clear();
        s.sync();
    }
    // Point XDG_CONFIG_HOME at an empty dir: no GTK3 file => GTK3 built-in defaults.
    qputenv("XDG_CONFIG_HOME", (root + "/emptycfg").toUtf8());
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::Single);
    CHECK(!RoxOptions::newButton1());
    CHECK(RoxOptions::dclickResizes());
    RoxOptions::setClickMode(RoxOptions::ClickMode::Double);
    RoxOptions::setNewButton1(true);
    RoxOptions::setDclickResizes(false);
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::Double);
    CHECK(RoxOptions::newButton1());
    CHECK(!RoxOptions::dclickResizes());
    RoxOptions::setClickMode(RoxOptions::ClickMode::SingleDirs);
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::SingleDirs);
    {
        QSettings s;
        s.clear();
        s.sync();
    }

    if (failures == 0)
        std::printf("roxopen: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
