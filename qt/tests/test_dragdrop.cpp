#include "filedrop.h"
#include "fileops.h"
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <cstdio>

static bool check(bool ok, const char *message) {
    if (!ok) std::fprintf(stderr, "FAIL %s\n", message);
    return ok;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rox-filer2-qt-test"));
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temp dir")) return 1;
    const QString source = temp.filePath(QStringLiteral("file.txt"));
    const QString destination = temp.filePath(QStringLiteral("dest"));
    QDir().mkpath(destination);
    QFile file(source);
    if (!check(file.open(QIODevice::WriteOnly) && file.write("example") > 0, "create source")) return 1;
    file.close();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(source)});
    QWidget window;
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction,
                          &mime, Qt::LeftButton, Qt::NoModifier);
    FileDrop::allow(&enter);
    if (!check(enter.isAccepted() && enter.dropAction() == Qt::CopyAction,
               "accept local file drag")) return 1;
    QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction,
                        &mime, Qt::LeftButton, Qt::NoModifier);
    FileDrop::allow(&move);
    if (!check(move.isAccepted(), "accept drag move")) return 1;
    QMimeData remote;
    remote.setUrls({QUrl(QStringLiteral("https://example.org/file"))});
    QDragEnterEvent bad(QPoint(10,10), Qt::CopyAction, &remote,
                        Qt::LeftButton, Qt::NoModifier);
    FileDrop::allow(&bad);
    if (!check(!bad.isAccepted(), "reject remote URL")) return 1;
    QDropEvent drop(QPointF(10,10), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::ControlModifier);
    FileDrop::handle(&drop, &window, destination);
    if (!check(drop.isAccepted(), "accept drop to destination")) return 1;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (QFileInfo::exists(QDir(destination).filePath(QStringLiteral("file.txt")))) loop.quit();
    });
    poll.start(20);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!check(QFileInfo::exists(QDir(destination).filePath(QStringLiteral("file.txt"))),
               "Ctrl+Drop copied file")) return 1;
    FileDrop::log(QStringLiteral("unit-test"));
    if (!check(QFileInfo::exists(FileDrop::logPath()), "dnd log created")) return 1;
    std::puts("OK: MIME acceptance, move acceptance, Ctrl+Drop copy, DnD log");
    return 0;
}
