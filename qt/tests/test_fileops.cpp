// Optional offscreen smoke test. Run after building with
// cmake -DROX_QT_BUILD_TESTS=ON ... && ctest --test-dir ... --output-on-failure
#include "fileops.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QAbstractButton>
#include <QMessageBox>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool check(bool ok, const char *description)
{
    if (!ok) std::fprintf(stderr, "FAIL: %s\n", description);
    return ok;
}

bool writeFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("Sonic file operation test\n") > 0;
}

bool awaitTransfer(const QStringList &sources, const QString &destination, bool move,
                   const QString &newName = QString())
{
    bool done = false;
    bool result = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    FileOps::transfer(nullptr, sources, destination, move, [&](bool ok) {
        done = true;
        result = ok;
        loop.quit();
    }, newName);
    if (!done) {
        timeout.start(10000);
        loop.exec();
    }
    return done && result;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!check(tmp.isValid(), "temporary directory")) return 1;
    const QString sourceDir = tmp.filePath(QStringLiteral("source"));
    const QString destDir = tmp.filePath(QStringLiteral("destination"));
    const QString moveDir = tmp.filePath(QStringLiteral("moved"));
    if (!check(QDir().mkpath(sourceDir) && QDir().mkpath(destDir) && QDir().mkpath(moveDir),
               "create test directories")) return 1;
    // Record all successfully copied destination paths across Qt panes.
    QStringList emittedDestinations;
    QObject::connect(FileOps::events(), &FileTransferEvents::destinationsCreated,
                     &app, [&emittedDestinations](const QStringList &paths) {
        emittedDestinations += paths;
    });
    const QString source = QDir(sourceDir).filePath(QStringLiteral("archivo con espacios.txt"));
    const QString folder = QDir(sourceDir).filePath(QStringLiteral("subfolder"));
    if (!check(writeFile(source) && QDir().mkpath(folder) &&
               writeFile(QDir(folder).filePath(QStringLiteral("inside.txt"))), "create files")) return 1;

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(source), QUrl::fromLocalFile(folder)});
    if (!check(FileOps::pathsFromMime(&mime).size() == 2, "URI list with multiple files")) return 1;
    mime.setUrls({QUrl(QStringLiteral("https://example.org/test")), QUrl::fromLocalFile(source)});
    if (!check(FileOps::pathsFromMime(&mime).isEmpty(), "reject mixed remote/local drag")) return 1;

    FileOps::setClipboard({source}, true);
    bool cut = false;
    if (!check(FileOps::clipboardPaths(&cut) == QStringList{source} && cut, "clipboard cut marker")) return 1;
    FileOps::setClipboard({source}, false);
    if (!check(FileOps::clipboardPaths(&cut) == QStringList{source} && !cut, "clipboard copy marker")) return 1;

    // Regression: copying 3 selected files from /home/essora must NEVER copy
    // the containing home folder or an adjacent unselected directory.
    const QString userHome = tmp.filePath(QStringLiteral("home/essora"));
    const QString chosenDestination = tmp.filePath(QStringLiteral("chosen"));
    if (!check(QDir().mkpath(userHome) && QDir().mkpath(chosenDestination),
               "create faux home and target")) return 1;
    QStringList exactlyThree;
    for (int n = 1; n <= 3; ++n) {
        const QString path = QDir(userHome).filePath(QStringLiteral("selected_%1.txt").arg(n));
        if (!check(writeFile(path), "create selected source")) return 1;
        exactlyThree << path;
    }
    const QString unselectedFolder = QDir(userHome).filePath(QStringLiteral("private_archive"));
    if (!check(QDir().mkpath(unselectedFolder) &&
               writeFile(QDir(unselectedFolder).filePath(QStringLiteral("secret.txt"))),
               "create unselected directory")) return 1;
    if (!check(awaitTransfer(exactlyThree, chosenDestination, false), "copy only 3 selected items")) return 1;
    for (int n = 1; n <= 3; ++n)
        if (!check(QFileInfo::exists(QDir(chosenDestination).filePath(QStringLiteral("selected_%1.txt").arg(n))),
                   "selected file was copied")) return 1;
    if (!check(!QFileInfo::exists(QDir(chosenDestination).filePath(QStringLiteral("private_archive"))) &&
               !QFileInfo::exists(QDir(chosenDestination).filePath(QStringLiteral("essora"))),
               "unselected folder and entire home were NOT copied")) return 1;

    if (!check(awaitTransfer({source, folder}, destDir, false), "copy file and directory")) return 1;
    if (!check(QFileInfo(QDir(destDir).filePath(QFileInfo(source).fileName())).exists() &&
               QFileInfo(QDir(destDir).filePath(QStringLiteral("subfolder/inside.txt"))).exists(),
               "copy results present")) return 1;
    if (!check(awaitTransfer({source}, moveDir, true), "move file")) return 1;
    if (!check(!QFileInfo(source).exists() &&
               QFileInfo(QDir(moveDir).filePath(QStringLiteral("archivo con espacios.txt"))).exists(),
               "move removed original")) return 1;

    const QString moved = QDir(moveDir).filePath(QStringLiteral("archivo con espacios.txt"));
    if (!check(FileOps::createLinksIn(nullptr, {moved}, sourceDir, true), "relative link")) return 1;
    const QString relative = QDir(sourceDir).filePath(QFileInfo(moved).fileName());
    if (!check(QFileInfo(relative).isSymLink(), "relative symlink exists")) return 1;
    char link[4096] = {};
    const ssize_t n = ::readlink(QFile::encodeName(relative).constData(), link, sizeof(link) - 1);
    if (!check(n > 0 && link[0] != '/', "relative target not absolute")) return 1;

    if (!check(FileOps::createLinksIn(nullptr, {moved}, sourceDir, false), "absolute link")) return 1;
    const QString absolute = QDir(sourceDir).filePath(QStringLiteral("archivo con espacios (copy).txt"));
    const ssize_t n2 = ::readlink(QFile::encodeName(absolute).constData(), link, sizeof(link) - 1);
    if (!check(n2 > 0 && link[0] == '/', "absolute symlink target")) return 1;
    std::puts("OK: URI transfer, copy, move, clipboard and both link types");
    // GTK Duplicate/Copy savebox parity: a full absolute path can replace
    // both the destination directory and the filename, without truncation.
    const QString requestedPath = QDir(destDir).filePath(QStringLiteral("archivo nuevo con espacios.txt"));
    if (!check(FileOps::resolveSingleDestination(moved, requestedPath, false) == requestedPath,
               "absolute destination with a new filename")) return 1;
    const QString folderTarget = FileOps::resolveSingleDestination(moved, destDir, false);
    if (!check(folderTarget == QDir(destDir).filePath(QFileInfo(moved).fileName()),
               "existing destination directory expands to full path")) return 1;
    if (!check(FileOps::resolveSingleDestination(moved, destDir + QStringLiteral("/"), false) == folderTarget,
               "destination with trailing slash expands to full path")) return 1;
    if (!check(FileOps::resolveSingleDestination(moved, moved, false) != moved,
               "copy onto source generates a unique full path")) return 1;
    if (!check(FileOps::resolveSingleDestination(moved, moved, true) == moved,
               "move onto source stays unchanged and may be rejected")) return 1;
    if (!check(FileOps::resolveSingleDestination(moved, QStringLiteral("relative/file.txt"), false).isEmpty(),
               "relative path is refused")) return 1;
    if (!check(awaitTransfer({moved}, QFileInfo(requestedPath).absolutePath(), false,
                             QFileInfo(requestedPath).fileName()),
               "copy to full absolute destination path")) return 1;
    if (!check(QFileInfo::exists(requestedPath), "full-path copy wrote requested filename")) return 1;

    const QString fullDestination = QDir(destDir).filePath(QStringLiteral("new renamed file.txt"));
    if (!check(awaitTransfer({moved}, destDir, false, QStringLiteral("new renamed file.txt")),
               "copy to full destination path with new filename")) return 1;
    if (!check(QFileInfo::exists(fullDestination), "renamed destination exists")) return 1;
    if (!check(emittedDestinations.contains(fullDestination),
               "transfer notification includes full destination")) return 1;

    // 1.0.6: on a normal Unix filesystem a root copy still keeps the owner.
    if (geteuid() == 0) {
        const QString owned = tmp.filePath(QStringLiteral("owned-by-1500.txt"));
        const QString keepDir = tmp.filePath(QStringLiteral("keep-owner"));
        if (!check(writeFile(owned) && QDir().mkpath(keepDir)
                       && ::chown(QFile::encodeName(owned).constData(), 1500, 1500) == 0,
                   "ownership fixture")) return 1;
        if (!check(awaitTransfer({owned}, keepDir, false), "root copy on Unix filesystem")) return 1;
        struct stat st {};
        const QByteArray copied = QFile::encodeName(QDir(keepDir).filePath(QStringLiteral("owned-by-1500.txt")));
        if (!check(::stat(copied.constData(), &st) == 0 && st.st_uid == 1500 && st.st_gid == 1500,
                   "root copy on Unix filesystem preserves the owner")) return 1;
    }

    // 1.0.6, optional: ROX_QT_NOOWNER_DIR=<writable FAT/exFAT/NTFS mount>.
    // As root, copying a file owned by another user there must succeed (GTK3
    // action.c ignores failed lchown), and a folder-merge move must finish.
    const QString noOwner = qEnvironmentVariable("ROX_QT_NOOWNER_DIR");
    if (!noOwner.isEmpty() && geteuid() == 0) {
        QString errorShown;
        QTimer watcher;
        QObject::connect(&watcher, &QTimer::timeout, [&errorShown] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            for (QAbstractButton *b : box->buttons())
                if (b->text() == QLatin1String("Replace")) { b->click(); return; }
            errorShown += box->text() + QLatin1Char(' ') + box->detailedText();
            box->done(0);
        });
        watcher.start(50);
        const QString userFile = tmp.filePath(QStringLiteral("user-owned.iso"));
        if (!check(writeFile(userFile) && ::chown(QFile::encodeName(userFile).constData(), 1500, 1500) == 0,
                   "user-owned source file")) return 1;
        if (!check(awaitTransfer({userFile}, noOwner, false),
                   "root copy to ownerless filesystem succeeds")) {
            std::fprintf(stderr, "  dialog: %s\n", qPrintable(errorShown.simplified()));
            return 1;
        }
        if (!check(QFileInfo::exists(QDir(noOwner).filePath(QStringLiteral("user-owned.iso"))),
                   "copied file present on ownerless filesystem")) return 1;

        const QString mergeSrc = tmp.filePath(QStringLiteral("merge"));
        const QString mergeDst = QDir(noOwner).filePath(QStringLiteral("merge"));
        if (!check(QDir().mkpath(mergeSrc) && QDir().mkpath(mergeDst)
                       && writeFile(mergeSrc + QStringLiteral("/a.txt"))
                       && ::chown(QFile::encodeName(mergeSrc + QStringLiteral("/a.txt")).constData(), 1500, 1500) == 0,
                   "merge fixture")) return 1;
        // The destination folder exists: the watcher answers Replace, which is
        // the folder-merge path (cp -a ... && rm -rf).
        if (!check(awaitTransfer({mergeSrc}, noOwner, true),
                   "root folder-merge move to ownerless filesystem succeeds")) return 1;
        if (!check(!QFileInfo::exists(mergeSrc), "merge move removed the source")) return 1;
        if (!check(QFileInfo::exists(mergeDst + QStringLiteral("/a.txt")), "merged file present")) return 1;
        watcher.stop();
        if (!check(errorShown.isEmpty(), "no error dialog on ownerless filesystem")) {
            std::fprintf(stderr, "  dialog: %s\n", qPrintable(errorShown.simplified()));
            return 1;
        }
        std::fprintf(stderr, "PASS: ownerless destination %s\n", qPrintable(noOwner));
    }
    return 0;
}
