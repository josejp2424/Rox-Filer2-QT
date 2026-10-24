#include "mainwindow.h"
#include "imagevolumes.h"
#include "imagemounter.h"
#include "filedrop.h"
#include "themebridge.h"
#include "rootpalette.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QLocale>
#include <QRegularExpression>
#include <QProcess>
#include <QSettings>
#include <QStyle>
#include <QStandardPaths>
#include <QTranslator>
#include <QUrl>
#include <cstdio>
#include <unistd.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Executed unprivileged by roxfiler-qt-root to capture the *effective* Qt
    // palette and style, including settings that are not in kdeglobals.
    if (app.arguments().contains(QStringLiteral("--theme-snapshot"))) {
        const QSettings kdeGlobals(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                                       + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
        QString effectiveIcon = kdeGlobals.value(QStringLiteral("Icons/Theme")).toString().trimmed();
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.+-]{1,96}$"))
                 .match(effectiveIcon).hasMatch())
            effectiveIcon = QIcon::themeName();
        if (effectiveIcon.isEmpty() || effectiveIcon == QStringLiteral("hicolor"))
            effectiveIcon = QStringLiteral("hicolor");
        const QByteArray data = (QStringLiteral("PALETTE=")
            + SonicThemeBridge::encode(app.palette()) + QStringLiteral("\nSTYLE=")
            + (app.style() ? app.style()->objectName() : QString())
            + QStringLiteral("\nICON=") + effectiveIcon
            + QStringLiteral("\nFONT=") + app.font().toString()
            + QLatin1Char('\n')).toUtf8();
        fwrite(data.constData(), 1, size_t(data.size()), stdout);
        return 0;
    }
    // Only roxfiler-qt-root forwards this snapshot from the calling user's Qt
    // session.  A direct root invocation retains the ordinary root settings.
    if (geteuid() == 0) {
        QPalette inherited(app.palette());
        if (SonicThemeBridge::apply(inherited, qEnvironmentVariable("ROX_QT_THEME_PALETTE"))) {
            app.setPalette(inherited);
            // Some native Qt styles paint menus/toolbars using their own
            // platform palettes, ignoring QApplication::setPalette(). Apply
            // ONLY literal user-derived colours on those surfaces; this does
            // not install or load any user-owned theme plugin into root.
            RoxRootPalette::applyChrome(app, inherited);
        }
        const QString inheritedFont = qEnvironmentVariable("ROX_QT_THEME_FONT");
        if (!inheritedFont.isEmpty()) {
            QFont font;
            if (font.fromString(inheritedFont)) app.setFont(font);
        }
    }
    QCoreApplication::setOrganizationName(QStringLiteral("ROX-Filer2"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("roxfiler2-qt"));
    QCoreApplication::setApplicationName(QStringLiteral("rox-filer2-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ROX_QT_VERSION));
    FileDrop::log(QStringLiteral("START ROX-Filer2-QT %1 Qt %2 platform=%3 display='%4' log='%5'")
        .arg(QStringLiteral(ROX_QT_VERSION), QString::fromLatin1(qVersion()),
             QGuiApplication::platformName(), qEnvironmentVariable("DISPLAY"), FileDrop::logPath()));
    const bool isRoot = geteuid() == 0;
    if (isRoot) {
        // The root launcher copies only approved kdeglobals colour/icon
        // preferences into an isolated root-owned XDG_CONFIG_HOME. Keep
        // ROX application settings separate from the caller's account.
        QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope,
                           QStringLiteral("/var/empty"));
    }
    FileDrop::log(QStringLiteral("THEME style='%1' QT_QPA_PLATFORMTHEME='%2' QT_STYLE_OVERRIDE='%3' "
                                 "XDG_CONFIG_HOME='%4' XDG_CONFIG_DIRS='%5' window=%6 font='%7'")
        .arg(app.style() ? app.style()->objectName() : QString(),
             qEnvironmentVariable("QT_QPA_PLATFORMTHEME"), qEnvironmentVariable("QT_STYLE_OVERRIDE"),
             qEnvironmentVariable("XDG_CONFIG_HOME"), qEnvironmentVariable("XDG_CONFIG_DIRS"),
             app.palette().color(QPalette::Window).name(), app.font().toString()));
    QGuiApplication::setDesktopFileName(isRoot ? QStringLiteral("roxfiler-qt-root")
                                                : QStringLiteral("rox-filer2-qt"));

    // SonicDE uses the Essora icon set and the standard hicolor fallback.
    QStringList iconPaths = QIcon::themeSearchPaths();
    const QStringList preferred = {
        QStringLiteral("/usr/share/icons"),
        QStringLiteral("/usr/local/share/icons"),
        QStringLiteral("/opt/essora/sonicde/share/icons"),
        QDir::homePath() + QStringLiteral("/.icons"),
        QDir::homePath() + QStringLiteral("/.local/share/icons")
    };
    for (const QString &path : preferred) {
        if (!iconPaths.contains(path))
            iconPaths.prepend(path);
    }
    QIcon::setThemeSearchPaths(iconPaths);
    // Respect the user's Qt/SonicDE icon theme (including the root launcher's
    // sanitized kdeglobals). Only use Essora's icon theme if none was selected.
    const QSettings kdeGlobals(
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
    const QString configuredIconTheme = kdeGlobals.value(QStringLiteral("Icons/Theme")).toString().trimmed();
    const QRegularExpression safeTheme(QStringLiteral("^[A-Za-z0-9_.+-]{1,96}$"));
    const QString inheritedIconTheme = isRoot ? qEnvironmentVariable("ROX_QT_THEME_ICON") : QString();
    if (safeTheme.match(inheritedIconTheme).hasMatch()) {
        QIcon::setThemeName(inheritedIconTheme);
    } else if (safeTheme.match(configuredIconTheme).hasMatch()) {
        QIcon::setThemeName(configuredIconTheme);
    } else {
        const QString sessionIconTheme = QIcon::themeName();
        if (sessionIconTheme.isEmpty() || sessionIconTheme == QStringLiteral("hicolor"))
            QIcon::setThemeName(QStringLiteral("hicolor"));
    }
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));

    const QString iconName = isRoot ? QStringLiteral("rox-filer2-qt-root")
                                    : QStringLiteral("rox-filer2-qt");
    const QIcon iconFallback(isRoot ? QStringLiteral(":/rox-qt/rox-filer2-qt-root.png")
                                    : QStringLiteral(":/rox-qt/rox-filer2-qt.png"));
    app.setWindowIcon(QIcon::fromTheme(iconName, iconFallback));

    QTranslator translator;
    const QLocale locale = QLocale::system();
    QString language = locale.name().section(QLatin1Char('_'), 0, 0);
    const QString translationDir = QStringLiteral(ROX_QT_TRANSLATION_DIR);
    if (translator.load(QStringLiteral("rox-filer2-qt_") + locale.name(), translationDir) ||
        translator.load(QStringLiteral("rox-filer2-qt_") + language, translationDir)) {
        app.installTranslator(&translator);
    }

    QStringList paths;
    const QStringList args = app.arguments().mid(1);
    // Read-only, uses exactly the same detector as the sidebar and desktop.
    // Helps diagnose installations where mount namespaces or source strings
    // differ from the standard loop mount records.
    if (args.contains(QStringLiteral("--list-images"))) {
        // What the interface shows: images mounted by ROX (state files).
        const auto managed = ImageMounter::managedMounts();
        fprintf(stdout, "Images mounted by ROX (shown and unmountable): %lld\n",
                static_cast<long long>(managed.size()));
        fprintf(stdout, "State directory: %s\n",
                qPrintable(ImageMounter::detail::stateDirectory()));
        for (const auto &m : managed) {
            const QByteArray line = (m.mountpoint + QStringLiteral(" | ") + m.blockdev +
                QStringLiteral(" | ") + m.image + (m.method == ImageMounter::Method::Root
                    ? QStringLiteral(" | root") : QStringLiteral(" | udisks")) + QLatin1Char('\n')).toUtf8();
            fwrite(line.constData(), 1, size_t(line.size()), stdout);
        }
        // Diagnostic only: every loop/image mount the kernel reports.
        const auto images = ImageVolumes::mounted();
        fprintf(stdout, "Kernel loop/image mounts (diagnostic, not shown): %lld\n",
                static_cast<long long>(images.size()));
        for (const auto &image : images) {
            const QByteArray line = (image.mountPoint + QStringLiteral(" | ") +
                image.device + QStringLiteral(" | ") + image.fsType +
                QStringLiteral(" | ") + image.imagePath +
                (image.protectedMount ? QStringLiteral(" | protected") :
                                        QStringLiteral(" | user-mount")) + QLatin1Char('\n')).toUtf8();
            fwrite(line.constData(), 1, size_t(line.size()), stdout);
        }
        return 0;
    }
    if (args.contains(QStringLiteral("--desktop"))) {
        if (geteuid() == 0) {
            fprintf(stderr, "Desktop drive icons must not run as root.\n");
            return 1;
        }
        const QString drives = QCoreApplication::applicationDirPath() + QStringLiteral("/rox-qt-drives");
        if (!QProcess::startDetached(drives, {QStringLiteral("--force")})) {
            fprintf(stderr, "Unable to start Qt desktop drive icons.\n");
            return 1;
        }
        return 0;
    }
    for (const QString &arg : args) {
        if (arg == QStringLiteral("--version") || arg == QStringLiteral("-v")) {
            printf("Rox-Filer2-QT %s\n", ROX_QT_VERSION);
            return 0;
        }
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        const QUrl url(arg);
        const QString local = arg.startsWith(QStringLiteral("file:")) ? url.toLocalFile() : arg;
        if (!local.isEmpty())
            paths << local;
    }

    MainWindow window(paths);
    window.show();
    return app.exec();
}
