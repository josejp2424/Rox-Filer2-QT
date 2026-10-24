#include "hoverpreview.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QWidget>

#include <csignal>
#include <unistd.h>

HoverPreview *HoverPreview::instance()
{
    static HoverPreview *self = nullptr;
    if (!self)
        self = new HoverPreview(qApp);
    return self;
}

HoverPreview::HoverPreview(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(1000); // same delay as the GTK build
    connect(&m_timer, &QTimer::timeout, this, &HoverPreview::start);
    m_enabled = QSettings().value(QStringLiteral("preview/hoverMedia"), true).toBool();
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        cancelPending();
        stop();
        if (m_process)
            m_process->waitForFinished(1500);
    });
}

void HoverPreview::setEnabled(bool enabled)
{
    m_enabled = enabled;
    QSettings().setValue(QStringLiteral("preview/hoverMedia"), enabled);
    if (!enabled) {
        cancelPending();
        stop();
    }
}

void HoverPreview::cancelPending()
{
    m_timer.stop();
    m_pendingPath.clear();
}

void HoverPreview::consider(QObject *owner, const QString &path, const QPoint &globalPos)
{
    if (!m_enabled || path.isEmpty()) {
        cancelPending();
        stop();
        return;
    }
    if (path == m_playingPath || path == m_pendingPath)
        return;

    const QFileInfo fi(path);
    bool audio = false;
    bool video = false;
    if (fi.isFile() && fi.size() > 0) {
        static const QMimeDatabase db;
        const QString mime = db.mimeTypeForFile(fi).name();
        audio = mime.startsWith(QLatin1String("audio/"));
        video = mime.startsWith(QLatin1String("video/"));
    }

    cancelPending();
    stop();
    if (!audio && !video)
        return;

    m_owner = owner;
    m_pendingPath = path;
    m_pendingVideo = video;
    m_pendingPos = globalPos;
    m_timer.start();
}

void HoverPreview::stopIfOwner(QObject *owner)
{
    if (!owner || m_owner == owner || !m_owner) {
        cancelPending();
        stop();
    }
}

void HoverPreview::stop()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        const qint64 pid = m_process->processId();
        // Negative PID: the whole process group (run-as-spot/su/mpv), never
        // an unrelated mpv instance owned by the user.
        if (pid > 0)
            ::kill(-static_cast<pid_t>(pid), SIGTERM);
    }
    m_playingPath.clear();
    if (m_popup)
        m_popup->hide();
}

QWidget *HoverPreview::videoPopup()
{
    if (!m_popup) {
        m_popup = new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint |
                                           Qt::WindowTransparentForInput |
                                           Qt::WindowDoesNotAcceptFocus |
                                           Qt::WindowStaysOnTopHint);
        m_popup->setAttribute(Qt::WA_NativeWindow);
        m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_popup->setAttribute(Qt::WA_DontCreateNativeAncestors);
        QPalette pal = m_popup->palette();
        pal.setColor(QPalette::Window, Qt::black);
        m_popup->setPalette(pal);
        m_popup->setAutoFillBackground(true);
        m_popup->resize(320, 180);
        connect(qApp, &QCoreApplication::aboutToQuit, m_popup, &QObject::deleteLater);
    }
    return m_popup;
}

void HoverPreview::start()
{
    const QString path = m_pendingPath;
    const bool wantVideo = m_pendingVideo;
    const QPoint pos = m_pendingPos;
    m_pendingPath.clear();
    if (!m_enabled || path.isEmpty() || !m_owner || !QFileInfo(path).isFile())
        return;

    const QString mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
    if (mpv.isEmpty()) {
        if (!m_warnedNoMpv) {
            m_warnedNoMpv = true;
            emit message(tr("mpv is not installed: audio/video preview on hover is unavailable."));
        }
        return;
    }

    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    const bool embedVideo = wantVideo && !wayland;
    if (wantVideo && wayland && !m_warnedWayland) {
        m_warnedWayland = true;
        emit message(tr("Wayland session: video files are previewed as audio only."));
    }

    QStringList args;
    if (embedVideo) {
        QWidget *popup = videoPopup();
        QRect geo(pos + QPoint(18, 22), popup->size());
        if (QScreen *screen = QGuiApplication::screenAt(pos)) {
            const QRect avail = screen->availableGeometry();
            if (geo.right() > avail.right())
                geo.moveRight(pos.x() - 18);
            if (geo.bottom() > avail.bottom())
                geo.moveBottom(pos.y() - 18);
            geo.moveLeft(qMax(avail.left(), geo.left()));
            geo.moveTop(qMax(avail.top(), geo.top()));
        }
        popup->move(geo.topLeft());
        popup->show();
        args << QStringLiteral("--wid=%1").arg(quint64(popup->winId()))
             << QStringLiteral("--no-osc")
             << QStringLiteral("--osd-level=0")
             << QStringLiteral("--no-input-default-bindings")
             << QStringLiteral("--input-vo-keyboard=no")
             << QStringLiteral("--input-cursor=no")
             << QStringLiteral("--keep-open=no");
    } else {
        args << QStringLiteral("--no-video") << QStringLiteral("--audio-display=no");
    }
    args << QStringLiteral("--really-quiet") << QStringLiteral("--no-terminal")
         << QStringLiteral("--") << path;

    QString program = mpv;
    // Same rule as the GTK build: the real run-as-spot script decides whether
    // it has to switch from root to spot; for normal users it just execs.
    const QString runAsSpot = QStandardPaths::findExecutable(QStringLiteral("run-as-spot"));
    if (!runAsSpot.isEmpty()) {
        args.prepend(mpv);
        program = runAsSpot;
    }

    auto *process = new QProcess(this);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process->setWorkingDirectory(QFileInfo(path).absolutePath());
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
    connect(process, &QProcess::finished, this, [this, process] {
        if (m_process == process) {
            m_playingPath.clear();
            if (m_popup)
                m_popup->hide();
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        if (m_process == process) {
            m_playingPath.clear();
            if (m_popup)
                m_popup->hide();
        }
        process->deleteLater();
    });

    m_process = process;
    m_playingPath = path;
    process->start(program, args);
}
