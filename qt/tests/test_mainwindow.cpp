// Offscreen smoke test of the whole window: Options dialog (Mouse and opening
// page), window resize, paired windows and "close after open" host requests.

#include "browserpage.h"
#include "filepane.h"
#include "mainwindow.h"
#include "roxoptions.h"
#include "imagemounter.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QToolButton>
#include <QListView>
#include <QKeyEvent>
#include <QScreen>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void pump(int ms)
{
    for (int i = 0; i < ms / 10; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("rox-qt-test"));
    QCoreApplication::setApplicationName(QStringLiteral("window-test"));
    { QSettings s; s.clear(); s.sync(); }
    QTemporaryDir cfg; // empty config dir: no GTK3 Options file
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    QTemporaryDir tmp;
    QDir(tmp.path()).mkpath("a");
    QDir(tmp.path()).mkpath("runtime");
    qputenv("XDG_RUNTIME_DIR", (tmp.path() + "/runtime").toUtf8()); // private Image Mounter state

    MainWindow w({tmp.path()});
    w.resize(560, 420);
    w.show();
    pump(600);
    auto *pane = w.findChild<FilePane *>();
    CHECK(pane != nullptr);
    if (!pane) return 2;

    // Defaults come from the GTK3 built-ins.
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::Single);
    CHECK(RoxOptions::dclickResizes());

    // ---- Options: the page exists, changes are stored, Cancel stores nothing
    bool foundPage = false;
    QTimer::singleShot(500, &w, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        CHECK(dlg != nullptr);
        if (!dlg) return;
        auto *categories = dlg->findChild<QListWidget *>();
        for (int i = 0; categories && i < categories->count(); ++i)
            if (categories->item(i)->text() == QStringLiteral("Mouse and opening")) foundPage = true;
        for (QComboBox *combo : dlg->findChildren<QComboBox *>())
            if (combo->count() == 3 && combo->findData(int(RoxOptions::ClickMode::Double)) >= 0)
                combo->setCurrentIndex(combo->findData(int(RoxOptions::ClickMode::Double)));
        for (QCheckBox *box : dlg->findChildren<QCheckBox *>())
            if (box->text().startsWith(QStringLiteral("Double click on empty space"))) box->setChecked(false);
        dlg->reject(); // Cancel
    });
    emit pane->hostRequest(FilePane::HostAction::Options, QString());
    CHECK(foundPage);
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::Single);   // cancelled: unchanged
    CHECK(RoxOptions::dclickResizes());

    QTimer::singleShot(500, &w, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        CHECK(dlg != nullptr);
        if (!dlg) return;
        for (QComboBox *combo : dlg->findChildren<QComboBox *>())
            if (combo->count() == 3 && combo->findData(int(RoxOptions::ClickMode::Double)) >= 0)
                combo->setCurrentIndex(combo->findData(int(RoxOptions::ClickMode::Double)));
        for (QCheckBox *box : dlg->findChildren<QCheckBox *>())
            if (box->text().startsWith(QStringLiteral("Double click on empty space"))) box->setChecked(false);
        dlg->accept(); // OK
    });
    emit pane->hostRequest(FilePane::HostAction::Options, QString());
    CHECK(RoxOptions::clickMode() == RoxOptions::ClickMode::Double);
    CHECK(!RoxOptions::dclickResizes());
    CHECK(!RoxOptions::newButton1());   // untouched stays at its default

    // ---- window resize request
    const QSize before = w.size();
    auto *firstPage = w.findChild<BrowserPage *>();
    FilePane *active = firstPage ? firstPage->activePane() : pane;
    emit active->hostRequest(FilePane::HostAction::ResizeToFit,
                             QStringLiteral("%1x%2").arg(active->currentViewSize().width() + 100)
                                                    .arg(active->currentViewSize().height() + 60));
    pump(100);
    CHECK(w.size().width() >= before.width() + 90 || w.size().width() == w.screen()->availableGeometry().width());
    CHECK(w.size().height() >= before.height() + 50 || w.size().height() == w.screen()->availableGeometry().height());

    // ---- paired windows == split
    auto *page = w.findChild<BrowserPage *>();
    CHECK(page && !page->splitEnabled());
    emit pane->hostRequest(FilePane::HostAction::PairedWindows, QString());
    pump(100);
    CHECK(page->splitEnabled());
    emit pane->hostRequest(FilePane::HostAction::PairedWindows, QString()); // already split: stays split
    pump(100);
    CHECK(page->splitEnabled());

    // ---- CloseAfterOpen closes the current tab, not the window, when there are several
    auto *tabs = w.findChild<QTabWidget *>();
    CHECK(tabs != nullptr);
    emit pane->requestNewTab(tmp.path() + "/a");
    pump(300);
    CHECK(tabs->count() == 2);
    emit pane->hostRequest(FilePane::HostAction::CloseAfterOpen, QString());
    pump(300);
    CHECK(tabs->count() == 1);
    CHECK(w.isVisible());

    // ---- Mounted images in the GUI (GTK3 parity): shown in Places with an
    // eject button that really unmounts; opening an image opens its contents.
    const QString mksquashfs = ImageMounter::detail::helperPath(QStringLiteral("mksquashfs"));
    if (::geteuid() == 0 && !mksquashfs.isEmpty() &&
        !ImageMounter::detail::helperPath(QStringLiteral("losetup")).isEmpty()) {
        QTimer closer; // record and close any error dialog
        QStringList dialogs;
        QObject::connect(&closer, &QTimer::timeout, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                dialogs << box->text();
                box->done(0);
            }
        });
        closer.start(50);
        QDir(tmp.path()).mkpath("payload");
        QFile f(tmp.path() + "/payload/dentro.txt");
        f.open(QIODevice::WriteOnly); f.write("x"); f.close();
        const QString sfs = tmp.path() + "/Prueba GUI.sfs";
        QProcess::execute(mksquashfs, {tmp.path() + "/payload", sfs, "-quiet", "-noappend"});

        // open the .sfs from the file pane: mounts it and opens a new tab there
        auto *page0 = w.findChild<BrowserPage *>();
        FilePane *active = page0->activePane();
        active->navigate(tmp.path());
        pump(800);
        const int tabsBefore = tabs->count();
        // what the user does: select the image and press Enter (ROX "open")
        QListView *view = nullptr;
        for (QListView *v : active->findChildren<QListView *>())
            if (v->isVisible()) view = v;
        CHECK(view != nullptr);
        if (view) {
            QModelIndex target;
            for (int i = 0; i < 200 && !target.isValid(); ++i) {
                const QModelIndex root = view->rootIndex();
                for (int r = 0; r < view->model()->rowCount(root); ++r) {
                    const QModelIndex idx = view->model()->index(r, 0, root);
                    if (idx.data(Qt::DisplayRole).toString() == QStringLiteral("Prueba GUI.sfs")) target = idx;
                }
                if (!target.isValid()) pump(20);
            }
            CHECK(target.isValid());
            view->setFocus();
            view->setCurrentIndex(target);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(view, &enter);
        }
        for (int i = 0; i < 300 && tabs->count() == tabsBefore; ++i) pump(20);
        const QString mp = ImageMounter::mountedPoint(sfs);
        CHECK(dialogs.isEmpty());
        CHECK(!mp.isEmpty() && mp.startsWith("/media/rox-filer2-images/"));
        CHECK(tabs->count() == tabsBefore + 1);
        auto *newPage = qobject_cast<BrowserPage *>(tabs->widget(tabs->count() - 1));
        CHECK(newPage && newPage->activePane()->currentPath() == mp);

        // Places shows it under "Mounted images" with an eject button
        auto findEject = [&]() -> QToolButton * {
            auto *places = w.findChild<QListWidget *>();
            for (QToolButton *b : places->findChildren<QToolButton *>())
                if (b->toolTip() == QStringLiteral("Unmount Image")) {
                    for (QToolButton *o : b->parentWidget()->findChildren<QToolButton *>())
                        if (o->text() == QStringLiteral("Prueba GUI.sfs")) return b;
                }
            return nullptr;
        };
        QToolButton *eject = nullptr;
        for (int i = 0; i < 300 && !(eject = findEject()); ++i) pump(20);
        CHECK(eject != nullptr);
        bool header = false;
        auto *places = w.findChild<QListWidget *>();
        for (int i = 0; i < places->count(); ++i)
            if (places->item(i)->text() == QStringLiteral("Mounted images")) header = true;
        CHECK(header);
        if (eject) {
            eject->click();
            for (int i = 0; i < 300 && !ImageMounter::mountedPoint(sfs).isEmpty(); ++i) pump(20);
            CHECK(ImageMounter::mountedPoint(sfs).isEmpty());
            CHECK(!ImageMounter::detail::mountpointIsActive(mp));
            for (int i = 0; i < 300 && findEject(); ++i) pump(20);
            CHECK(findEject() == nullptr); // the row disappears
        }
        CHECK(dialogs.isEmpty());
    } else {
        std::printf("window: image GUI checks SKIPPED (need root, losetup, mksquashfs)\n");
    }

    { QSettings s; s.clear(); s.sync(); }
    if (failures == 0) std::printf("window: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
