#pragma once

#include <QStringList>
#include <QString>
#include <Qt>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;
class QWidget;

namespace FileDrop {
QString logPath();
void log(const QString &entry);
void traceMove(QWidget *receiver, const QString &destination, bool accepted);
void startDrag(QWidget *source, const QStringList &paths);
bool accepts(const QMimeData *mime);
void allow(QDragEnterEvent *event);
void allow(QDragMoveEvent *event);
void handle(QDropEvent *event, QWidget *parent, const QString &destination);
}
