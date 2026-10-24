#pragma once

/*
 * ROX-Filer2-QT - play media while the pointer rests on a file.
 *
 * Port of filer.c audio_preview_* from the GTK build:
 *   - 1 second delay before starting, like the GTK frontend
 *   - mpv in its own process group, stopped with kill(-pgid) so the whole
 *     run-as-spot -> su -> mpv chain dies (Puppy/EssoraPup root sessions)
 *   - only one preview at a time, global to the application
 *
 * Extension over GTK: the GTK build only plays audio files (it passes --no-video
 * and skips video files). Here video files are also previewed: on X11/XLibre
 * mpv renders into a small frameless popup next to the pointer (--wid). On
 * Wayland --wid cannot work, so video files play their audio track only.
 */

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QTimer>

class QProcess;
class QWidget;

class HoverPreview final : public QObject
{
    Q_OBJECT
public:
    static HoverPreview *instance();

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    /* Called when the item under the pointer changes. */
    void consider(QObject *owner, const QString &path, const QPoint &globalPos);
    void stopIfOwner(QObject *owner);
    void stop();

signals:
    void message(const QString &text);

private:
    explicit HoverPreview(QObject *parent = nullptr);
    void start();
    void cancelPending();
    QWidget *videoPopup();

    QTimer m_timer;
    QString m_pendingPath;
    QString m_playingPath;
    bool m_pendingVideo = false;
    QPoint m_pendingPos;
    QPointer<QObject> m_owner;
    QPointer<QProcess> m_process;
    QWidget *m_popup = nullptr;
    bool m_enabled = true;
    bool m_warnedNoMpv = false;
    bool m_warnedWayland = false;
};
