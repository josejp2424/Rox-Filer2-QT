/* ROX-Filer2-QT drive desktop companion, X11/XLibre, no GVfs.
 * A separate, normal-user process. It does not own the wallpaper or desktop.
 * Uses small bottom-layer tool windows, not a full-screen desktop overlay.
 */
#include "drivemount.h"
#include "imagemounter.h"
#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QPalette>
#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QTranslator>
#include <QtAlgorithms>
#include <QStringList>
#include <QLockFile>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <unistd.h>

namespace {
struct Drive {
    QString path;
    QString label;
    QString mount;
    QString fsType;
    bool removable = false;
    bool image = false;
    bool protectedImage = false;
    QString imagePath;
};

QString debugPath()
{
    const QString custom = qEnvironmentVariable("XDG_STATE_HOME");
    const QString base = QDir::isAbsolutePath(custom) ? custom : QDir::homePath() + QStringLiteral("/.local/state");
    return QDir(base).filePath(QStringLiteral("rox-filer2-qt/drives.log"));
}
void trace(const QString &entry)
{
    const QString path = debugPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return;
    const QFileInfo info(path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | (info.size() > 524288 ? QIODevice::Truncate : QIODevice::Append))) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QString safe(entry);
    safe.replace(QLatin1Char('\n'), QLatin1Char(' '));
    safe.replace(QLatin1Char('\r'), QLatin1Char(' '));
    file.write(QStringLiteral("[%1] %2\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate), safe).toUtf8());
}

QList<Drive> enumerate()
{
    QList<Drive> out;
    const QString lsblk = QStandardPaths::findExecutable(QStringLiteral("lsblk"));
    if (lsblk.isEmpty()) trace(QStringLiteral("lsblk not installed; displaying mounted images only"));
    QProcess p;
    if (!lsblk.isEmpty()) p.start(lsblk, {QStringLiteral("-J"), QStringLiteral("-b"), QStringLiteral("-o"),
                    QStringLiteral("PATH,LABEL,TYPE,FSTYPE,MOUNTPOINT,RM,HOTPLUG,SIZE,MODEL")});
    if (!lsblk.isEmpty() && (!p.waitForFinished(2000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)) {
        p.kill(); p.waitForFinished(500);
        trace(QStringLiteral("lsblk failed or timed out"));
    }
    const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
    std::function<void(const QJsonArray&, bool)> walk = [&](const QJsonArray &arr, bool parentRemovable) {
        for (const QJsonValue &val : arr) {
            const QJsonObject obj = val.toObject();
            const QString type = obj.value(QStringLiteral("type")).toString();
            const bool removable = parentRemovable || obj.value(QStringLiteral("rm")).toVariant().toBool()
                || obj.value(QStringLiteral("hotplug")).toVariant().toBool();
            const QString fsType = obj.value(QStringLiteral("fstype")).toString();
            const QString path = obj.value(QStringLiteral("path")).toString();
            const bool useful = type == QStringLiteral("part") || type == QStringLiteral("crypt")
                || type == QStringLiteral("lvm") || type.startsWith(QStringLiteral("raid"))
                || (type == QStringLiteral("disk") && !fsType.isEmpty());
            if (useful && path.startsWith(QStringLiteral("/dev/")) && !fsType.isEmpty() &&
                !QRegularExpression(QStringLiteral("^/dev/loop[0-9]+(?:p[0-9]+)?$")).match(path).hasMatch() &&
                fsType != QStringLiteral("swap") && fsType != QStringLiteral("crypto_LUKS")) {
                Drive d;
                d.path = path;
                d.label = obj.value(QStringLiteral("label")).toString();
                d.mount = obj.value(QStringLiteral("mountpoint")).toString();
                d.fsType = fsType;
                d.removable = removable;
                if (d.label.isEmpty() && !d.mount.isEmpty() && d.mount != QStringLiteral("/"))
                    d.label = QFileInfo(d.mount).fileName();
                if (d.label.isEmpty()) d.label = QFileInfo(d.path).fileName();
                out.append(d);
            }
            walk(obj.value(QStringLiteral("children")).toArray(), removable);
        }
    };
    walk(doc.object().value(QStringLiteral("blockdevices")).toArray(), false);
    // Raw loop devices never appear (filtered above).  Like GTK3 drives.c,
    // show only the images ROX itself mounted, from the Image Mounter state
    // files: Puppy/Essora's own SFS loop layers are never listed.
    for (const ImageMounter::Managed &image : ImageMounter::managedMounts()) {
        Drive d;
        d.path = image.blockdev;
        d.mount = image.mountpoint;
        d.fsType = image.image.endsWith(QStringLiteral(".iso"), Qt::CaseInsensitive)
            ? QStringLiteral("iso9660") : QString();
        d.label = QFileInfo(image.image).fileName();
        d.image = true;
        d.protectedImage = false;
        d.imagePath = image.image;
        out.append(d);
    }
    std::sort(out.begin(), out.end(), [](const Drive &a, const Drive &b) {
        return QString::localeAwareCompare(a.label, b.label) < 0;
    });
    return out;
}

class DriveIcon final : public QWidget {
public:
    std::function<void(const Drive &, const QString &)> request;
    explicit DriveIcon(QWidget *parent = nullptr) : QWidget(parent,
            Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
            Qt::WindowStaysOnBottomHint | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);
        setObjectName(QStringLiteral("rox-qt-drive-icon"));
    }
    void setDrive(const Drive &next, int iconSize, bool drawBackground) {
        drive = next;
        sizePx = iconSize;
        backgroundEnabled = drawBackground;
        setFixedSize(qMax(100, iconSize + 36), iconSize + 48);
        QStringList info = {drive.label, drive.path,
            drive.mount.isEmpty() ? tr("Not mounted") : drive.mount};
        if (drive.image) {
            if (!drive.imagePath.isEmpty()) info << drive.imagePath;
            if (drive.protectedImage) info << tr("System image (protected)");
        }
        setToolTip(info.join(QLatin1Char('\n')));
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        // Clear old alpha pixels explicitly when switching from framed to
        // frameless icons. WA_NoSystemBackground does not clear for us.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor surface = palette().color(QPalette::Window);
        const QColor text = palette().color(QPalette::WindowText);
        // Default is transparent: the wallpaper remains visible around each drive.
        // This is an opt-in icon background, not a native window decoration.
        if (backgroundEnabled) {
            QColor bg = surface;
            bg.setAlpha(195);
            painter.setPen(Qt::NoPen);
            painter.setBrush(bg);
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 9, 9);
        }
        const QString iconName = drive.image
            ? (drive.fsType == QStringLiteral("iso9660") || drive.fsType == QStringLiteral("udf")
                ? QStringLiteral("media-optical") : QStringLiteral("drive-harddisk"))
            : (drive.removable ? QStringLiteral("drive-removable-media")
                               : QStringLiteral("drive-harddisk"));
        QIcon icon = QIcon::fromTheme(iconName);
        if (icon.isNull()) icon = QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon);
        const QPixmap pix = icon.pixmap(sizePx, sizePx);
        painter.drawPixmap((width() - sizePx) / 2, 5, pix);
        const QString label = fontMetrics().elidedText(drive.label, Qt::ElideRight, width() - 12);
        const QRect captionRect(4, sizePx + 8, width() - 8, 24);
        if (!backgroundEnabled) {
            // Subtle shadow only behind glyphs, not a tile under the icon.
            painter.setPen(QColor(0, 0, 0, 180));
            painter.drawText(captionRect.translated(1, 1), Qt::AlignCenter, label);
        }
        painter.setPen(text);
        painter.drawText(captionRect, Qt::AlignCenter, label);
        if (!drive.mount.isEmpty()) {
            QIcon check = QIcon::fromTheme(QStringLiteral("emblem-mounted"));
            if (check.isNull()) check = QIcon::fromTheme(QStringLiteral("emblem-default"));
            check.paint(&painter, QRect((width() + sizePx) / 2 - 18, sizePx - 13, 18, 18));
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && request) request(drive, QStringLiteral("open"));
        QWidget::mouseDoubleClickEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override {
        QMenu menu;
        QAction *open = menu.addAction(tr("Open"));
        QAction *mount = menu.addAction(tr("Mount"));
        QAction *unmount = menu.addAction(tr("Unmount"));
        mount->setEnabled(!drive.image && drive.mount.isEmpty());
        unmount->setEnabled(!drive.protectedImage && !drive.mount.isEmpty() &&
            !QStringList{QStringLiteral("/"), QStringLiteral("/boot"), QStringLiteral("/boot/efi")}.contains(drive.mount));
        menu.addSeparator();
        QAction *info = menu.addAction(tr("Drive information"));
        menu.addSeparator();
        QMenu *layoutMenu = menu.addMenu(QCoreApplication::translate("DriveIcon", "Icon layout"));
        QAction *vertical = layoutMenu->addAction(QCoreApplication::translate("DriveIcon", "Vertical"));
        QAction *horizontal = layoutMenu->addAction(QCoreApplication::translate("DriveIcon", "Horizontal"));
        vertical->setCheckable(true);
        horizontal->setCheckable(true);
        QSettings settings;
        const bool isHorizontal = settings.value(QStringLiteral("desktop/layout"),
                                                 QStringLiteral("vertical")).toString() == QStringLiteral("horizontal");
        vertical->setChecked(!isHorizontal);
        horizontal->setChecked(isHorizontal);
        QMenu *positionMenu = menu.addMenu(QCoreApplication::translate("DriveIcon", "Icon position"));
        const QString currentEdge = settings.value(QStringLiteral("desktop/edge"),
                                                   QStringLiteral("right")).toString();
        QMap<QAction *, QString> positionActions;
        const struct { const char *label; const char *key; } positions[] = {
            {"Top", "top"}, {"Bottom", "bottom"}, {"Left", "left"}, {"Right", "right"}
        };
        for (const auto &position : positions) {
            QAction *option = positionMenu->addAction(
                QCoreApplication::translate("DriveIcon", position.label));
            option->setCheckable(true);
            const QString side = QString::fromLatin1(position.key);
            option->setChecked(side == currentEdge);
            positionActions.insert(option, side);
        }
        QAction *background = menu.addAction(QCoreApplication::translate("DriveIcon", "Show icon background"));
        background->setCheckable(true);
        background->setChecked(settings.value(QStringLiteral("desktop/showBackground"), false).toBool());
        QAction *choice = menu.exec(event->globalPos());
        if (!choice) return;
        if (positionActions.contains(choice) || choice == vertical || choice == horizontal || choice == background) {
            if (positionActions.contains(choice))
                settings.setValue(QStringLiteral("desktop/edge"), positionActions.value(choice));
            else if (choice == vertical || choice == horizontal)
                settings.setValue(QStringLiteral("desktop/layout"),
                                  choice == horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"));
            else
                settings.setValue(QStringLiteral("desktop/showBackground"), background->isChecked());
            settings.sync();
            if (request) request(drive, QStringLiteral("refresh-settings"));
            return;
        }
        if (!request) return;
        if (choice == open) request(drive, QStringLiteral("open"));
        else if (choice == mount) request(drive, QStringLiteral("mount"));
        else if (choice == unmount) request(drive, QStringLiteral("unmount"));
        else if (choice == info) {
            QMessageBox::information(this, tr("Drive information"),
                  QStringLiteral("%1\n%2\n%3\n%4\n%5")
                    .arg(drive.label, drive.path, drive.fsType,
                         drive.mount.isEmpty() ? tr("Not mounted") : drive.mount,
                         drive.imagePath));
        }
    }
private:
    Drive drive;
    int sizePx = 64;
    bool backgroundEnabled = false;
};

class DesktopDriveController final : public QObject {
public:
    explicit DesktopDriveController(bool force, QObject *parent = nullptr) : QObject(parent), m_force(force) {
        connect(&poll, &QTimer::timeout, this, [this] { refresh(); });
        poll.setInterval(4000);
        poll.start();
        refresh();
    }
    ~DesktopDriveController() override { qDeleteAll(widgets); }
private:
    void operate(const Drive &d, const QString &operation) {
        if (operation == QStringLiteral("refresh-settings")) {
            refresh();
            return;
        }
        if (operation == QStringLiteral("open") && !d.mount.isEmpty()) {
            QProcess::startDetached(QStringLiteral("/usr/bin/rox-filer2-qt"), {d.mount});
            return;
        }
        if (d.image && d.protectedImage) return;
        if (d.image && operation == QStringLiteral("mount")) return;
        if (d.image && operation == QStringLiteral("unmount")) {
            // Image Mounter: umount + detach the loop + remove the state file.
            if (pending.contains(d.path)) return;
            pending.insert(d.path);
            ImageMounter::unmount(nullptr, d.imagePath, [this, d](bool ok) {
                pending.remove(d.path);
                trace(QStringLiteral("image unmount %1 success=%2")
                      .arg(d.imagePath, ok ? QStringLiteral("yes") : QStringLiteral("no")));
                refresh();
            });
            return;
        }
        if (pending.contains(d.path)) return;
        pending.insert(d.path);
        const auto action = operation == QStringLiteral("unmount")
            ? SonicDriveMount::Action::Unmount : SonicDriveMount::Action::Mount;
        SonicDriveMount::run(this, nullptr, action, d.path,
            [this, d, operation](bool ok, const QString &mountPath) {
            pending.remove(d.path);
            trace(QStringLiteral("manual %1 %2 success=%3")
                  .arg(operation, d.path, ok ? QStringLiteral("yes") : QStringLiteral("no")));
            if (ok && operation == QStringLiteral("open")) {
                QString path = mountPath;
                if (path.isEmpty()) {
                    const auto devices = enumerate();
                    for (const auto &x : devices)
                        if (x.path == d.path) { path = x.mount; break; }
                }
                if (!path.isEmpty())
                    QProcess::startDetached(QStringLiteral("/usr/bin/rox-filer2-qt"), {path});
            }
            refresh();
        });
    }
    void refresh() {
        QSettings settings;
        settings.sync();
        if (!m_force && !settings.value(QStringLiteral("desktop/drivesEnabled"), false).toBool()) {
            trace(QStringLiteral("disabled; exiting"));
            QCoreApplication::quit();
            return;
        }
        const bool internal = settings.value(QStringLiteral("desktop/showInternal"), false).toBool();
        const bool removable = settings.value(QStringLiteral("desktop/showRemovable"), true).toBool();
        const bool onlyMounted = settings.value(QStringLiteral("desktop/onlyMounted"), false).toBool();
        const bool showSystem = settings.value(QStringLiteral("desktop/showSystem"), false).toBool();
        const bool showImages = settings.value(QStringLiteral("desktop/showImages"), true).toBool();
        const bool autoMount = settings.value(QStringLiteral("desktop/autoMountRemovable"), false).toBool();
        const QString edge = settings.value(QStringLiteral("desktop/edge"), QStringLiteral("right")).toString();
        const bool horizontal = settings.value(QStringLiteral("desktop/layout"),
                           QStringLiteral("vertical")).toString() == QStringLiteral("horizontal");
        const bool background = settings.value(QStringLiteral("desktop/showBackground"), false).toBool();
        const int topMargin = qBound(0, settings.value(QStringLiteral("desktop/topMargin"), 96).toInt(), 2000);
        const int edgeMargin = qBound(0, settings.value(QStringLiteral("desktop/edgeMargin"), 18).toInt(), 400);
        const int horizontalOffset = qBound(0, settings.value(QStringLiteral("desktop/horizontalOffset"), 18).toInt(), 2000);
        const int iconSize = qBound(32, settings.value(QStringLiteral("desktop/iconSize"), 64).toInt(), 96);
        const QList<Drive> listed = enumerate();
        QSet<QString> visible;
        QList<Drive> shown;
        for (const Drive &d : listed) {
            if (d.image) {
                if (!showImages) continue; // Protected images are VISIBLE; only eject is restricted.
            } else if ((d.removable && !removable) || (!d.removable && !internal)) continue;
            if (!showSystem && (d.mount == QStringLiteral("/") || d.mount == QStringLiteral("/boot") || d.mount == QStringLiteral("/boot/efi"))) continue;
            if (autoMount && !d.image && d.removable && d.mount.isEmpty() && !attempted.contains(d.path)) {
                attempted.insert(d.path);
                const QString udisks = QStandardPaths::findExecutable(QStringLiteral("udisksctl"));
                if (!udisks.isEmpty()) {
                    // No privilege escalation or password popups during login.
                    QProcess::startDetached(udisks, {QStringLiteral("mount"), QStringLiteral("--no-user-interaction"),
                                                    QStringLiteral("-b"), d.path});
                    trace(QStringLiteral("silent automount requested: ") + d.path);
                }
            }
            if (onlyMounted && d.mount.isEmpty()) continue;
            const QString key = d.image ? d.path + QLatin1Char('\n') + d.mount : d.path;
            visible.insert(key);
            shown.append(d);
        }
        for (auto it = widgets.begin(); it != widgets.end();) {
            if (!visible.contains(it.key())) { delete it.value(); it = widgets.erase(it); }
            else ++it;
        }
        const QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect area = screen->availableGeometry();
        const int width = qMax(100, iconSize + 36);
        const int height = iconSize + 48;
        const int minX = area.left() + 8;
        const int maxX = area.right() - width - 8;
        const int minY = area.top() + 8;
        const int maxY = area.bottom() - height - 8;
        // Edge and layout are independent: Top/Bottom/Left/Right anchors
        // can each flow horizontally (rows) or vertically (columns).
        const bool fromRight = edge == QStringLiteral("right");
        const bool fromBottom = edge == QStringLiteral("bottom");
        const bool fromTop = edge == QStringLiteral("top");
        const bool sideAnchor = !fromBottom && !fromTop;
        const int firstX = sideAnchor
            ? (fromRight ? maxX - edgeMargin : minX + edgeMargin)
            : minX + horizontalOffset;
        const int firstY = sideAnchor ? minY + topMargin
            : (fromBottom ? maxY - edgeMargin : minY + edgeMargin);
        const int stepX = (width + 10) * (fromRight ? -1 : 1);
        const int stepY = (height + 10) * (fromBottom ? -1 : 1);
        int x = firstX;
        int y = firstY;
        for (const Drive &d : shown) {
            const QString key = d.image ? d.path + QLatin1Char('\n') + d.mount : d.path;
            DriveIcon *icon = widgets.value(key, nullptr);
            if (!icon) {
                icon = new DriveIcon;
                icon->request = [this](const Drive &item, const QString &operation) { operate(item, operation); };
                widgets.insert(key, icon);
                trace(QStringLiteral("icon added: ") + d.path);
            }
            icon->setDrive(d, iconSize, background);
            if (horizontal) {
                if (x < minX || x > maxX) {
                    x = firstX;
                    y += stepY;
                }
            } else if (y < minY || y > maxY) {
                y = firstY;
                x += stepX;
            }
            if (x < minX || x > maxX || y < minY || y > maxY) {
                // No hidden overlap with panels, widgets or another screen.
                icon->hide();
                continue;
            }
            icon->move(x, y);
            if (!icon->isVisible()) icon->show();
            if (horizontal) x += stepX;
            else y += stepY;
        }
    }
    bool m_force = false;
    QTimer poll;
    QMap<QString,DriveIcon*> widgets;
    QSet<QString> pending;
    QSet<QString> attempted;
};
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ROX-Filer2"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("roxfiler2-qt"));
    QCoreApplication::setApplicationName(QStringLiteral("rox-filer2-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ROX_QT_VERSION));
    app.setQuitOnLastWindowClosed(false);
    QTranslator translator;
    const QLocale locale = QLocale::system();
    const QString translationDir = QStringLiteral("/usr/share/rox-filer2-qt/translations");
    const QString lang = locale.name().section(QLatin1Char('_'), 0, 0);
    if (translator.load(QStringLiteral("rox-filer2-qt_") + locale.name(), translationDir) ||
        translator.load(QStringLiteral("rox-filer2-qt_") + lang, translationDir))
        app.installTranslator(&translator);
    // Match the session's SonicDE theme, using Essora only as a fallback.
    // Mirror the main Qt frontend; do not force the icon set on every user.
    const QSettings kdeGlobals(
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
    const QString configuredTheme = kdeGlobals.value(QStringLiteral("Icons/Theme")).toString().trimmed();
    const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9_.+-]{1,96}$"));
    if (safeName.match(configuredTheme).hasMatch()) {
        QIcon::setThemeName(configuredTheme);
    } else if (QIcon::themeName().isEmpty() || QIcon::themeName() == QStringLiteral("hicolor")) {
        QIcon::setThemeName(QStringLiteral("hicolor"));
    }
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    if (geteuid() == 0) { trace(QStringLiteral("refusing root desktop icon session")); return 1; }
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        trace(QStringLiteral("desktop icons require X11/XLibre; compositor integration needed for Wayland"));
        return 1;
    }
    const bool force = app.arguments().contains(QStringLiteral("--force"));
    if (!force && !QSettings().value(QStringLiteral("desktop/drivesEnabled"), false).toBool()) return 0;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString directory = !runtime.isEmpty() && QFileInfo(runtime).isDir() ? runtime : QDir::tempPath();
    QLockFile lock(QDir(directory).filePath(QStringLiteral("rox-qt-drives-%1.lock").arg(getuid())));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) return 0;
    trace(QStringLiteral("START version %1 Qt %2").arg(QStringLiteral(ROX_QT_VERSION), QString::fromLatin1(qVersion())));
    DesktopDriveController controller(force);
    return app.exec();
}
