// Offscreen interaction test: real mouse/key events delivered to FilePane's
// views, checking the ROX click semantics (bind.c / filer.c / run.c).

#include "filepane.h"
#include "roxoptions.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QListView>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

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

static void pump(int ms)
{
    for (int i = 0; i < ms / 10; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
}

static bool waitFor(const std::function<bool()> &cond, int ms = 4000)
{
    for (int i = 0; i < ms / 20; ++i) {
        if (cond())
            return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return cond();
}

static QModelIndex indexOf(QListView *view, const QString &name)
{
    const QAbstractItemModel *m = view->model();
    const QModelIndex root = view->rootIndex();
    for (int r = 0; r < m->rowCount(root); ++r) {
        const QModelIndex idx = m->index(r, 0, root);
        if (idx.data(Qt::DisplayRole).toString() == name)
            return idx;
    }
    return QModelIndex();
}

static QPoint centerOf(QListView *view, const QString &name)
{
    const QModelIndex idx = indexOf(view, name);
    return idx.isValid() ? view->visualRect(idx).center() : QPoint(-1, -1);
}

static void send(QWidget *vp, QEvent::Type type, const QPoint &p, Qt::MouseButton button,
                 Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
    QMouseEvent ev(type, QPointF(p), QPointF(vp->mapToGlobal(p)), button, buttons, mods);
    QApplication::sendEvent(vp, &ev);
}

static void click(QListView *view, const QPoint &p, Qt::MouseButton button = Qt::LeftButton,
                  Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWidget *vp = view->viewport();
    send(vp, QEvent::MouseButtonPress, p, button, mods);
    send(vp, QEvent::MouseButtonRelease, p, button, mods);
}

// What a real double click delivers: press, release, DblClick (instead of the
// second press), release.
static void doubleClick(QListView *view, const QPoint &p, Qt::MouseButton button = Qt::LeftButton)
{
    QWidget *vp = view->viewport();
    send(vp, QEvent::MouseButtonPress, p, button);
    send(vp, QEvent::MouseButtonRelease, p, button);
    send(vp, QEvent::MouseButtonDblClick, p, button);
    send(vp, QEvent::MouseButtonRelease, p, button);
}

static void setMode(RoxOptions::ClickMode mode)
{
    RoxOptions::setClickMode(mode);
}

static bool markerAppears(const QString &marker)
{
    return waitFor([&] { return QFileInfo::exists(marker); }, 3000);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("rox-qt-test"));
    QCoreApplication::setApplicationName(QStringLiteral("clicks-test"));
    { QSettings s; s.clear(); s.sync(); }

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString marker = root + QStringLiteral("/marker");

    // Layout: aaa-folder/ (contains trap.sh), bbb-app/ (AppRun), ccc-script (exec), ddd.txt
    QDir(root).mkpath(QStringLiteral("aaa-folder"));
    ::chmod(QFile::encodeName(root + "/aaa-folder").constData(), 0755);
    write(root + "/aaa-folder/trap.sh",
          QByteArray("#!/bin/sh\necho trap > '") + marker.toUtf8() + ".trap'\n", 0755);
    QDir(root).mkpath(QStringLiteral("bbb-app"));
    ::chmod(QFile::encodeName(root + "/bbb-app").constData(), 0755);
    write(root + "/bbb-app/AppRun", QByteArray("#!/bin/sh\necho app > '") + marker.toUtf8() + "'\n", 0755);
    write(root + "/ccc-script", QByteArray("#!/bin/sh\necho script > '") + marker.toUtf8() + "'\n", 0755);
    write(root + "/ddd.txt", "text\n", 0644);

    FilePane pane;
    pane.resize(900, 600);
    pane.show();
    pane.navigate(root);
    auto *view = pane.findChild<QListView *>(QStringLiteral("sonic-icons"));
    CHECK(view != nullptr);
    if (!view)
        return 2;
    CHECK(waitFor([&] { return indexOf(view, "ddd.txt").isValid() && indexOf(view, "aaa-folder").isValid(); }));
    pump(300); // let the icon layout settle

    // (QtTest's QSignalSpy is not available in every Qt build: count by hand.)
    QStringList newTab;
    int closeCount = 0;
    int resizeCount = 0;
    QString lastResize;
    QObject::connect(&pane, &FilePane::requestNewTab, &pane, [&](const QString &p) { newTab << p; });
    QObject::connect(&pane, &FilePane::hostRequest, &pane,
                     [&](FilePane::HostAction a, const QString &arg) {
        if (a == FilePane::HostAction::CloseAfterOpen) ++closeCount;
        if (a == FilePane::HostAction::ResizeToFit) { ++resizeCount; lastResize = arg; }
    });
    auto closeRequests = [&] { return closeCount; };
    auto backToRoot = [&] {
        pane.navigate(root);
        waitFor([&] { return pane.currentPath() == root && indexOf(view, "ddd.txt").isValid(); });
        pump(200);
        QFile::remove(marker);
        QFile::remove(marker + ".trap");
        newTab.clear();
        closeCount = 0;
        pane.clearFileSelection();
    };

    /* ================= Single click (GTK3 default) ================= */
    setMode(RoxOptions::ClickMode::Single);
    CHECK(pane.currentPath() == root);

    // plain click on a folder enters it
    click(view, centerOf(view, "aaa-folder"));
    CHECK(pane.currentPath() == root + "/aaa-folder");
    backToRoot();

    // plain click on an application directory RUNS it (does not enter it)
    click(view, centerOf(view, "bbb-app"));
    CHECK(markerAppears(marker));
    CHECK(pane.currentPath() == root);
    CHECK(closeRequests() == 0); // button 1 never closes
    backToRoot();

    // plain click on an executable runs it
    click(view, centerOf(view, "ccc-script"));
    CHECK(markerAppears(marker));
    backToRoot();

    // Ctrl+click toggles selection and does NOT open anything
    click(view, centerOf(view, "ccc-script"), Qt::LeftButton, Qt::ControlModifier);
    CHECK(!QFileInfo::exists(marker));
    CHECK(pane.selectedPaths() == QStringList{root + "/ccc-script"});
    click(view, centerOf(view, "ddd.txt"), Qt::LeftButton, Qt::ControlModifier);
    CHECK(pane.selectedPaths().size() == 2);
    click(view, centerOf(view, "ccc-script"), Qt::LeftButton, Qt::ControlModifier);
    CHECK(pane.selectedPaths() == QStringList{root + "/ddd.txt"});
    backToRoot();

    // a press that moves beyond the drag distance does not open (it is a drag)
    {
        QWidget *vp = view->viewport();
        const QPoint p = centerOf(view, "aaa-folder");
        send(vp, QEvent::MouseButtonPress, p, Qt::LeftButton);
        send(vp, QEvent::MouseButtonRelease, p + QPoint(60, 40), Qt::LeftButton);
        CHECK(pane.currentPath() == root);
    }
    backToRoot();

    // middle click on a folder opens a new tab and does not navigate or close
    click(view, centerOf(view, "aaa-folder"), Qt::MiddleButton);
    CHECK(newTab.size() == 1 && newTab.first() == root + "/aaa-folder");
    CHECK(pane.currentPath() == root);
    CHECK(closeRequests() == 0);
    backToRoot();

    // middle click on a program runs it AND asks to close the window/tab
    click(view, centerOf(view, "ccc-script"), Qt::MiddleButton);
    CHECK(markerAppears(marker));
    CHECK(closeRequests() == 1);
    backToRoot();

    // Shift+click "looks inside": an application directory is entered, NOT run
    click(view, centerOf(view, "bbb-app"), Qt::LeftButton, Qt::ShiftModifier);
    CHECK(waitFor([&] { return pane.currentPath() == root + "/bbb-app"; }));
    pump(300);
    CHECK(!QFileInfo::exists(marker));
    backToRoot();

    // middle click on an application directory runs it and asks to close
    click(view, centerOf(view, "bbb-app"), Qt::MiddleButton);
    CHECK(markerAppears(marker));
    CHECK(closeRequests() == 1);
    CHECK(pane.currentPath() == root);
    backToRoot();

    // Alt+Return runs the cursor item and asks to close (ROX return_pressed)
    {
        view->setFocus();
        view->setCurrentIndex(indexOf(view, "ccc-script"));
        QKeyEvent altRet(QEvent::KeyPress, Qt::Key_Return, Qt::AltModifier);
        QApplication::sendEvent(view, &altRet);
        CHECK(markerAppears(marker));
        CHECK(closeRequests() == 1);
    }
    backToRoot();

    // double-click events in single mode must not open a second time
    {
        const QPoint p = centerOf(view, "bbb-app");
        doubleClick(view, p);
        CHECK(markerAppears(marker));
        QFile::remove(marker);
        pump(400);
        CHECK(!QFileInfo::exists(marker)); // ran exactly once
    }
    backToRoot();

    // Return opens the cursor item, Backspace goes to the parent
    {
        view->setFocus();
        view->setCurrentIndex(indexOf(view, "aaa-folder"));
        QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &ret);
        CHECK(pane.currentPath() == root + "/aaa-folder");
        QKeyEvent bs(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(view, &bs);
        CHECK(waitFor([&] { return pane.currentPath() == root; }));
    }
    backToRoot();

    /* ================= Double click ================= */
    setMode(RoxOptions::ClickMode::Double);

    // single click only selects
    click(view, centerOf(view, "aaa-folder"));
    CHECK(pane.currentPath() == root);
    CHECK(pane.selectedPaths() == QStringList{root + "/aaa-folder"});
    click(view, centerOf(view, "ccc-script"));
    CHECK(!QFileInfo::exists(marker));
    CHECK(pane.selectedPaths() == QStringList{root + "/ccc-script"});

    // double click on an executable runs it; on a folder enters it
    doubleClick(view, centerOf(view, "ccc-script"));
    CHECK(markerAppears(marker));
    backToRoot();
    doubleClick(view, centerOf(view, "aaa-folder"));
    CHECK(pane.currentPath() == root + "/aaa-folder");
    backToRoot();

    // middle click ("adjust") toggles selection instead of opening
    click(view, centerOf(view, "ddd.txt"), Qt::MiddleButton);
    CHECK(pane.selectedPaths() == QStringList{root + "/ddd.txt"});
    click(view, centerOf(view, "ddd.txt"), Qt::MiddleButton);
    CHECK(pane.selectedPaths().isEmpty());
    CHECK(newTab.isEmpty());
    backToRoot();

    // Shift+Return on a folder looks inside; Return on an AppDir runs it
    {
        view->setFocus();
        view->setCurrentIndex(indexOf(view, "bbb-app"));
        QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &ret);
        CHECK(markerAppears(marker));
        CHECK(pane.currentPath() == root);
        QFile::remove(marker);
        QKeyEvent sret(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        QApplication::sendEvent(view, &sret); // Look Inside the application
        CHECK(waitFor([&] { return pane.currentPath() == root + "/bbb-app"; }));
        CHECK(!QFileInfo::exists(marker));
    }
    backToRoot();

    /* ================= Single click on folders only ================= */
    setMode(RoxOptions::ClickMode::SingleDirs);
    click(view, centerOf(view, "aaa-folder"));
    CHECK(pane.currentPath() == root + "/aaa-folder");
    backToRoot();
    // an AppDir is not a "folder" here: a single click only selects it
    click(view, centerOf(view, "bbb-app"));
    CHECK(!QFileInfo::exists(marker));
    CHECK(pane.selectedPaths() == QStringList{root + "/bbb-app"});
    backToRoot();
    // files need a double click
    click(view, centerOf(view, "ccc-script"));
    CHECK(!QFileInfo::exists(marker));
    doubleClick(view, centerOf(view, "ccc-script"));
    CHECK(markerAppears(marker));
    backToRoot();

    // The double-click trap: the first click enters aaa-folder, and the DblClick
    // that follows lands on whatever is now under the pointer (trap.sh, in the
    // same cell). It must NOT be executed.
    {
        const QPoint p = centerOf(view, "aaa-folder");
        QWidget *vp = view->viewport();
        send(vp, QEvent::MouseButtonPress, p, Qt::LeftButton);
        send(vp, QEvent::MouseButtonRelease, p, Qt::LeftButton);
        CHECK(pane.currentPath() == root + "/aaa-folder");
        CHECK(waitFor([&] { return indexOf(view, "trap.sh").isValid(); }));
        pump(200);
        const QPoint q = centerOf(view, "trap.sh");
        send(vp, QEvent::MouseButtonDblClick, q, Qt::LeftButton);
        send(vp, QEvent::MouseButtonRelease, q, Qt::LeftButton);
        pump(500);
        CHECK(!QFileInfo::exists(marker + ".trap"));
    }
    backToRoot();

    /* ================= Resize Window ================= */
    setMode(RoxOptions::ClickMode::Single);
    {
        const QSize few = pane.desiredViewSize();
        CHECK(few.width() >= 320 && few.height() >= 160);

        // double click on empty background resizes when the option is on...
        const QPoint bg(view->viewport()->width() - 8, view->viewport()->height() - 8);
        CHECK(!indexOf(view, "ddd.txt").isValid() || view->indexAt(bg) == QModelIndex());
        RoxOptions::setDclickResizes(true);
        resizeCount = 0;
        doubleClick(view, bg);
        CHECK(resizeCount == 1);
        CHECK(lastResize == QStringLiteral("%1x%2").arg(few.width()).arg(few.height()));
        // ...and not when it is off
        RoxOptions::setDclickResizes(false);
        resizeCount = 0;
        doubleClick(view, bg);
        CHECK(resizeCount == 0);
        // Ctrl+E / the menu action path
        pane.requestResize();
        CHECK(resizeCount == 1);

        // more items -> a bigger window wanted
        QDir(root).mkpath(QStringLiteral("many"));
        for (int i = 0; i < 60; ++i)
            write(root + QStringLiteral("/many/file-%1.txt").arg(i, 2, 10, QLatin1Char('0')), "x", 0644);
        pane.navigate(root + "/many");
        CHECK(waitFor([&] { return indexOf(view, "file-59.txt").isValid(); }));
        pump(300);
        const QSize many = pane.desiredViewSize();
        CHECK(many.width() * many.height() > few.width() * few.height());
    }

    { QSettings s; s.clear(); s.sync(); }
    if (failures == 0)
        std::printf("clicks: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
